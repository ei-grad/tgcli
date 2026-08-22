#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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
    [[nodiscard]] CorrelatedInviteLink retain() const;
    [[nodiscard]] std::shared_ptr<const void> protection() const;
    void release();

  private:
    struct State;
    explicit CorrelatedInviteLink(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_;

    friend class InviteLinkRegistry;
};

class InviteLinkRegistry final {
  public:
    static InviteLinkRegistry& instance();

    [[nodiscard]] CorrelatedInviteLink register_link(std::string invite_link);
    [[nodiscard]] std::string redact(std::string_view value) const;

  private:
    struct Entry {
        std::vector<std::string> aliases;
    };

    void release(std::uint64_t registration);

    mutable std::mutex mutex_;
    std::map<std::uint64_t, Entry> links_;
    std::uint64_t next_registration_ = 1;

    friend class CorrelatedInviteLink;
};

inline constexpr std::string_view kInviteLinkReplacement = "<redacted:invite-link>";

} // namespace tgcli::redaction
