#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

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

using DestructivePlan = std::variant<LogoutPlan, AccountRemovePlan>;

std::optional<LogoutPlan> make_logout_plan(std::string account, std::string& error);
std::optional<AccountRemovePlan> make_account_remove_plan(AccountRemovePlanInput input,
                                                          std::string& error);

std::optional<LogoutPlan> parse_logout_plan(const nlohmann::json& value, std::string& error);
std::optional<AccountRemovePlan> parse_account_remove_plan(const nlohmann::json& value,
                                                           std::string& error);
std::optional<DestructivePlan> parse_destructive_plan(const nlohmann::json& value,
                                                      std::string& error);

nlohmann::json serialize(const LogoutPlan& plan);
nlohmann::json serialize(const AccountRemovePlan& plan);
nlohmann::json serialize(const DestructivePlan& plan);

bool valid_config_snapshot_identity(std::string_view identity, bool allow_missing = true);

} // namespace tgcli::proto
