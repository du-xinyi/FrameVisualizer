#include "frame_transport/frame_sender.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zmq.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "frame_meta.pb.h"
#include "frame_transport/zmq_raii.h"

namespace frame_scope
{
    namespace
    {
        /**
         * @brief 将纳秒时长转换为协议使用的无符号整数
         */
        uint64_t durationToNs(const std::chrono::nanoseconds duration)
        {
            return static_cast<uint64_t>(duration.count());
        }

        /**
         * @brief 为未显式配置的发送器生成进程内会话标识
         */
        std::string makeSessionId()
        {
            return "session-" + std::to_string(unixTimestampNs());
        }

        /**
         * @brief 解析调用方声明或按通道数推断的像素排列
         *
         * @param frame 提供通道数的输入图像
         * @param requested 调用方配置的排列
         *
         * @return 已确定的像素排列
         *
         * @throws std::invalid_argument 自动推断时图像通道数不受支持
         */
        PixelLayout resolvePixelLayout(const cv::Mat &frame, const PixelLayout requested)
        {
            if (requested != PixelLayout::Automatic)
            {
                return requested;
            }

            switch (frame.channels())
            {
                case 1:
                    return PixelLayout::Gray;
                case 3:
                    return PixelLayout::Bgr;
                case 4:
                    return PixelLayout::Bgra;
                default:
                    throw std::invalid_argument("FrameSender supports 1, 3, or 4 channel images");
            }
        }

        /**
         * @brief 返回像素排列要求的通道数，Automatic 返回 0
         */
        int expectedChannels(const PixelLayout layout)
        {
            switch (layout)
            {
                case PixelLayout::Gray:
                    return 1;
                case PixelLayout::Bgr:
                case PixelLayout::Rgb:
                    return 3;
                case PixelLayout::Bgra:
                case PixelLayout::Rgba:
                    return 4;
                case PixelLayout::Automatic:
                    break;
            }
            return 0;
        }

        /**
         * @brief 将发送端像素排列映射为 FrameMeta 像素格式
         */
        PixelFormat protoPixelFormat(const PixelLayout layout)
        {
            switch (layout)
            {
                case PixelLayout::Bgr:
                    return PIXEL_FORMAT_BGR8;
                case PixelLayout::Rgb:
                    return PIXEL_FORMAT_RGB8;
                case PixelLayout::Gray:
                    return PIXEL_FORMAT_GRAY8;
                case PixelLayout::Bgra:
                    return PIXEL_FORMAT_BGRA8;
                case PixelLayout::Rgba:
                    return PIXEL_FORMAT_RGBA8;
                case PixelLayout::Automatic:
                    break;
            }
            return PIXEL_FORMAT_UNSPECIFIED;
        }

        /**
         * @brief 将公开时间戳时钟域映射为 protobuf 枚举
         */
        TimestampDomain protoTimestampDomain(const FrameTimestampDomain domain)
        {
            return domain == FrameTimestampDomain::UnixEpoch ? TIMESTAMP_DOMAIN_UNIX_EPOCH : TIMESTAMP_DOMAIN_MONOTONIC;
        }

        /**
         * @brief 编码后的负载及其被接收端解码后应采用的通道排列
         */
        struct EncodedFrame
        {
            std::vector<unsigned char> bytes; ///< 待发送的完整图像负载
            PixelLayout decodedLayout = PixelLayout::Automatic; ///< 负载解码后的像素排列
        };

        /**
         * @brief 将输入图像转换为 OpenCV JPEG 编码器接受的排列并执行压缩
         *
         * @param frame 待编码图像
         * @param layout 输入像素排列
         * @param quality JPEG 编码质量
         *
         * @return JPEG 负载及解码后的像素排列
         *
         * @throws std::logic_error layout 尚未从 Automatic 解析
         * @throws std::runtime_error OpenCV JPEG 编码失败
         */
        EncodedFrame encodeJpeg(const cv::Mat &frame, const PixelLayout layout, const int quality)
        {
            cv::Mat jpegInput;
            PixelLayout decodedLayout = layout;

            // JPEG 不保存 alpha，且 OpenCV 编码器按 BGR 解释三通道输入
            switch (layout)
            {
                case PixelLayout::Rgb:
                    cv::cvtColor(frame, jpegInput, cv::COLOR_RGB2BGR);
                    decodedLayout = PixelLayout::Bgr;
                    break;
                case PixelLayout::Bgra:
                    cv::cvtColor(frame, jpegInput, cv::COLOR_BGRA2BGR);
                    decodedLayout = PixelLayout::Bgr;
                    break;
                case PixelLayout::Rgba:
                    cv::cvtColor(frame, jpegInput, cv::COLOR_RGBA2BGR);
                    decodedLayout = PixelLayout::Bgr;
                    break;
                case PixelLayout::Bgr:
                case PixelLayout::Gray:
                    jpegInput = frame;
                    break;
                case PixelLayout::Automatic:
                    throw std::logic_error("automatic pixel layout was not resolved");
            }

            EncodedFrame encoded;
            encoded.decodedLayout = decodedLayout;
            if (!cv::imencode(".jpg", jpegInput, encoded.bytes,
                {cv::IMWRITE_JPEG_QUALITY, quality}))
            {
                throw std::runtime_error("FrameSender failed to encode JPEG frame");
            }
            return encoded;
        }

        /**
         * @brief 将图像逐行复制为连续 RAW 负载
         *
         * @details 逐行复制允许输入为带步长的非连续 cv::Mat 视图
         *
         * @param frame 待复制图像
         * @param layout 输入像素排列
         *
         * @return 连续像素负载及其原始排列
         */
        EncodedFrame encodeRaw(const cv::Mat &frame, const PixelLayout layout)
        {
            const std::size_t rowBytes = static_cast<std::size_t>(frame.cols) * frame.elemSize();
            EncodedFrame encoded;
            encoded.decodedLayout = layout;
            encoded.bytes.resize(rowBytes * static_cast<std::size_t>(frame.rows));

            for (int row = 0; row < frame.rows; ++row)
            {
                std::memcpy(encoded.bytes.data() + static_cast<std::size_t>(row) * rowBytes,
                    frame.ptr(row), rowBytes);
            }
            return encoded;
        }
    } // namespace

    uint64_t monotonicTimestampNs()
    {
        return durationToNs(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()));
    }

    uint64_t unixTimestampNs()
    {
        return durationToNs(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()));
    }

    /**
     * @brief FrameSender 的编码、协议组装、同步和 ZMQ 资源实现
     */
    class FrameSender::Impl
    {
    public:

        explicit Impl(FrameSenderOptions options)
            : options_(std::move(options)), context_(1), socket_(context_, ZMQ_PUB)
        {
            validateOptions();
            if (options_.sessionId.empty())
            {
                options_.sessionId = makeSessionId();
            }

            // 实时预览优先：限制积压，并在销毁时不等待未发送消息
            socket_.setIntOption(ZMQ_SNDHWM, options_.sendHighWaterMark);
            constexpr int lingerMs = 0;
            socket_.setIntOption(ZMQ_LINGER, lingerMs);
            if (options_.endpointMode == EndpointMode::Bind)
            {
                socket_.bind(options_.endpoint);
            }
            else
            {
                socket_.connect(options_.endpoint);
            }
        }

        FrameSendResult publish(const cv::Mat &frame, const FrameInfo &info)
        {
            // 同一 PUB socket 及其自动序号必须由单一临界区串行访问
            std::scoped_lock lock(mutex_);
            validateFrame(frame);

            const PixelLayout inputLayout = resolvePixelLayout(frame, options_.pixelLayout);
            if (frame.channels() != expectedChannels(inputLayout))
            {
                throw std::invalid_argument("FrameSender pixel layout does not match Mat channels");
            }

            // 编码耗时随当前帧写入元数据，供接收端展示链路统计
            const auto encodeStarted = std::chrono::steady_clock::now();
            const EncodedFrame encoded = options_.encoding == FrameEncoding::Jpeg ?
                    encodeJpeg(frame, inputLayout, options_.jpegQuality) :
                    encodeRaw(frame, inputLayout);
            const auto encodeLatency = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - encodeStarted);

            // 显式帧号会同时推进下一次自动编号，避免后续自动序号回退
            const uint64_t frameIndex = info.frameIndex.value_or(nextFrameIndex_);
            nextFrameIndex_ = frameIndex == std::numeric_limits<uint64_t>::max() ? frameIndex : frameIndex + 1;
            const uint64_t captureTimestamp = info.captureTimestampNs != 0 ?
                    info.captureTimestampNs :
                    monotonicTimestampNs();
            const FrameTimestampDomain timestampDomain = info.captureTimestampNs != 0 ?
                    info.timestampDomain :
                    FrameTimestampDomain::Monotonic;

            FrameMeta meta;
            fillMeta(meta, frame, encoded, info, frameIndex, captureTimestamp,
                timestampDomain, static_cast<uint64_t>(encodeLatency.count()));

            std::string serializedMeta;
            if (!meta.SerializeToString(&serializedMeta))
            {
                throw std::runtime_error("FrameSender failed to serialize FrameMeta");
            }

            // 线路顺序固定为 topic、FrameMeta、payload
            socket_.send(options_.topic.data(), options_.topic.size(), ZMQ_SNDMORE);
            socket_.send(serializedMeta.data(), serializedMeta.size(), ZMQ_SNDMORE);
            socket_.send(encoded.bytes.data(), encoded.bytes.size(), 0);

            return {frameIndex, encoded.bytes.size()};
        }

        [[nodiscard]] const std::string &endpoint() const noexcept
        {
            return options_.endpoint;
        }

    private:

        /**
         * @brief 校验实例生命周期内不再变化的发送配置
         */
        void validateOptions() const
        {
            if (options_.endpoint.empty() || options_.topic.empty() || options_.sourceId.empty())
            {
                throw std::invalid_argument("FrameSender endpoint, topic, and sourceId cannot be empty");
            }
            if (options_.jpegQuality < 1 || options_.jpegQuality > 100)
            {
                throw std::invalid_argument("FrameSender JPEG quality must be in [1, 100]");
            }
            if (options_.sendHighWaterMark <= 0)
            {
                throw std::invalid_argument("FrameSender sendHighWaterMark must be positive");
            }
        }

        /**
         * @brief 校验单帧尺寸与当前编码链路支持的像素深度
         */
        static void validateFrame(const cv::Mat &frame)
        {
            if (frame.empty() || frame.rows <= 0 || frame.cols <= 0)
            {
                throw std::invalid_argument("FrameSender cannot publish an empty frame");
            }
            if (frame.depth() != CV_8U)
            {
                throw std::invalid_argument("FrameSender currently supports only 8-bit images");
            }
        }

        /**
         * @brief 根据编码结果和动态帧信息填充完整 FrameMeta
         *
         * @param meta 接收协议字段的消息对象
         * @param frame 提供尺寸和未压缩大小的输入图像
         * @param encoded 提供负载大小和解码排列的编码结果
         * @param info 调用方提供的动态统计信息
         * @param frameIndex 本次采用的帧号
         * @param captureTimestamp 本次采用的采集时间戳
         * @param timestampDomain 采集时间戳所属时钟域
         * @param encodeLatencyUs 图像编码耗时，单位为微秒
         */
        void fillMeta(FrameMeta &meta, const cv::Mat &frame, const EncodedFrame &encoded,
            const FrameInfo &info, const uint64_t frameIndex,
            const uint64_t captureTimestamp,
            const FrameTimestampDomain timestampDomain,
            const uint64_t encodeLatencyUs) const
        {
            meta.set_source_id(options_.sourceId);
            meta.set_session_id(options_.sessionId);
            meta.set_frame_index(frameIndex);
            meta.set_publish_sequence(frameIndex);
            meta.set_schema_version(SCHEMA_VERSION_V2);
            meta.set_trace_id(
                info.traceId.empty() ? options_.sourceId + "-" + std::to_string(frameIndex) : info.traceId);

            // 图像描述始终对应接收端解码后的排列，而不是编码前的输入排列
            auto *image = meta.mutable_image();
            image->set_width(static_cast<uint32_t>(frame.cols));
            image->set_height(static_cast<uint32_t>(frame.rows));
            image->set_channels(static_cast<uint32_t>(expectedChannels(encoded.decodedLayout)));
            image->set_stride_bytes(static_cast<uint32_t>(
                static_cast<std::size_t>(frame.cols) *
                static_cast<std::size_t>(expectedChannels(encoded.decodedLayout))));
            image->set_bytes_per_channel(1);
            image->set_pixel_format(protoPixelFormat(encoded.decodedLayout));
            image->set_color_space(COLOR_SPACE_SRGB);

            auto *payload = meta.mutable_payload();
            payload->set_encoding(options_.encoding == FrameEncoding::Jpeg ?
                PAYLOAD_ENCODING_JPEG :
                PAYLOAD_ENCODING_RAW);
            payload->set_payload_bytes(encoded.bytes.size());
            payload->set_uncompressed_bytes(
                static_cast<uint64_t>(frame.total()) *
                static_cast<uint64_t>(expectedChannels(encoded.decodedLayout)));
            payload->set_key_frame(true);
            payload->set_fragment_index(0);
            payload->set_fragment_count(1);

            auto *timing = meta.mutable_timing();
            timing->set_capture_timestamp_ns(captureTimestamp);
            timing->set_capture_timestamp_domain(protoTimestampDomain(timestampDomain));
            timing->set_publish_timestamp_ns(monotonicTimestampNs());
            timing->set_publish_timestamp_domain(TIMESTAMP_DOMAIN_MONOTONIC);
            timing->set_enqueue_latency_us(info.enqueueLatencyUs);
            timing->set_pipeline_latency_us(info.pipelineLatencyUs);
            timing->set_encode_latency_us(encodeLatencyUs);

            auto *stats = meta.mutable_stats();
            stats->set_fps(info.sourceFps);
            stats->set_dropped_frames(info.droppedFrames);
        }

        FrameSenderOptions options_; ///< 发布端点、来源标识和编码策略
        zmq::Context context_; ///< 必须晚于 socket_ 销毁的 libzmq context
        zmq::Socket socket_; ///< 发送三段帧消息的 PUB socket
        std::mutex mutex_; ///< 串行保护 socket_ 和 nextFrameIndex_
        uint64_t nextFrameIndex_ = 0; ///< 未显式指定帧号时使用的下一序号
    };

    FrameSender::FrameSender(FrameSenderOptions options)
        : impl_(std::make_unique<Impl>(std::move(options)))
    {
    }

    FrameSender::~FrameSender() = default;

    FrameSender::FrameSender(FrameSender &&) noexcept = default;

    FrameSender &FrameSender::operator=(FrameSender &&) noexcept = default;

    FrameSendResult FrameSender::publish(const cv::Mat &frame, const FrameInfo &info) const
    {
        if (!impl_)
        {
            throw std::logic_error("cannot publish with a moved-from FrameSender");
        }
        return impl_->publish(frame, info);
    }

    const std::string &FrameSender::endpoint() const noexcept
    {
        static constexpr std::string empty;
        return impl_ ? impl_->endpoint() : empty;
    }
} // namespace frame_scope
