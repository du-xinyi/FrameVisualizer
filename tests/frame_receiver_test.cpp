#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>
#include <zmq.h>

#include "frame_meta.pb.h"
#include "frame_transport/frame_receiver.h"
#include "frame_transport/source_filter.h"
#include "frame_transport/zmq_raii.h"

namespace
{
    using namespace std::chrono_literals;

    std::string ipcPath(const std::string &suffix)
    {
        return "/tmp/frame-scope-test-" + std::to_string(getpid()) + "-" + suffix + ".sock";
    }

    void sendFrame(frame_scope::zmq::Socket &publisher, const std::string &sourceId,
                   const std::vector<unsigned char> &payload)
    {
        frame_scope::FrameMeta meta;
        meta.set_source_id(sourceId);
        meta.mutable_payload()->set_payload_bytes(payload.size());
        std::string metadata;
        ASSERT_TRUE(meta.SerializeToString(&metadata));

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
}

TEST(SourceFilterTest, AutoLocksFirstSourceAndRejectsOtherSources)
{
    std::string lockedSource;
    std::vector<std::string> detectedSources;

    const auto first = frame_scope::evaluateSourceFilter(
        "camera-0", true, true, lockedSource, detectedSources);
    const auto other = frame_scope::evaluateSourceFilter(
        "camera-1", true, true, lockedSource, detectedSources);

    EXPECT_TRUE(first.accepted);
    EXPECT_TRUE(first.autoLocked);
    EXPECT_EQ(lockedSource, "camera-0");
    EXPECT_FALSE(other.accepted);
    EXPECT_EQ(detectedSources, (std::vector<std::string>{"camera-0", "camera-1"}));
}

TEST(SourceFilterTest, ManualSelectionAcceptsOnlySelectedSourceWithoutDuplicates)
{
    std::string lockedSource = "camera-1";
    std::vector<std::string> detectedSources;

    const auto rejected = frame_scope::evaluateSourceFilter(
        "camera-0", true, false, lockedSource, detectedSources);
    const auto accepted = frame_scope::evaluateSourceFilter(
        "camera-1", true, false, lockedSource, detectedSources);
    frame_scope::evaluateSourceFilter("camera-1", true, false, lockedSource, detectedSources);

    EXPECT_FALSE(rejected.accepted);
    EXPECT_TRUE(accepted.accepted);
    EXPECT_EQ(detectedSources, (std::vector<std::string>{"camera-0", "camera-1"}));
}

TEST(SourceFilterTest, AllCameraModeStillDiscoversSources)
{
    std::string lockedSource;
    std::vector<std::string> detectedSources;

    const auto decision = frame_scope::evaluateSourceFilter(
        "camera-2", false, false, lockedSource, detectedSources);

    EXPECT_TRUE(decision.accepted);
    EXPECT_TRUE(decision.discovered);
    EXPECT_EQ(detectedSources, (std::vector<std::string>{"camera-2"}));
}

TEST(FrameReceiverTest, KeepsLatestSelectedSourceWhenAnotherSourceArrivesLast)
{
    const std::string socketPath = ipcPath("selected-source");
    const std::string endpoint = "ipc://" + socketPath;
    std::filesystem::remove(socketPath);

    frame_scope::zmq::Context publisherContext;
    frame_scope::zmq::Socket publisher(publisherContext, ZMQ_PUB);
    publisher.setIntOption(ZMQ_SNDHWM, 128);
    publisher.bind(endpoint);

    frame_scope::FrameReceiverOptions options;
    options.endpoint = endpoint;
    options.receiveHighWaterMark = 128;
    options.maxMessagesToDrain = 128;
    frame_scope::FrameReceiver receiver(options);
    std::this_thread::sleep_for(200ms);

    const std::vector<unsigned char> payload(256 * 1024, 0x5a);
    for (int i = 0; i < 8; ++i)
    {
        sendFrame(publisher, "selected", payload);
        sendFrame(publisher, "other", payload);
    }
    std::this_thread::sleep_for(50ms);

    const auto result = receiver.receiveLatest(
        [](const frame_scope::FrameMessageHeaderView &header)
        {
            frame_scope::FrameMeta meta;
            return meta.ParseFromArray(header.metadata.data(),
                       static_cast<int>(header.metadata.size())) &&
                   meta.source_id() == "selected";
        });

    EXPECT_EQ(messageSourceId(result.message), "selected");
    EXPECT_GT(result.filteredMessages, 0U);
    EXPECT_GT(result.copiedPayloadBytes, 0U);
    EXPECT_LT(result.copiedPayloadBytes, result.receivedMessages * payload.size());

    std::filesystem::remove(socketPath);
}
