#pragma once

#include <functional>

namespace tgcli::daemon::testing {

// The server owns config, activity, session-construction and dispatcher-lookup
// stages. The remaining labels let acceptance fixtures place independent
// sentinels at injected dependencies.
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

// Invoked after routed-account and production config checks. Tests use it for
// the remaining hook/auth/path sentinels before activity admission.
using RequestAdmissionProbe = std::function<void()>;

} // namespace tgcli::daemon::testing
