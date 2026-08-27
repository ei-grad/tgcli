#pragma once

#include "daemon/write_operation.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace tgcli::daemon::write_contract {

class Arguments final {
  public:
    [[nodiscard]] WriteOperation operation() const noexcept;
    [[nodiscard]] const nlohmann::json& value() const noexcept;

  private:
    Arguments(WriteOperation operation, nlohmann::json value);
    WriteOperation operation_;
    nlohmann::json value_;
    friend std::optional<Arguments> make_arguments(WriteOperation operation, nlohmann::json value,
                                                   std::string& error);
};

class Plan final {
  public:
    [[nodiscard]] WriteOperation operation() const noexcept;
    [[nodiscard]] const std::string& account() const noexcept;
    [[nodiscard]] const nlohmann::json& value() const noexcept;

  private:
    Plan(WriteOperation operation, std::string account, nlohmann::json value);
    WriteOperation operation_;
    std::string account_;
    nlohmann::json value_;
    friend std::optional<Plan> make_plan(WriteOperation operation, std::string account,
                                         nlohmann::json value, std::string& error);
};

class Result final {
  public:
    [[nodiscard]] WriteOperation operation() const noexcept;
    [[nodiscard]] const nlohmann::json& value() const noexcept;

  private:
    Result(WriteOperation operation, nlohmann::json value);
    WriteOperation operation_;
    nlohmann::json value_;
    friend std::optional<Result> make_result(WriteOperation operation, nlohmann::json value,
                                             std::string& error);
};

class StoredTerminal final {
  public:
    [[nodiscard]] WriteOperation operation() const noexcept;
    [[nodiscard]] const nlohmann::json& value() const noexcept;
    [[nodiscard]] bool success() const noexcept;

  private:
    StoredTerminal(WriteOperation operation, nlohmann::json value);
    WriteOperation operation_;
    nlohmann::json value_;
    friend std::optional<StoredTerminal>
    make_stored_terminal(WriteOperation operation, nlohmann::json value, std::string& error);
};

std::optional<Arguments> make_arguments(WriteOperation operation, nlohmann::json value,
                                        std::string& error);
std::optional<Plan> make_plan(WriteOperation operation, std::string account, nlohmann::json value,
                              std::string& error);
std::optional<Result> make_result(WriteOperation operation, nlohmann::json value,
                                  std::string& error);
std::optional<StoredTerminal> make_stored_terminal(WriteOperation operation, nlohmann::json value,
                                                   std::string& error);
std::optional<StoredTerminal> make_result_terminal(const Result& result, std::string& error);
std::optional<StoredTerminal> make_error_terminal(WriteOperation operation, std::string code,
                                                  std::string message, nlohmann::json details,
                                                  int exit_code, std::string& error);

bool terminal_matches_plan(const StoredTerminal& terminal, const Plan& plan);

} // namespace tgcli::daemon::write_contract
