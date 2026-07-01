#pragma once

/**
 * @brief 可写入配置文件的界面参数集合
 */
struct UiConfig
{
    int windowWidth = 1920; ///< 上次正常窗口状态的宽度
    int windowHeight = 1080; ///< 上次正常窗口状态的高度
    float controlRatio = 0.25F; ///< 控制面板宽度占窗口宽度的比例
    float uiFontSize = 32.0F; ///< ImGui 字体的像素尺寸
};

inline constexpr float kMinUiFontSize = 14.0F; ///< 用户可设置字号的下界
inline constexpr float kMaxUiFontSize = 42.0F; ///< 用户可设置字号的上界
inline constexpr int kMinWindowWidth = 1280; ///< SDL 窗口允许缩放到的最小宽度
inline constexpr int kMinWindowHeight = 760; ///< SDL 窗口允许缩放到的最小高度

/**
 * @brief 读取并校正本地界面配置
 *
 * @param config 接收已识别并校正到有效范围的配置项
 *
 * @return 配置文件成功打开时返回 true，不存在或无法打开时返回 false
 */
bool loadConfig(UiConfig &config);

/**
 * @brief 以临时文件替换方式持久化界面配置
 *
 * @param config 待写入的配置
 *
 * @return 临时文件成功写入并替换目标文件时返回 true
 */
bool saveConfig(const UiConfig &config);
