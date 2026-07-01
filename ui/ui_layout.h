#pragma once

#include "ui.h"

#include <imgui.h>

#include <algorithm>

/**
 * @brief 控制面板和图像视口共享的布局边界
 */
namespace ui_detail
{
    inline constexpr float kMinControlPanelWidth = 500.0F; ///< 常规窗口下控制区的目标最小宽度
    inline constexpr float kMaxControlPanelRatio = 0.45F; ///< 展开控制区可占窗口的最大比例
    inline constexpr float kCollapsedControlPanelWidth = 68.0F; ///< 仅容纳展开按钮的折叠宽度
    inline constexpr float kMinZoom = 0.1F; ///< 手动图像缩放下限
    inline constexpr float kMaxZoom = 8.0F; ///< 手动图像缩放上限

    /**
     * @brief 根据折叠状态和窗口宽度计算控制面板实际宽度
     *
     * @param state 包含折叠状态和用户宽度比例的应用状态
     * @param displaySize 当前 ImGui 显示区域尺寸
     *
     * @return 受最小宽度和最大比例约束的控制面板宽度
     */
    inline float controlPanelWidth(const AppState &state, const ImVec2 &displaySize)
    {
        if (state.controlsCollapsed)
        {
            return std::min(kCollapsedControlPanelWidth, displaySize.x * 0.2F);
        }

        const float minWidth = std::min(kMinControlPanelWidth, displaySize.x * 0.65F);
        const float maxWidth = std::max(minWidth, displaySize.x * kMaxControlPanelRatio);

        return std::clamp(displaySize.x * state.controlRatio, minWidth, maxWidth);
    }
}
