#pragma once

#include <functional>

namespace tgcli::daemon::testing {

// The server currently owns the session-construction and dispatcher-lookup
// stages. The remaining labels let acceptance fixtures place independent
// sentinels at injected request dependencies until those dependencies move
// into the production admission pipeline.
enum class RequestObservationStage {
    ConfigRead,
    HookExecution,
    AuthStateRead,
    PathResolution,
    ActivityAdmission,
    SessionConstruction,
    DispatcherLookup,
};

using RequestObservationObserver = std::function<void(RequestObservationStage)>;

// Invoked immediately after the routed-account comparison and before the
// first current request-specific object. Production leaves it empty while
// config/activity admission is not wired; tests inject the future boundaries.
using RequestAdmissionProbe = std::function<void()>;

} // namespace tgcli::daemon::testing
