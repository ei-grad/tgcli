#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::proto {

struct RootIdentity {
    std::string path;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t owner = 0;

    friend bool operator==(const RootIdentity&, const RootIdentity&) = default;
};

struct AccountRemovePlanInput {
    std::string account;
    bool keep_session = false;
    std::array<std::string, 2> delete_paths;
    std::string config_path;
    std::string config_snapshot;
    std::optional<RootIdentity> data_root;
    std::optional<RootIdentity> state_root;
    std::optional<std::string> reassign_default;

    friend bool operator==(const AccountRemovePlanInput&, const AccountRemovePlanInput&) = default;
};

class LogoutPlan final {
  public:
    [[nodiscard]] const std::string& account() const;

    friend bool operator==(const LogoutPlan&, const LogoutPlan&) = default;

  private:
    explicit LogoutPlan(std::string account);

    std::string account_;

    friend std::optional<LogoutPlan> make_logout_plan(std::string account, std::string& error);
};

class AccountRemovePlan final {
  public:
    [[nodiscard]] const std::string& account() const;
    [[nodiscard]] bool keep_session() const;
    [[nodiscard]] bool remote_logout() const;
    [[nodiscard]] const std::array<std::string, 2>& delete_paths() const;
    [[nodiscard]] const std::string& config_path() const;
    [[nodiscard]] const std::string& config_snapshot() const;
    [[nodiscard]] const std::optional<RootIdentity>& data_root() const;
    [[nodiscard]] const std::optional<RootIdentity>& state_root() const;
    [[nodiscard]] const std::optional<std::string>& reassign_default() const;

    friend bool operator==(const AccountRemovePlan&, const AccountRemovePlan&) = default;

  private:
    explicit AccountRemovePlan(AccountRemovePlanInput input);

    AccountRemovePlanInput input_;

    friend std::optional<AccountRemovePlan> make_account_remove_plan(AccountRemovePlanInput input,
                                                                     std::string& error);
};

class MsgDeletePlan final {
  public:
    [[nodiscard]] const std::string& account() const;
    [[nodiscard]] const nlohmann::json& chat() const;
    [[nodiscard]] const std::vector<std::int64_t>& message_ids() const;
    [[nodiscard]] bool requested_for_all() const;
    [[nodiscard]] bool effective_for_all() const;

    friend bool operator==(const MsgDeletePlan&, const MsgDeletePlan&) = default;

  private:
    MsgDeletePlan(std::string account, nlohmann::json chat, std::vector<std::int64_t> message_ids,
                  bool requested_for_all, bool effective_for_all);

    std::string account_;
    nlohmann::json chat_;
    std::vector<std::int64_t> message_ids_;
    bool requested_for_all_ = false;
    bool effective_for_all_ = false;

    friend std::optional<MsgDeletePlan> parse_msg_delete_plan(const nlohmann::json& value,
                                                              std::string& error);
};

class ChatLeavePlan final {
  public:
    [[nodiscard]] const std::string& account() const;
    [[nodiscard]] const nlohmann::json& chat() const;

    friend bool operator==(const ChatLeavePlan&, const ChatLeavePlan&) = default;

  private:
    ChatLeavePlan(std::string account, nlohmann::json chat);

    std::string account_;
    nlohmann::json chat_;

    friend std::optional<ChatLeavePlan> parse_chat_leave_plan(const nlohmann::json& value,
                                                              std::string& error);
};

using DestructivePlan = std::variant<LogoutPlan, AccountRemovePlan, MsgDeletePlan, ChatLeavePlan>;

std::optional<LogoutPlan> make_logout_plan(std::string account, std::string& error);
std::optional<AccountRemovePlan> make_account_remove_plan(AccountRemovePlanInput input,
                                                          std::string& error);

std::optional<LogoutPlan> parse_logout_plan(const nlohmann::json& value, std::string& error);
std::optional<AccountRemovePlan> parse_account_remove_plan(const nlohmann::json& value,
                                                           std::string& error);
std::optional<MsgDeletePlan> parse_msg_delete_plan(const nlohmann::json& value, std::string& error);
std::optional<ChatLeavePlan> parse_chat_leave_plan(const nlohmann::json& value, std::string& error);
std::optional<DestructivePlan> parse_destructive_plan(const nlohmann::json& value,
                                                      std::string& error);

nlohmann::json serialize(const LogoutPlan& plan);
nlohmann::json serialize(const AccountRemovePlan& plan);
nlohmann::json serialize(const MsgDeletePlan& plan);
nlohmann::json serialize(const ChatLeavePlan& plan);
nlohmann::json serialize(const DestructivePlan& plan);

bool valid_config_snapshot_identity(std::string_view identity, bool allow_missing = true);

} // namespace tgcli::proto
