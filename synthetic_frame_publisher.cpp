#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <opencv2/imgproc.hpp>

#include "frame_transport/frame_sender.h"

namespace
{
    constexpr int kWidth = 1280; ///< 模拟源输出图像的固定宽度
    constexpr int kHeight = 720; ///< 模拟源输出图像的固定高度
    constexpr double kFps = 30.0; ///< 运动计算和发送节拍采用的目标帧率
    constexpr const char *kDefaultEndpoint = "tcp://*:5555"; ///< 未指定参数时绑定的发布端点
    constexpr const char *kTopic = "sim.camera.0"; ///< 多部分消息首段携带的订阅主题

    /**
     * @brief 解析模拟发布器绑定端点
     *
     * @param argc 命令行参数数量
     * @param argv 可选首个参数提供自定义端点
     *
     * @return 用户指定端点或默认通配地址
     */
    std::string parseEndpoint(const int argc, char **argv)
    {
        if (argc >= 2)
        {
            return argv[1];
        }

        return kDefaultEndpoint;
    }

    /**
     * @brief 生成可观察颜色、几何和时间连续性的模拟画面
     *
     * @param frameIndex 驱动运动图形和文本标签的帧序号
     *
     * @return 固定尺寸的 CV_8UC3 BGR 图像
     */
    cv::Mat makeFrame(const uint64_t frameIndex)
    {
        cv::Mat frame(kHeight, kWidth, CV_8UC3, cv::Scalar(24, 26, 32));

        // 横纵方向使用不同颜色变化，便于发现方向翻转和通道顺序错误
        for (int y = 0; y < kHeight; ++y)
        {
            const double fy = static_cast<double>(y) / static_cast<double>(kHeight);

            for (int x = 0; x < kWidth; ++x)
            {
                const double fx = static_cast<double>(x) / static_cast<double>(kWidth);
                auto &pixel = frame.at<cv::Vec3b>(y, x);

                pixel[0] = static_cast<unsigned char>(40.0 + 95.0 * fx);
                pixel[1] = static_cast<unsigned char>(35.0 + 80.0 * fy);
                pixel[2] = static_cast<unsigned char>(70.0 + 90.0 * (1.0 - fx));
            }
        }

        // 等距网格为画面缩放比例和边缘裁剪提供静态参照
        for (int x = 0; x < kWidth; x += 80)
        {
            cv::line(frame, {x, 0}, {x, kHeight}, cv::Scalar(65, 70, 78), 1, cv::LINE_AA);
        }

        for (int y = 0; y < kHeight; y += 80)
        {
            cv::line(frame, {0, y}, {kWidth, y}, cv::Scalar(65, 70, 78), 1, cv::LINE_AA);
        }

        // 圆心水平和垂直周期不同，使重复帧与乱序更容易被肉眼识别
        const double t = static_cast<double>(frameIndex) / kFps;
        const int circleX = static_cast<int>((std::sin(t * 1.7) * 0.42 + 0.5) * static_cast<double>(kWidth));
        const int circleY = static_cast<int>((std::cos(t * 1.2) * 0.35 + 0.5) * static_cast<double>(kHeight));

        cv::circle(frame, {circleX, circleY}, 72, cv::Scalar(40, 210, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(frame, {circleX, circleY}, 76, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

        // 水平条包含画面外区间，以连续方式从左侧再次出现
        const double barOffset = std::fmod(static_cast<double>(frameIndex) * 9.0,
                                           static_cast<double>(kWidth + 240));
        const int barX = static_cast<int>(barOffset) - 240;

        cv::rectangle(frame, cv::Rect(barX, 520, 240, 90), cv::Scalar(240, 80, 80), cv::FILLED);
        cv::rectangle(frame, cv::Rect(barX, 520, 240, 90), cv::Scalar(255, 255, 255), 2);

        std::ostringstream label;
        label << "Simulated ZMQ Stream  " << kWidth << "x" << kHeight
              << "  frame=" << frameIndex << "  fps=" << kFps;

        cv::putText(frame,
                    label.str(),
                    {36, 58},
                    cv::FONT_HERSHEY_SIMPLEX,
                    1.0,
                    cv::Scalar(245, 245, 245),
                    2,
                    cv::LINE_AA);

        cv::putText(frame,
                    "BGR/JPEG + FrameMeta protobuf + payload",
                    {36, 104},
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.82,
                    cv::Scalar(210, 225, 255),
                    2,
                    cv::LINE_AA);

        return frame;
    }

}

int main(const int argc, char **argv)
{
    const std::string endpoint = parseEndpoint(argc, argv);

    frame_scope::FrameSenderOptions options;
    options.endpoint = endpoint;
    options.topic = kTopic;
    options.sourceId = "simulated-camera-0";
    options.sessionId = "local-simulation";
    options.encoding = frame_scope::FrameEncoding::Jpeg;
    options.jpegQuality = 90;
    frame_scope::FrameSender sender(std::move(options));

    std::cout << "Publishing simulated stream on " << endpoint << '\n'
              << "Topic: " << kTopic << std::endl;

    // 为 PUB/SUB 订阅握手预留时间，减少启动瞬间不可避免的首帧丢失
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    uint64_t frameIndex = 0;
    const auto frameInterval = std::chrono::microseconds(static_cast<int>(1'000'000.0 / kFps));
    auto nextFrameAt = std::chrono::steady_clock::now();

    for (;;)
    {
        const cv::Mat frame = makeFrame(frameIndex);
        frame_scope::FrameInfo info;
        info.frameIndex = frameIndex;
        info.sourceFps = kFps;
        const frame_scope::FrameSendResult result = sender.publish(frame, info);

        if (frameIndex % 30 == 0)
        {
            std::cout << "Published frame " << result.frameIndex
                      << ", payload bytes=" << result.payloadBytes << std::endl;
        }

        ++frameIndex;
        nextFrameAt += frameInterval;

        std::this_thread::sleep_until(nextFrameAt);
    }
}
