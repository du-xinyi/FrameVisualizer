#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <zmq.h>

namespace frame_scope::zmq
{
    /**
     * @brief 将 libzmq 最近一次错误转换为包含操作上下文的异常
     *
     * @param operation 发生错误的操作描述
     *
     * @return 包含 zmq_errno() 对应错误文本的运行时异常
     */
    std::runtime_error lastError(std::string_view operation);

    /**
     * @brief 独占管理一个 libzmq context
     *
     * @details context 必须晚于由其创建的所有 Socket 销毁，因此禁止复制和移动
     */
    class Context
    {
    public:

        /**
         * @brief 创建 context 并设置 I/O 线程数量
         *
         * @param ioThreads libzmq 用于处理网络 I/O 的线程数量
         *
         * @throws std::runtime_error context 创建或选项设置失败
         */
        explicit Context(int ioThreads = 1);

        /**
         * @brief 终止并释放 context
         */
        ~Context();

        /**
         * @brief context 具有独占所有权，禁止复制
         */
        Context(const Context &) = delete;

        /**
         * @brief context 具有独占所有权，禁止复制赋值
         */
        Context &operator=(const Context &) = delete;

        /**
         * @brief 防止 Socket 保存的 context 关系因移动而失效
         */
        Context(Context &&) = delete;

        /**
         * @brief 防止 Socket 保存的 context 关系因移动赋值而失效
         */
        Context &operator=(Context &&) = delete;

        /**
         * @brief 返回传给 libzmq C API 的原始 context 句柄
         *
         * @return 不转移所有权的 context 句柄
         */
        [[nodiscard]] void *get() const noexcept;

    private:

        void *handle_ = nullptr; ///< 当前对象独占的 libzmq context 句柄
    };

    /**
     * @brief 独占管理一个 libzmq socket
     *
     * @details Socket 可以移动，用于在端点切换成功后替换现有连接。构造它的 Context
     * 必须覆盖 Socket 的完整生命周期
     */
    class Socket
    {
    public:

        /**
         * @brief 在指定 context 中创建给定类型的 socket
         *
         * @param context socket 所属且生命周期更长的 context
         * @param type libzmq socket 类型，例如 ZMQ_PUB 或 ZMQ_SUB
         *
         * @throws std::runtime_error socket 创建失败
         */
        Socket(Context &context, int type);

        /**
         * @brief 关闭并释放 socket
         */
        ~Socket();

        /**
         * @brief socket 具有独占所有权，禁止复制
         */
        Socket(const Socket &) = delete;

        /**
         * @brief socket 具有独占所有权，禁止复制赋值
         */
        Socket &operator=(const Socket &) = delete;

        /**
         * @brief 转移 socket 所有权，源对象变为空对象
         *
         * @param other 提供待接管句柄的 socket
         */
        Socket(Socket &&other) noexcept;

        /**
         * @brief 关闭当前 socket 后接管另一个 socket 的所有权
         *
         * @param other 提供待接管句柄的 socket
         *
         * @return 当前对象
         */
        Socket &operator=(Socket &&other) noexcept;

        /**
         * @brief 设置值类型为 int 的 socket 选项
         *
         * @param option libzmq socket 选项标识
         * @param value 待设置的整数值
         *
         * @throws std::runtime_error 选项设置失败
         */
        void setIntOption(int option, int value);

        /**
         * @brief 设置具有任意字节表示的 socket 选项
         *
         * @param option libzmq socket 选项标识
         * @param value 选项数据起始地址
         * @param size 选项数据字节数
         *
         * @throws std::runtime_error 选项设置失败
         */
        void setOption(int option, const void *value, std::size_t size);

        /**
         * @brief 将 socket 绑定到端点
         *
         * @param endpoint libzmq 端点字符串
         *
         * @throws std::runtime_error 绑定失败
         */
        void bind(const std::string &endpoint);

        /**
         * @brief 将 socket 连接到端点
         *
         * @param endpoint libzmq 端点字符串
         *
         * @throws std::runtime_error 连接失败
         */
        void connect(const std::string &endpoint);

        /**
         * @brief 发送一个完整消息段
         *
         * @param data 消息数据起始地址
         * @param size 消息数据字节数
         * @param flags libzmq 发送标志，可包含 ZMQ_SNDMORE
         *
         * @throws std::runtime_error 消息发送失败
         */
        void send(const void *data, std::size_t size, int flags);

        /**
         * @brief 返回传给 libzmq C API 的原始 socket 句柄
         *
         * @return 不转移所有权的 socket 句柄
         */
        [[nodiscard]] void *get() const noexcept;

    private:

        /**
         * @brief 关闭当前句柄；空对象调用不产生操作
         */
        void close() noexcept;

        void *handle_ = nullptr; ///< 当前对象独占的 libzmq socket 句柄
    };

    /**
     * @brief 管理一个通过 zmq_msg_init() 初始化的消息对象
     */
    class Message
    {
    public:

        /**
         * @brief 初始化空消息
         *
         * @throws std::runtime_error 消息初始化失败
         */
        Message();

        /**
         * @brief 调用 zmq_msg_close() 释放消息资源
         */
        ~Message();

        /**
         * @brief zmq_msg_t 资源不可共享，禁止复制
         */
        Message(const Message &) = delete;

        /**
         * @brief zmq_msg_t 资源不可共享，禁止复制赋值
         */
        Message &operator=(const Message &) = delete;

        /**
         * @brief 从 socket 接收一个消息段
         *
         * @param socket 接收消息的 socket
         * @param flags libzmq 接收标志，可使用 ZMQ_DONTWAIT
         *
         * @return 接收的字节数；失败时返回 -1 并由 zmq_errno() 提供原因
         */
        int receive(Socket &socket, int flags);

        /**
         * @brief 返回消息负载起始地址
         *
         * @return 由当前消息拥有且在消息下次变更或关闭前有效的数据地址
         */
        [[nodiscard]] const void *data() noexcept;

        /**
         * @brief 返回消息负载大小
         *
         * @return 负载字节数
         */
        [[nodiscard]] std::size_t size() const noexcept;

        /**
         * @brief 判断当前段之后是否还有 multipart 消息段
         *
         * @return 存在后续消息段时返回 true
         */
        [[nodiscard]] bool more() const noexcept;

    private:

        zmq_msg_t message_{}; ///< 当前对象独占且已初始化的 libzmq 消息
    };
} // namespace frame_scope::zmq_c
