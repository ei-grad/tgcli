#pragma once

#include "common/secure_wipe.hpp"

#include <functional>
#include <optional>
#include <termios.h>

#include <nlohmann/json.hpp>

namespace tgcli::cli {

enum class PromptResultKind { Answer, Cancelled, Unavailable, Error };

struct PromptResult {
    PromptResult(PromptResultKind kind_value, secure::WipeObserver wipe_observer_value = {});
    PromptResult(PromptResultKind kind_value, nlohmann::json&& answer_value,
                 secure::WipeObserver wipe_observer_value = {});
    ~PromptResult();
    PromptResult(const PromptResult&) = delete;
    PromptResult& operator=(const PromptResult&) = delete;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    PromptResult(PromptResult&& other);
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    PromptResult& operator=(PromptResult&& other);

    PromptResultKind kind;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
    nlohmann::json answer = // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
        nlohmann::json::object();

  private:
    secure::WipeObserver wipe_observer_;
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
    TerminalPrompt(int input_fd, int output_fd, TerminalAttributeSetter set_attributes,
                   secure::WipeObserver wipe_observer);

    PromptResult prompt(const nlohmann::json& challenge) override;

  private:
    int input_fd_;
    int output_fd_;
    TerminalAttributeSetter set_attributes_;
    secure::WipeObserver wipe_observer_;
};

} // namespace tgcli::cli
