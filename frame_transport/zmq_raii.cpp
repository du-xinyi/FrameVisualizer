#include "frame_transport/zmq_raii.h"

#include <cerrno>
#include <utility>

namespace frame_scope::zmq
{
    std::runtime_error lastError(const std::string_view operation)
    {
        // 先保存线程局部错误码，避免后续库调用覆盖原始失败原因
        const int error = zmq_errno();

        return std::runtime_error(std::string(operation) + ": " + zmq_strerror(error));
    }

    Context::Context(const int ioThreads): handle_(zmq_ctx_new())
    {
        if (handle_ == nullptr)
        {
            throw lastError("zmq_ctx_new failed");
        }

        if (zmq_ctx_set(handle_, ZMQ_IO_THREADS, ioThreads) != 0)
        {
            // 清理动作可能改变 errno，因此在终止 context 前构造异常
            const auto error = lastError("zmq_ctx_set failed");
            zmq_ctx_term(handle_);
            handle_ = nullptr;

            throw error;
        }
    }

    Context::~Context()
    {
        if (handle_ != nullptr)
        {
            // 信号中断不代表终止失败，持续重试直至 context 完成释放或出现其他错误
            while (zmq_ctx_term(handle_) != 0 && zmq_errno() == EINTR)
            {
            }
        }
    }

    void *Context::get() const noexcept
    {
        return handle_;
    }

    Socket::Socket(Context &context, const int type): handle_(zmq_socket(context.get(), type))
    {
        if (handle_ == nullptr)
        {
            throw lastError("zmq_socket failed");
        }
    }

    Socket::~Socket()
    {
        close();
    }

    Socket::Socket(Socket &&other) noexcept: handle_(std::exchange(other.handle_, nullptr))
    {
    }

    Socket &Socket::operator=(Socket &&other) noexcept
    {
        if (this != &other)
        {
            // 先释放现有句柄，再将源对象置空以维持单一所有权
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }

        return *this;
    }

    void Socket::setIntOption(const int option, const int value)
    {
        // 统一 int 选项的地址和字节数，避免调用方重复处理 C API 约定
        setOption(option, &value, sizeof(value));
    }

    void Socket::setOption(const int option, const void *value, const std::size_t size)
    {
        if (zmq_setsockopt(handle_, option, value, size) != 0)
        {
            throw lastError("zmq_setsockopt failed");
        }
    }

    void Socket::bind(const std::string &endpoint)
    {
        if (zmq_bind(handle_, endpoint.c_str()) != 0)
        {
            throw lastError("zmq_bind failed");
        }
    }

    void Socket::connect(const std::string &endpoint)
    {
        if (zmq_connect(handle_, endpoint.c_str()) != 0)
        {
            throw lastError("zmq_connect failed");
        }
    }

    void Socket::send(const void *data, const std::size_t size, const int flags)
    {
        if (zmq_send(handle_, data, size, flags) < 0)
        {
            throw lastError("zmq_send failed");
        }
    }

    void *Socket::get() const noexcept
    {
        return handle_;
    }

    void Socket::close() noexcept
    {
        if (handle_ != nullptr)
        {
            // 析构路径无法报告关闭错误；无论结果如何都清空句柄以防重复关闭
            zmq_close(handle_);
            handle_ = nullptr;
        }
    }

    Message::Message()
    {
        if (zmq_msg_init(&message_) != 0)
        {
            throw lastError("zmq_msg_init failed");
        }
    }

    Message::~Message()
    {
        zmq_msg_close(&message_);
    }

    int Message::receive(Socket &socket, const int flags)
    {
        return zmq_msg_recv(&message_, socket.get(), flags);
    }

    const void *Message::data() noexcept
    {
        return zmq_msg_data(&message_);
    }

    std::size_t Message::size() const noexcept
    {
        return zmq_msg_size(&message_);
    }

    bool Message::more() const noexcept
    {
        return zmq_msg_more(&message_) != 0;
    }
} // namespace frame_scope::zmq_c
