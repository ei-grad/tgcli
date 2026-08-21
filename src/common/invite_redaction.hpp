#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace tgcli::redaction {

class InviteLinkRegistry;

class CorrelatedInviteLink final {
  public:
    CorrelatedInviteLink() = default;
    CorrelatedInviteLink(CorrelatedInviteLink&& other) noexcept;
    CorrelatedInviteLink& operator=(CorrelatedInviteLink&& other) noexcept;
    ~CorrelatedInviteLink();
    CorrelatedInviteLink(const CorrelatedInviteLink&) = delete;
    CorrelatedInviteLink& operator=(const CorrelatedInviteLink&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    void release();

  private:
    CorrelatedInviteLink(InviteLinkRegistry* registry, std::uint64_t registration) noexcept;

    InviteLinkRegistry* registry_ = nullptr;
    std::uint64_t registration_ = 0;

    friend class InviteLinkRegistry;
};

class InviteLinkRegistry final {
  public:
    static InviteLinkRegistry& instance();

    [[nodiscard]] CorrelatedInviteLink register_link(std::string invite_link);
    [[nodiscard]] std::string redact(std::string_view value) const;

  private:
    void release(std::uint64_t registration);

    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::string> links_;
    std::uint64_t next_registration_ = 1;

    friend class CorrelatedInviteLink;
};

inline constexpr std::string_view kInviteLinkReplacement = "<redacted:invite-link>";

} // namespace tgcli::redaction
