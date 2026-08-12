#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <zmq.h>

#include "frame_meta.pb.h"
#include "frame_transport/frame_receiver.h"
#include "frame_transport/zmq_raii.h"

namespace
{
    using namespace std::chrono_literals;

    constexpr int kSourceCount = 8;
    constexpr int kFrameRounds = 32;
    constexpr std::size_t kPayloadBytes = 512 * 1024;
    constexpr std::string_view kSelectedSource = "camera-0";

    struct Measurement
    {
        double elapsedMs = 0.0;
        frame_scope::FrameReceiveResult receive;
        std::string deliveredSource;
    };

    void sendFrame(frame_scope::zmq::Socket &publisher, const std::string &sourceId,
                   const std::vector<unsigned char> &payload)
    {
        frame_scope::FrameMeta meta;
        meta.set_source_id(sourceId);
        meta.mutable_payload()->set_payload_bytes(payload.size());
        std::string metadata;
        if (!meta.SerializeToString(&metadata))
        {
            throw std::runtime_error("failed to serialize benchmark metadata");
        }

        const std::string topic = "camera." + sourceId;
        publisher.send(topic.data(), topic.size(), ZMQ_SNDMORE);
        publisher.send(metadata.data(), metadata.size(), ZMQ_SNDMORE);
        publisher.send(payload.data(), payload.size(), 0);
    }

    std::string messageSourceId(const frame_scope::MultipartFrameMessage &message)
    {
        if (message.size() < 2)
        {
            return {};
        }
        frame_scope::FrameMeta meta;
        if (!meta.ParseFromArray(message[1].data(), static_cast<int>(message[1].size())))
        {
            return {};
        }
        return meta.source_id();
    }

    Measurement measure(const bool filterBeforePayloadCopy)
    {
        const std::string suffix = filterBeforePayloadCopy ? "filtered" : "unfiltered";
        const std::string socketPath = "/tmp/frame-scope-benchmark-" +
            std::to_string(getpid()) + "-" + suffix + ".sock";
        const std::string endpoint = "ipc://" + socketPath;
        std::filesystem::remove(socketPath);

        frame_scope::zmq::Context publisherContext;
        frame_scope::zmq::Socket publisher(publisherContext, ZMQ_PUB);
        publisher.setIntOption(ZMQ_SNDHWM, 4096);
        publisher.bind(endpoint);

        frame_scope::FrameReceiverOptions options;
        options.endpoint = endpoint;
        frame_scope::FrameReceiver receiver(options);
        std::this_thread::sleep_for(250ms);

        const std::vector<unsigned char> payload(kPayloadBytes, 0x5a);
        for (int round = 0; round < kFrameRounds; ++round)
        {
            for (int source = 0; source < kSourceCount; ++source)
            {
                sendFrame(publisher, "camera-" + std::to_string(source), payload);
            }
        }
        std::this_thread::sleep_for(100ms);

        const auto started = std::chrono::steady_clock::now();
        frame_scope::FrameReceiveResult receive;
        if (filterBeforePayloadCopy)
        {
            receive = receiver.receiveLatest(
                [](const frame_scope::FrameMessageHeaderView &header)
                {
                    frame_scope::FrameMeta meta;
                    return meta.ParseFromArray(header.metadata.data(),
                               static_cast<int>(header.metadata.size())) &&
                           meta.source_id() == kSelectedSource;
                });
        }
        else
        {
            receive = receiver.receiveLatest();
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();

        Measurement measurement;
        measurement.elapsedMs = elapsedMs;
        measurement.deliveredSource = messageSourceId(receive.message);
        measurement.receive = std::move(receive);
        std::filesystem::remove(socketPath);
        return measurement;
    }

    void printMeasurement(const char *label, const Measurement &measurement)
    {
        constexpr double bytesPerMiB = 1024.0 * 1024.0;
        std::cout << std::left << std::setw(18) << label
                  << " messages=" << measurement.receive.receivedMessages
                  << " filtered=" << measurement.receive.filteredMessages
                  << " payload-copied=" << std::fixed << std::setprecision(1)
                  << static_cast<double>(measurement.receive.copiedPayloadBytes) / bytesPerMiB
                  << " MiB drain=" << std::setprecision(2) << measurement.elapsedMs
                  << " ms delivered=" << measurement.deliveredSource << '\n';
    }
}

int main()
{
    const Measurement unfiltered = measure(false);
    const Measurement filtered = measure(true);
    printMeasurement("post-copy filter", unfiltered);
    printMeasurement("pre-copy filter", filtered);

    if (filtered.deliveredSource != kSelectedSource || filtered.receive.filteredMessages == 0 ||
        filtered.receive.copiedPayloadBytes * 2 >= unfiltered.receive.copiedPayloadBytes)
    {
        std::cerr << "multi-source receiver benchmark did not demonstrate the expected reduction\n";
        return 1;
    }
    return 0;
}
