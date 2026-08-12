#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <opencv2/core/mat.hpp>

namespace frame_scope
{

/**
 * @brief 图像在线路上的编码方式
 */
enum class FrameEncoding
{
    Jpeg, ///< 将图像编码为单帧 JPEG 负载
    Raw ///< 逐行复制未压缩像素负载
};

/**
 * @brief 调用方传入的像素通道排列
 */
enum class PixelLayout
{
    Automatic, ///< 根据通道数推断 Gray、BGR 或 BGRA
    Bgr, ///< OpenCV 默认的蓝、绿、红三通道排列
    Rgb, ///< 红、绿、蓝三通道排列
    Gray, ///< 单通道灰度排列
    Bgra, ///< 蓝、绿、红、透明度四通道排列
    Rgba ///< 红、绿、蓝、透明度四通道排列
};

/**
 * @brief 发布端创建 ZMQ 套接字的方式
 */
enum class EndpointMode
{
    Bind, ///< 发布器负责绑定并监听端点
    Connect ///< 发布器连接由其他组件绑定的端点
};

/**
 * @brief 外部时间戳所属的时钟域
 */
enum class FrameTimestampDomain
{
    Monotonic, ///< steady_clock 单调时钟域
    UnixEpoch ///< system_clock 的 Unix Epoch 时钟域
};

/**
 * @brief 发布器生命周期内保持不变的配置
 */
struct FrameSenderOptions
{
    std::string endpoint = "tcp://*:5555"; ///< PUB socket 使用的 ZMQ 端点
    EndpointMode endpointMode = EndpointMode::Bind; ///< 端点由本实例绑定或连接
    std::string topic = "frame.camera.0"; ///< multipart 消息第一段携带的主题
    std::string sourceId = "camera-0"; ///< 写入 FrameMeta 的数据源标识
    std::string sessionId; ///< 发布会话标识；空值时由发送器自动生成
    FrameEncoding encoding = FrameEncoding::Jpeg; ///< 图像负载编码方式
    PixelLayout pixelLayout = PixelLayout::Automatic; ///< 输入 cv::Mat 的通道语义
    int jpegQuality = 90; ///< JPEG 编码质量，取值范围为 1 到 100
    int sendHighWaterMark = 2; ///< libzmq 发送队列高水位，必须大于 0
};

/**
 * @brief 单帧可选的动态采集与统计信息
 */
struct FrameInfo
{
    std::optional<uint64_t> frameIndex; ///< 数据源帧号；空值时由发送器自动递增
    uint64_t captureTimestampNs = 0; ///< 采集时间戳；0 表示由发送器取当前单调时钟
    FrameTimestampDomain timestampDomain = FrameTimestampDomain::Monotonic; ///< 采集时间戳时钟域
    std::string traceId; ///< 帧级追踪标识；空值时由 sourceId 和帧号生成
    double sourceFps = 0.0; ///< 数据源报告的帧率
    uint64_t droppedFrames = 0; ///< 发布端累计丢弃帧数
    uint64_t enqueueLatencyUs = 0; ///< 采集到进入发送队列的耗时，单位为微秒
    uint64_t pipelineLatencyUs = 0; ///< 采集到发布前的管线耗时，单位为微秒
};

/** @brief 一次成功发送的结果 */
struct FrameSendResult
{
    uint64_t frameIndex = 0; ///< 本次消息实际写入的帧号
    std::size_t payloadBytes = 0; ///< 编码后图像负载字节数
};

/**
 * @brief 将 cv::Mat 发布为 frame-scope 可直接接收的 ZMQ 消息
 *
 * @details 实例内部串行化 publish 调用，可由多个线程共享。构造、编码或发送失败时
 * 抛出 std::invalid_argument 或 std::runtime_error。
 */
class FrameSender
{
  public:
    /**
     * @brief 创建并初始化 PUB 发送器
     *
     * @param options 端点、来源信息和编码配置
     *
     * @throws std::invalid_argument 配置字段超出支持范围
     * @throws std::runtime_error libzmq context、socket、绑定或连接初始化失败
     */
    explicit FrameSender(FrameSenderOptions options = {});

    /**
     * @brief 关闭 PUB socket 并释放 libzmq context
     */
    ~FrameSender();

    /**
     * @brief 发送器独占底层 ZMQ 资源，禁止复制
     */
    FrameSender(const FrameSender &) = delete;

    /**
     * @brief 发送器独占底层 ZMQ 资源，禁止复制赋值
     */
    FrameSender &operator=(const FrameSender &) = delete;

    /**
     * @brief 转移发送器所有权
     *
     * @param other 提供底层发送资源的发送器
     */
    FrameSender(FrameSender &&) noexcept;

    /**
     * @brief 接管另一个发送器的底层资源
     *
     * @param other 提供底层发送资源的发送器
     *
     * @return 当前对象
     */
    FrameSender &operator=(FrameSender &&) noexcept;

    /**
     * @brief 编码并立即发布一帧
     *
     * @details 依次发送 topic、FrameMeta 和图像负载三个消息段。未指定帧号和采集
     * 时间戳时由发送器生成。同一实例的并发调用会在内部串行执行
     *
     * @param frame 待发送的 8 位单通道、三通道或四通道图像
     * @param info 当前帧的可选序号、时间和统计信息
     *
     * @return 实际帧号和编码负载大小
     *
     * @throws std::invalid_argument 图像为空、深度或通道布局不受支持
     * @throws std::logic_error 在已被移动的发送器上调用
     * @throws std::runtime_error 图像编码、序列化或 libzmq 发送失败
     */
    [[nodiscard]] FrameSendResult publish(const cv::Mat &frame, const FrameInfo &info = {}) const;

    /**
     * @brief 返回发布器实际使用的端点
     *
     * @return 当前端点；对象已被移动时返回空字符串
     */
    [[nodiscard]] const std::string &endpoint() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief 获取当前 steady_clock 时间
 *
 * @return 单调时钟相对其纪元的纳秒数
 */
uint64_t monotonicTimestampNs();

/**
 * @brief 获取当前 Unix Epoch 时间
 *
 * @return system_clock 相对 Unix Epoch 的纳秒数
 */
uint64_t unixTimestampNs();

} // namespace frame_scope
