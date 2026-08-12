#include "ui_config.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    constexpr int kMaxWindowDimension = 16'384; ///< 防止损坏配置生成异常窗口尺寸的上界
    constexpr float kMinControlPanelRatio = 0.1F; ///< 持久化控制区宽度比例下界
    constexpr float kMaxControlPanelRatio = 0.45F; ///< 持久化控制区宽度比例上界
    constexpr auto kConfigPath = ".frame-scope.cfg"; ///< 当前工作目录中的正式配置文件
    constexpr auto kTemporaryConfigPath = ".frame-scope.cfg.tmp"; ///< 替换正式文件前的完整临时配置

    /** @brief 将非有限值和越界配置恢复到应用支持范围 */
    void sanitizeConfig(UiConfig &config)
    {
        config.windowWidth =
                std::clamp(config.windowWidth, kMinWindowWidth, kMaxWindowDimension);
        config.windowHeight =
                std::clamp(config.windowHeight, kMinWindowHeight, kMaxWindowDimension);

        if (!std::isfinite(config.controlRatio))
        {
            config.controlRatio = UiConfig{}.controlRatio;
        }

        if (!std::isfinite(config.uiFontSize))
        {
            config.uiFontSize = UiConfig{}.uiFontSize;
        }

        config.controlRatio =
                std::clamp(config.controlRatio, kMinControlPanelRatio, kMaxControlPanelRatio);
        config.uiFontSize =
                std::clamp(config.uiFontSize, kMinUiFontSize, kMaxUiFontSize);
    }

    /** @brief 严格解析不允许前后残留字符的配置数值 */
    template<typename Value>
    std::optional<Value> parseConfigValue(const std::string_view text)
    {
        Value value{};
        const char *begin = text.data();
        const char *end = begin + text.size();
        const auto [parsedEnd, error] = std::from_chars(begin, end, value);
        if (error != std::errc{} || parsedEnd != end)
        {
            return std::nullopt;
        }

        return value;
    }
}

bool loadConfig(UiConfig &config)
{
    std::ifstream file(kConfigPath);
    if (!file.is_open())
    {
        return false;
    }

    std::string line;
    while (std::getline(file, line))
    {
        const std::string_view lineView(line);
        const auto separator = lineView.find('=');
        if (separator == std::string::npos)
        {
            continue;
        }

        const std::string_view key = lineView.substr(0, separator);
        const std::string_view value = lineView.substr(separator + 1);
        if (key == "window_width")
        {
            if (const auto parsed = parseConfigValue<int>(value))
            {
                config.windowWidth = *parsed;
            }
        }
        else if (key == "window_height")
        {
            if (const auto parsed = parseConfigValue<int>(value))
            {
                config.windowHeight = *parsed;
            }
        }
        else if (key == "control_ratio")
        {
            if (const auto parsed = parseConfigValue<float>(value))
            {
                config.controlRatio = *parsed;
            }
        }
        else if (key == "ui_font_size")
        {
            if (const auto parsed = parseConfigValue<float>(value))
            {
                config.uiFontSize = *parsed;
            }
        }
    }
    sanitizeConfig(config);

    return true;
}

bool saveConfig(const UiConfig &config)
{
    std::ofstream file(kTemporaryConfigPath, std::ios::trunc);
    if (!file.is_open())
    {
        return false;
    }
    file << "window_width=" << config.windowWidth << '\n'
            << "window_height=" << config.windowHeight << '\n'
            << "control_ratio=" << config.controlRatio << '\n'
            << "ui_font_size=" << config.uiFontSize << '\n';
    file.close();
    if (!file)
    {
        return false;
    }

    std::error_code error;
    std::filesystem::rename(kTemporaryConfigPath, kConfigPath, error);
    if (error)
    {
        std::filesystem::remove(kTemporaryConfigPath, error);

        return false;
    }

    return true;
}
