#include "cli/prompt.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

using nlohmann::json;

namespace {

json secret_challenge() {
    return {{"kind", "password"},
            {"nonce", "00112233445566778899aabbccddeeff"},
            {"sequence", 1},
            {"client_generation", 3},
            {"auth_sequence", 8},
            {"secret", true},
            {"prompt", "Password: "},
            {"details",
             {{"hint", ""},
              {"has_recovery_email", false},
              {"has_passport_data", false},
              {"recovery_email_pattern", ""}}}};
}

enum class RestoreFault { None, EintrOnce, Persistent };

constexpr std::string_view kRestoreFailureMessage =
    "tgcli: failed to restore terminal echo; input remains hidden";

class PromptProcess {
  public:
    explicit PromptProcess(bool nonblocking_input = false,
                           RestoreFault restore_fault = RestoreFault::None)
        : master_(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)) {
        REQUIRE(master_ >= 0);
        REQUIRE(::grantpt(master_) == 0);
        REQUIRE(::unlockpt(master_) == 0);
        const char* slave_name = ::ptsname(master_);
        REQUIRE(slave_name != nullptr);
        slave_ = ::open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
        REQUIRE(slave_ >= 0);
        REQUIRE(::tcgetattr(slave_, &original_) == 0);
        std::array<int, 2> stdout_pipe{-1, -1};
        std::array<int, 2> stderr_pipe{-1, -1};
        REQUIRE(::pipe(stdout_pipe.data()) == 0);
        REQUIRE(::pipe(stderr_pipe.data()) == 0);

        std::fflush(nullptr);
        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            static_cast<void>(::setpgid(0, 0));
            ::close(master_);
            ::close(stdout_pipe[0]);
            ::close(stderr_pipe[0]);
            static_cast<void>(::dup2(stdout_pipe[1], STDOUT_FILENO));
            static_cast<void>(::dup2(stderr_pipe[1], STDERR_FILENO));
            ::close(stdout_pipe[1]);
            ::close(stderr_pipe[1]);
            for (const int signal : {SIGINT, SIGTERM, SIGTSTP}) {
                static_cast<void>(::signal(signal, SIG_DFL));
            }
            if (nonblocking_input) {
                const int flags = ::fcntl(slave_, F_GETFL);
                static_cast<void>(::fcntl(slave_, F_SETFL, flags | O_NONBLOCK));
            }
            tgcli::cli::TerminalPrompt prompt(
                slave_, STDERR_FILENO,
                [restore_fault, calls = 0](int fd, int action, const termios* attributes) mutable {
                    ++calls;
                    if (calls >= 2 && restore_fault == RestoreFault::Persistent) {
                        errno = EIO;
                        return -1;
                    }
                    if (calls == 2 && restore_fault == RestoreFault::EintrOnce) {
                        errno = EINTR;
                        return -1;
                    }
                    return ::tcsetattr(fd, action, attributes);
                });
            const auto result = prompt.prompt(secret_challenge());
            ::close(slave_);
            ::_exit(20 + static_cast<int>(result.kind));
        }

        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        stdout_fd_ = stdout_pipe[0];
        stderr_fd_ = stderr_pipe[0];
    }

    PromptProcess(const PromptProcess&) = delete;
    PromptProcess& operator=(const PromptProcess&) = delete;
    PromptProcess(PromptProcess&&) = delete;
    PromptProcess& operator=(PromptProcess&&) = delete;

    ~PromptProcess() {
        if (pid_ > 0) {
            static_cast<void>(::kill(pid_, SIGKILL));
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        if (slave_ >= 0) {
            while (::tcsetattr(slave_, TCSANOW, &original_) != 0 && errno == EINTR) {
            }
        }
        for (const int descriptor : {stdout_fd_, stderr_fd_, slave_, master_}) {
            if (descriptor >= 0) {
                ::close(descriptor);
            }
        }
    }

    [[nodiscard]] pid_t pid() const {
        return pid_;
    }

    [[nodiscard]] int master() const {
        return master_;
    }

    [[nodiscard]] std::string wait_for_prompt() const {
        auto output = read_until(stderr_fd_, "Password: ");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!echo_disabled() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(echo_disabled());
        return output;
    }

    [[nodiscard]] std::string read_stdout() const {
        return read_to_end(stdout_fd_);
    }

    [[nodiscard]] std::string read_stderr() const {
        return read_to_end(stderr_fd_);
    }

    [[nodiscard]] bool echo_matches_original() const {
        termios current{};
        REQUIRE(::tcgetattr(slave_, &current) == 0);
        constexpr tcflag_t mask = ECHO | ECHONL;
        return (current.c_lflag & mask) == (original_.c_lflag & mask);
    }

    [[nodiscard]] bool echo_disabled() const {
        termios current{};
        REQUIRE(::tcgetattr(slave_, &current) == 0);
        return (current.c_lflag & static_cast<tcflag_t>(ECHO | ECHONL)) == 0;
    }

    int wait() {
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        return status;
    }

    [[nodiscard]] int wait_stopped() const {
        int status = 0;
        while (::waitpid(pid_, &status, WUNTRACED) < 0 && errno == EINTR) {
        }
        return status;
    }

  private:
    static std::string read_until(int fd, std::string_view expected) {
        std::string output;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (output.find(expected) == std::string::npos &&
               std::chrono::steady_clock::now() < deadline) {
            pollfd readable{fd, POLLIN, 0};
            if (::poll(&readable, 1, 50) <= 0) {
                continue;
            }
            std::array<char, 256> buffer{};
            const ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count <= 0) {
                break;
            }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
        INFO(output);
        REQUIRE(output.find(expected) != std::string::npos);
        return output;
    }

    static std::string read_to_end(int fd) {
        std::string output;
        std::array<char, 256> buffer{};
        for (;;) {
            const ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return output;
    }

    pid_t pid_ = -1;
    int master_ = -1;
    int slave_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    termios original_{};
};

} // namespace

TEST_CASE("secret terminal prompt does not echo the submitted sentinel", "[prompt][pty]") {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    REQUIRE(master >= 0);
    REQUIRE(::grantpt(master) == 0);
    REQUIRE(::unlockpt(master) == 0);
    const char* slave_name = ::ptsname(master);
    REQUIRE(slave_name != nullptr);
    const int slave = ::open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    REQUIRE(slave >= 0);
    std::array<int, 2> prompt_pipe{-1, -1};
    REQUIRE(::pipe(prompt_pipe.data()) == 0);

    tgcli::cli::TerminalPrompt prompt(slave, prompt_pipe[1]);
    auto result =
        std::async(std::launch::async, [&prompt] { return prompt.prompt(secret_challenge()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    constexpr std::string_view sentinel = "NEVER_ECHO_THIS_SENTINEL\n";
    REQUIRE(::write(master, sentinel.data(), sentinel.size()) ==
            static_cast<ssize_t>(sentinel.size()));
    const auto response = result.get();
    REQUIRE(response.kind == tgcli::cli::PromptResultKind::Answer);
    CHECK(response.answer["value"] == "NEVER_ECHO_THIS_SENTINEL");

    pollfd readable{master, POLLIN, 0};
    std::string terminal_output;
    if (::poll(&readable, 1, 50) > 0) {
        std::array<char, 256> buffer{};
        const auto count = ::read(master, buffer.data(), buffer.size());
        if (count > 0) {
            terminal_output.assign(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    CHECK(terminal_output.find("NEVER_ECHO_THIS_SENTINEL") == std::string::npos);

    ::close(prompt_pipe[0]);
    ::close(prompt_pipe[1]);
    ::close(slave);
    ::close(master);
}

TEST_CASE("secret prompt process restores termios on success, EOF, and read failure",
          "[prompt][pty][process]") {
    SECTION("success") {
        PromptProcess process;
        const auto prompt_output = process.wait_for_prompt();
        CHECK(process.echo_disabled());
        constexpr std::string_view sentinel = "PROCESS_SECRET_SENTINEL\n";
        REQUIRE(::write(process.master(), sentinel.data(), sentinel.size()) ==
                static_cast<ssize_t>(sentinel.size()));
        const int status = process.wait();
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 20 + static_cast<int>(tgcli::cli::PromptResultKind::Answer));
        CHECK(process.echo_matches_original());
        CHECK(process.read_stdout().empty());
        const auto stderr_output = prompt_output + process.read_stderr();
        CHECK(stderr_output.find("PROCESS_SECRET_SENTINEL") == std::string::npos);
    }

    SECTION("canonical EOF is cancellation") {
        PromptProcess process;
        static_cast<void>(process.wait_for_prompt());
        CHECK(process.echo_disabled());
        constexpr char terminal_eof = '\x04';
        REQUIRE(::write(process.master(), &terminal_eof, 1) == 1);
        const int status = process.wait();
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) ==
              20 + static_cast<int>(tgcli::cli::PromptResultKind::Cancelled));
        CHECK(process.echo_matches_original());
        CHECK(process.read_stdout().empty());
        static_cast<void>(process.read_stderr());
    }

    SECTION("read failure is an error") {
        PromptProcess process(/*nonblocking_input=*/true);
        const int status = process.wait();
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 20 + static_cast<int>(tgcli::cli::PromptResultKind::Error));
        CHECK(process.echo_matches_original());
        CHECK(process.read_stdout().empty());
        CHECK(process.read_stderr().find("Password: ") != std::string::npos);
    }

    SECTION("restore retries EINTR") {
        PromptProcess process(/*nonblocking_input=*/false, RestoreFault::EintrOnce);
        const auto prompt_output = process.wait_for_prompt();
        constexpr std::string_view sentinel = "RESTORE_EINTR_SENTINEL\n";
        REQUIRE(::write(process.master(), sentinel.data(), sentinel.size()) ==
                static_cast<ssize_t>(sentinel.size()));
        const int status = process.wait();
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 20 + static_cast<int>(tgcli::cli::PromptResultKind::Answer));
        CHECK(process.echo_matches_original());
        CHECK(process.read_stdout().empty());
        const auto stderr_output = prompt_output + process.read_stderr();
        CHECK(stderr_output.find(kRestoreFailureMessage) == std::string::npos);
        CHECK(stderr_output.find("RESTORE_EINTR_SENTINEL") == std::string::npos);
    }

    SECTION("hard restore failure is a fail-closed error") {
        PromptProcess process(/*nonblocking_input=*/false, RestoreFault::Persistent);
        const auto prompt_output = process.wait_for_prompt();
        constexpr std::string_view sentinel = "RESTORE_FAILURE_SENTINEL\n";
        REQUIRE(::write(process.master(), sentinel.data(), sentinel.size()) ==
                static_cast<ssize_t>(sentinel.size()));
        const int status = process.wait();
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 20 + static_cast<int>(tgcli::cli::PromptResultKind::Error));
        CHECK(process.echo_disabled());
        CHECK(process.read_stdout().empty());
        const auto stderr_output = prompt_output + process.read_stderr();
        CHECK(stderr_output.find(kRestoreFailureMessage) != std::string::npos);
        CHECK(stderr_output.find("RESTORE_FAILURE_SENTINEL") == std::string::npos);
    }
}

TEST_CASE("secret prompt restores termios before terminating signals", "[prompt][pty][process]") {
    for (const int signal : {SIGINT, SIGTERM}) {
        DYNAMIC_SECTION(signal) {
            PromptProcess process;
            const auto prompt_output = process.wait_for_prompt();
            CHECK(process.echo_disabled());
            REQUIRE(::kill(process.pid(), signal) == 0);
            const int status = process.wait();
            REQUIRE(WIFSIGNALED(status));
            CHECK(WTERMSIG(status) == signal);
            CHECK(process.echo_matches_original());
            CHECK(process.read_stdout().empty());
            const auto stderr_output = prompt_output + process.read_stderr();
            CHECK(stderr_output.find("Password: ") != std::string::npos);
        }
    }
}

TEST_CASE("secret prompt reports fail-closed restore failure before terminating signals",
          "[prompt][pty][process]") {
    for (const int signal : {SIGINT, SIGTERM}) {
        DYNAMIC_SECTION(signal) {
            PromptProcess process(/*nonblocking_input=*/false, RestoreFault::Persistent);
            const auto prompt_output = process.wait_for_prompt();
            REQUIRE(::kill(process.pid(), signal) == 0);
            const int status = process.wait();
            REQUIRE(WIFSIGNALED(status));
            CHECK(WTERMSIG(status) == signal);
            CHECK(process.echo_disabled());
            CHECK(process.read_stdout().empty());
            const auto stderr_output = prompt_output + process.read_stderr();
            CHECK(stderr_output.find(kRestoreFailureMessage) != std::string::npos);
        }
    }
}

TEST_CASE("secret prompt restores before SIGTSTP and hides input again after resume",
          "[prompt][pty][process]") {
    PromptProcess process;
    const auto first_prompt = process.wait_for_prompt();
    CHECK(process.echo_disabled());
    REQUIRE(::kill(process.pid(), SIGTSTP) == 0);
    const int stopped = process.wait_stopped();
    REQUIRE(WIFSTOPPED(stopped));
    CHECK(WSTOPSIG(stopped) == SIGTSTP);
    CHECK(process.echo_matches_original());
    REQUIRE(::kill(process.pid(), SIGCONT) == 0);
    const auto resumed_prompt = process.wait_for_prompt();
    CHECK(process.echo_disabled());
    constexpr std::string_view sentinel = "RESUMED_SECRET_SENTINEL\n";
    REQUIRE(::write(process.master(), sentinel.data(), sentinel.size()) ==
            static_cast<ssize_t>(sentinel.size()));
    const int status = process.wait();
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 20 + static_cast<int>(tgcli::cli::PromptResultKind::Answer));
    CHECK(process.echo_matches_original());
    CHECK(process.read_stdout().empty());
    const auto stderr_output = first_prompt + resumed_prompt + process.read_stderr();
    CHECK(stderr_output.find("RESUMED_SECRET_SENTINEL") == std::string::npos);
}
