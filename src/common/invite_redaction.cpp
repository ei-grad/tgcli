#include "common/invite_redaction.hpp"

#include "common/secure_wipe.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace tgcli::redaction {

CorrelatedInviteLink::CorrelatedInviteLink(InviteLinkRegistry* registry,
                                           std::uint64_t registration) noexcept
    : registry_(registry), registration_(registration) {}

CorrelatedInviteLink::CorrelatedInviteLink(CorrelatedInviteLink&& other) noexcept
    : registry_(std::exchange(other.registry_, nullptr)),
      registration_(std::exchange(other.registration_, 0)) {}

CorrelatedInviteLink& CorrelatedInviteLink::operator=(CorrelatedInviteLink&& other) noexcept {
    if (this != &other) {
        release();
        registry_ = std::exchange(other.registry_, nullptr);
        registration_ = std::exchange(other.registration_, 0);
    }
    return *this;
}

CorrelatedInviteLink::~CorrelatedInviteLink() {
    release();
}

bool CorrelatedInviteLink::valid() const noexcept {
    return registry_ != nullptr && registration_ != 0;
}

void CorrelatedInviteLink::release() {
    if (!valid()) {
        return;
    }
    registry_->release(registration_);
    registry_ = nullptr;
    registration_ = 0;
}

InviteLinkRegistry& InviteLinkRegistry::instance() {
    static InviteLinkRegistry registry;
    return registry;
}

CorrelatedInviteLink InviteLinkRegistry::register_link(std::string invite_link) {
    if (invite_link.empty()) {
        return {};
    }
    const std::lock_guard lock(mutex_);
    if (next_registration_ == 0 ||
        next_registration_ == std::numeric_limits<std::uint64_t>::max()) {
        return {};
    }
    const auto registration = next_registration_++;
    links_.emplace(registration, std::move(invite_link));
    return {this, registration};
}

std::string InviteLinkRegistry::redact(std::string_view value) const {
    const std::lock_guard lock(mutex_);
    if (links_.empty()) {
        return std::string(value);
    }
    std::vector<const std::string*> ordered;
    ordered.reserve(links_.size());
    for (const auto& [registration, link] : links_) {
        static_cast<void>(registration);
        ordered.push_back(&link);
    }
    std::ranges::sort(ordered, [](const std::string* left, const std::string* right) {
        return left->size() > right->size();
    });

    std::string result;
    result.reserve(value.size());
    std::size_t offset = 0;
    while (offset < value.size()) {
        std::size_t next = std::string_view::npos;
        const std::string* matched = nullptr;
        for (const auto* link : ordered) {
            const auto found = value.find(*link, offset);
            if (found < next ||
                (found == next && matched != nullptr && link->size() > matched->size())) {
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
        offset = next + matched->size();
    }
    return result;
}

void InviteLinkRegistry::release(std::uint64_t registration) {
    const std::lock_guard lock(mutex_);
    const auto found = links_.find(registration);
    if (found == links_.end()) {
        return;
    }
    secure::wipe(found->second);
    links_.erase(found);
}

} // namespace tgcli::redaction
