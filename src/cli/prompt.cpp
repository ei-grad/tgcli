#include "cli/prompt.hpp"

#include "proto/frame.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <string>
#include <string_view>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace tgcli::cli {

namespace {

// A sigaction handler can communicate only through lock-free signal-safe state.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t pending_prompt_signal = 0;

constexpr std::string_view kRestoreFailureMessage =
    "\ntgcli: failed to restore terminal echo; input remains hidden\n";

extern "C" void record_prompt_signal(int signal) {
    pending_prompt_signal = signal;
}

class PromptSignalGuard {
  public:
    PromptSignalGuard() : installed_(install_all()) {}

    PromptSignalGuard(const PromptSignalGuard&) = delete;
    PromptSignalGuard& operator=(const PromptSignalGuard&) = delete;
    PromptSignalGuard(PromptSignalGuard&&) = delete;
    PromptSignalGuard& operator=(PromptSignalGuard&&) = delete;

    ~PromptSignalGuard() {
        restore_all();
    }

    [[nodiscard]] bool installed() const {
        return installed_;
    }

    static int take_pending() {
        const int signal = pending_prompt_signal;
        pending_prompt_signal = 0;
        return signal;
    }

    bool suspend_and_resume() {
        restore(SIGTSTP);
        if (::kill(::getpid(), SIGTSTP) != 0) {
            return false;
        }
        return install(SIGTSTP);
    }

    void forward_terminal(int signal) {
        restore_all();
        installed_ = false;
        static_cast<void>(::kill(::getpid(), signal));
    }

  private:
    static constexpr std::array<int, 3> kSignals{SIGINT, SIGTERM, SIGTSTP};

    bool install_all() {
        pending_prompt_signal = 0;
        if (!std::ranges::all_of(kSignals, [this](int signal) { return install(signal); })) {
            restore_all();
            return false;
        }
        return true;
    }

    bool install(int signal) {
        const auto index = signal_index(signal);
        struct sigaction action {};
        action.sa_handler = record_prompt_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (::sigaction(signal, &action, &previous_.at(index)) != 0) {
            return false;
        }
        active_.at(index) = true;
        return true;
    }

    void restore(int signal) {
        const auto index = signal_index(signal);
        if (active_.at(index)) {
            static_cast<void>(::sigaction(signal, &previous_.at(index), nullptr));
            active_.at(index) = false;
        }
    }

    void restore_all() {
        for (const int signal : kSignals) {
            restore(signal);
        }
    }

    static std::size_t signal_index(int signal) {
        for (std::size_t index = 0; index < kSignals.size(); ++index) {
            if (kSignals.at(index) == signal) {
                return index;
            }
        }
        return 0;
    }

    std::array<struct sigaction, kSignals.size()> previous_{};
    std::array<bool, kSignals.size()> active_{};
    bool installed_ = false;
};

class EchoGuard {
  public:
    EchoGuard(int fd, bool disable, const TerminalPrompt::TerminalAttributeSetter& setter)
        : fd_(fd), set_attributes_(setter) {
        if (!disable || ::tcgetattr(fd_, &saved_) != 0) {
            return;
        }
        termios hidden = saved_;
        hidden.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL));
        active_ = set_attributes(hidden);
    }

    EchoGuard(const EchoGuard&) = delete;
    EchoGuard& operator=(const EchoGuard&) = delete;
    EchoGuard(EchoGuard&&) = delete;
    EchoGuard& operator=(EchoGuard&&) = delete;

    ~EchoGuard() {
        static_cast<void>(restore());
    }

    [[nodiscard]] bool active_or_unneeded(bool secret) const {
        return !secret || active_;
    }

    [[nodiscard]] bool restore() {
        if (!active_) {
            return true;
        }
        if (!set_attributes(saved_)) {
            return false;
        }
        active_ = false;
        return true;
    }

  private:
    [[nodiscard]] bool set_attributes(const termios& attributes) const {
        for (;;) {
            if (set_attributes_(fd_, TCSANOW, &attributes) == 0) {
                return true;
            }
            if (errno != EINTR) {
                return false;
            }
        }
    }

    int fd_;
    const TerminalPrompt::TerminalAttributeSetter& set_attributes_;
    termios saved_{};
    bool active_ = false;
};

bool write_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t count = ::write(fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

enum class ReadKind { Value, Eof, Error, Interrupted };

struct ReadResult {
    ReadResult(ReadKind kind_value, int signal_value = 0,
               secure::WipeObserver wipe_observer_value = {})
        : kind(kind_value), signal(signal_value), wipe_observer(std::move(wipe_observer_value)) {}
    ~ReadResult() {
        secure::wipe(value, wipe_observer, "prompt_input");
    }
    ReadResult(const ReadResult&) = delete;
    ReadResult& operator=(const ReadResult&) = delete;
    // Copying preserves the source allocation until its bytes have been wiped.
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,cppcoreguidelines-prefer-member-initializer,performance-noexcept-move-constructor)
    ReadResult(ReadResult&& other) : kind(other.kind), signal(other.signal) {
        secure::transfer(other.value, value, other.wipe_observer, "prompt_input_move_source");
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
        wipe_observer = std::move(other.wipe_observer);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    ReadResult& operator=(ReadResult&& other) {
        if (this != &other) {
            secure::wipe(value, wipe_observer, "prompt_input");
            kind = other.kind;
            signal = other.signal;
            secure::transfer(other.value, value, other.wipe_observer, "prompt_input_move_source");
            wipe_observer = std::move(other.wipe_observer);
        }
        return *this;
    }

    ReadKind kind;
    std::string value;
    int signal = 0;
    secure::WipeObserver wipe_observer;
};

void report_restore_failure_and_forward(int output_fd, const ReadResult& input,
                                        PromptSignalGuard& signals) {
    static_cast<void>(write_all(output_fd, kRestoreFailureMessage));
    if (input.kind == ReadKind::Interrupted &&
        (input.signal == SIGINT || input.signal == SIGTERM)) {
        signals.forward_terminal(input.signal);
    }
}

void finish_secret_prompt_line(int output_fd, bool secret) {
    if (secret) {
        static_cast<void>(write_all(output_fd, "\n"));
    }
}

ReadResult read_line(int fd, const secure::WipeObserver& wipe_observer) {
    ReadResult result{ReadKind::Value, 0, wipe_observer};
    for (;;) {
        if (const int signal = PromptSignalGuard::take_pending(); signal != 0) {
            return {ReadKind::Interrupted, signal, wipe_observer};
        }
        char character = 0;
        const ssize_t count = ::read(fd, &character, 1);
        if (count < 0 && errno == EINTR) {
            if (const int signal = PromptSignalGuard::take_pending(); signal != 0) {
                return {ReadKind::Interrupted, signal, wipe_observer};
            }
            continue;
        }
        if (count < 0) {
            return {ReadKind::Error, 0, wipe_observer};
        }
        if (count == 0) {
            return {ReadKind::Eof, 0, wipe_observer};
        }
        if (character == '\n') {
            break;
        }
        if (character != '\r') {
            result.value.push_back(character);
        }
    }
    return result;
}

nlohmann::json answer_identity(const nlohmann::json& challenge) {
    return {{"nonce", challenge["nonce"]},
            {"sequence", challenge["sequence"]},
            {"client_generation", challenge["client_generation"]},
            {"auth_sequence", challenge["auth_sequence"]}};
}

} // namespace

PromptResult::PromptResult(PromptResultKind kind_value, secure::WipeObserver wipe_observer_value)
    : kind(kind_value), wipe_observer_(std::move(wipe_observer_value)) {}

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
PromptResult::PromptResult(PromptResultKind kind_value, nlohmann::json&& answer_value,
                           secure::WipeObserver wipe_observer_value)
    : kind(kind_value), wipe_observer_(std::move(wipe_observer_value)) {
    secure::transfer(answer_value, answer, wipe_observer_, "prompt_answer_source");
}

PromptResult::~PromptResult() {
    secure::wipe(answer, wipe_observer_, "prompt_answer");
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,cppcoreguidelines-prefer-member-initializer,performance-noexcept-move-constructor)
PromptResult::PromptResult(PromptResult&& other) : kind(other.kind) {
    secure::transfer(other.answer, answer, other.wipe_observer_, "prompt_result_move_source");
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    wipe_observer_ = std::move(other.wipe_observer_);
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
PromptResult& PromptResult::operator=(PromptResult&& other) {
    if (this != &other) {
        secure::wipe(answer, wipe_observer_, "prompt_answer");
        kind = other.kind;
        secure::transfer(other.answer, answer, other.wipe_observer_, "prompt_result_move_source");
        wipe_observer_ = std::move(other.wipe_observer_);
    }
    return *this;
}

TerminalPrompt::TerminalPrompt() : TerminalPrompt(STDIN_FILENO, STDERR_FILENO) {}

TerminalPrompt::TerminalPrompt(int input_fd, int output_fd)
    : TerminalPrompt(input_fd, output_fd, ::tcsetattr) {}

TerminalPrompt::TerminalPrompt(int input_fd, int output_fd, TerminalAttributeSetter set_attributes)
    : TerminalPrompt(input_fd, output_fd, std::move(set_attributes), {}) {}

TerminalPrompt::TerminalPrompt(int input_fd, int output_fd, TerminalAttributeSetter set_attributes,
                               secure::WipeObserver wipe_observer)
    : input_fd_(input_fd), output_fd_(output_fd), set_attributes_(std::move(set_attributes)),
      wipe_observer_(std::move(wipe_observer)) {}

PromptResult TerminalPrompt::prompt(const nlohmann::json& challenge) {
    std::string validation_error;
    if (!proto::validate_challenge_payload(challenge, validation_error)) {
        return {PromptResultKind::Error};
    }
    if (::isatty(input_fd_) == 0) {
        return {PromptResultKind::Unavailable};
    }
    if (!write_all(output_fd_, challenge["prompt"].get_ref<const std::string&>())) {
        return {PromptResultKind::Error};
    }

    const bool secret = challenge["secret"].get<bool>();
    PromptSignalGuard signals;
    if (secret && !signals.installed()) {
        return {PromptResultKind::Error};
    }

    ReadResult input{ReadKind::Error, 0, wipe_observer_};
    for (;;) {
        EchoGuard echo(input_fd_, secret, set_attributes_);
        if (!echo.active_or_unneeded(secret)) {
            return {PromptResultKind::Error};
        }
        input = read_line(input_fd_, wipe_observer_);
        const bool echo_restored = echo.restore();
        if (!echo_restored) {
            report_restore_failure_and_forward(output_fd_, input, signals);
            return {PromptResultKind::Error};
        }
        finish_secret_prompt_line(output_fd_, secret);
        if (input.kind != ReadKind::Interrupted) {
            break;
        }
        if (input.signal == SIGTSTP) {
            if (!signals.suspend_and_resume() ||
                !write_all(output_fd_, challenge["prompt"].get_ref<const std::string&>())) {
                return {PromptResultKind::Error};
            }
            continue;
        }
        signals.forward_terminal(input.signal);
        return {PromptResultKind::Error};
    }

    auto answer = answer_identity(challenge);
    if (input.kind == ReadKind::Eof) {
        answer["cancelled"] = true;
        return {PromptResultKind::Cancelled, std::move(answer), wipe_observer_};
    }
    if (input.kind == ReadKind::Error) {
        return {PromptResultKind::Error};
    }
    const auto kind = proto::parse_challenge_kind(challenge["kind"].get_ref<const std::string&>());
    if (!kind) {
        return {PromptResultKind::Error};
    }
    if (proto::challenge_kind_expects_boolean(*kind)) {
        answer["value"] = input.value == "y" || input.value == "yes";
    } else {
        answer["value"] = input.value;
        secure::wipe(input.value, wipe_observer_, "prompt_input_source");
    }
    return {PromptResultKind::Answer, std::move(answer), wipe_observer_};
}

} // namespace tgcli::cli
