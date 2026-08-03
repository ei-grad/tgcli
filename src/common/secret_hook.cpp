#include "common/secret_hook.hpp"

#include "common/secret_hook_test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tgcli::secret_hook {

namespace {

constexpr std::array<std::string_view, 13> kInheritedEnvironment = {
    "HOME",           "PATH",
    "LANG",           "LC_ALL",
    "LC_CTYPE",       "XDG_CONFIG_HOME",
    "XDG_DATA_HOME",  "XDG_STATE_HOME",
    "XDG_CACHE_HOME", "XDG_RUNTIME_DIR",
    "GNUPGHOME",      "PASSWORD_STORE_DIR",
    "SSH_AUTH_SOCK"};

class Descriptor {
  public:
    explicit Descriptor(int fd = -1) : fd_(fd) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Descriptor() {
        reset();
    }
    [[nodiscard]] int get() const {
        return fd_;
    }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_;
};

struct Pipe {
    Descriptor read;
    Descriptor write;
};

class SensitiveValue {
  public:
    SensitiveValue() = default;
    SensitiveValue(const SensitiveValue&) = delete;
    SensitiveValue& operator=(const SensitiveValue&) = delete;
    SensitiveValue(SensitiveValue&&) = delete;
    SensitiveValue& operator=(SensitiveValue&&) = delete;
    ~SensitiveValue() {
        volatile char* bytes = value.data();
        for (std::size_t index = 0; index < value.size(); ++index) {
            bytes[index] = '\0';
        }
    }

    std::string value;
};

bool create_pipe(Pipe& result) {
    std::array<int, 2> descriptors = {-1, -1};
    if (::pipe(descriptors.data()) != 0) {
        return false;
    }
    result.read.reset(descriptors[0]);
    result.write.reset(descriptors[1]);
    return std::ranges::all_of(descriptors, [](int descriptor) {
        const int flags = ::fcntl(descriptor, F_GETFD);
        return flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
    });
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::vector<std::string> allowed_environment() {
    std::vector<std::string> result;
    result.reserve(kInheritedEnvironment.size());
    bool path_present = false;
    for (const auto name : kInheritedEnvironment) {
        const std::string key(name);
        if (const char* value = std::getenv(key.c_str()); value != nullptr) {
            result.push_back(key + "=" + value);
            path_present = path_present || name == "PATH";
        }
    }
    if (!path_present) {
        result.emplace_back("PATH=/usr/bin:/bin");
    }
    return result;
}

HookResult failure(HookField field, HookFailure reason, std::optional<int> status = std::nullopt) {
    return {{}, HookError{field, reason, status}};
}

void terminate_group(pid_t child, bool child_reaped) {
    if (child > 0) {
        ::kill(-child, SIGKILL);
    }
    if (!child_reaped) {
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

enum class DrainResult { Open, Closed, Error };

DrainResult drain_output(Descriptor& descriptor, bool& open, std::string* output,
                         std::size_t& byte_count) {
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count > 0) {
            byte_count += static_cast<std::size_t>(count);
            if (output != nullptr && output->size() <= kMaxOutputBytes) {
                const auto available = kMaxOutputBytes + 1 - output->size();
                output->append(buffer.data(), std::min(static_cast<std::size_t>(count), available));
            }
            if (byte_count > kMaxOutputBytes) {
                return DrainResult::Open;
            }
            continue;
        }
        if (count == 0) {
            descriptor.reset();
            open = false;
            return DrainResult::Closed;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return DrainResult::Open;
        }
        descriptor.reset();
        open = false;
        return DrainResult::Error;
    }
}

std::optional<int> read_spawn_error(Descriptor& descriptor, bool& open) {
    int child_errno = 0;
    while (true) {
        const ssize_t count = ::read(descriptor.get(), &child_errno, sizeof(child_errno));
        if (count == static_cast<ssize_t>(sizeof(child_errno))) {
            descriptor.reset();
            open = false;
            return child_errno;
        }
        if (count == 0) {
            descriptor.reset();
            open = false;
            return std::nullopt;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return std::nullopt;
        }
        descriptor.reset();
        open = false;
        return EIO;
    }
}

} // namespace

std::string_view field_name(HookField field) {
    switch (field) {
    case HookField::ApiId:
        return "api_id_cmd";
    case HookField::ApiHash:
        return "api_hash_cmd";
    case HookField::DatabaseKey:
        return "db_key_cmd";
    case HookField::Password:
        return "password_cmd";
    case HookField::BotToken:
        return "bot_token_cmd";
    }
    return "unknown_cmd";
}

std::string_view failure_name(HookFailure reason) {
    switch (reason) {
    case HookFailure::Spawn:
        return "spawn";
    case HookFailure::Exit:
        return "exit";
    case HookFailure::Signal:
        return "signal";
    case HookFailure::Timeout:
        return "timeout";
    case HookFailure::StdoutEmpty:
        return "stdout_empty";
    case HookFailure::StdoutInvalid:
        return "stdout_invalid";
    case HookFailure::StdoutTooLarge:
        return "stdout_too_large";
    case HookFailure::StderrTooLarge:
        return "stderr_too_large";
    }
    return "spawn";
}

// Hook execution is a bounded process state machine. The sequential structure
// keeps kill/reap/redaction decisions adjacent to every failure boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
HookResult run_impl(const HookRequest& request, const testing::RunHooks& hooks) {
    const auto start = std::chrono::steady_clock::now();
    auto deadline = start + kMaximumRuntime;
    if (request.request_deadline && *request.request_deadline < deadline) {
        deadline = *request.request_deadline;
    }
    if (start >= deadline) {
        return failure(request.field, HookFailure::Timeout);
    }

    Pipe standard_output;
    Pipe standard_error;
    Pipe spawn_error;
    if (!create_pipe(standard_output) || !create_pipe(standard_error) ||
        !create_pipe(spawn_error) || !set_nonblocking(standard_output.read.get()) ||
        !set_nonblocking(standard_error.read.get()) || !set_nonblocking(spawn_error.read.get())) {
        return failure(request.field, HookFailure::Spawn);
    }

    auto environment = allowed_environment();
    std::vector<char*> environment_pointers;
    environment_pointers.reserve(environment.size() + 1);
    for (auto& entry : environment) {
        environment_pointers.push_back(entry.data());
    }
    environment_pointers.push_back(nullptr);
    std::string shell = "/bin/sh";
    std::string command_option = "-c";
    std::string command = request.command;

    const pid_t child = ::fork();
    if (child < 0) {
        return failure(request.field, HookFailure::Spawn);
    }
    if (child == 0) {
        standard_output.read.reset();
        standard_error.read.reset();
        spawn_error.read.reset();
        int child_errno = 0;
        if (::setpgid(0, 0) != 0 || ::chdir("/") != 0) {
            child_errno = errno;
        }
        const int null_input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (child_errno == 0 && null_input < 0) {
            child_errno = errno;
        }
        if (child_errno == 0 && (::dup2(null_input, STDIN_FILENO) < 0 ||
                                 ::dup2(standard_output.write.get(), STDOUT_FILENO) < 0 ||
                                 ::dup2(standard_error.write.get(), STDERR_FILENO) < 0)) {
            child_errno = errno;
        }
        if (null_input >= 0) {
            ::close(null_input);
        }
        if (child_errno == 0) {
            std::array<char*, 4> arguments = {shell.data(), command_option.data(), command.data(),
                                              nullptr};
            ::execve("/bin/sh", arguments.data(), environment_pointers.data());
            child_errno = errno;
        }
        const auto ignored = ::write(spawn_error.write.get(), &child_errno, sizeof(child_errno));
        (void)ignored;
        ::_exit(127);
    }

    standard_output.write.reset();
    standard_error.write.reset();
    spawn_error.write.reset();
    if (::setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        terminate_group(child, false);
        return failure(request.field, HookFailure::Spawn);
    }
    if (hooks.on_spawn) {
        hooks.on_spawn(child);
    }
    SensitiveValue output;
    std::size_t stdout_bytes = 0;
    std::size_t stderr_bytes = 0;
    bool stdout_open = true;
    bool stderr_open = true;
    bool spawn_open = true;
    bool child_reaped = false;
    int child_status = 0;

    while (!child_reaped || stdout_open || stderr_open || spawn_open) {
        if (stdout_open && drain_output(standard_output.read, stdout_open, &output.value,
                                        stdout_bytes) == DrainResult::Error) {
            terminate_group(child, child_reaped);
            return failure(request.field, HookFailure::Spawn);
        }
        if (stderr_open && drain_output(standard_error.read, stderr_open, nullptr, stderr_bytes) ==
                               DrainResult::Error) {
            terminate_group(child, child_reaped);
            return failure(request.field, HookFailure::Spawn);
        }
        if (spawn_open) {
            if (const auto spawn_errno = read_spawn_error(spawn_error.read, spawn_open);
                spawn_errno) {
                terminate_group(child, child_reaped);
                return failure(request.field, HookFailure::Spawn);
            }
        }
        if (stdout_bytes > kMaxOutputBytes) {
            terminate_group(child, child_reaped);
            return failure(request.field, HookFailure::StdoutTooLarge);
        }
        if (stderr_bytes > kMaxOutputBytes) {
            terminate_group(child, child_reaped);
            return failure(request.field, HookFailure::StderrTooLarge);
        }
        if (!child_reaped) {
            const pid_t waited = ::waitpid(child, &child_status, WNOHANG);
            if (waited == child) {
                child_reaped = true;
            } else if (waited < 0 && errno != EINTR) {
                terminate_group(child, false);
                return failure(request.field, HookFailure::Spawn);
            }
        }
        if (child_reaped && !stdout_open && !stderr_open && !spawn_open) {
            if (hooks.before_accept) {
                hooks.before_accept();
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                terminate_group(child, true);
                return failure(request.field, HookFailure::Timeout);
            }
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            terminate_group(child, child_reaped);
            return failure(request.field, HookFailure::Timeout);
        }

        std::array<pollfd, 3> descriptors{};
        nfds_t count = 0;
        const auto add_descriptor = [&](bool open, int fd) {
            if (open) {
                descriptors.at(count++) = pollfd{fd, POLLIN | POLLHUP, 0};
            }
        };
        add_descriptor(stdout_open, standard_output.read.get());
        add_descriptor(stderr_open, standard_error.read.get());
        add_descriptor(spawn_open, spawn_error.read.get());
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int timeout = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, 100));
        if (::poll(descriptors.data(), count, timeout) < 0 && errno != EINTR) {
            terminate_group(child, child_reaped);
            return failure(request.field, HookFailure::Spawn);
        }
    }

    if (WIFSIGNALED(child_status)) {
        return failure(request.field, HookFailure::Signal, WTERMSIG(child_status));
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return failure(request.field, HookFailure::Exit,
                       WIFEXITED(child_status) ? std::optional<int>(WEXITSTATUS(child_status))
                                               : std::nullopt);
    }

    if (output.value.ends_with("\r\n")) {
        output.value.resize(output.value.size() - 2);
    } else if (output.value.ends_with('\n')) {
        output.value.pop_back();
    }
    if (output.value.empty()) {
        return failure(request.field, HookFailure::StdoutEmpty);
    }
    if (std::ranges::any_of(output.value, [](char character) {
            return character == '\r' || character == '\n' || character == '\0';
        })) {
        return failure(request.field, HookFailure::StdoutInvalid);
    }
    return {std::move(output.value), {}};
}

HookResult run(const HookRequest& request) {
    return run_impl(request, {});
}

namespace testing {

HookResult run(const HookRequest& request, const RunHooks& hooks) {
    return run_impl(request, hooks);
}

} // namespace testing

bool parse_api_id(std::string_view value, std::int32_t& parsed) {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    std::int64_t wide = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), wide);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
        wide <= 0 || wide > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    parsed = static_cast<std::int32_t>(wide);
    return true;
}

std::string describe(const HookError& error) {
    std::string result = "hook " + std::string(field_name(error.field)) +
                         " failed: " + std::string(failure_name(error.reason));
    if (error.status) {
        result += " (status " + std::to_string(*error.status) + ")";
    }
    return result;
}

} // namespace tgcli::secret_hook
