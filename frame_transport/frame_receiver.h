#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace frameviz
{
    /**
     * @brief 一个 ZMQ multipart 消息段的独立字节存储
     */
    using FrameMessagePart = std::vector<unsigned char>;

    /**
     * @brief 按线路顺序保存一条完整 ZMQ multipart 消息
     */
    using MultipartFrameMessage = std::vector<FrameMessagePart>;

    /**
     * @brief ZMQ SUB 接收器生命周期内保持不变的初始配置
     */
    struct FrameReceiverOptions
    {
        std::string endpoint = "tcp://127.0.0.1:5555"; ///< SUB socket 连接的 ZMQ 端点
        std::string topic; ///< 订阅前缀；空字符串表示接收全部主题
        int receiveHighWaterMark = 2; ///< libzmq 接收队列允许保留的消息数量
        int receiveTimeoutMs = 1; ///< multipart 后续消息段的最大等待毫秒数
        std::size_t maxMessagesToDrain = 64; ///< 单次调用为追赶最新帧允许排空的消息上限
    };

    /**
     * @brief 接收 FrameVisualizer 线路消息并优先返回队列中的最新一帧
     *
     * @details 类内部管理 libzmq context 和 SUB socket。receiveLatest() 非阻塞检查
     * 第一段消息，并有界排空积压消息。返回数据拥有独立存储，可安全交给其他线程。
     * 成员函数没有内部并发同步，应由同一线程串行调用
     */
    class FrameReceiver
    {
    public:

        /**
         * @brief 创建并连接 SUB 接收器
         *
         * @param options 接收端点、订阅主题和队列策略
         *
         * @throws std::invalid_argument 配置字段超出支持范围
         * @throws std::runtime_error libzmq context、socket 或连接初始化失败
         */
        explicit FrameReceiver(FrameReceiverOptions options = {});

        /** @brief 关闭 SUB socket 并释放 libzmq context */
        ~FrameReceiver();

        /** @brief 接收器独占底层 ZMQ 资源，禁止复制 */
        FrameReceiver(const FrameReceiver &) = delete;

        /** @brief 接收器独占底层 ZMQ 资源，禁止复制赋值 */
        FrameReceiver &operator=(const FrameReceiver &) = delete;

        /**
         * @brief 转移接收器所有权
         *
         * @param other 提供底层接收资源的接收器
         */
        FrameReceiver(FrameReceiver &&) noexcept;

        /**
         * @brief 接管另一个接收器的底层资源
         *
         * @param other 提供底层接收资源的接收器
         *
         * @return 当前对象
         */
        FrameReceiver &operator=(FrameReceiver &&) noexcept;

        /**
         * @brief 非阻塞返回当前队列中的最新完整消息
         *
         * @details 当队列中存在多条消息时，有界排空旧消息并只返回最后一条。每个消息段
         * 均复制到独立存储，不再引用 libzmq 消息内存
         *
         * @return 最新 multipart 消息；当前无数据时返回空集合
         *
         * @throws std::logic_error 在已被移动的接收器上调用
         * @throws std::runtime_error libzmq 接收失败
         */
        [[nodiscard]] MultipartFrameMessage receiveLatest() const;

        /**
         * @brief 使用新 SUB socket 切换端点
         *
         * @details 先完成新 socket 的创建与连接，再替换当前 socket。切换失败时保留原连接
         *
         * @param endpoint 新的 ZMQ 连接端点
         *
         * @throws std::invalid_argument endpoint 为空
         * @throws std::logic_error 在已被移动的接收器上调用
         * @throws std::runtime_error 新 socket 创建或连接失败
         */
        void changeEndpoint(const std::string &endpoint) const;

        /**
         * @brief 返回当前活动端点
         *
         * @return 当前端点；对象已被移动时返回空字符串
         */
        [[nodiscard]] const std::string &endpoint() const noexcept;

    private:

        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace frameviz
