#include "ui_context_impl.h"
#include "ui_layout.h"

#include <imgui.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    using ui_detail::controlPanelWidth;
    using ui_detail::kMaxZoom;
    using ui_detail::kMinZoom;

    /** @brief 将帧位置按源帧率格式化为播放器时间文本 */
    void formatVideoTime(char *buffer, const std::size_t size, const int64_t frame,
                         const float fps)
    {
        const int64_t seconds = fps > 0.0F ?
            static_cast<int64_t>(static_cast<double>(std::max<int64_t>(0, frame)) / fps) : 0;
        const int64_t hours = seconds / 3600;
        const int64_t minutes = seconds / 60 % 60;
        const int64_t remainingSeconds = seconds % 60;
        if (hours > 0)
        {
            std::snprintf(buffer, size, "%lld:%02lld:%02lld",
                static_cast<long long>(hours), static_cast<long long>(minutes),
                static_cast<long long>(remainingSeconds));
        }
        else
        {
            std::snprintf(buffer, size, "%02lld:%02lld", static_cast<long long>(minutes),
                static_cast<long long>(remainingSeconds));
        }
    }

    /** @brief 将失败、陈旧、暂停和正常状态映射为视口状态栏颜色 */
    ImU32 statusBarColor(const AppState &state)
    {
        if (state.connectionStatus.rfind("failed:", 0) == 0)
        {
            return IM_COL32(86, 30, 30, 245);
        }
        if (state.connectionStatus == "stale")
        {
            return IM_COL32(86, 58, 24, 245);
        }
        if (state.paused || state.connectionStatus == "paused")
        {
            return IM_COL32(80, 64, 28, 245);
        }
        return IM_COL32(20, 23, 28, 245);
    }

    /** @brief 从直方图缓存复制指定 bin 的各通道悬停数据 */
    void setHistogramHoverInfo(AppState &state, const int value)
    {
        if (state.histogramCacheChannelCount <= 0)
        {
            state.histogramHoverValid = false;
            return;
        }

        state.histogramHoverBin = std::clamp(value, 0, state.histogramCacheMaxBin);
        state.histogramHoverChannelCount = 0;
        state.histogramHoverLabels = {};
        state.histogramHoverPixels = {};

        const auto addRow = [&state](const std::string &label, const int pixels)
        {
            if (state.histogramHoverChannelCount >= static_cast<int>(state.histogramHoverLabels.size()))
            {
                return;
            }
            const std::size_t index = static_cast<std::size_t>(state.histogramHoverChannelCount);
            state.histogramHoverLabels[index] = label;
            state.histogramHoverPixels[index] = pixels;
            ++state.histogramHoverChannelCount;
        };

        for (int i = 0; i < state.histogramCacheChannelCount; ++i)
        {
            const std::size_t index = static_cast<std::size_t>(i);
            addRow(
                state.histogramCacheLabels[index],
                state.histogramCachePixels[index][static_cast<std::size_t>(state.histogramHoverBin)]);
        }
        state.histogramHoverValid = state.histogramHoverChannelCount > 0;
    }

    /** @brief 同一源像素在 BGR、HSV 和灰度空间中的采样结果 */
    struct PixelSample
    {
        cv::Vec3b bgr{}; ///< 源像素的蓝、绿、红分量
        cv::Vec3b hsv{}; ///< 源像素转换后的色相、饱和度、明度分量
        int gray = 0; ///< 源像素按 OpenCV BGR 权重计算的灰度值
    };

    /** @brief 将 8 位灰度、BGR 或 BGRA 像素转换为统一采样结果 */
    PixelSample samplePixel(const cv::Mat &frame, const int x, const int y)
    {
        PixelSample sample;
        if (frame.channels() == 1)
        {
            const unsigned char gray = frame.at<unsigned char>(y, x);
            sample.bgr = {gray, gray, gray};
        }
        else if (frame.channels() == 3)
        {
            sample.bgr = frame.at<cv::Vec3b>(y, x);
        }
        else
        {
            const cv::Vec4b bgra = frame.at<cv::Vec4b>(y, x);
            sample.bgr = {bgra[0], bgra[1], bgra[2]};
        }

        cv::Mat bgrView(1, 1, CV_8UC3, sample.bgr.val);
        cv::Mat hsvView(1, 1, CV_8UC3, sample.hsv.val);
        cv::cvtColor(bgrView, hsvView, cv::COLOR_BGR2HSV);
        sample.gray = cvRound(0.114 * sample.bgr[0] + 0.587 * sample.bgr[1] +
            0.299 * sample.bgr[2]);
        return sample;
    }
}

void UiContext::Impl::drawImageWindow(AppState &state, const ImVec2 &displaySize)
{
    const float frameX = controlPanelWidth(state, displaySize);
    const float frameHeight = state.histogramMode != HistogramMode::Off ?
            std::max(360.0F, displaySize.y - 280.0F) :
            displaySize.y;

    // 主视口固定占用控制面板右侧区域，不允许 ImGui 自身滚动或移动
    ImGui::SetNextWindowPos({frameX, 0.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({std::max(320.0F, displaySize.x - frameX), frameHeight},
        ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.075F, 0.082F, 0.095F, 1.0F));
    ImGui::Begin("Frame", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    if (frameTex_.id != 0 && !state.lastFrame.empty())
    {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float statusBarHeight = ImGui::GetTextLineHeightWithSpacing() + 18.0F;
        const float videoControlsHeight = state.videoFilePath.empty() ? 0.0F :
            ImGui::GetFrameHeightWithSpacing() + 12.0F;
        const ImVec2 imageAvailable(available.x,
            std::max(1.0F, available.y - statusBarHeight - videoControlsHeight));
        const ImVec2 imageSize = scaledImageSize(state.lastFrame, imageAvailable, state);
        const ImVec2 cursor = ImGui::GetCursorPos();
        const float offsetX = (imageAvailable.x - imageSize.x) * 0.5F;
        const float offsetY = (imageAvailable.y - imageSize.y) * 0.5F;
        const float panX = state.fitToWindow ? 0.0F : state.panX;
        const float panY = state.fitToWindow ? 0.0F : state.panY;
        ImGui::SetCursorPos({cursor.x + offsetX + panX, cursor.y + offsetY + panY});
        const ImVec2 imageMin = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(frameTex_.id)), imageSize);
        const ImVec2 imageMax(imageMin.x + imageSize.x, imageMin.y + imageSize.y);
        ImGui::GetWindowDrawList()->AddRect(imageMin, imageMax, IM_COL32(95, 105, 122, 220), 0.0F,
            0, 1.5F);
        ImGuiIO &io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;
        const bool imageHovered = mouse.x >= imageMin.x && mouse.x < imageMax.x &&
                mouse.y >= imageMin.y && mouse.y < imageMax.y;
        // 将纹理显示矩形中的屏幕坐标限制并映射回源图像坐标
        const auto mouseToPixel = [&](const ImVec2 &position)
        {
            const float u = std::clamp((position.x - imageMin.x) / imageSize.x, 0.0F, 0.999999F);
            const float v = std::clamp((position.y - imageMin.y) / imageSize.y, 0.0F, 0.999999F);
            return ImVec2(u * static_cast<float>(state.lastFrame.cols),
                v * static_cast<float>(state.lastFrame.rows));
        };

        // Shift 与左键组合优先启动 ROI，避免与图像平移手势冲突
        if (state.roiEnabled && imageHovered && io.KeyShift &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 pixel = mouseToPixel(mouse);
            state.roiStartX = static_cast<int>(pixel.x);
            state.roiStartY = static_cast<int>(pixel.y);
            state.roiX = state.roiStartX;
            state.roiY = state.roiStartY;
            state.roiWidth = 0;
            state.roiHeight = 0;
            state.roiDragging = true;
            state.roiValid = false;
        }
        // ROI 使用包含拖动终点的整数像素范围，并在释放时触发统计刷新
        if (state.roiDragging)
        {
            const ImVec2 pixel = mouseToPixel(mouse);
            const int currentX = std::clamp(static_cast<int>(pixel.x), 0, state.lastFrame.cols - 1);
            const int currentY = std::clamp(static_cast<int>(pixel.y), 0, state.lastFrame.rows - 1);
            state.roiX = std::min(state.roiStartX, currentX);
            state.roiY = std::min(state.roiStartY, currentY);
            state.roiWidth = std::abs(currentX - state.roiStartX) + 1;
            state.roiHeight = std::abs(currentY - state.roiStartY) + 1;
            state.roiValid = state.roiWidth > 1 && state.roiHeight > 1;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                state.roiDragging = false;
                state.roiChanged = true;
            }
        }

        // 将源图像 ROI 按当前显示尺寸投影为屏幕覆盖层
        if (state.roiValid)
        {
            const float x0 = imageMin.x + static_cast<float>(state.roiX) /
                    static_cast<float>(state.lastFrame.cols) *
                    imageSize.x;
            const float y0 = imageMin.y + static_cast<float>(state.roiY) /
                    static_cast<float>(state.lastFrame.rows) *
                    imageSize.y;
            const float x1 = imageMin.x + static_cast<float>(state.roiX + state.roiWidth) /
                    static_cast<float>(state.lastFrame.cols) *
                    imageSize.x;
            const float y1 = imageMin.y + static_cast<float>(state.roiY + state.roiHeight) /
                    static_cast<float>(state.lastFrame.rows) *
                    imageSize.y;
            ImGui::GetWindowDrawList()->AddRectFilled({x0, y0}, {x1, y1},
                IM_COL32(70, 150, 255, 28));
            ImGui::GetWindowDrawList()->AddRect({x0, y0}, {x1, y1}, IM_COL32(90, 175, 255, 255),
                0.0F, 0, 2.0F);
        }

        // 以鼠标下的源像素为锚点切换到手动缩放并修正平移量
        if (imageHovered && io.MouseWheel != 0.0F && io.KeyCtrl)
        {
            const float oldZoom =
                    state.fitToWindow ?
                    imageSize.x / static_cast<float>(std::max(1, state.lastFrame.cols)) :
                    state.zoom;
            const float zoomFactor = io.MouseWheel > 0.0F ? 1.15F : (1.0F / 1.15F);
            const float newZoom = std::clamp(oldZoom * zoomFactor, static_cast<float>(kMinZoom),
                static_cast<float>(kMaxZoom));
            const float actualFactor = oldZoom > 0.0F ? newZoom / oldZoom : 1.0F;
            const ImVec2 local(mouse.x - imageMin.x, mouse.y - imageMin.y);
            const ImVec2 newImageSize(static_cast<float>(state.lastFrame.cols) * newZoom,
                static_cast<float>(state.lastFrame.rows) * newZoom);
            const float newOffsetX = (imageAvailable.x - newImageSize.x) * 0.5F;
            const float newOffsetY = (imageAvailable.y - newImageSize.y) * 0.5F;
            const ImVec2 imageAreaMin(imageMin.x - offsetX - panX, imageMin.y - offsetY - panY);
            const ImVec2 desiredTop(mouse.x - local.x * actualFactor,
                mouse.y - local.y * actualFactor);

            state.fitToWindow = false;
            state.zoom = newZoom;
            state.panX = desiredTop.x - imageAreaMin.x - newOffsetX;
            state.panY = desiredTop.y - imageAreaMin.y - newOffsetY;
        }
        // 仅在手动缩放且未按 Shift 时将左键拖动解释为平移
        if (imageHovered && !io.KeyShift && !state.fitToWindow &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F))
        {
            state.panX += io.MouseDelta.x;
            state.panY += io.MouseDelta.y;
        }

        // 对悬停采样限频，避免每个渲染帧重复执行颜色空间转换
        if (!state.lastFrame.empty() && state.lastFrame.depth() == CV_8U &&
            (state.lastFrame.channels() == 1 || state.lastFrame.channels() == 3 ||
                state.lastFrame.channels() == 4) &&
            imageHovered && !state.paused && state.hoverSampleTimer >= 0.15F)
        {
            state.hoverSampleTimer = 0.0F;
            const float u = (mouse.x - imageMin.x) / imageSize.x;
            const float v = (mouse.y - imageMin.y) / imageSize.y;
            const int x = std::clamp(static_cast<int>(u * static_cast<float>(state.lastFrame.cols)),
                0, state.lastFrame.cols - 1);
            const int y = std::clamp(static_cast<int>(v * static_cast<float>(state.lastFrame.rows)),
                0, state.lastFrame.rows - 1);

            const PixelSample sample = samplePixel(state.lastFrame, x, y);

            state.hoverPixelValid = true;
            state.hoverX = x;
            state.hoverY = y;
            state.hoverB = sample.bgr[0];
            state.hoverG = sample.bgr[1];
            state.hoverR = sample.bgr[2];
            state.hoverH = sample.hsv[0];
            state.hoverS = sample.hsv[1];
            state.hoverV = sample.hsv[2];
            state.hoverGray = sample.gray;
        }

        // 底部状态栏复用主窗口绘制列表，避免占用额外布局高度
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 barMin(windowPos.x,
            windowPos.y + windowSize.y - statusBarHeight - videoControlsHeight);
        const ImVec2 barMax(windowPos.x + windowSize.x,
            windowPos.y + windowSize.y - videoControlsHeight);
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(barMin, barMax, statusBarColor(state));
        drawList->AddLine(barMin, {barMax.x, barMin.y}, IM_COL32(78, 86, 100, 255), 1.0F);

        char statusText[256];
        if (state.paused)
        {
            std::snprintf(statusText, sizeof(statusText), "PAUSED   %s",
                state.hoverPixelValid ? "pixel sample locked" : "frame locked");
        }
        else if (!state.videoFilePath.empty())
        {
            std::snprintf(statusText, sizeof(statusText), "VIDEO   frame %lld / %lld",
                static_cast<long long>(state.videoFramePosition + 1),
                static_cast<long long>(state.videoFrameCount));
        }
        else if (state.connectionStatus.rfind("failed:", 0) == 0 ||
            state.connectionStatus == "stale")
        {
            std::snprintf(statusText, sizeof(statusText), "STATUS %s   Age %.0f ms",
                state.connectionStatus.c_str(), state.frameAgeMs);
        }
        else if (state.hoverPixelValid)
        {
            std::snprintf(statusText, sizeof(statusText),
                "XY %d,%d   BGR %d,%d,%d   HSV %d,%d,%d   Gray %d", state.hoverX,
                state.hoverY, state.hoverB, state.hoverG, state.hoverR, state.hoverH,
                state.hoverS, state.hoverV, state.hoverGray);
        }
        else
        {
            std::snprintf(statusText, sizeof(statusText), "Hover image to inspect pixel values");
        }
        drawList->AddText({barMin.x + 16.0F, barMin.y + 9.0F}, IM_COL32(235, 238, 244, 255),
            statusText);

        if (!state.videoFilePath.empty())
        {
            const float closeSize = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos({windowPos.x + windowSize.x - closeSize - 10.0F,
                windowPos.y + 10.0F});
            if (ImGui::Button("X##close_video", {closeSize, closeSize}))
            {
                state.videoCloseRequested = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Close video and return to ZMQ");
            }

            const ImVec2 controlsMin(windowPos.x, barMax.y);
            drawList->AddRectFilled(controlsMin,
                {windowPos.x + windowSize.x, windowPos.y + windowSize.y},
                IM_COL32(28, 32, 39, 255));
            ImGui::SetCursorScreenPos({controlsMin.x + 12.0F, controlsMin.y + 6.0F});

            const int64_t firstFrame = 0;
            const int64_t lastFrame = std::max<int64_t>(0, state.videoFrameCount - 1);
            int64_t requestedFrame = std::clamp(state.videoFramePosition, firstFrame, lastFrame);
            char currentTime[32];
            char duration[32];
            formatVideoTime(currentTime, sizeof(currentTime), state.videoFramePosition,
                state.sourceFps);
            formatVideoTime(duration, sizeof(duration), lastFrame, state.sourceFps);
            char timeText[72];
            std::snprintf(timeText, sizeof(timeText), "%s / %s", currentTime, duration);

            const float speedWidth = 82.0F;
            const float timeWidth = ImGui::CalcTextSize(timeText).x + 8.0F;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const bool showTime = windowSize.x >= timeWidth + speedWidth + 180.0F;
            const float sliderWidth = std::max(80.0F,
                windowSize.x - speedWidth - (showTime ? timeWidth + spacing : 0.0F) -
                spacing * 2.0F - 24.0F);
            ImGui::SetNextItemWidth(sliderWidth);
            if (ImGui::SliderScalar("##video_seek", ImGuiDataType_S64, &requestedFrame,
                &firstFrame, &lastFrame, ""))
            {
                state.requestedVideoFrame = requestedFrame;
                state.videoSeekRequested = true;
            }
            ImGui::SameLine();
            if (showTime)
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(timeText);
                ImGui::SameLine();
            }

            constexpr const char *speedLabels[] = {"0.25x", "0.5x", "1x", "1.5x", "2x", "4x"};
            constexpr float speeds[] = {0.25F, 0.5F, 1.0F, 1.5F, 2.0F, 4.0F};
            int speedIndex = 2;
            for (int i = 0; i < 6; ++i)
            {
                if (std::abs(state.videoPlaybackSpeed - speeds[i]) < 0.01F)
                {
                    speedIndex = i;
                    break;
                }
            }
            ImGui::SetNextItemWidth(speedWidth);
            if (ImGui::Combo("##video_speed", &speedIndex, speedLabels, 6))
            {
                state.videoPlaybackSpeed = speeds[speedIndex];
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Playback speed");
            }
        }
    }
    else
    {
        ImGui::Text("Waiting for ZMQ image...");
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // 直方图启用时使用主视口下方的独立固定窗口
    if (state.histogramMode != HistogramMode::Off)
    {
        ImGui::SetNextWindowPos({frameX, frameHeight}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({
                std::max(320.0F, displaySize.x - frameX),
                std::max(220.0F, displaySize.y - frameHeight)
            },
            ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16.0F, 12.0F});
        ImGui::Begin("Histogram", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        constexpr float infoWidth = 320.0F;
        ImGui::BeginChild("HistogramInfo", {std::min(infoWidth, available.x * 0.35F), available.y},
            false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted("Histogram");
        ImGui::Separator();
        if (state.histogramHoverValid)
        {
            ImGui::Text("Bin: %d", state.histogramHoverBin);
            ImGui::Separator();
            if (ImGui::BeginTable("HistogramHoverTable", 2, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch, 1.2F);
                ImGui::TableSetupColumn("Pixels", ImGuiTableColumnFlags_WidthStretch, 1.0F);
                ImGui::TableHeadersRow();
                for (int i = 0; i < state.histogramHoverChannelCount; ++i)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(
                        state.histogramHoverLabels[static_cast<std::size_t>(i)].c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", state.histogramHoverPixels[static_cast<std::size_t>(i)]);
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Pixels = count in this bin");
        }
        else
        {
            ImGui::TextDisabled("Hover plot");
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // 保持直方图纹理纵横比并在剩余区域居中
        const ImVec2 imageArea = ImGui::GetContentRegionAvail();
        if (histogramTex_.id == 0)
        {
            ImGui::TextDisabled("Waiting for histogram...");
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        const ImVec2 textureSize(static_cast<float>(histogramTex_.width),
            static_cast<float>(histogramTex_.height));
        const float scale = std::min(imageArea.x / textureSize.x, imageArea.y / textureSize.y);
        const ImVec2 imageSize(textureSize.x * scale, textureSize.y * scale);
        const ImVec2 cursor = ImGui::GetCursorPos();
        ImGui::SetCursorPos({
            cursor.x + std::max(0.0F, (imageArea.x - imageSize.x) * 0.5F),
            cursor.y + std::max(0.0F, (imageArea.y - imageSize.y) * 0.5F)
        });
        const ImVec2 histMin = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(histogramTex_.id)),
            imageSize);
        const ImVec2 histMax(histMin.x + imageSize.x, histMin.y + imageSize.y);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        state.histogramHoverValid = false;
        // 只有绘图区横向范围参与 bin 映射，边距和标签区域不响应悬停
        if (mouse.x >= histMin.x && mouse.x < histMax.x && mouse.y >= histMin.y &&
            mouse.y < histMax.y)
        {
            constexpr float plotLeft = static_cast<float>(HistogramLayout::kLeftMargin);
            constexpr float plotRight =
                    static_cast<float>(HistogramLayout::kLeftMargin + HistogramLayout::kPlotWidth);
            const float textureX =
                    (mouse.x - histMin.x) / imageSize.x * static_cast<float>(histogramTex_.width);
            if (textureX >= plotLeft && textureX <= plotRight)
            {
                const float normalized = (textureX - plotLeft) / (plotRight - plotLeft);
                const int maxValue = state.histogramCacheMaxBin;
                state.histogramHoverBin = std::clamp(
                    static_cast<int>(normalized * static_cast<float>(maxValue)), 0, maxValue);
                setHistogramHoverInfo(state, state.histogramHoverBin);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}
