#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace frame_scope
{
    /** @brief 一次发布源过滤判断产生的状态变化和接收决策 */
    struct SourceFilterDecision
    {
        bool accepted = true; ///< 当前 sourceId 是否应进入解码队列
        bool discovered = false; ///< sourceId 是否首次加入已发现源列表
        bool autoLocked = false; ///< 是否在本次判断中自动锁定首个有效源
    };

    /**
     * @brief 更新已发现源并判断当前帧是否匹配源锁定条件
     *
     * @param sourceId protobuf 元数据中的发布源标识
     * @param enabled 是否启用源过滤
     * @param autoLock 是否在未指定源时锁定首个有效源
     * @param lockedSourceId 当前锁定源，自动锁定时会被更新
     * @param detectedSources 已发现源列表，新源会追加到末尾
     *
     * @return 当前帧接收决策及状态变化
     */
    SourceFilterDecision evaluateSourceFilter(std::string_view sourceId, bool enabled,
                                              bool autoLock, std::string &lockedSourceId,
                                              std::vector<std::string> &detectedSources);
}
