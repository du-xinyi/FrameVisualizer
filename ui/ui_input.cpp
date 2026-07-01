#include "ui_input.h"

#include "ui.h"

#include <algorithm>

namespace
{
    constexpr float kMinZoom = 0.1F; ///< 键盘缩小操作允许达到的最低倍率
    constexpr float kMaxZoom = 8.0F; ///< 键盘放大操作允许达到的最高倍率

    /** @brief 判断模式是否使用 BGR 彩色视图或其分量 */
    constexpr bool isBgrMode(const ViewMode mode)
    {
        return mode == ViewMode::Color || mode == ViewMode::B || mode == ViewMode::G ||
                mode == ViewMode::R;
    }

    /** @brief 判断模式是否使用 HSV 彩色视图或其分量 */
    constexpr bool isHsvMode(const ViewMode mode)
    {
        return mode == ViewMode::Hsv || mode == ViewMode::H || mode == ViewMode::S ||
                mode == ViewMode::V;
    }
}

void handleKey(const int key, AppState &state)
{
    const bool inBgrGroup = isBgrMode(state.mode);
    const bool inHsvGroup = isHsvMode(state.mode);

    switch (key)
    {
        case '1':
            state.mode = ViewMode::Color;
            break;
        case '2':
            state.mode = ViewMode::Gray;
            break;
        case '3':
            state.mode = ViewMode::Hsv;
            break;
        case '4':
            // 相同数字键在当前颜色空间内切换第一显示分量与完整彩色视图
            if (inBgrGroup)
            {
                state.mode = state.mode == ViewMode::R ? ViewMode::Color : ViewMode::R;
            }
            else if (inHsvGroup)
            {
                state.mode = state.mode == ViewMode::H ? ViewMode::Hsv : ViewMode::H;
            }
            break;
        case '5':
            if (inBgrGroup)
            {
                state.mode = state.mode == ViewMode::G ? ViewMode::Color : ViewMode::G;
            }
            else if (inHsvGroup)
            {
                state.mode = state.mode == ViewMode::S ? ViewMode::Hsv : ViewMode::S;
            }
            break;
        case '6':
            if (inBgrGroup)
            {
                state.mode = state.mode == ViewMode::B ? ViewMode::Color : ViewMode::B;
            }
            else if (inHsvGroup)
            {
                state.mode = state.mode == ViewMode::V ? ViewMode::Hsv : ViewMode::V;
            }
            break;
        case 'h':
        case 'H':
            state.histogramMode =
                    state.histogramMode == HistogramMode::Off ? HistogramMode::Linear : HistogramMode::Off;
            state.histogramRefreshRequested = state.histogramMode != HistogramMode::Off;
            break;
        case ' ':
            state.paused = !state.paused;
            break;
        case '+':
        case '=':
            state.fitToWindow = false;
            state.zoom = std::clamp(state.zoom * 1.25F, kMinZoom, kMaxZoom);
            break;
        case '-':
        case '_':
            state.fitToWindow = false;
            state.zoom = std::clamp(state.zoom / 1.25F, kMinZoom, kMaxZoom);
            break;
        case 'f':
        case 'F':
            state.fitToWindow = !state.fitToWindow;
            if (state.fitToWindow)
            {
                state.panX = 0.0F;
                state.panY = 0.0F;
            }
            break;
        case 's':
        case 'S':
            state.saveViewRequested = true;
            break;
        case 27:
        case 'q':
        case 'Q':
            state.quit = true;
            break;
        default:
            break;
    }
}
