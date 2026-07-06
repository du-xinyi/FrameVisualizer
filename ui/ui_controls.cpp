#include "ui_context_impl.h"
#include "ui_layout.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
    using ui_detail::controlPanelWidth;
    using ui_detail::kMaxControlPanelRatio;
    using ui_detail::kMaxZoom;
    using ui_detail::kMinControlPanelWidth;
    using ui_detail::kMinZoom;

    /** @brief 将连接、陈旧和暂停状态映射为控制面板强调色 */
    ImVec4 statusColor(const AppState &state)
    {
        if (state.connectionStatus.rfind("failed:", 0) == 0)
        {
            return {0.95F, 0.26F, 0.22F, 1.0F};
        }
        if (state.connectionStatus == "stale")
        {
            return {0.95F, 0.62F, 0.18F, 1.0F};
        }
        if (state.paused || state.connectionStatus == "paused")
        {
            return {0.95F, 0.74F, 0.28F, 1.0F};
        }
        if (state.connectionStatus == "receiving" || state.connectionStatus == "playing")
        {
            return {0.36F, 0.82F, 0.45F, 1.0F};
        }
        return {0.70F, 0.74F, 0.80F, 1.0F};
    }

    /** @brief 组合协议像素格式与实际解码后的 OpenCV 类型 */
    std::string imageFormatName(const cv::Mat &frame, const std::string &pixelFormat)
    {
        if (frame.empty())
        {
            return "-";
        }

        const char *depth = "Unknown";
        switch (frame.depth())
        {
            case CV_8U: depth = "8U";
                break;
            case CV_8S: depth = "8S";
                break;
            case CV_16U: depth = "16U";
                break;
            case CV_16S: depth = "16S";
                break;
            case CV_32S: depth = "32S";
                break;
            case CV_32F: depth = "32F";
                break;
            case CV_64F: depth = "64F";
                break;
            default: break;
        }
        const std::string cvType = "CV_" + std::string(depth) + "C" + std::to_string(frame.channels());
        return pixelFormat.empty() ? cvType : pixelFormat + " / " + cvType;
    }
}

void UiContext::Impl::drawControlPanel(AppState &state, const ImVec2 &displaySize)
{
    const float panelWidth = controlPanelWidth(state, displaySize);
    ImGui::SetNextWindowPos({0.0F, 0.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({panelWidth, displaySize.y}, ImGuiCond_Always);
    ImGui::Begin("Controls", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

    // 折叠模式不创建其余控件，避免不可见控件继续参与交互
    if (state.controlsCollapsed)
    {
        if (ImGui::Button(">##expand_controls", {ImGui::GetContentRegionAvail().x, 0.0F}))
        {
            state.controlsCollapsed = false;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Expand settings");
        }
        ImGui::End();
        return;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Settings");
    const float collapseButtonWidth = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - collapseButtonWidth);
    if (ImGui::Button("<##collapse_controls", {collapseButtonWidth, 0.0F}))
    {
        state.controlsCollapsed = true;
        state.resizingControlWidth = false;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Collapse settings");
    }
    ImGui::Separator();

    const float footerHeight =
            ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 2.0F;
    ImGui::BeginChild("ControlBody", {0.0F, -footerHeight}, false);
    const bool videoMode = !state.videoFilePath.empty();

    if (videoMode && ImGui::CollapsingHeader("Video Source", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("VideoSourceSummary", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Status");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(statusColor(state), "%s",
                state.connectionStatus.empty() ? "-" : state.connectionStatus.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("File");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", state.videoFilePath.c_str());
            ImGui::EndTable();
        }
    }

    if (!videoMode && ImGui::CollapsingHeader("Connection", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("ConnectionSummary", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Status");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(statusColor(state), "%s",
                state.connectionStatus.empty() ? "-" : state.connectionStatus.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Endpoint");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", state.endpoint.c_str());
            ImGui::EndTable();
        }

        const bool submitEndpoint = ImGui::InputTextWithHint(
            "##endpoint", "tcp://127.0.0.1:5555", state.endpointInput.data(),
            state.endpointInput.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const float halfButtonWidth =
                (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
        if (ImGui::Button("Apply EP", {halfButtonWidth, 0.0F}) || submitEndpoint)
        {
            state.requestedEndpoint = state.endpointInput.data();
            state.endpointChangeRequested = !state.requestedEndpoint.empty();
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Active", {halfButtonWidth, 0.0F}))
        {
            std::snprintf(state.endpointInput.data(), state.endpointInput.size(), "%s",
                state.endpoint.c_str());
        }

        if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_SpanAvailWidth))
        {
            ImGui::TextWrapped("Topic: %s", state.topic.empty() ? "*" : state.topic.c_str());
            ImGui::TextWrapped("Source: %s", state.sourceId.empty() ? "-" : state.sourceId.c_str());
            ImGui::TextWrapped("Payload: %s",
                state.payloadInfo.empty() ? "-" : state.payloadInfo.c_str());
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Frame Info", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const auto tableItem = [](const char *label, const std::string &value)
        {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", value.c_str());
        };
        if (ImGui::BeginTable("FrameSummary", 4,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupColumn("L1", ImGuiTableColumnFlags_WidthFixed, 110.0F);
            ImGui::TableSetupColumn("V1", ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn("L2", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupColumn("V2", ImGuiTableColumnFlags_WidthStretch, 1.0F);

            char sourceText[32];
            if (state.sourceFps > 0.0F)
            {
                std::snprintf(sourceText, sizeof(sourceText), "%.1f fps", state.sourceFps);
            }
            else
            {
                std::snprintf(sourceText, sizeof(sourceText), "-");
            }
            char renderText[32];
            std::snprintf(renderText, sizeof(renderText), "%.1f fps", state.renderFps);
            char decodeText[32];
            std::snprintf(decodeText, sizeof(decodeText), "%.2f ms", state.decodeTimeMs);

            if (videoMode)
            {
                ImGui::TableNextRow();
                tableItem("Frame", std::to_string(state.videoFramePosition + 1) + " / " +
                    std::to_string(state.videoFrameCount));
                tableItem("Source", sourceText);

                ImGui::TableNextRow();
                char speedText[24];
                std::snprintf(speedText, sizeof(speedText), "%.2gx", state.videoPlaybackSpeed);
                tableItem("Speed", speedText);
                tableItem("Display", renderText);

                ImGui::TableNextRow();
                tableItem("Decode", decodeText);
                tableItem("Image", state.lastFrame.empty() ? "No frame" :
                    std::to_string(state.lastFrame.cols) + " x " +
                    std::to_string(state.lastFrame.rows));

                ImGui::TableNextRow();
                tableItem("Format", imageFormatName(state.lastFrame, state.pixelFormat));
                tableItem("State", state.paused ? "Paused" :
                    (state.connectionStatus == "ended" ? "Ended" : "Playing"));
            }
            else
            {
                ImGui::TableNextRow();
                tableItem("Frames", std::to_string(state.frameCount));
                tableItem("Source", sourceText);

                ImGui::TableNextRow();
                char receiveText[32];
                std::snprintf(receiveText, sizeof(receiveText), "%.1f fps", state.receiveFps);
                tableItem("Receive", receiveText);
                tableItem("Display", renderText);

                ImGui::TableNextRow();
                char ageText[32];
                std::snprintf(ageText, sizeof(ageText), "%.0f ms", state.frameAgeMs);
                tableItem("Age", ageText);
                tableItem("Decode", decodeText);

                ImGui::TableNextRow();
                if (!state.lastFrame.empty())
                {
                    tableItem("Image", std::to_string(state.lastFrame.cols) + " x " +
                        std::to_string(state.lastFrame.rows));
                    tableItem("Format", imageFormatName(state.lastFrame, state.pixelFormat));
                }
                else
                {
                    tableItem("Image", "No frame");
                    tableItem("Format", "-");
                }
            }
            ImGui::EndTable();
        }
        if (!videoMode && ImGui::TreeNodeEx("Timing", ImGuiTreeNodeFlags_SpanAvailWidth))
        {
            ImGui::Text("Source frame: %llu",
                static_cast<unsigned long long>(state.sourceFrameIndex));
            ImGui::Text("Skipped locally: %llu",
                static_cast<unsigned long long>(state.skippedFrames));
            ImGui::Text("Publisher dropped: %llu",
                static_cast<unsigned long long>(state.publisherDroppedFrames));
            ImGui::Text("Decode: %.2f ms", state.decodeTimeMs);
            if (state.endToEndLatencyValid)
            {
                ImGui::Text("End-to-end: %.2f ms", state.endToEndLatencyMs);
            }
            else
            {
                ImGui::TextDisabled("End-to-end: unavailable");
            }
            ImGui::Text("Pipeline: %.2f ms  Encode: %.2f ms", state.pipelineLatencyMs,
                state.encodeLatencyMs);
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Viewport", ImGuiTreeNodeFlags_SpanAvailWidth))
        {
            ImGui::Text("Fit: %s", state.fitToWindow ? "yes" : "no");
            ImGui::Text("Zoom: %.2fx", state.zoom);
            ImGui::Text("Pan: %.0f, %.0f", state.panX, state.panY);
            ImGui::Text("Histogram: %s", state.histogramMode == HistogramMode::Off ? "off" : "on");
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Pointer", ImGuiTreeNodeFlags_SpanAvailWidth))
        {
            if (state.hoverPixelValid)
            {
                ImGui::Text("XY: %d, %d", state.hoverX, state.hoverY);
                ImGui::Text("BGR: %d, %d, %d", state.hoverB, state.hoverG, state.hoverR);
                ImGui::Text("HSV: %d, %d, %d", state.hoverH, state.hoverS, state.hoverV);
                ImGui::Text("Gray: %d", state.hoverGray);
            }
            else
            {
                ImGui::TextDisabled("Hover image");
            }
            ImGui::TreePop();
        }
    }
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    if (ImGui::CollapsingHeader("Image", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Color mode");
        const auto drawModeButton = [&state](const char *label, const ViewMode mode,
            const ViewMode fallbackMode, const ImVec2 size,
            const ImVec4 &normal, const ImVec4 &hovered)
        {
            const bool active = state.mode == mode;
            ImGui::PushStyleColor(ImGuiCol_Button, active ? hovered : normal);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, hovered);
            const bool clicked = ImGui::Button(label, size);
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            ImGui::PopStyleColor(3);
            if (active)
            {
                ImGui::GetWindowDrawList()->AddRect(itemMin, itemMax, IM_COL32(255, 255, 255, 230),
                    ImGui::GetStyle().FrameRounding, 0, 3.0F);
            }
            if (clicked)
            {
                state.mode = active ? fallbackMode : mode;
            }
        };
        const auto drawModeRow =
                [&](const char *groupLabel, const std::array<const char *, 3> &labels,
            const std::array<ViewMode, 3> &modes, const std::array<ImVec4, 3> &normalColors,
            const std::array<ImVec4, 3> &hoverColors)
        {
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float labelWidth = std::clamp(contentWidth * 0.24F, 108.0F, 170.0F);
            const float btnW = (contentWidth - labelWidth - spacing * 3.0F) / 3.0F;
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", groupLabel);
            ImGui::SameLine(labelWidth);
            for (std::size_t i = 0; i < labels.size(); ++i)
            {
                if (i != 0)
                {
                    ImGui::SameLine();
                }
                const ViewMode fallbackMode =
                        groupLabel[0] == 'H' ? ViewMode::Hsv : (groupLabel[0] == 'B' ? ViewMode::Color : modes[i]);
                drawModeButton(labels[i], modes[i], fallbackMode, {btnW, 0.0F}, normalColors[i],
                    hoverColors[i]);
            }
        };
        const bool graySource = !state.lastFrame.empty() && state.lastFrame.channels() == 1;
        if (graySource)
        {
            state.mode = ViewMode::Gray;
        }

        if (graySource)
        {
            const float labelWidth = std::clamp(contentWidth * 0.24F, 108.0F, 170.0F);
            const float grayButtonWidth =
                    contentWidth - labelWidth - ImGui::GetStyle().ItemSpacing.x;
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Gray");
            ImGui::SameLine(labelWidth);
            drawModeButton("Luma", ViewMode::Gray, ViewMode::Gray, {grayButtonWidth, 0.0F},
                ImVec4(0.22F, 0.24F, 0.27F, 1.0F), ImVec4(0.48F, 0.50F, 0.54F, 1.0F));
        }
        else
        {
            drawModeRow("Composite", {"RGB", "Gray", "HSV"},
                {ViewMode::Color, ViewMode::Gray, ViewMode::Hsv},
                {
                    ImVec4(0.14F, 0.28F, 0.45F, 1.0F), ImVec4(0.22F, 0.24F, 0.27F, 1.0F),
                    ImVec4(0.24F, 0.24F, 0.43F, 1.0F)
                },
                {
                    ImVec4(0.22F, 0.52F, 0.86F, 1.0F), ImVec4(0.48F, 0.50F, 0.54F, 1.0F),
                    ImVec4(0.45F, 0.40F, 0.78F, 1.0F)
                });
        }

        const bool bgrSelected = state.mode == ViewMode::Color || state.mode == ViewMode::B ||
                state.mode == ViewMode::G || state.mode == ViewMode::R;
        const bool hsvSelected = state.mode == ViewMode::Hsv || state.mode == ViewMode::H ||
                state.mode == ViewMode::S || state.mode == ViewMode::V;
        if (!graySource && bgrSelected)
        {
            drawModeRow("RGB Ch", {"Red", "Green", "Blue"}, {ViewMode::R, ViewMode::G, ViewMode::B},
                {
                    ImVec4(0.42F, 0.16F, 0.16F, 1.0F), ImVec4(0.12F, 0.32F, 0.22F, 1.0F),
                    ImVec4(0.12F, 0.22F, 0.45F, 1.0F)
                },
                {
                    ImVec4(0.78F, 0.26F, 0.26F, 1.0F), ImVec4(0.18F, 0.58F, 0.32F, 1.0F),
                    ImVec4(0.18F, 0.38F, 0.86F, 1.0F)
                });
        }
        else if (!graySource && hsvSelected)
        {
            drawModeRow("HSV Ch", {"Hue", "Sat", "Value"}, {ViewMode::H, ViewMode::S, ViewMode::V},
                {
                    ImVec4(0.36F, 0.20F, 0.44F, 1.0F), ImVec4(0.36F, 0.30F, 0.14F, 1.0F),
                    ImVec4(0.28F, 0.30F, 0.34F, 1.0F)
                },
                {
                    ImVec4(0.64F, 0.34F, 0.78F, 1.0F), ImVec4(0.70F, 0.52F, 0.20F, 1.0F),
                    ImVec4(0.55F, 0.58F, 0.64F, 1.0F)
                });
        }

        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Scale");
        const float scaleLabelWidth = std::clamp(contentWidth * 0.24F, 108.0F, 170.0F);
        ImGui::SameLine(scaleLabelWidth);
        const float scaleButtonWidth =
                (contentWidth - scaleLabelWidth - ImGui::GetStyle().ItemSpacing.x * 2.0F) / 2.0F;
        const auto drawScaleButton = [](const char *label, const bool active, const ImVec2 size)
        {
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18F, 0.43F, 0.72F, 1.0F));
            }
            const bool clicked = ImGui::Button(label, size);
            if (active)
            {
                ImGui::PopStyleColor();
            }
            return clicked;
        };

        const bool actualSize = !state.fitToWindow && std::abs(state.zoom - 1.0F) < 0.001F;
        if (drawScaleButton("100%", actualSize, {scaleButtonWidth, 0.0F}))
        {
            state.fitToWindow = false;
            state.zoom = 1.0F;
            state.panX = 0.0F;
            state.panY = 0.0F;
        }
        ImGui::SameLine();
        if (drawScaleButton("Fit", state.fitToWindow, {scaleButtonWidth, 0.0F}))
        {
            state.fitToWindow = true;
            state.panX = 0.0F;
            state.panY = 0.0F;
        }

        if (!state.fitToWindow)
        {
            ImGui::SliderFloat("Zoom", &state.zoom, static_cast<float>(kMinZoom),
                static_cast<float>(kMaxZoom), "%.2fx");
        }

        // 本地视频已有完整时间轴，避免同时显示语义不同的实时流历史滑块。
        if (state.paused && state.videoFilePath.empty() && state.historySize > 0)
        {
            const int latestPosition = state.historySize - 1;
            int historyPosition = latestPosition - state.historyOffset;
            ImGui::TextDisabled("History frame: %d / %d", historyPosition + 1, state.historySize);
            constexpr float splitterSafetyWidth = 32.0F; ///< 端点输入框为面板分隔条预留的宽度
            ImGui::SetNextItemWidth(
                std::max(120.0F, ImGui::GetContentRegionAvail().x - splitterSafetyWidth));
            if (ImGui::SliderInt("##history_seek", &historyPosition, 0, latestPosition, "%d"))
            {
                state.historyOffset = latestPosition - historyPosition;
                state.historySeekRequested = true;
            }
            const float historyButtonWidth =
                    (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0F) / 3.0F;
            ImGui::BeginDisabled(state.historyOffset >= state.historySize - 1);
            if (ImGui::Button("Previous", {historyButtonWidth, 0.0F}))
            {
                state.previousFrameRequested = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(state.historyOffset <= 0);
            if (ImGui::Button("Next", {historyButtonWidth, 0.0F}))
            {
                state.nextFrameRequested = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Latest", {historyButtonWidth, 0.0F}))
            {
                state.liveFrameRequested = true;
            }
        }
    }

    if (ImGui::CollapsingHeader("Analysis", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Histogram");
        const float histogramModeWidth =
                (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0F) / 3.0F;
        const auto histogramModeButton =
                [&state, histogramModeWidth](const char *label, const HistogramMode mode)
        {
            const bool active = state.histogramMode == mode;
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18F, 0.43F, 0.72F, 1.0F));
            }
            const bool clicked = ImGui::Button(label, {histogramModeWidth, 0.0F});
            if (active)
            {
                ImGui::PopStyleColor();
            }
            if (clicked && active)
            {
                state.histogramMode = HistogramMode::Off;
                state.analysisSettingsChanged = true;
                state.histogramRefreshRequested = false;
            }
            else if (clicked && !active)
            {
                state.histogramMode = mode;
                state.analysisSettingsChanged = true;
                state.histogramRefreshRequested = true;
            }
        };
        histogramModeButton("Linear", HistogramMode::Linear);
        ImGui::SameLine();
        histogramModeButton("Log", HistogramMode::Log);
        ImGui::SameLine();
        histogramModeButton("Cumulative", HistogramMode::Cumulative);
        ImGui::Spacing();

        if (ImGui::Checkbox("ROI selection", &state.roiEnabled))
        {
            if (!state.roiEnabled)
            {
                state.roiValid = false;
                state.roiDragging = false;
                state.roiChanged = true;
            }
        }
        if (state.roiEnabled)
        {
            ImGui::TextDisabled("Shift + drag on image");
        }
        if (state.roiValid)
        {
            ImGui::Text("ROI: %d,%d  %dx%d", state.roiX, state.roiY, state.roiWidth,
                state.roiHeight);
            ImGui::Text("Mean BGR: %.1f  %.1f  %.1f", state.roiMean[0], state.roiMean[1],
                state.roiMean[2]);
            ImGui::Text("Std BGR: %.1f  %.1f  %.1f", state.roiStdDev[0], state.roiStdDev[1],
                state.roiStdDev[2]);
            ImGui::Text("Gray range: %.0f - %.0f", state.roiMin, state.roiMax);
            if (ImGui::Button("Clear ROI"))
            {
                state.roiValid = false;
                state.roiChanged = true;
            }
        }
        if (ImGui::Checkbox("Exposure warnings", &state.showExposureWarnings))
        {
            state.analysisSettingsChanged = true;
        }
        if (state.showExposureWarnings)
        {
            state.analysisSettingsChanged |=
                    ImGui::SliderInt("Shadows", &state.shadowThreshold, 0, 64);
            state.analysisSettingsChanged |=
                    ImGui::SliderInt("Highlights", &state.highlightThreshold, 191, 255);
        }
    }

    if (!state.saveStatus.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", state.saveStatus.c_str());
    }

    if (ImGui::CollapsingHeader("Help"))
    {
        ImGui::TextDisabled("UI: %.0f px", uiFontSize_);
        if (!state.videoFilePath.empty())
        {
            ImGui::TextWrapped("Video: timeline seek, speed menu, top-right X closes");
            ImGui::TextWrapped("Left/Right: 1 frame; hold: 5 then 10 frames");
            ImGui::TextWrapped("Shift+Left/Right: 5 frames; Space pause/resume");
            ImGui::Separator();
        }
        ImGui::TextWrapped("1 RGB, 2 Gray, 3 HSV");
        ImGui::TextWrapped("4 Red/Hue, 5 Green/Sat, 6 Blue/Value");
        ImGui::TextWrapped("H histogram, F fit%s",
            state.videoFilePath.empty() ? ", Space pause" : "");
        ImGui::TextWrapped("Ctrl+wheel zoom, drag pan, Shift+drag ROI");
        ImGui::TextWrapped("Ctrl+Shift+wheel resize UI, +/- zoom");
        ImGui::TextWrapped("S save, Q/Esc quit");
    }
    ImGui::EndChild();

    ImGui::Separator();
    const float footerActionWidth =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0F) / 3.0F;
    const bool wasPausedForButton = state.paused;
    if (wasPausedForButton)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78F, 0.46F, 0.14F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95F, 0.58F, 0.20F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.95F, 0.65F, 0.28F, 1.0F));
    }
    if (ImGui::Button(state.paused ? "Resume##action" : "Pause##action", {footerActionWidth, 0.0F}))
    {
        state.paused = !state.paused;
    }
    if (wasPausedForButton)
    {
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save", {footerActionWidth, 0.0F}))
    {
        ImGui::OpenPopup("Save Options");
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", {footerActionWidth, 0.0F}))
    {
        state.quit = true;
    }

    if (ImGui::BeginPopupModal("Save Options", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const float popupButtonWidth = 300.0F;
        ImGui::TextUnformatted("Save");
        ImGui::Separator();
        ImGui::TextDisabled("Current View: displayed mode and channel");
        ImGui::BeginDisabled(state.lastViewFrame.empty() && state.lastFrame.empty());
        if (ImGui::Button("Save Current View", {popupButtonWidth, 0.0F}))
        {
            state.saveViewRequested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Raw Frame: original received frame");
        ImGui::BeginDisabled(state.lastFrame.empty());
        if (ImGui::Button("Save Raw Frame", {popupButtonWidth, 0.0F}))
        {
            state.saveRawRequested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Histogram: current histogram image");
        ImGui::BeginDisabled(state.lastHistogramFrame.empty());
        if (ImGui::Button("Save Histogram", {popupButtonWidth, 0.0F}))
        {
            state.saveHistogramRequested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::Button("Cancel", {popupButtonWidth, 0.0F}))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void UiContext::Impl::drawControlSplitter(AppState &state, const ImVec2 &displaySize)
{
    constexpr float handleWidth = 14.0F; ///< 分隔条用于鼠标命中的交互宽度
    if (displaySize.x <= 1.0F || displaySize.y <= 1.0F)
    {
        state.resizingControlWidth = false;
        return;
    }
    if (state.controlsCollapsed)
    {
        state.resizingControlWidth = false;
        const float controlWidth = controlPanelWidth(state, displaySize);
        ImGui::GetForegroundDrawList()->AddRectFilled({controlWidth - 2.0F, 0.0F},
            {controlWidth + 2.0F, displaySize.y},
            IM_COL32(70, 75, 85, 255));
        return;
    }
    const float minPx = std::min(kMinControlPanelWidth, displaySize.x * 0.65F);
    const float maxPx = std::max(minPx, displaySize.x * kMaxControlPanelRatio);

    if (!state.resizingControlWidth)
    {
        state.controlRatio =
                std::clamp(state.controlRatio, minPx / displaySize.x, maxPx / displaySize.x);
    }

    const float controlWidth = std::clamp(displaySize.x * state.controlRatio, minPx, maxPx);

    ImGuiIO &io = ImGui::GetIO();
    const float handleX = controlWidth - handleWidth * 0.5F;
    const bool hovered = io.MousePos.x >= handleX - handleWidth &&
            io.MousePos.x <= handleX + handleWidth && io.MousePos.y >= 0.0F &&
            io.MousePos.y <= displaySize.y;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
        !ImGui::IsAnyItemActive())
    {
        state.resizingControlWidth = true;
    }
    if (state.resizingControlWidth)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const float clampedX = std::clamp(io.MousePos.x, minPx, maxPx);
            state.controlRatio = clampedX / displaySize.x;
        }
        else
        {
            state.resizingControlWidth = false;
        }
    }

    const ImU32 color = state.resizingControlWidth || hovered ?
            IM_COL32(105, 160, 245, 255) :
            IM_COL32(70, 75, 85, 255);
    ImDrawList *drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled({controlWidth - 3.0F, 0.0F}, {controlWidth + 3.0F, displaySize.y},
        color);
}
