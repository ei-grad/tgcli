#pragma once

#include <functional>
#include <optional>
#include <termios.h>

#include <nlohmann/json.hpp>

namespace tgcli::cli {

enum class PromptResultKind { Answer, Cancelled, Unavailable, Error };

struct PromptResult {
    PromptResultKind kind;
    nlohmann::json answer = nlohmann::json::object();
};

class ChallengePrompt {
  public:
    ChallengePrompt() = default;
    ChallengePrompt(const ChallengePrompt&) = delete;
    ChallengePrompt& operator=(const ChallengePrompt&) = delete;
    ChallengePrompt(ChallengePrompt&&) = delete;
    ChallengePrompt& operator=(ChallengePrompt&&) = delete;
    virtual ~ChallengePrompt() = default;

    virtual PromptResult prompt(const nlohmann::json& challenge) = 0;
};

class TerminalPrompt final : public ChallengePrompt {
  public:
    using TerminalAttributeSetter = std::function<int(int, int, const termios*)>;

    TerminalPrompt();
    TerminalPrompt(int input_fd, int output_fd);
    TerminalPrompt(int input_fd, int output_fd, TerminalAttributeSetter set_attributes);

    PromptResult prompt(const nlohmann::json& challenge) override;

  private:
    int input_fd_;
    int output_fd_;
    TerminalAttributeSetter set_attributes_;
};

} // namespace tgcli::cli
