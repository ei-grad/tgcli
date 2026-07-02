#pragma once

// Exit-code table from DESIGN.md §5 — a stable contract; changing it is a
// contract-class spec change (REVIEW.md §7).
namespace tgcli {

enum ExitCode : int {
    kOk = 0,
    kGeneric = 1,
    kUsage = 2,
    kNotAuthed = 3,
    kNotFound = 4,
    kRateLimited = 5,
    kDenied = 6,
    kTimeout = 7,
};

} // namespace tgcli
