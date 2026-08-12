#include "frame_transport/source_filter.h"

#include <algorithm>

namespace frame_scope
{
    SourceFilterDecision evaluateSourceFilter(const std::string_view sourceId, const bool enabled,
                                              const bool autoLock, std::string &lockedSourceId,
                                              std::vector<std::string> &detectedSources)
    {
        SourceFilterDecision decision;
        if (!sourceId.empty() && std::ranges::find(detectedSources, sourceId) ==
            detectedSources.end())
        {
            detectedSources.emplace_back(sourceId);
            decision.discovered = true;
        }

        if (enabled && autoLock && lockedSourceId.empty() && !sourceId.empty())
        {
            lockedSourceId = sourceId;
            decision.autoLocked = true;
        }

        decision.accepted = !enabled || lockedSourceId.empty() || sourceId == lockedSourceId;
        return decision;
    }
}
