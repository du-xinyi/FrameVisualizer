#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "frame_meta.pb.h"
#include "frame_transport/frame_receiver.h"
#include "ui/ui.h"

/**
 * @brief FrameVisualizer 的消息接收、解码调度与应用主循环
 *
 * @details 接收 protobuf 多部分帧及兼容的单部分图像消息，将解码工作交给后台线程，
 * 并在主线程维护历史帧、统计状态和 UI 生命周期
 */
namespace
{
constexpr std::size_t kMaxLegacyHeaderBytes = 128; ///< 传统 Mat 文本头允许占用的最大字节数
constexpr std::size_t kMaxPayloadBytes = 512U * 1024U * 1024U; ///< 单帧负载的防御性大小上限
constexpr std::size_t kFrameHistoryCapacity = 60; ///< 暂停浏览缓存保留的最大帧数
constexpr std::size_t kFrameHistoryMaxBytes = 256U * 1024U * 1024U; ///< 历史缓存的内存预算


using ByteSpan = std::span<const unsigned char>;
using ZmqMessage = frameviz::FrameMessagePart;

/** @brief 为 ZMQ 消息数据创建不拥有内存的只读字节视图 */
ByteSpan messageBytes(const ZmqMessage &message)
{
    return {message.data(), message.size()};
}

/**
 * @brief 计算两个尺寸的乘积并检测 size_t 溢出
 *
 * @param left 左操作数
 * @param right 右操作数
 *
 * @return 可表示的乘积，溢出时返回 std::nullopt
 */
std::optional<std::size_t> checkedMultiply(const std::size_t left, const std::size_t right)
{
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
    {
        return std::nullopt;
    }

    return left * right;
}

/**
 * @brief 验证整数是否为 OpenCV 可构造的 Mat 类型编码
 *
 * @param type 待验证的 OpenCV 类型值
 *
 * @return 深度、通道数和编码组合均有效时返回 true
 */
bool isValidCvType(const int type)
{
    if (type < 0)
    {
        return false;
    }

    const int depth = CV_MAT_DEPTH(type);
    const int channels = CV_MAT_CN(type);

    return depth >= CV_8U && depth < CV_DEPTH_MAX && channels > 0 && channels <= CV_CN_MAX &&
           type == CV_MAKETYPE(depth, channels);
}
}

namespace
{

/**
 * @brief 解析带有文本 Mat 描述头的传统单部分消息
 *
 * @param bytes 格式为 rows、cols、type、换行符及连续像素数据的消息
 *
 * @return 尺寸和负载完全匹配时返回独立图像，否则返回 std::nullopt
 */
std::optional<cv::Mat> decodeRawMatMessage(const ByteSpan bytes)
{
    const auto newline = std::find(bytes.begin(), bytes.end(), static_cast<unsigned char>('\n'));
    if (newline == bytes.end() ||
        static_cast<std::size_t>(std::distance(bytes.begin(), newline)) > kMaxLegacyHeaderBytes)
    {
        return std::nullopt;
    }

    const std::string header(bytes.begin(), newline);
    std::istringstream headerStream(header);
    int rows = 0;
    int cols = 0;
    int type = 0;
    headerStream >> rows >> cols >> type;
    headerStream >> std::ws;
    if (!headerStream || !headerStream.eof() || rows <= 0 || cols <= 0 || !isValidCvType(type))
    {
        return std::nullopt;
    }

    const auto payloadBegin = newline + 1;
    const std::size_t payloadSize =
        static_cast<std::size_t>(std::distance(payloadBegin, bytes.end()));
    const auto elementSize = static_cast<std::size_t>(CV_ELEM_SIZE(type));
    const auto pixelCount =
        checkedMultiply(static_cast<std::size_t>(rows), static_cast<std::size_t>(cols));
    const auto expectedSize = pixelCount ? checkedMultiply(*pixelCount, elementSize) : std::nullopt;
    if (!expectedSize || payloadSize != *expectedSize)
    {
        return std::nullopt;
    }

    cv::Mat frame(rows, cols, type);
    std::copy(payloadBegin, bytes.end(), frame.ptr<unsigned char>());
    return frame;
}

/**
 * @brief 使用 OpenCV 自动识别并解码压缩图像负载
 *
 * @param bytes JPEG、PNG 或其他 imdecode 支持的字节数据
 *
 * @return 解码后的图像，负载非法或解码失败时返回 std::nullopt
 */
std::optional<cv::Mat> decodeEncodedImage(const ByteSpan bytes)
{
    if (bytes.empty() || bytes.size() > kMaxPayloadBytes ||
        bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return std::nullopt;
    }

    cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1,
                    const_cast<unsigned char *>(bytes.data()));
    cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
    if (decoded.empty())
    {
        return std::nullopt;
    }

    return decoded;
}

/**
 * @brief 将协议中的单通道字节宽度映射为 OpenCV 像素深度
 */
int cvDepthFromBytesPerChannel(const uint32_t bytesPerChannel)
{
    switch (bytesPerChannel)
    {
    case 1:
        return CV_8U;
    case 2:
        return CV_16U;
    case 4:
        return CV_32F;
    default:
        return -1;
    }
}

/**
 * @brief 根据协议图像描述构造 OpenCV 深度与通道类型
 *
 * @param image protobuf 图像布局描述
 *
 * @return 可用的 OpenCV Mat 类型，字段不受支持时返回 std::nullopt
 */
std::optional<int> cvTypeFromImageSpec(const frameviz::ImageSpec &image)
{
    const int depth = cvDepthFromBytesPerChannel(image.bytes_per_channel());
    if (depth < 0 || image.channels() == 0 || image.channels() > CV_CN_MAX)
    {
        return std::nullopt;
    }

    return CV_MAKETYPE(depth, static_cast<int>(image.channels()));
}

/**
 * @brief 按 FrameMeta 图像布局解码未压缩像素负载
 *
 * @details 验证尺寸、通道、步长和负载边界。RGB 与 RGBA 输入会转换为 OpenCV
 * 默认的 BGR 或 BGRA 顺序，返回图像不引用消息缓冲区
 *
 * @param meta 描述像素布局和格式的帧元数据
 * @param payload 原始像素负载
 *
 * @return 完成校验和颜色转换的图像，协议字段不受支持时返回 std::nullopt
 */
std::optional<cv::Mat> decodeRawPayload(const frameviz::FrameMeta &meta,
                                        const ByteSpan payload)
{
    if (payload.size() > kMaxPayloadBytes)
    {
        return std::nullopt;
    }

    const auto &image = meta.image();
    if (image.width() == 0 || image.height() == 0 ||
        image.width() > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        image.height() > static_cast<uint32_t>(std::numeric_limits<int>::max()))
    {
        return std::nullopt;
    }

    const auto type = cvTypeFromImageSpec(image);
    if (!type)
    {
        return std::nullopt;
    }

    const std::size_t width = image.width();
    const std::size_t height = image.height();
    const std::size_t channels = image.channels();
    const std::size_t bytesPerChannel = image.bytes_per_channel();
    const auto rowElements = checkedMultiply(width, channels);
    const auto minimumStride =
        rowElements ? checkedMultiply(*rowElements, bytesPerChannel) : std::nullopt;
    if (!minimumStride)
    {
        return std::nullopt;
    }

    // 像素格式同时约束通道布局，避免按不一致的元数据解释负载
    int colorConversion = -1;
    switch (image.pixel_format())
    {
    case frameviz::PIXEL_FORMAT_RGB8:
        if (channels != 3 || bytesPerChannel != 1)
        {
            return std::nullopt;
        }
        colorConversion = cv::COLOR_RGB2BGR;
        break;
    case frameviz::PIXEL_FORMAT_RGBA8:
        if (channels != 4 || bytesPerChannel != 1)
        {
            return std::nullopt;
        }
        colorConversion = cv::COLOR_RGBA2BGRA;
        break;
    case frameviz::PIXEL_FORMAT_BGR8:
        if (channels != 3 || bytesPerChannel != 1)
        {
            return std::nullopt;
        }
        break;
    case frameviz::PIXEL_FORMAT_BGRA8:
        if (channels != 4 || bytesPerChannel != 1)
        {
            return std::nullopt;
        }
        break;
    case frameviz::PIXEL_FORMAT_GRAY8:
        if (channels != 1 || bytesPerChannel != 1)
        {
            return std::nullopt;
        }
        break;
    case frameviz::PIXEL_FORMAT_UNSPECIFIED:
        break;
    default:
        return std::nullopt;
    }

    // 以完整步长计算所需字节，拒绝截断行和尺寸乘法溢出
    const std::size_t stride = image.stride_bytes() == 0 ? *minimumStride : image.stride_bytes();
    const auto requiredBytes = checkedMultiply(stride, height);
    if (stride < *minimumStride || !requiredBytes || payload.size() < *requiredBytes)
    {
        return std::nullopt;
    }

    // 临时 Mat 仅借用消息内存，返回前必须通过转换或 clone 获得独立存储
    cv::Mat view(static_cast<int>(image.height()), static_cast<int>(image.width()), *type,
                 const_cast<unsigned char *>(payload.data()), stride);
    cv::Mat frame;
    if (colorConversion >= 0)
    {
        cv::cvtColor(view, frame, colorConversion);
    }
    else
    {
        frame = view.clone();
    }

    return frame;
}

/** @brief 根据 FrameMeta 负载编码选择压缩或原始图像解码路径 */
std::optional<cv::Mat> decodeProtoPayload(const frameviz::FrameMeta &meta,
                                          const ByteSpan payload)
{
    switch (meta.payload().encoding())
    {
    case frameviz::PAYLOAD_ENCODING_JPEG:
        return decodeEncodedImage(payload);
    case frameviz::PAYLOAD_ENCODING_RAW:
        return decodeRawPayload(meta, payload);
    default:
        return std::nullopt;
    }
}

/** @brief 生成人类可读的编码、尺寸和像素格式摘要 */
std::string payloadDescription(const frameviz::FrameMeta &meta)
{
    std::ostringstream stream;
    stream << frameviz::PayloadEncoding_Name(meta.payload().encoding()) << " "
           << meta.image().width() << "x" << meta.image().height() << " "
           << frameviz::PixelFormat_Name(meta.image().pixel_format());
    return stream.str();
}

/** @brief 将协议像素格式转换为界面使用的短名称 */
std::string pixelFormatName(const frameviz::PixelFormat format)
{
    switch (format)
    {
    case frameviz::PIXEL_FORMAT_BGR8:
        return "BGR8";
    case frameviz::PIXEL_FORMAT_RGB8:
        return "RGB8";
    case frameviz::PIXEL_FORMAT_GRAY8:
        return "GRAY8";
    case frameviz::PIXEL_FORMAT_NV12:
        return "NV12";
    case frameviz::PIXEL_FORMAT_YUV420P:
        return "YUV420P";
    case frameviz::PIXEL_FORMAT_BGRA8:
        return "BGRA8";
    case frameviz::PIXEL_FORMAT_RGBA8:
        return "RGBA8";
    case frameviz::PIXEL_FORMAT_UNSPECIFIED:
        return "Unspecified";
    default:
        return "Unknown";
    }
}

/**
 * @brief 将发布端元数据转换为界面统计状态
 *
 * @details 发布会话变化时重置序号统计，并且仅在时钟域明确且可比较时计算
 * 端到端延迟
 *
 * @param meta 最近采用帧的协议元数据
 * @param state 接收源信息、序号和延迟统计的应用状态
 */
void updateFrameMetadata(const frameviz::FrameMeta &meta, AppState &state)
{
    if (state.sourceSessionId != meta.session_id())
    {
        state.sourceSessionId = meta.session_id();
        state.lastSourceFrameIndex = 0;
        state.skippedFrames = 0;
    }

    state.sourceFrameIndex = meta.frame_index();
    if (state.lastSourceFrameIndex != 0 && meta.frame_index() > state.lastSourceFrameIndex + 1)
    {
        state.skippedFrames += meta.frame_index() - state.lastSourceFrameIndex - 1;
    }
    state.lastSourceFrameIndex = meta.frame_index();
    state.publisherDroppedFrames = meta.stats().dropped_frames();
    state.sourceFps = static_cast<float>(meta.stats().fps());
    state.pixelFormat = pixelFormatName(meta.image().pixel_format());
    state.pipelineLatencyMs = static_cast<float>(meta.timing().pipeline_latency_us()) / 1000.0F;
    state.encodeLatencyMs = static_cast<float>(meta.timing().encode_latency_us()) / 1000.0F;
    state.endToEndLatencyValid = false;

    const uint64_t captureNs = meta.timing().capture_timestamp_ns();
    uint64_t nowNs = 0;
    if (meta.timing().capture_timestamp_domain() == frameviz::TIMESTAMP_DOMAIN_MONOTONIC)
    {
        nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
    }
    else if (meta.timing().capture_timestamp_domain() ==
             frameviz::TIMESTAMP_DOMAIN_UNIX_EPOCH)
    {
        nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    }
    if (captureNs > 0 && nowNs >= captureNs)
    {
        state.endToEndLatencyMs = static_cast<float>(nowNs - captureNs) / 1'000'000.0F;
        state.endToEndLatencyValid = true;
    }
}

/** @brief 依次按传统 Mat 消息和压缩图像解释单部分负载 */
std::optional<cv::Mat> decodeLegacyImageMessage(const ZmqMessage &message)
{
    const ByteSpan bytes = messageBytes(message);
    if (auto rawMat = decodeRawMatMessage(bytes))
    {
        return rawMat;
    }
    return decodeEncodedImage(bytes);
}

/** @brief 后台线程向主线程交付的完整解码结果 */
struct DecodedPacket
{
    // === 帧内容与元数据 ===
    cv::Mat frame; ///< 不引用 ZMQ 消息内存的解码图像
    std::string topic; ///< 多部分协议携带的订阅主题
    std::string sourceId; ///< protobuf 发布源标识
    std::string payloadInfo; ///< 用于状态面板的负载摘要
    std::optional<frameviz::FrameMeta> meta; ///< 传统消息不存在的可选协议元数据

    // === 解码调度 ===
    float decodeTimeMs = 0.0F; ///< 后台线程本次解码耗时
    uint64_t generation = 0; ///< 提交任务时的端点代数，用于拒绝旧连接结果
};

/**
 * @brief 将接收消息解析为 protobuf 帧或兼容的传统图像
 *
 * @param messages 一组保持 ZMQ 分帧顺序的消息
 *
 * @return 校验和解码成功的结果，否则返回 std::nullopt
 */
std::optional<DecodedPacket> decodeReceivedMessages(const std::vector<ZmqMessage> &messages)
{
    if (messages.empty())
    {
        return std::nullopt;
    }

    // 三段及以上消息优先按 topic、FrameMeta、payload 协议解析
    if (messages.size() >= 3)
    {
        frameviz::FrameMeta meta;
        const auto &metaPart = messages[1];
        if (metaPart.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
            meta.ParseFromArray(metaPart.data(), static_cast<int>(metaPart.size())))
        {
            const uint64_t declaredBytes = meta.payload().payload_bytes();
            if ((declaredBytes != 0 && declaredBytes != messages[2].size()) ||
                messages[2].size() > kMaxPayloadBytes)
            {
                return std::nullopt;
            }
            auto decoded = decodeProtoPayload(meta, messageBytes(messages[2]));
            if (decoded)
            {
                DecodedPacket packet;
                packet.frame = std::move(*decoded);
                packet.topic.assign(reinterpret_cast<const char *>(messages[0].data()),
                                    messages[0].size());
                packet.sourceId = meta.source_id();
                packet.payloadInfo = payloadDescription(meta);
                packet.meta = std::move(meta);
                return packet;
            }
            return std::nullopt;
        }
    }

    // 元数据无法识别时仅使用最后一段尝试传统兼容格式
    auto decoded = decodeLegacyImageMessage(messages.back());
    if (!decoded)
    {
        return std::nullopt;
    }
    DecodedPacket packet;
    packet.frame = std::move(*decoded);
    packet.payloadInfo = "legacy single-part";
    return packet;
}

/**
 * @brief 只保留最新待处理任务和最新完成结果的后台解码器
 *
 * @details submit 会覆盖尚未开始的任务，工作线程完成解码后也会覆盖未消费结果，
 * 以固定内存换取实时显示优先级
 */
class LatestFrameDecoder
{
  public:
    /** @brief 启动消息解码工作线程 */
    LatestFrameDecoder() : worker_([this] { run(); }) {}

    /** @brief 请求工作线程退出并等待其完成 */
    ~LatestFrameDecoder()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        worker_.join();
    }

    /**
     * @brief 提交最新消息并覆盖尚未处理的旧任务
     *
     * @param messages 待解码的完整 ZMQ 消息组
     * @param generation 当前端点代数
     */
    void submit(std::vector<ZmqMessage> messages, const uint64_t generation)
    {
        {
            std::lock_guard lock(mutex_);
            pending_ = Work{std::move(messages), generation};
        }
        condition_.notify_one();
    }

    /**
     * @brief 非阻塞取出最近完成的解码结果
     *
     * @return 有完成结果时转移其所有权，否则返回 std::nullopt
     */
    std::optional<DecodedPacket> poll()
    {
        std::lock_guard lock(mutex_);
        if (!completed_)
        {
            return std::nullopt;
        }
        auto packet = std::move(completed_);
        completed_.reset();
        return packet;
    }

  private:
    struct Work
    {
        std::vector<ZmqMessage> messages; ///< 待解析的一组完整 ZMQ 消息
        uint64_t generation = 0; ///< 消息接收时对应的端点代数
    };

    /** @brief 等待最新任务、执行解码并发布完成结果 */
    void run()
    {
        for (;;)
        {
            Work work;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (stopping_)
                {
                    return;
                }
                work = std::move(*pending_);
                pending_.reset();
            }

            const auto started = std::chrono::steady_clock::now();
            std::optional<DecodedPacket> packet;
            try
            {
                packet = decodeReceivedMessages(work.messages);
            }
            catch (const std::exception &error)
            {
                const auto now = std::chrono::steady_clock::now();
                // 将连续解码异常压缩为每秒一条日志，避免错误流淹没终端
                if (lastErrorLog_.time_since_epoch().count() == 0 ||
                    now - lastErrorLog_ >= std::chrono::seconds(1))
                {
                    std::cerr << "Frame decode failed: " << error.what();
                    if (suppressedErrors_ != 0)
                    {
                        std::cerr << " (" << suppressedErrors_ << " similar errors suppressed)";
                    }
                    std::cerr << '\n';
                    lastErrorLog_ = now;
                    suppressedErrors_ = 0;
                }
                else
                {
                    ++suppressedErrors_;
                }
                continue;
            }
            if (packet)
            {
                packet->decodeTimeMs = std::chrono::duration<float, std::milli>(
                                           std::chrono::steady_clock::now() - started)
                                           .count();
                packet->generation = work.generation;
                std::lock_guard lock(mutex_);
                completed_ = std::move(*packet);
            }
        }
    }

    // === 解码任务调度 ===
    std::mutex mutex_; ///< 保护任务槽、结果槽和停止状态
    std::condition_variable condition_; ///< 唤醒等待新任务或停止请求的工作线程
    std::optional<Work> pending_; ///< 尚未开始且可被新提交覆盖的任务槽
    std::optional<DecodedPacket> completed_; ///< 尚未消费且可被新结果覆盖的结果槽

    // === 工作线程生命周期 ===
    bool stopping_ = false; ///< 是否请求工作线程退出
    std::thread worker_; ///< 串行执行图像解码的后台线程

    // === 错误日志节流 ===
    std::chrono::steady_clock::time_point lastErrorLog_{}; ///< 最近一次输出解码异常的时刻
    std::size_t suppressedErrors_ = 0; ///< 当前日志节流窗口内省略的异常数量
};

}

namespace
{

/** @brief 读取首个命令行参数作为订阅端点，缺省时使用本地默认值 */
std::string parseEndpoint(const int argc, char **argv)
{
    if (argc >= 2)
    {
        return argv[1];
    }

    return "tcp://127.0.0.1:5555";
}

/**
 * @brief 受帧数和总字节预算约束的最近帧历史
 *
 * @details 缓存共享 cv::Mat 存储，至少保留最新一帧，即使单帧本身超过字节预算
 */
class FrameHistory
{
  public:
    /** @brief 丢弃全部历史帧并重置字节统计 */
    void clear()
    {
        frames_.clear();
        bytes_ = 0;
    }

    /**
     * @brief 追加一帧并从队首淘汰超出预算的旧帧
     *
     * @param frame 与当前帧共享存储的历史图像
     */
    void push(const cv::Mat &frame)
    {
        frames_.push_back(frame);
        const std::size_t addedBytes = frameBytes(frame);
        bytes_ = addedBytes > std::numeric_limits<std::size_t>::max() - bytes_
                     ? std::numeric_limits<std::size_t>::max()
                     : bytes_ + addedBytes;
        while (frames_.size() > 1 &&
               (frames_.size() > kFrameHistoryCapacity || bytes_ > kFrameHistoryMaxBytes))
        {
            const std::size_t removedBytes = frameBytes(frames_.front());
            frames_.pop_front();
            if (bytes_ == std::numeric_limits<std::size_t>::max() || removedBytes > bytes_)
            {
                recalculateBytes();
            }
            else
            {
                bytes_ -= removedBytes;
            }
        }
    }

    /**
     * @brief 查询历史缓存是否没有可浏览帧
     *
     * @return 缓存为空时返回 true
     */
    [[nodiscard]] bool empty() const { return frames_.empty(); }

    /**
     * @brief 获取当前缓存帧数
     *
     * @return 可按历史偏移访问的帧数量
     */
    [[nodiscard]] int size() const { return static_cast<int>(frames_.size()); }

    /**
     * @brief 按距离最新帧的偏移获取历史图像
     *
     * @param offset 零表示最新帧，数值增加时向更早帧移动
     *
     * @return 边界裁剪后的偏移对应图像
     */
    [[nodiscard]] const cv::Mat &atOffset(const int offset) const
    {
        const int clampedOffset = std::clamp(offset, 0, size() - 1);
        return frames_[frames_.size() - 1U - static_cast<std::size_t>(clampedOffset)];
    }

  private:
    static std::size_t frameBytes(const cv::Mat &frame)
    {
        const auto bytes = checkedMultiply(frame.total(), frame.elemSize());
        return bytes.value_or(std::numeric_limits<std::size_t>::max());
    }

    /** @brief 在累计字节数饱和或失去精度后重新遍历计算 */
    void recalculateBytes()
    {
        bytes_ = 0;
        for (const cv::Mat &frame : frames_)
        {
            const std::size_t size = frameBytes(frame);
            if (size > std::numeric_limits<std::size_t>::max() - bytes_)
            {
                bytes_ = std::numeric_limits<std::size_t>::max();
                return;
            }
            bytes_ += size;
        }
    }

    std::deque<cv::Mat> frames_; ///< 按接收顺序保存且队尾最新的图像集合
    std::size_t bytes_ = 0; ///< 所有缓存图像逻辑像素数据的估算总量
};

/**
 * @brief 以固定最短统计窗口估算接收和展示帧率
 *
 * @details 计数窗口达到一秒后使用实际经过时间计算速率并开始新窗口
 */
class FrameRateTracker
{
  public:
    /**
     * @brief 以指定时刻开始新的空统计窗口
     *
     * @param now 新统计窗口的起始时刻
     */
    void reset(const std::chrono::steady_clock::time_point now)
    {
        windowStart_ = now;
        received_ = 0;
        displayed_ = 0;
    }

    /**
     * @brief 记录一帧被主线程成功采用
     *
     * @param now 当前帧被采用的时刻
     */
    void recordReceived(const std::chrono::steady_clock::time_point now)
    {
        if (received_ == 0 && displayed_ == 0)
        {
            windowStart_ = now;
        }
        ++received_;
    }

    /** @brief 记录一帧新内容被 UI 实际展示 */
    void recordDisplayed() { ++displayed_; }

    /**
     * @brief 窗口达到一秒时更新应用帧率并清零计数
     *
     * @param now 当前单调时钟时刻
     * @param state 接收计算结果的应用状态
     */
    void update(const std::chrono::steady_clock::time_point now, AppState &state)
    {
        const float seconds = std::chrono::duration<float>(now - windowStart_).count();
        if (seconds < 1.0F)
        {
            return;
        }
        state.receiveFps = static_cast<float>(received_) / seconds;
        state.renderFps = static_cast<float>(displayed_) / seconds;
        reset(now);
    }

  private:
    std::chrono::steady_clock::time_point windowStart_ = std::chrono::steady_clock::now(); ///< 当前统计窗口起点
    int received_ = 0; ///< 当前窗口成功采用的解码帧数
    int displayed_ = 0; ///< 当前窗口实际上传并展示的新帧数
};

/**
 * @brief 管理本地视频文件的顺序解码、播放节拍和随机定位
 *
 * @details 文件帧按媒体帧率和用户倍速调度，不采用 ZMQ 实时流的积压丢帧策略
 */
class VideoFilePlayer
{
  public:
    /**
     * @brief 打开视频文件并读取其帧率和总帧数
     *
     * @param path 本地视频文件路径
     *
     * @throws std::runtime_error OpenCV 无法打开指定文件
     */
    void open(const std::string &path)
    {
        cv::VideoCapture next(path);
        if (!next.isOpened())
        {
            throw std::runtime_error("cannot open video file: " + path);
        }

        double fps = next.get(cv::CAP_PROP_FPS);
        if (!std::isfinite(fps) || fps <= 0.0)
        {
            fps = 30.0;
        }
        capture_ = std::move(next);
        frameInterval_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / fps));
        nextFrameTime_ = std::chrono::steady_clock::now();
        fps_ = fps;
        const double frameCount = capture_.get(cv::CAP_PROP_FRAME_COUNT);
        frameCount_ = std::isfinite(frameCount) && frameCount > 0.0 ?
            static_cast<int64_t>(frameCount) : 0;
        frameIndex_ = 0;
        ended_ = false;
    }

    /** @brief 释放当前视频并清除播放结束和帧位置状态 */
    void close()
    {
        capture_.release();
        ended_ = false;
        frameIndex_ = 0;
    }

    /**
     * @brief 返回当前是否持有已打开的视频
     *
     * @return 视频解码资源已打开时返回 true
     */
    [[nodiscard]] bool isOpen() const { return capture_.isOpened(); }

    /**
     * @brief 返回顺序读取是否已经到达视频末尾
     *
     * @return 最近一次读取到达末尾时返回 true
     */
    [[nodiscard]] bool ended() const { return ended_; }

    /**
     * @brief 返回视频声明的帧率
     *
     * @return 媒体帧率，元数据无效时为 30 FPS
     */
    [[nodiscard]] double fps() const { return fps_; }

    /**
     * @brief 返回下一次顺序读取使用的零基帧索引
     *
     * @return 下一次顺序读取的帧索引
     */
    [[nodiscard]] uint64_t frameIndex() const { return frameIndex_; }

    /**
     * @brief 返回视频声明的总帧数
     *
     * @return 总帧数，无法获取时为 0
     */
    [[nodiscard]] int64_t frameCount() const { return frameCount_; }

    /**
     * @brief 更新播放倍速并从当前时刻重新安排下一帧
     *
     * @param speed 相对媒体帧率的倍速，内部限制在 0.25 到 4.0
     */
    void setPlaybackSpeed(const float speed)
    {
        playbackSpeed_ = std::clamp(speed, 0.25F, 4.0F);
        updateFrameInterval();
        nextFrameTime_ = std::chrono::steady_clock::now() + frameInterval_;
    }

    /**
     * @brief 定位到指定帧并立即解码该帧
     *
     * @param targetFrame 零基目标帧索引
     * @param frame 接收解码结果
     *
     * @return 定位和解码均成功时返回 true
     */
    bool seekTo(const int64_t targetFrame, cv::Mat &frame)
    {
        if (!isOpen())
        {
            return false;
        }
        const int64_t upper = frameCount_ > 0 ? frameCount_ - 1 : targetFrame;
        const int64_t clampedTarget = std::clamp(targetFrame, int64_t{0}, upper);
        if (!capture_.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(clampedTarget)))
        {
            return false;
        }
        frameIndex_ = static_cast<uint64_t>(clampedTarget);
        ended_ = false;
        nextFrameTime_ = std::chrono::steady_clock::now();
        return readDue(nextFrameTime_, frame);
    }

    /**
     * @brief 在播放期限到达时顺序解码下一帧
     *
     * @param now 调度判断使用的单调时钟时间
     * @param frame 接收解码结果
     *
     * @return 本次实际产生新帧时返回 true
     */
    bool readDue(const std::chrono::steady_clock::time_point now, cv::Mat &frame)
    {
        if (!isOpen() || ended_ || now < nextFrameTime_)
        {
            return false;
        }
        const auto started = std::chrono::steady_clock::now();
        if (!capture_.read(frame) || frame.empty())
        {
            ended_ = true;
            return false;
        }
        decodeTimeMs_ = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        ++frameIndex_;
        nextFrameTime_ += frameInterval_;
        if (nextFrameTime_ <= now)
        {
            // 暂停恢复或解码过慢时从当前时刻重新计时，避免突发追赶旧时间线。
            nextFrameTime_ = now + frameInterval_;
        }
        return true;
    }

    /**
     * @brief 返回最近一次成功视频解码的耗时
     *
     * @return 解码耗时，单位为毫秒
     */
    [[nodiscard]] float decodeTimeMs() const { return decodeTimeMs_; }

  private:
    /** @brief 根据媒体帧率和当前倍速计算相邻展示帧间隔 */
    void updateFrameInterval()
    {
        frameInterval_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / (fps_ * playbackSpeed_)));
    }

    cv::VideoCapture capture_; ///< 当前文件的 OpenCV 视频解码资源
    std::chrono::steady_clock::duration frameInterval_{}; ///< 当前倍速下的展示帧间隔
    std::chrono::steady_clock::time_point nextFrameTime_{}; ///< 下一帧允许解码的单调时钟期限
    double fps_ = 0.0; ///< 视频媒体帧率或无有效元数据时的回退帧率
    int64_t frameCount_ = 0; ///< 视频声明的总帧数，未知时为 0
    uint64_t frameIndex_ = 0; ///< 下一次顺序读取对应的零基帧索引
    float decodeTimeMs_ = 0.0F; ///< 最近一次成功读取和解码的耗时
    float playbackSpeed_ = 1.0F; ///< 相对媒体帧率的当前播放倍速
    bool ended_ = false; ///< 最近一次顺序读取是否已经到达文件末尾
};

/** @brief 将有效 ROI 规范化到当前帧边界，空交集会使其失效 */
void clampRoiToFrame(AppState &state)
{
    if (!state.roiValid)
    {
        return;
    }
    const cv::Rect roi = selectedRoi(state.lastFrame, state);
    state.roiValid = !roi.empty();
    if (state.roiValid)
    {
        state.roiX = roi.x;
        state.roiY = roi.y;
        state.roiWidth = roi.width;
        state.roiHeight = roi.height;
    }
}

/**
 * @brief 采用后台解码结果并同步流状态、历史缓存与 ROI 统计
 *
 * @param packet 当前端点生成的有效解码结果
 * @param state 接收帧数据和统计字段的应用状态
 * @param history 接收新帧的历史缓存
 * @param frameRates 记录本次成功接收的帧率统计器
 */
void acceptDecodedPacket(DecodedPacket packet, AppState &state, FrameHistory &history,
                         FrameRateTracker &frameRates)
{
    const auto receiveTime = std::chrono::steady_clock::now();
    state.decodeTimeMs = packet.decodeTimeMs;
    state.topic = std::move(packet.topic);
    state.sourceId = std::move(packet.sourceId);
    state.payloadInfo = std::move(packet.payloadInfo);
    if (packet.meta)
    {
        updateFrameMetadata(*packet.meta, state);
    }
    else
    {
        state.endToEndLatencyValid = false;
        state.pixelFormat.clear();
    }

    state.lastFrameTime = receiveTime;
    state.lastFrame = std::move(packet.frame);
    ++state.frameCount;
    ++state.imageRevision;
    frameRates.recordReceived(receiveTime);
    history.push(state.lastFrame);
    state.historySize = history.size();
    state.historyOffset = 0;
    clampRoiToFrame(state);
    updateRoiStatistics(state);
}

/**
 * @brief 在暂停模式下消费历史导航请求并切换当前帧
 *
 * @param state 包含导航请求并接收所选历史帧的应用状态
 * @param history 只读历史帧缓存
 */
void processHistoryNavigation(AppState &state, const FrameHistory &history)
{
    if (!state.paused || history.empty())
    {
        state.previousFrameRequested = false;
        state.nextFrameRequested = false;
        state.liveFrameRequested = false;
        state.historySeekRequested = false;
        return;
    }

    const int maxOffset = history.size() - 1;
    int nextOffset = std::clamp(state.historyOffset, 0, maxOffset);
    if (state.previousFrameRequested)
    {
        nextOffset = std::min(maxOffset, nextOffset + 1);
    }
    if (state.nextFrameRequested)
    {
        nextOffset = std::max(0, nextOffset - 1);
    }
    if (state.liveFrameRequested)
    {
        nextOffset = 0;
    }

    const bool selectionRequested = nextOffset != state.historyOffset ||
                                    state.previousFrameRequested || state.nextFrameRequested ||
                                    state.liveFrameRequested || state.historySeekRequested;
    if (selectionRequested)
    {
        state.historyOffset = nextOffset;
        state.lastFrame = history.atOffset(nextOffset);
        ++state.imageRevision;
        clampRoiToFrame(state);
        updateRoiStatistics(state);
    }

    state.previousFrameRequested = false;
    state.nextFrameRequested = false;
    state.liveFrameRequested = false;
    state.historySeekRequested = false;
}

/** @brief 根据暂停状态、首帧状态和帧龄生成连接状态文本 */
void updateConnectionStatus(AppState &state)
{
    if (state.paused)
    {
        state.connectionStatus = "paused";
        return;
    }
    if (!state.videoFilePath.empty())
    {
        if (state.connectionStatus != "ended")
        {
            state.connectionStatus = "playing";
        }
        return;
    }
    if (state.lastFrameTime.time_since_epoch().count() == 0)
    {
        if (state.connectionStatus.rfind("failed:", 0) != 0)
        {
            state.connectionStatus = "waiting frames";
        }
        return;
    }
    state.connectionStatus = state.frameAgeMs > 2000.0F ? "stale" : "receiving";
}

/** @brief 清除不能跨数据源保留的帧、统计和分析状态 */
void resetStreamState(AppState &state)
{
    state.connectionStatus = "waiting frames";
    state.topic.clear();
    state.sourceId.clear();
    state.payloadInfo.clear();
    state.pixelFormat.clear();
    state.frameCount = 0;
    state.sourceFps = 0.0F;
    state.receiveFps = 0.0F;
    state.renderFps = 0.0F;
    state.frameAgeMs = 0.0F;
    state.decodeTimeMs = 0.0F;
    state.endToEndLatencyMs = 0.0F;
    state.endToEndLatencyValid = false;
    state.pipelineLatencyMs = 0.0F;
    state.encodeLatencyMs = 0.0F;
    state.sourceFrameIndex = 0;
    state.lastSourceFrameIndex = 0;
    state.skippedFrames = 0;
    state.publisherDroppedFrames = 0;
    state.sourceSessionId.clear();
    state.lastFrameTime = {};
    state.historySize = 0;
    state.historyOffset = 0;
    state.roiValid = false;
    state.filteredFrameCount = 0;
    if (state.autoLockSource)
    {
        state.lockedSourceId.clear();
    }
    state.detectedSources.clear();
    state.lastFrame.release();
}

/**
 * @brief 尝试以新套接字替换当前订阅连接
 *
 * @details 只有新套接字成功创建和连接后才替换旧套接字；失败时恢复输入框并保留
 * 当前端点
 *
 * @param receiver 当前活动接收器，成功时切换到新端点
 * @param state 提供切换请求并接收连接结果
 *
 * @return 端点实际发生切换时返回 true
 */
bool applyEndpointChange(frameviz::FrameReceiver &receiver, AppState &state)
{
    if (!state.endpointChangeRequested)
    {
        return false;
    }
    state.endpointChangeRequested = false;

    std::string nextEndpoint = std::move(state.requestedEndpoint);
    state.requestedEndpoint.clear();
    if (nextEndpoint.empty() || nextEndpoint == state.endpoint)
    {
        state.endpointInputResetRequested = true;
        return false;
    }

    try
    {
        receiver.changeEndpoint(nextEndpoint);
        state.endpoint = receiver.endpoint();
        resetStreamState(state);
        std::cout << "Switched endpoint to " << state.endpoint << '\n';
        return true;
    }
    catch (const std::exception &error)
    {
        state.connectionStatus = std::string("failed: ") + error.what();
        state.endpointInputResetRequested = true;
        return false;
    }
}

}

/**
 * @brief 运行 FrameVisualizer 应用生命周期
 *
 * @details 初始化帧接收器、后台解码器和桌面 UI，在主循环中处理端点切换、最新帧
 * 接收、暂停历史浏览、统计刷新、ROI 分析与渲染。循环在 UI 请求退出后结束
 *
 * @param argc 命令行参数数量
 * @param argv 命令行参数；argv[1] 可指定初始 ZMQ 订阅端点
 *
 * @return UI 正常结束时返回 0
 *
 * @throws std::exception 接收器或 UI 初始化以及运行期资源操作失败
 */
int runApplication(const int argc, char **argv)
{
    AppState state;
    state.endpoint = parseEndpoint(argc, argv);
    state.connectionStatus = "waiting frames";

    frameviz::FrameReceiverOptions receiverOptions;
    receiverOptions.endpoint = state.endpoint;
    frameviz::FrameReceiver receiver(std::move(receiverOptions));

    UiContext ui;
    ui.init("Frame Visualizer", state);

    LatestFrameDecoder frameDecoder;
    VideoFilePlayer videoPlayer;

    std::cout << "Listening on " << state.endpoint << '\n'
              << "Message formats: topic + FrameMeta protobuf + payload, or "
                 "legacy single-part image bytes."
              << '\n';

    bool previousPausedState = false;
    FrameHistory frameHistory;
    uint64_t endpointGeneration = 0;
    FrameRateTracker frameRates;
    frameRates.reset(std::chrono::steady_clock::now());
    float appliedPlaybackSpeed = 1.0F;

    const auto closeVideo = [&]
    {
        videoPlayer.close();
        state.videoFilePath.clear();
        state.videoFrameCount = 0;
        state.videoFramePosition = 0;
        state.videoSeekRequested = false;
        state.videoCloseRequested = false;
        resetStreamState(state);
        frameHistory.clear();
        frameRates.reset(std::chrono::steady_clock::now());
        ++endpointGeneration;
        ui.onEndpointChanged(state);
    };

    while (!state.quit)
    {
        ui.processEvents(state);
        if (state.videoFileOpenRequested)
        {
            state.videoFileOpenRequested = false;
            const std::string requestedPath = std::move(state.requestedVideoFile);
            state.requestedVideoFile.clear();
            try
            {
                videoPlayer.open(requestedPath);
                state.videoFilePath = requestedPath;
                resetStreamState(state);
                state.videoFilePath = requestedPath;
                state.sourceId = requestedPath;
                state.payloadInfo = "local video";
                state.sourceFps = static_cast<float>(videoPlayer.fps());
                state.videoFrameCount = videoPlayer.frameCount();
                state.videoFramePosition = 0;
                state.videoPlaybackSpeed = 1.0F;
                state.videoCloseRequested = false;
                appliedPlaybackSpeed = 1.0F;
                state.connectionStatus = "playing";
                frameHistory.clear();
                frameRates.reset(std::chrono::steady_clock::now());
                ++endpointGeneration;
                ui.onEndpointChanged(state);
                std::cout << "Playing local video " << requestedPath << '\n';
            }
            catch (const std::exception &error)
            {
                state.connectionStatus = std::string("failed: ") + error.what();
            }
        }

        if (state.videoCloseRequested)
        {
            if (videoPlayer.isOpen())
            {
                closeVideo();
            }
            else
            {
                state.videoCloseRequested = false;
            }
        }

        // 应用端点即明确切回网络输入，即使端点文本没有变化。
        if (state.endpointChangeRequested && videoPlayer.isOpen())
        {
            closeVideo();
        }
        if (applyEndpointChange(receiver, state))
        {
            ++endpointGeneration;
            frameHistory.clear();
            frameRates.reset(std::chrono::steady_clock::now());
            ui.onEndpointChanged(state);
        }

        // 主线程只提交接收队列中的最新消息，降低实时预览累积延迟
        std::vector<ZmqMessage> messages;
        if (!videoPlayer.isOpen())
        {
            messages = receiver.receiveLatest();
        }
        if (!videoPlayer.isOpen() && !state.paused && !messages.empty())
        {
            frameDecoder.submit(std::move(messages), endpointGeneration);
        }
        if (auto decoded = frameDecoder.poll();
            decoded && !state.paused && decoded->generation == endpointGeneration)
        {
            if (state.sourceLockEnabled)
            {
                if (!decoded->sourceId.empty())
                {
                    if (std::find(state.detectedSources.begin(), state.detectedSources.end(),
                                  decoded->sourceId) == state.detectedSources.end())
                    {
                        state.detectedSources.push_back(decoded->sourceId);
                    }
                }
                if (state.lockedSourceId.empty() && state.autoLockSource && !decoded->sourceId.empty())
                {
                    state.lockedSourceId = decoded->sourceId;
                    std::cout << "Auto-locked to source: " << state.lockedSourceId << '\n';
                }
                if (!state.lockedSourceId.empty() && decoded->sourceId != state.lockedSourceId)
                {
                    ++state.filteredFrameCount;
                }
                else
                {
                    acceptDecodedPacket(std::move(*decoded), state, frameHistory, frameRates);
                }
            }
            else
            {
                acceptDecodedPacket(std::move(*decoded), state, frameHistory, frameRates);
            }
        }

        if (videoPlayer.isOpen())
        {
            if (std::abs(appliedPlaybackSpeed - state.videoPlaybackSpeed) > 0.001F)
            {
                videoPlayer.setPlaybackSpeed(state.videoPlaybackSpeed);
                appliedPlaybackSpeed = state.videoPlaybackSpeed;
            }

            cv::Mat frame;
            bool hasFrame = false;
            if (state.videoSeekRequested)
            {
                state.videoSeekRequested = false;
                hasFrame = videoPlayer.seekTo(state.requestedVideoFrame, frame);
            }
            else if (!state.paused)
            {
                hasFrame = videoPlayer.readDue(std::chrono::steady_clock::now(), frame);
            }
            if (hasFrame)
            {
                DecodedPacket packet;
                packet.frame = std::move(frame);
                packet.sourceId = state.videoFilePath;
                packet.payloadInfo = "local video";
                packet.decodeTimeMs = videoPlayer.decodeTimeMs();
                acceptDecodedPacket(std::move(packet), state, frameHistory, frameRates);
                state.sourceFps = static_cast<float>(videoPlayer.fps());
                state.sourceFrameIndex = videoPlayer.frameIndex();
                state.videoFramePosition = videoPlayer.frameIndex() == 0 ? 0 :
                    static_cast<int64_t>(videoPlayer.frameIndex() - 1);
            }
            else if (videoPlayer.ended())
            {
                state.connectionStatus = "ended";
            }
        }

        // 暂停切换的首轮以最新帧为基准，再应用用户历史导航请求
        if (state.paused && !previousPausedState)
        {
            state.historyOffset = 0;
        }
        processHistoryNavigation(state, frameHistory);
        previousPausedState = state.paused;

        // 连接状态使用本地收帧时刻，避免依赖发布端时钟同步
        const auto now = std::chrono::steady_clock::now();
        frameRates.update(now, state);
        if (!state.paused && state.lastFrameTime.time_since_epoch().count() != 0)
        {
            state.frameAgeMs =
                std::chrono::duration<float, std::milli>(now - state.lastFrameTime).count();
        }
        updateConnectionStatus(state);

        if (state.roiChanged)
        {
            updateRoiStatistics(state);
        }

        // 仅在 UI 实际采用新实时帧时增加展示帧率计数
        if (ui.updateAndRender(state))
        {
            frameRates.recordDisplayed();
        }
    }

    return 0;
}

/**
 * @brief FrameVisualizer 进程入口
 *
 * @details 将应用生命周期抛出的标准异常转换为错误日志和非零进程退出码
 *
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 *
 * @return 应用正常退出时返回 0，发生标准异常时返回 1
 */
int main(const int argc, char **argv)
{
    try
    {
        return runApplication(argc, argv);
    }
    catch (const std::exception &error)
    {
        std::cerr << "FrameVisualizer failed: " << error.what() << '\n';
        return 1;
    }
}
