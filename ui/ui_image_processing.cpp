#include "ui_image_processing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace
{
    /** @brief 将单通道视图模式映射到对应颜色空间的通道索引 */
    constexpr int channelIndex(const ViewMode mode)
    {
        switch (mode)
        {
            case ViewMode::B:
            case ViewMode::H:
                return 0;
            case ViewMode::G:
            case ViewMode::S:
                return 1;
            case ViewMode::R:
            case ViewMode::V:
                return 2;
            default:
                return -1;
        }
    }

    /** @brief 将受支持图像规范化为纹理上传使用的 CV_8UC3 BGR 数据 */
    cv::Mat ensureBgr(const cv::Mat &frame)
    {
        cv::Mat bgr;
        // 常规通道布局按 OpenCV 默认 BGR 顺序转换
        if (frame.channels() == 1)
        {
            cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
        }
        else if (frame.channels() == 3)
        {
            bgr = frame;
        }
        else if (frame.channels() == 4)
        {
            cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        }
        else
        {
            // 未知多通道布局仅使用第一通道，避免猜测其颜色语义
            cv::Mat firstChannel;
            cv::extractChannel(frame, firstChannel, 0);
            cv::normalize(firstChannel, bgr, 0, 255, cv::NORM_MINMAX, CV_8UC1);
            cv::cvtColor(bgr, bgr, cv::COLOR_GRAY2BGR);
        }

        if (bgr.depth() == CV_8U)
        {
            return bgr;
        }
        // 非 8 位数据按当前值域归一化，确保显示覆盖完整亮度范围
        cv::Mat normalized;
        cv::normalize(bgr, normalized, 0, 255, cv::NORM_MINMAX, CV_8U);
        return normalized;
    }

    /** @brief 将受支持图像转换为用于统计与掩码计算的 8 位灰度图 */
    cv::Mat ensureGray8(const cv::Mat &frame)
    {
        if (frame.depth() == CV_8U)
        {
            if (frame.channels() == 1)
            {
                return frame;
            }
            cv::Mat gray;
            if (frame.channels() == 3)
            {
                cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
                return gray;
            }
            if (frame.channels() == 4)
            {
                cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
                return gray;
            }
        }

        cv::Mat gray;
        cv::cvtColor(ensureBgr(frame), gray, cv::COLOR_BGR2GRAY);
        return gray;
    }

    /** @brief 将单通道数据映射为可直接显示的等值三通道图像 */
    cv::Mat colorizeSingleChannel(const cv::Mat &channel)
    {
        cv::Mat normalized;
        if (channel.depth() == CV_8U)
        {
            normalized = channel;
        }
        else
        {
            cv::normalize(channel, normalized, 0, 255, cv::NORM_MINMAX, CV_8UC1);
        }
        cv::Mat bgr;
        cv::cvtColor(normalized, bgr, cv::COLOR_GRAY2BGR);
        return bgr;
    }

    /**
     * @brief 将浮点直方图保存为悬停查询使用的整数 bin 缓存
     *
     * @param state 接收通道标签和逐 bin 计数的应用状态
     * @param histograms OpenCV 生成的各通道 CV_32FC1 直方图
     * @param labels 与直方图顺序一致的通道名称
     */
    void updateHistogramCache(AppState &state, const std::vector<cv::Mat> &histograms,
        const std::vector<std::string> &labels)
    {
        const std::size_t channelCount = std::min<std::size_t>(
            {histograms.size(), labels.size(), state.histogramCachePixels.size()});
        const int binCount = histograms.empty() ? 0 : histograms.front().rows;
        state.histogramCacheMaxBin = std::clamp(binCount - 1, 0, 255);
        state.histogramCacheChannelCount = static_cast<int>(channelCount);
        state.histogramCacheLabels = {};
        state.histogramCachePixels = {};

        for (int channel = 0; channel < state.histogramCacheChannelCount; ++channel)
        {
            const std::size_t index = static_cast<std::size_t>(channel);
            state.histogramCacheLabels[index] = labels[index];
            if (histograms[index].empty() || histograms[index].type() != CV_32FC1)
            {
                continue;
            }
            const int bins = std::min(histograms[index].rows,
                static_cast<int>(state.histogramCachePixels[index].size()));
            for (int bin = 0; bin < bins; ++bin)
            {
                const double pixels =
                        std::clamp(static_cast<double>(histograms[index].at<float>(bin)), 0.0,
                            static_cast<double>(std::numeric_limits<int>::max()));
                state.histogramCachePixels[index][static_cast<std::size_t>(bin)] =
                        static_cast<int>(std::lround(pixels));
            }
        }
    }
}

namespace ui_detail
{
    cv::Mat makeDisplayImage(const cv::Mat &frame, const ViewMode mode)
    {
        switch (mode)
        {
            case ViewMode::Color:
            case ViewMode::Hsv:
                return ensureBgr(frame);
            case ViewMode::Gray:
                return colorizeSingleChannel(ensureGray8(frame));
            // BGR 分量视图保持通道原始数值并复制到三通道显示
            case ViewMode::B:
            case ViewMode::G:
            case ViewMode::R:
            {
                cv::Mat channel;
                cv::extractChannel(ensureBgr(frame), channel, channelIndex(mode));
                return colorizeSingleChannel(channel);
            }
            // HSV 分量需先转换颜色空间再提取目标通道
            case ViewMode::H:
            case ViewMode::S:
            case ViewMode::V:
            {
                cv::Mat hsv;
                cv::cvtColor(ensureBgr(frame), hsv, cv::COLOR_BGR2HSV);
                cv::Mat channel;
                cv::extractChannel(hsv, channel, channelIndex(mode));
                return colorizeSingleChannel(channel);
            }
        }
        return ensureBgr(frame);
    }

    void applyExposureWarnings(cv::Mat &display, const cv::Mat &source, const int shadowThreshold,
        const int highlightThreshold)
    {
        const cv::Mat gray = ensureGray8(source);
        cv::Mat shadowMask;
        cv::Mat highlightMask;
        cv::compare(gray, shadowThreshold, shadowMask, cv::CMP_LE);
        cv::compare(gray, highlightThreshold, highlightMask, cv::CMP_GE);

        display = ensureBgr(display);
        if (display.data == source.data)
        {
            display = display.clone();
        }
        display.setTo(cv::Scalar(255, 70, 30), shadowMask);
        display.setTo(cv::Scalar(40, 50, 255), highlightMask);
    }

    cv::Mat makeHistogram(const cv::Mat &frame, const ViewMode mode, AppState *state)
    {
        cv::Mat bgrCache;
        const auto bgr = [&]() -> const cv::Mat &
        {
            if (bgrCache.empty())
            {
                bgrCache = ensureBgr(frame);
            }
            return bgrCache;
        };

        std::vector<cv::Mat> channels;
        std::vector<cv::Scalar> colors;
        std::vector<std::string> labels;
        channels.reserve(3);
        colors.reserve(3);
        labels.reserve(3);
        std::string title;
        float rangeMax = 256.0F;

        const auto addChannel = [&](const cv::Mat &channel, const cv::Scalar &color,
            const std::string &label)
        {
            channels.push_back(channel);
            colors.push_back(color);
            labels.push_back(label);
        };

        switch (mode)
        {
            case ViewMode::Color:
            {
                std::vector<cv::Mat> splitChannels;
                cv::split(bgr(), splitChannels);
                addChannel(splitChannels[2], {105, 135, 255}, "R");
                addChannel(splitChannels[1], {95, 220, 120}, "G");
                addChannel(splitChannels[0], {245, 105, 105}, "B");
                title = "RGB Histogram";
                break;
            }
            case ViewMode::Gray:
                addChannel(ensureGray8(frame), {205, 210, 220}, "Gray");
                title = "Gray Histogram";
                break;
            case ViewMode::B:
            case ViewMode::G:
            case ViewMode::R:
            {
                static const std::array<cv::Scalar, 3> kColors = {
                    cv::Scalar(245, 105, 105), cv::Scalar(95, 220, 120), cv::Scalar(105, 135, 255)
                };
                static const std::array<std::string, 3> kLabels = {"Blue", "Green", "Red"};
                const int index = channelIndex(mode);
                cv::Mat channel;
                cv::extractChannel(bgr(), channel, index);
                addChannel(channel, kColors[static_cast<std::size_t>(index)],
                    kLabels[static_cast<std::size_t>(index)]);
                title = kLabels[static_cast<std::size_t>(index)] + " Channel Histogram";
                break;
            }
            case ViewMode::Hsv:
            {
                cv::Mat hsv;
                cv::cvtColor(bgr(), hsv, cv::COLOR_BGR2HSV);
                std::vector<cv::Mat> splitChannels;
                cv::split(hsv, splitChannels);
                addChannel(splitChannels[0], {180, 100, 225}, "H");
                addChannel(splitChannels[1], {95, 205, 230}, "S");
                addChannel(splitChannels[2], {210, 215, 225}, "V");
                title = "HSV Histogram";
                break;
            }
            case ViewMode::H:
            case ViewMode::S:
            case ViewMode::V:
            {
                static const std::array<cv::Scalar, 3> kColors = {
                    cv::Scalar(180, 100, 225), cv::Scalar(95, 205, 230), cv::Scalar(210, 215, 225)
                };
                static const std::array<std::string, 3> kLabels = {"Hue", "Saturation", "Value"};
                cv::Mat hsv;
                cv::cvtColor(bgr(), hsv, cv::COLOR_BGR2HSV);
                const int index = channelIndex(mode);
                cv::Mat channel;
                cv::extractChannel(hsv, channel, index);
                addChannel(channel, kColors[static_cast<std::size_t>(index)],
                    kLabels[static_cast<std::size_t>(index)]);
                title = kLabels[static_cast<std::size_t>(index)] + " Histogram";
                rangeMax = mode == ViewMode::H ? 180.0F : 256.0F;
                break;
            }
        }

        const int histogramSize = mode == ViewMode::H ? 180 : 256;
        constexpr int width = HistogramLayout::kWidth;
        constexpr int height = HistogramLayout::kHeight;
        constexpr int left = HistogramLayout::kLeftMargin;
        constexpr int top = HistogramLayout::kTopMargin;
        constexpr int plotWidth = HistogramLayout::kPlotWidth;
        constexpr int plotHeight = HistogramLayout::kPlotHeight;
        const float range[] = {0.0F, rangeMax};
        const float *histogramRange[] = {range};
        std::vector<cv::Mat> histograms(channels.size());
        for (std::size_t i = 0; i < channels.size(); ++i)
        {
            cv::calcHist(&channels[i], 1, nullptr, cv::Mat(), histograms[i], 1, &histogramSize,
                histogramRange);
        }
        if (state != nullptr)
        {
            updateHistogramCache(*state, histograms, labels);
        }
        for (cv::Mat &histogram: histograms)
        {
            if (state != nullptr && state->histogramMode == HistogramMode::Cumulative)
            {
                for (int bin = 1; bin < histogramSize; ++bin)
                {
                    histogram.at<float>(bin) += histogram.at<float>(bin - 1);
                }
            }
            if (state != nullptr && state->histogramMode == HistogramMode::Log)
            {
                cv::log(histogram + 1.0F, histogram);
            }
            cv::normalize(histogram, histogram, 0, plotHeight, cv::NORM_MINMAX);
        }

        if (state != nullptr && state->histogramMode == HistogramMode::Log)
        {
            title += " (Log)";
        }
        if (state != nullptr && state->histogramMode == HistogramMode::Cumulative)
        {
            title += " (Cumulative)";
        }

        cv::Mat image(height, width, CV_8UC3, cv::Scalar(18, 20, 24));
        const cv::Point plotTopLeft(left, top);
        const cv::Point plotBottomRight(left + plotWidth, top + plotHeight);
        cv::rectangle(image, plotTopLeft, plotBottomRight, {28, 31, 37}, cv::FILLED);
        for (int i = 0; i <= 4; ++i)
        {
            const int x = left + cvRound(plotWidth * (static_cast<double>(i) / 4.0));
            cv::line(image, {x, top}, {x, top + plotHeight}, {50, 55, 65}, 1, cv::LINE_AA);
            const int value = cvRound((rangeMax - 1.0F) * (static_cast<float>(i) / 4.0F));
            cv::putText(image, std::to_string(value), {x - 12, height - 14},
                cv::FONT_HERSHEY_SIMPLEX, 0.48, {165, 170, 180}, 1, cv::LINE_AA);
        }
        for (int i = 0; i <= 4; ++i)
        {
            const int y = top + cvRound(plotHeight * (static_cast<double>(i) / 4.0));
            cv::line(image, {left, y}, {left + plotWidth, y}, {45, 50, 60}, 1, cv::LINE_AA);
        }
        cv::rectangle(image, plotTopLeft, plotBottomRight, {84, 92, 108}, 1, cv::LINE_AA);

        for (int bin = 1; bin < histogramSize; ++bin)
        {
            const int previousX =
                    left + cvRound(plotWidth * (static_cast<double>(bin - 1) / (histogramSize - 1)));
            const int currentX =
                    left + cvRound(plotWidth * (static_cast<double>(bin) / (histogramSize - 1)));
            for (std::size_t channel = 0; channel < histograms.size(); ++channel)
            {
                const int current = cvRound(histograms[channel].at<float>(bin));
                const int previous = cvRound(histograms[channel].at<float>(bin - 1));
                cv::line(image, {previousX, top + plotHeight - previous},
                    {currentX, top + plotHeight - current}, colors[channel], 2);
            }
        }

        cv::putText(image, title, {left, 26}, cv::FONT_HERSHEY_SIMPLEX, 0.68, {230, 234, 240}, 1,
            cv::LINE_AA);
        cv::putText(image, "Intensity", {width - 112, height - 14}, cv::FONT_HERSHEY_SIMPLEX, 0.48,
            {165, 170, 180}, 1, cv::LINE_AA);
        cv::putText(image, "Pixels", {10, top - 12}, cv::FONT_HERSHEY_SIMPLEX, 0.48,
            {165, 170, 180}, 1, cv::LINE_AA);

        const int legendX = width - 170;
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            const int y = 18 + static_cast<int>(i) * 22;
            cv::rectangle(image, {legendX, y - 10}, {legendX + 18, y + 4}, colors[i], cv::FILLED);
            cv::putText(image, labels[i], {legendX + 28, y + 5}, cv::FONT_HERSHEY_SIMPLEX, 0.52,
                {225, 228, 235}, 1, cv::LINE_AA);
        }
        return image;
    }
}

cv::Rect selectedRoi(const cv::Mat &frame, const AppState &state)
{
    if (!state.roiValid || frame.empty())
    {
        return {0, 0, frame.cols, frame.rows};
    }
    return cv::Rect(state.roiX, state.roiY, state.roiWidth, state.roiHeight) &
            cv::Rect(0, 0, frame.cols, frame.rows);
}

void updateRoiStatistics(AppState &state)
{
    if (!state.roiValid || state.lastFrame.empty())
    {
        state.roiMean = {};
        state.roiStdDev = {};
        state.roiMin = 0.0;
        state.roiMax = 0.0;
        return;
    }

    const cv::Rect roi = selectedRoi(state.lastFrame, state);
    if (roi.empty())
    {
        state.roiValid = false;
        return;
    }
    const cv::Mat source = state.lastFrame(roi);
    cv::Scalar mean;
    cv::Scalar stdDev;
    cv::Mat gray;
    // 灰度 ROI 将单通道统计复制到三项，彩色 ROI 保留前三个通道统计
    if (source.depth() == CV_8U && source.channels() == 1)
    {
        cv::meanStdDev(source, mean, stdDev);
        state.roiMean.fill(mean[0]);
        state.roiStdDev.fill(stdDev[0]);
        gray = source;
    }
    else if (source.depth() == CV_8U && (source.channels() == 3 || source.channels() == 4))
    {
        cv::meanStdDev(source, mean, stdDev);
        for (std::size_t i = 0; i < state.roiMean.size(); ++i)
        {
            state.roiMean[i] = mean[static_cast<int>(i)];
            state.roiStdDev[i] = stdDev[static_cast<int>(i)];
        }
        gray = ensureGray8(source);
    }
    else
    {
        const cv::Mat bgr = ensureBgr(source);
        cv::meanStdDev(bgr, mean, stdDev);
        for (std::size_t i = 0; i < state.roiMean.size(); ++i)
        {
            state.roiMean[i] = mean[static_cast<int>(i)];
            state.roiStdDev[i] = stdDev[static_cast<int>(i)];
        }
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    }
    cv::minMaxLoc(gray, &state.roiMin, &state.roiMax);
}
