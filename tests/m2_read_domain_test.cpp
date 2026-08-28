#include "daemon/m2_read_domain.hpp"

#include <string>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;

TEST_CASE("M2 search cursor is canonical and scope-bound", "[m2-long-read][cursor]") {
    daemon::SearchCursor chat{.account = "primary",
                              .user_id = 10,
                              .limit = 25,
                              .query = "needle",
                              .scope = daemon::SearchScope::Chat,
                              .chat_id = -100,
                              .sender_user_id = 11,
                              .type = daemon::SearchType::Text,
                              .next_offset_message_id = 90,
                              .next_offset = std::nullopt,
                              .last_raw_message_id = 91,
                              .last_raw_order = std::nullopt};
    const auto chat_token = daemon::encode_search_cursor(chat);
    REQUIRE(daemon::decode_search_cursor(chat_token) == chat);
    CHECK_FALSE(daemon::decode_search_cursor(chat_token + "A"));

    chat.next_offset_message_id = 0;
    CHECK_FALSE(daemon::decode_search_cursor(daemon::encode_search_cursor(chat)));
    chat.next_offset_message_id = 91;
    CHECK_FALSE(daemon::decode_search_cursor(daemon::encode_search_cursor(chat)));
    chat.next_offset_message_id = 92;
    CHECK_FALSE(daemon::decode_search_cursor(daemon::encode_search_cursor(chat)));
    chat.next_offset_message_id = 90;

    daemon::SearchCursor global{
        .account = "primary",
        .user_id = 10,
        .limit = 100,
        .query = "needle",
        .scope = daemon::SearchScope::Global,
        .chat_id = std::nullopt,
        .sender_user_id = std::nullopt,
        .type = daemon::SearchType::Photo,
        .next_offset_message_id = std::nullopt,
        .next_offset = "opaque",
        .last_raw_message_id = std::nullopt,
        .last_raw_order = daemon::SearchRawOrder{.date = 100, .chat_id = -100, .message_id = 90}};
    const auto global_token = daemon::encode_search_cursor(global);
    REQUIRE(daemon::decode_search_cursor(global_token) == global);

    chat.next_offset = "cross-scope";
    CHECK_FALSE(daemon::decode_search_cursor(daemon::encode_search_cursor(chat)));
    global.next_offset = "dirty\rmarker";
    CHECK_FALSE(daemon::decode_search_cursor(daemon::encode_search_cursor(global)));
}

TEST_CASE("M2 members cursor keeps type-specific offsets and source count",
          "[m2-long-read][cursor]") {
    daemon::MembersCursor basic{.account = "primary",
                                .user_id = 10,
                                .limit = 50,
                                .chat_id = -20,
                                .chat_type = daemon::MembersChatType::BasicGroup,
                                .source_id = 20,
                                .filter = daemon::MembersFilter::Bots,
                                .query = std::nullopt,
                                .offset = 5,
                                .source_count = 7};
    const auto basic_token = daemon::encode_members_cursor(basic);
    REQUIRE(daemon::decode_members_cursor(basic_token) == basic);

    daemon::MembersCursor supergroup{.account = "primary",
                                     .user_id = 10,
                                     .limit = 200,
                                     .chat_id = -30,
                                     .chat_type = daemon::MembersChatType::Supergroup,
                                     .source_id = 30,
                                     .filter = daemon::MembersFilter::Query,
                                     .query = "CaseSensitive",
                                     .offset = 200,
                                     .source_count = std::nullopt};
    const auto supergroup_token = daemon::encode_members_cursor(supergroup);
    REQUIRE(daemon::decode_members_cursor(supergroup_token) == supergroup);

    supergroup.query = "";
    CHECK_FALSE(daemon::decode_members_cursor(daemon::encode_members_cursor(supergroup)));
    supergroup.query = "q";
    REQUIRE(daemon::decode_members_cursor(daemon::encode_members_cursor(supergroup)) == supergroup);
    supergroup.query = std::string(256, 'q');
    REQUIRE(daemon::decode_members_cursor(daemon::encode_members_cursor(supergroup)) == supergroup);
    supergroup.query = std::string(257, 'q');
    CHECK_FALSE(daemon::decode_members_cursor(daemon::encode_members_cursor(supergroup)));

    basic.source_count.reset();
    CHECK_FALSE(daemon::decode_members_cursor(daemon::encode_members_cursor(basic)));
    supergroup.source_count = 1;
    CHECK_FALSE(daemon::decode_members_cursor(daemon::encode_members_cursor(supergroup)));
    supergroup.source_count.reset();
    supergroup.query = "dirty\rquery";
    CHECK_FALSE(daemon::decode_members_cursor(daemon::encode_members_cursor(supergroup)));
}

TEST_CASE("M2 search inputs require exact pinned TDLib cleaning", "[m2-long-read][domain]") {
    CHECK(daemon::pinned_search_input("query"));
    CHECK(daemon::pinned_search_input("one  two"));
    CHECK_FALSE(daemon::pinned_search_input(""));
    CHECK_FALSE(daemon::pinned_search_input("dirty\rquery"));
    CHECK_FALSE(daemon::pinned_search_input(std::string("dirty\x01query", 11)));
    CHECK_FALSE(daemon::pinned_search_input("dirty\xE2\x80\xA8query"));
    CHECK(daemon::pinned_search_input(std::string(34'996, 'a')));
    CHECK_FALSE(daemon::pinned_search_input(std::string(34'997, 'a')));
    CHECK(daemon::pinned_search_input("\xE2\x80\x8E"));
    CHECK_FALSE(daemon::pinned_search_input("\xE2\x80\x8E\xE2\x80\x8F"));
}

TEST_CASE("M2 search and member filters map without widening", "[m2-long-read][domain]") {
    CHECK(daemon::parse_search_type("any") == daemon::SearchType::Any);
    CHECK(daemon::parse_search_type("doc") == daemon::SearchType::Document);
    CHECK_FALSE(daemon::parse_search_type("document"));
    CHECK(daemon::td_search_filter(daemon::SearchType::Text) == core::TdSearchMessagesFilter::Any);
    CHECK(daemon::search_postfilter(daemon::SearchType::Text, core::TdMessageContentKind::Text));
    CHECK_FALSE(
        daemon::search_postfilter(daemon::SearchType::Text, core::TdMessageContentKind::Other));

    CHECK(daemon::parse_members_filter("admins") == daemon::MembersFilter::Administrators);
    CHECK(daemon::parse_members_filter("search") == daemon::MembersFilter::Query);
    CHECK_FALSE(daemon::parse_members_filter("query"));
    CHECK_FALSE(daemon::parse_members_filter("all"));
    CHECK(daemon::td_members_filter(daemon::MembersFilter::Query) ==
          core::TdSupergroupMembersFilter::Search);
}
