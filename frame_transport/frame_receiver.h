#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace frame_scope
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
     * @brief 尚未复制图像负载时可检查的 multipart 消息头
     *
     * @details 视图只在过滤回调执行期间有效。标准协议中前两段分别是 topic 和
     * protobuf FrameMeta；传统单段消息的 metadata 为空
     */
    struct FrameMessageHeaderView
    {
        std::span<const unsigned char> topic; ///< multipart 第一段
        std::span<const unsigned char> metadata; ///< multipart 第二段；不存在时为空
    };

    /** @brief 返回 true 表示保留当前消息负载，false 表示仅排空而不复制 */
    using FrameMessageFilter = std::function<bool(const FrameMessageHeaderView &)>;

    /** @brief 一次有界接收与过滤的结果和工作量统计 */
    struct FrameReceiveResult
    {
        MultipartFrameMessage message; ///< 所有通过过滤消息中的最新一条
        std::size_t receivedMessages = 0; ///< 本次从 socket 排空的完整消息数
        std::size_t filteredMessages = 0; ///< 被过滤器拒绝的消息数
        std::size_t copiedPayloadBytes = 0; ///< 通过过滤后复制的 payload 总字节数
    };

    /**
     * @brief ZMQ SUB 接收器生命周期内保持不变的初始配置
     */
    struct FrameReceiverOptions
    {
        std::string endpoint = "tcp://127.0.0.1:5555"; ///< SUB socket 连接的 ZMQ 端点
        std::string topic; ///< 订阅前缀；空字符串表示接收全部主题
        int receiveHighWaterMark = 64; ///< libzmq 接收队列允许保留的消息数量
        int receiveTimeoutMs = 1; ///< multipart 后续消息段的最大等待毫秒数
        std::size_t maxMessagesToDrain = 64; ///< 单次调用为追赶最新帧允许排空的消息上限
    };

    /**
     * @brief 接收 frame-scope 线路消息并优先返回队列中的最新一帧
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
         * @return 最新 multipart 消息及本次排空统计；当前无数据时 message 为空
         *
         * @throws std::logic_error 在已被移动的接收器上调用
         * @throws std::runtime_error libzmq 接收失败
         */
        [[nodiscard]] FrameReceiveResult receiveLatest() const;

        /**
         * @brief 在复制 payload 前过滤消息并返回通过过滤器的最新一条
         *
         * @details 所有消息仍会完整排空以维持 multipart 边界；被拒绝消息的 payload
         * 直接由 libzmq 释放，不复制到应用缓冲区
         *
         * @param filter 检查 topic 和 protobuf 元数据的同步回调
         *
         * @return 最新匹配消息及本次排空统计
         */
        [[nodiscard]] FrameReceiveResult receiveLatest(const FrameMessageFilter &filter) const;

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
} // namespace frame_scope
