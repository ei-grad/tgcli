#pragma once

#include "proto/operation.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace tgcli::daemon::write_contract {

class Arguments final {
  public:
    [[nodiscard]] proto::M3Operation operation() const noexcept;
    [[nodiscard]] const nlohmann::json& value() const noexcept;

  private:
    Arguments(proto::M3Operation operation, nlohmann::json value);
    proto::M3Operation operation_;
    nlohmann::json value_;
    friend std::optional<Arguments> make_arguments(proto::M3Operation operation,
                                                   nlohmann::json value, std::string& error);
};

class Plan final {
  public:
    [[nodiscard]] proto::M3Operation operation() const noexcept;
    [[nodiscard]] const std::string& account() const noexcept;
    [[nodiscard]] const nlohmann::json& value() const noexcept;

  private:
    Plan(proto::M3Operation operation, std::string account, nlohmann::json value);
    proto::M3Operation operation_;
    std::string account_;
    nlohmann::json value_;
    friend std::optional<Plan> make_plan(proto::M3Operation operation, std::string account,
                                         nlohmann::json value, std::string& error);
};

class Result final {
  public:
    [[nodiscard]] proto::M3Operation operation() const noexcept;
    [[nodiscard]] const nlohmann::json& value() const noexcept;

  private:
    Result(proto::M3Operation operation, nlohmann::json value);
    proto::M3Operation operation_;
    nlohmann::json value_;
    friend std::optional<Result> make_result(proto::M3Operation operation, nlohmann::json value,
                                             std::string& error);
};

class StoredTerminal final {
  public:
    [[nodiscard]] proto::M3Operation operation() const noexcept;
    [[nodiscard]] const nlohmann::json& value() const noexcept;
    [[nodiscard]] bool success() const noexcept;

  private:
    StoredTerminal(proto::M3Operation operation, nlohmann::json value);
    proto::M3Operation operation_;
    nlohmann::json value_;
    friend std::optional<StoredTerminal>
    make_stored_terminal(proto::M3Operation operation, nlohmann::json value, std::string& error);
};

std::optional<Arguments> make_arguments(proto::M3Operation operation, nlohmann::json value,
                                        std::string& error);
std::optional<Plan> make_plan(proto::M3Operation operation, std::string account,
                              nlohmann::json value, std::string& error);
std::optional<Result> make_result(proto::M3Operation operation, nlohmann::json value,
                                  std::string& error);
std::optional<StoredTerminal> make_stored_terminal(proto::M3Operation operation,
                                                   nlohmann::json value, std::string& error);
std::optional<StoredTerminal> make_result_terminal(const Result& result, std::string& error);
std::optional<StoredTerminal> make_error_terminal(proto::M3Operation operation, std::string code,
                                                  std::string message, nlohmann::json details,
                                                  int exit_code, std::string& error);

bool terminal_matches_plan(const StoredTerminal& terminal, const Plan& plan);

} // namespace tgcli::daemon::write_contract
