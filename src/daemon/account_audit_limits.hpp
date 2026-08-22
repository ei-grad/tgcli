#pragma once

#include <cstddef>
#include <cstdint>

namespace tgcli::daemon::account_audit_limits {

inline constexpr std::uint64_t kProtocolPreReadBytes = 16'777'216;
inline constexpr std::uint64_t kIoChunkBytes = 65'536;
inline constexpr std::uint64_t kRequestSourceBytes = kProtocolPreReadBytes + kIoChunkBytes - 1;

inline constexpr std::uint64_t kChatTitleBytes = 1'048'576;
inline constexpr std::size_t kChatUsernameCount = 100;
inline constexpr std::size_t kChatUsernameBytes = 32;
inline constexpr std::size_t kSessionCount = 4'096;
inline constexpr std::uint64_t kSessionStringBytes = 1'048'576;
inline constexpr std::size_t kMessageTextScalars = 4'096;
inline constexpr std::uint64_t kMessageTextBytes = 16'384;
inline constexpr std::size_t kForwardItemCount = 100;

inline constexpr std::uint64_t kForwardTerminalBytes = 4'194'304;
inline constexpr std::uint64_t kSingleMessageTerminalBytes = 65'536;
inline constexpr std::uint64_t kOtherTerminalBytes = 32'768;
inline constexpr std::uint64_t kAuditEnvelopeBytes = 4'096;

inline constexpr std::uint64_t kIntentJsonBytes = 134'217'728;
inline constexpr std::uint64_t kNonVectorJsonBytes = 65'536;
inline constexpr std::uint64_t kVectorJsonBytes = kForwardTerminalBytes + kAuditEnvelopeBytes;
inline constexpr std::uint64_t kIntentLineBytes = kIntentJsonBytes + 1;
inline constexpr std::uint64_t kNonVectorLineBytes = kNonVectorJsonBytes + 1;
inline constexpr std::uint64_t kVectorLineBytes = kVectorJsonBytes + 1;

inline constexpr std::size_t kMaximumForwardProgressRecords = 101;
inline constexpr std::uint64_t kMaximumVectorLinesPerGroup =
    std::uint64_t{kMaximumForwardProgressRecords} + std::uint64_t{2};
inline constexpr std::uint64_t kMaximumGroupBytes = kIntentLineBytes +
                                                    std::uint64_t{3} * kNonVectorLineBytes +
                                                    kMaximumVectorLinesPerGroup * kVectorLineBytes;
inline constexpr std::uint64_t kMaximumGroupTailBytes = kMaximumGroupBytes - kIntentLineBytes;
inline constexpr std::uint64_t kRotationBytes = 33'554'432;
inline constexpr std::uint64_t kMaximumNonRotatingSegmentBytes =
    kRotationBytes + kMaximumGroupTailBytes;
inline constexpr std::uint64_t kMaximumSegmentBytes = kMaximumGroupBytes;
inline constexpr std::uint64_t kMaximumAuditBytes = std::uint64_t{5} * kMaximumSegmentBytes;
inline constexpr std::uint64_t kLegacySegmentBytes = 64ULL * 1024 * 1024;

inline constexpr std::uint64_t kMaximumEscapedChatIdentityBytes =
    6 * kChatTitleBytes + kChatUsernameCount * kChatUsernameBytes + 200 + 99 + 2 + 512;
inline constexpr std::uint64_t kMaximumIntentProofBytes =
    7 * kRequestSourceBytes + 2 * kMaximumEscapedChatIdentityBytes;
inline constexpr std::uint64_t kMaximumSessionIntentProofBytes =
    std::uint64_t{5} * 6 * kSessionStringBytes + kRequestSourceBytes;

static_assert(kRequestSourceBytes == 16'842'751);
static_assert(kMaximumEscapedChatIdentityBytes == 6'295'469);
static_assert(kMaximumIntentProofBytes == 130'490'195);
static_assert(kMaximumIntentProofBytes < kIntentJsonBytes);
static_assert(kMaximumSessionIntentProofBytes == 48'300'031);
static_assert(kVectorJsonBytes == 4'198'400);
static_assert(kMaximumVectorLinesPerGroup == 103);
static_assert(kMaximumGroupBytes == 566'849'643);
static_assert(kMaximumGroupTailBytes == 432'631'914);
static_assert(kMaximumNonRotatingSegmentBytes == 466'186'346);
static_assert(kMaximumSegmentBytes == 566'849'643);
static_assert(kMaximumAuditBytes == 2'834'248'215ULL);

} // namespace tgcli::daemon::account_audit_limits
