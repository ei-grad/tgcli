#pragma once

#include "common/utf8.hpp"
#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tgcli::daemon {

inline constexpr std::int32_t kDefaultSavedSearchLimit = 20;
inline constexpr std::int32_t kMaximumSavedSearchLimit = 100;

struct SavedReactionSelector {
    std::string canonical;
    core::TdReactionType reaction;
};

std::optional<SavedReactionSelector> parse_saved_reaction_selector(std::string_view selector);
inline bool valid_utf8(std::string_view value) {
    return common::valid_utf8(value);
}

struct SavedSearchCursor {
    std::string operation = "saved.search";
    std::string account;
    std::int64_t saved_messages_topic_id = 0;
    std::string tag;
    std::string query;
    std::int32_t limit = kDefaultSavedSearchLimit;
    std::int64_t from_message_id = 0;
    std::int32_t offset = 0;
};

std::string encode_saved_search_cursor(const SavedSearchCursor& cursor);
std::optional<SavedSearchCursor> decode_saved_search_cursor(std::string_view token);

class SavedCoordinator {
  public:
    SavedCoordinator(core::TdClient& client, std::string account)
        : client_(client), account_(std::move(account)) {}

    void tags(const proto::Request& request, RequestSession& session);
    void search(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
};

void register_saved_commands(Dispatcher& dispatcher, SavedCoordinator& coordinator);

} // namespace tgcli::daemon
