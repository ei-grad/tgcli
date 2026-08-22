#include "common/invite_redaction.hpp"

#include "common/invite_link.hpp"
#include "common/secure_wipe.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace tgcli::redaction {

struct CorrelatedInviteLink::State {
    State(InviteLinkRegistry* registry_value, std::uint64_t registration_value)
        : registry(registry_value), registration(registration_value) {}

    ~State() {
        registry->release(registration);
    }

    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    InviteLinkRegistry* registry;
    std::uint64_t registration;
};

CorrelatedInviteLink::CorrelatedInviteLink(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

CorrelatedInviteLink::CorrelatedInviteLink(CorrelatedInviteLink&& other) noexcept
    : state_(std::move(other.state_)) {}

CorrelatedInviteLink& CorrelatedInviteLink::operator=(CorrelatedInviteLink&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
    }
    return *this;
}

CorrelatedInviteLink::~CorrelatedInviteLink() {
    release();
}

bool CorrelatedInviteLink::valid() const noexcept {
    return state_ != nullptr;
}

CorrelatedInviteLink CorrelatedInviteLink::retain() const {
    return CorrelatedInviteLink(state_);
}

std::shared_ptr<const void> CorrelatedInviteLink::protection() const {
    return state_;
}

void CorrelatedInviteLink::release() {
    state_.reset();
}

InviteLinkRegistry& InviteLinkRegistry::instance() {
    static InviteLinkRegistry registry;
    return registry;
}

CorrelatedInviteLink InviteLinkRegistry::register_link(std::string_view invite_link,
                                                       const secure::WipeObserver& wipe_observer) {
    if (invite_link.empty()) {
        return {};
    }
    const std::lock_guard lock(mutex_);
    if (next_registration_ == 0 ||
        next_registration_ == std::numeric_limits<std::uint64_t>::max()) {
        return {};
    }
    const auto registration = next_registration_++;
    auto aliases = common::exact_telegram_invite_aliases(invite_link, wipe_observer);
    if (aliases.empty()) {
        aliases.emplace_back(invite_link, wipe_observer, "invite_alias");
    }
    links_.emplace(registration, Entry{.aliases = std::move(aliases)});
    return CorrelatedInviteLink(std::make_shared<CorrelatedInviteLink::State>(this, registration));
}

std::string InviteLinkRegistry::redact(std::string_view value) const {
    const std::lock_guard lock(mutex_);
    if (links_.empty()) {
        return std::string(value);
    }
    std::vector<const secure::SensitiveString*> ordered;
    ordered.reserve(links_.size());
    for (const auto& [registration, entry] : links_) {
        static_cast<void>(registration);
        for (const auto& alias : entry.aliases) {
            ordered.push_back(&alias);
        }
    }
    std::ranges::sort(ordered, [](const auto* left, const auto* right) {
        return left->view().size() > right->view().size();
    });

    std::string result;
    result.reserve(value.size());
    std::size_t offset = 0;
    while (offset < value.size()) {
        std::size_t next = std::string_view::npos;
        const secure::SensitiveString* matched = nullptr;
        for (const auto* link : ordered) {
            const auto found = value.find(link->view(), offset);
            if (found < next || (found == next && matched != nullptr &&
                                 link->view().size() > matched->view().size())) {
                next = found;
                matched = link;
            }
        }
        if (matched == nullptr || next == std::string_view::npos) {
            result.append(value.substr(offset));
            break;
        }
        result.append(value.substr(offset, next - offset));
        result.append(kInviteLinkReplacement);
        offset = next + matched->view().size();
    }
    return result;
}

void InviteLinkRegistry::release(std::uint64_t registration) {
    const std::lock_guard lock(mutex_);
    const auto found = links_.find(registration);
    if (found == links_.end()) {
        return;
    }
    links_.erase(found);
}

} // namespace tgcli::redaction
