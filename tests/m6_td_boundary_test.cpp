#include "core/td_runtime.hpp"
#include "support/scripted_td_runtime.hpp"

#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace {

using tgcli::core::TdFunctionKind;

TEST_CASE("M6 TD function names form the exact accepted boundary", "[m6][td-runtime]") {
    constexpr std::array expected{
        std::pair{TdFunctionKind::SearchContacts, std::string_view{"searchContacts"}},
        std::pair{TdFunctionKind::AddContact, std::string_view{"addContact"}},
        std::pair{TdFunctionKind::RemoveContacts, std::string_view{"removeContacts"}},
        std::pair{TdFunctionKind::SetMessageSenderBlockList,
                  std::string_view{"setMessageSenderBlockList"}},
        std::pair{TdFunctionKind::GetChatFolder, std::string_view{"getChatFolder"}},
        std::pair{TdFunctionKind::CreateChatFolder, std::string_view{"createChatFolder"}},
        std::pair{TdFunctionKind::EditChatFolder, std::string_view{"editChatFolder"}},
        std::pair{TdFunctionKind::DeleteChatFolder, std::string_view{"deleteChatFolder"}},
        std::pair{TdFunctionKind::GetForumTopics, std::string_view{"getForumTopics"}},
        std::pair{TdFunctionKind::GetForumTopic, std::string_view{"getForumTopic"}},
        std::pair{TdFunctionKind::CreateForumTopic, std::string_view{"createForumTopic"}},
        std::pair{TdFunctionKind::EditForumTopic, std::string_view{"editForumTopic"}},
        std::pair{TdFunctionKind::ToggleForumTopicIsClosed,
                  std::string_view{"toggleForumTopicIsClosed"}},
        std::pair{TdFunctionKind::GetChatMember, std::string_view{"getChatMember"}},
        std::pair{TdFunctionKind::SetChatTitle, std::string_view{"setChatTitle"}},
        std::pair{TdFunctionKind::SetChatPhoto, std::string_view{"setChatPhoto"}},
        std::pair{TdFunctionKind::SetChatDescription, std::string_view{"setChatDescription"}},
        std::pair{TdFunctionKind::CreateChatInviteLink, std::string_view{"createChatInviteLink"}},
        std::pair{TdFunctionKind::RevokeChatInviteLink, std::string_view{"revokeChatInviteLink"}},
        std::pair{TdFunctionKind::SetChatMemberStatus, std::string_view{"setChatMemberStatus"}},
        std::pair{TdFunctionKind::SetChatPermissions, std::string_view{"setChatPermissions"}},
        std::pair{TdFunctionKind::GetStorageStatistics, std::string_view{"getStorageStatistics"}},
        std::pair{TdFunctionKind::OptimizeStorage, std::string_view{"optimizeStorage"}},
    };

    for (const auto& [kind, name] : expected) {
        CHECK(tgcli::core::td_function_name(kind) == name);
    }
}

TEST_CASE("scripted M6 factory preserves closed request descriptors", "[m6][td-runtime]") {
    tgcli::test::ScriptedTdRuntime runtime;
    auto value = runtime.make_m6_function(
        tgcli::core::TdM6SearchContactsRequest{.query = "alice", .limit = 100});
    REQUIRE(value.function_data().has_value());
    CHECK(value.function_data()->kind() == TdFunctionKind::SearchContacts);
    REQUIRE(value.function_data()->fields().size() == 2);
    CHECK(value.function_data()->fields()[0] ==
          tgcli::core::TdFunctionField{"query", std::string{"alice"}});
    CHECK(value.function_data()->fields()[1] ==
          tgcli::core::TdFunctionField{"limit", std::int64_t{100}});
}

} // namespace
