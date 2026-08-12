#include "frame_transport/frame_receiver.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <zmq.h>

#include "frame_transport/zmq_raii.h"

namespace frame_scope
{
    namespace
    {
        /**
         * @brief 单个消息段的可选副本、线路大小及其后续段标记
         */
        struct ReceivedPart
        {
            FrameMessagePart bytes; ///< 当前消息段的独立字节存储
            bool more = false; ///< 当前 multipart 消息是否还有后续段
            std::size_t size = 0; ///< 线路消息段大小，包括未复制段
        };

        /** @brief 单条 multipart 消息的接收和过滤结果 */
        struct ReceivedMultipart
        {
            MultipartFrameMessage messages; ///< 通过过滤时保存的消息段
            bool received = false; ///< 是否从 socket 收到了第一段
            bool accepted = false; ///< 过滤器是否接受当前消息
            std::size_t copiedPayloadBytes = 0; ///< 实际复制的第三段及后续段字节数
        };

        /**
         * @brief 在创建任何 ZMQ 资源前校验接收配置
         *
         * @param options 待校验配置
         *
         * @throws std::invalid_argument 端点为空或数值字段超出支持范围
         */
        void validateOptions(const FrameReceiverOptions &options)
        {
            if (options.endpoint.empty())
            {
                throw std::invalid_argument("FrameReceiver endpoint cannot be empty");
            }
            if (options.receiveHighWaterMark <= 0)
            {
                throw std::invalid_argument("FrameReceiver receiveHighWaterMark must be positive");
            }
            if (options.receiveTimeoutMs < 0)
            {
                throw std::invalid_argument("FrameReceiver receiveTimeoutMs cannot be negative");
            }
            if (options.maxMessagesToDrain == 0)
            {
                throw std::invalid_argument("FrameReceiver maxMessagesToDrain must be positive");
            }
        }

        /**
         * @brief 创建、配置并连接一个 SUB socket
         *
         * @param context socket 所属的 context
         * @param options 端点、订阅前缀和队列配置
         *
         * @return 完成连接的 socket
         *
         * @throws std::runtime_error libzmq 操作失败
         */
        zmq::Socket makeSubscriberSocket(zmq::Context &context,
            const FrameReceiverOptions &options)
        {
            zmq::Socket socket(context, ZMQ_SUB);
            socket.setIntOption(ZMQ_RCVHWM, options.receiveHighWaterMark);
            socket.setOption(ZMQ_SUBSCRIBE, options.topic.data(), options.topic.size());
            socket.setIntOption(ZMQ_RCVTIMEO, options.receiveTimeoutMs);
            socket.connect(options.endpoint);
            return socket;
        }

        /**
         * @brief 接收一个消息段，并按需复制其负载
         *
         * @param socket 接收消息的 SUB socket
         * @param flags libzmq 接收标志
         * @param copyBytes 是否将消息内容复制到独立存储
         *
         * @return 接收的数据和后续段标记；无可用数据或等待超时时返回 std::nullopt
         *
         * @throws std::runtime_error 除 EAGAIN 外的 libzmq 接收错误
         */
        std::optional<ReceivedPart> receivePart(zmq::Socket &socket, const int flags,
                                                const bool copyBytes)
        {
            zmq::Message message;
            if (message.receive(socket, flags) < 0)
            {
                if (zmq_errno() == EAGAIN)
                {
                    return std::nullopt;
                }
                throw zmq::lastError("zmq_msg_recv failed");
            }

            // 消息内容必须脱离 zmq_msg_t 生命周期，才能安全提交给后台解码线程
            ReceivedPart part;
            part.size = message.size();
            if (copyBytes)
            {
                part.bytes.resize(part.size);
                if (!part.bytes.empty())
                {
                    std::memcpy(part.bytes.data(), message.data(), part.bytes.size());
                }
            }
            part.more = message.more();

            return part;
        }

        /**
         * @brief 接收一条完整 multipart 消息
         *
         * @details 第一段使用非阻塞接收，后续段使用 socket 配置的短超时
         *
         * @param socket 接收消息的 SUB socket
         *
         * @return 当前消息是否存在、是否通过过滤以及通过时的独立消息段副本
         */
        ReceivedMultipart receiveMultipart(zmq::Socket &socket,
                                            const FrameMessageFilter &filter)
        {
            ReceivedMultipart result;
            auto part = receivePart(socket, ZMQ_DONTWAIT, true);
            if (!part)
            {
                return result;
            }
            result.received = true;

            result.messages.reserve(3);
            result.messages.push_back(std::move(part->bytes));
            if (part->more)
            {
                part = receivePart(socket, 0, true);
                if (part)
                {
                    result.messages.push_back(std::move(part->bytes));
                }
            }

            const FrameMessageHeaderView header{
                result.messages.front(),
                result.messages.size() >= 2 ?
                    std::span<const unsigned char>(result.messages[1]) :
                    std::span<const unsigned char>{}
            };
            std::exception_ptr filterError;
            try
            {
                result.accepted = !filter || filter(header);
            }
            catch (...)
            {
                // 先排空剩余段以恢复下一条消息的边界，再把回调异常交还调用方。
                result.accepted = false;
                filterError = std::current_exception();
            }

            while (part && part->more)
            {
                part = receivePart(socket, 0, result.accepted);
                if (!part)
                {
                    break;
                }
                if (result.accepted)
                {
                    result.copiedPayloadBytes += part->size;
                    result.messages.push_back(std::move(part->bytes));
                }
            }

            if (filterError)
            {
                std::rethrow_exception(filterError);
            }

            if (!result.accepted)
            {
                result.messages.clear();
            }
            return result;
        }
    } // namespace

    /**
     * @brief FrameReceiver 的 ZMQ 资源、配置和接收策略实现
     */
    class FrameReceiver::Impl
    {
    public:

        explicit Impl(FrameReceiverOptions options)
            : options_(std::move(options)), context_(1), socket_(initializeSocket())
        {
        }

        FrameReceiveResult receiveLatest(const FrameMessageFilter &filter)
        {
            FrameReceiveResult result;
            // 用新消息持续覆盖结果，在有界工作量内尽量追赶实时流
            for (std::size_t i = 0; i < options_.maxMessagesToDrain; ++i)
            {
                ReceivedMultipart received = receiveMultipart(socket_, filter);
                if (!received.received)
                {
                    break;
                }
                ++result.receivedMessages;
                result.copiedPayloadBytes += received.copiedPayloadBytes;
                if (received.accepted)
                {
                    result.message = std::move(received.messages);
                }
                else
                {
                    ++result.filteredMessages;
                }
            }
            return result;
        }

        void changeEndpoint(const std::string &endpoint)
        {
            if (endpoint.empty())
            {
                throw std::invalid_argument("FrameReceiver endpoint cannot be empty");
            }
            if (endpoint == options_.endpoint)
            {
                return;
            }

            // 先连接临时 socket，保证失败时当前连接和配置均保持不变
            FrameReceiverOptions nextOptions = options_;
            nextOptions.endpoint = endpoint;
            zmq::Socket nextSocket = makeSubscriberSocket(context_, nextOptions);
            socket_ = std::move(nextSocket);
            options_.endpoint = endpoint;
        }

        [[nodiscard]] const std::string &endpoint() const noexcept { return options_.endpoint; }

    private:

        /**
         * @brief 校验初始配置并创建首个 SUB socket
         */
        zmq::Socket initializeSocket()
        {
            validateOptions(options_);
            return makeSubscriberSocket(context_, options_);
        }

        FrameReceiverOptions options_; ///< 当前端点及接收队列策略
        zmq::Context context_; ///< 必须晚于 socket_ 销毁的 libzmq context
        zmq::Socket socket_; ///< 当前活动端点对应的 SUB socket
    };

    FrameReceiver::FrameReceiver(FrameReceiverOptions options)
        : impl_(std::make_unique<Impl>(std::move(options)))
    {
    }

    FrameReceiver::~FrameReceiver() = default;

    FrameReceiver::FrameReceiver(FrameReceiver &&) noexcept = default;

    FrameReceiver &FrameReceiver::operator=(FrameReceiver &&) noexcept = default;

    FrameReceiveResult FrameReceiver::receiveLatest() const
    {
        return receiveLatest({});
    }

    FrameReceiveResult FrameReceiver::receiveLatest(const FrameMessageFilter &filter) const
    {
        if (!impl_)
        {
            throw std::logic_error("cannot receive with a moved-from FrameReceiver");
        }

        return impl_->receiveLatest(filter);
    }

    void FrameReceiver::changeEndpoint(const std::string &endpoint) const
    {
        if (!impl_)
        {
            throw std::logic_error("cannot change endpoint on a moved-from FrameReceiver");
        }

        impl_->changeEndpoint(endpoint);
    }

    const std::string &FrameReceiver::endpoint() const noexcept
    {
        static constexpr std::string empty;

        return impl_ ? impl_->endpoint() : empty;
    }
} // namespace frame_scope
