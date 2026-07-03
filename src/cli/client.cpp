#include "cli/client.hpp"

#include "cli/render.hpp"
#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "daemon/daemon_run.hpp"
#include "proto/frame_io.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tgcli/version.hpp>
#include <thread>
#include <unistd.h>
#include <variant>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace tgcli::cli {

namespace {

using nlohmann::json;

void print_error(const std::string& code, const std::string& message, const json& details) {
    const json error{{"error", {{"code", code}, {"message", message}, {"details", details}}}};
    std::fputs((error.dump() + "\n").c_str(), stderr);
}

std::string command_key(const std::vector<std::string>& command) {
    std::string key;
    for (const auto& part : command) {
        if (!key.empty()) {
            key += ' ';
        }
        key += part;
    }
    return key;
}

// Renders response frames and remembers the terminal outcome.
class FrameRenderer {
  public:
    FrameRenderer(std::string command, bool json_mode)
        : command_(std::move(command)), json_(json_mode) {}

    static void on_item(const json& data) {
        // Streams are NDJSON in both modes until a command needs more.
        std::fputs((data.dump() + "\n").c_str(), stdout);
    }

    void on_progress(const json& data) const {
        if (json_) {
            std::fputs((json{{"progress", data}}.dump() + "\n").c_str(), stderr);
        } else {
            std::fputs(("progress: " + data.dump() + "\n").c_str(), stderr);
        }
    }

    void on_result(const json& data) {
        if (json_) {
            std::fputs((data.dump() + "\n").c_str(), stdout);
        } else {
            std::fputs(render_human(command_, data).c_str(), stdout);
        }
        exit_code_ = kOk;
        done_ = true;
    }

    void on_error(const std::string& code, const std::string& message, const json& details,
                  int exit_code) {
        print_error(code, message, details);
        exit_code_ = exit_code;
        done_ = true;
    }

    [[nodiscard]] bool done() const {
        return done_;
    }
    [[nodiscard]] int exit_code() const {
        return exit_code_;
    }

  private:
    std::string command_;
    bool json_;
    bool done_ = false;
    int exit_code_ = kGeneric;
};

int connect_socket(const std::string& socket_path) {
    const int fd = net::socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Absolute path to the running executable, re-exec'd as the daemon. Linux
// uses the /proc self-symlink directly; macOS has no /proc, so ask dyld.
// Resolved before fork so no allocation happens in the forked child.
std::string current_exe_path() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // sets size to the required length
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path.resize(std::strlen(path.c_str()));
    return path;
#else
    return "/proc/self/exe";
#endif
}

// Double-fork so the daemon is reparented to init and the client never
// leaves a zombie; the grandchild re-execs this binary as `daemon run`.
bool spawn_daemon(const std::string& account, std::string& error) {
    const std::string exe = current_exe_path();
    if (exe.empty()) {
        error = "cannot determine executable path for daemon re-exec";
        return false;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        const pid_t grandchild = ::fork();
        if (grandchild != 0) {
            ::_exit(grandchild < 0 ? 1 : 0);
        }
        ::setsid();
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                ::close(devnull);
            }
        }
        ::execl(exe.c_str(), "tgcli", "daemon", "run", "--account", account.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "failed to spawn the daemon";
        return false;
    }
    return true;
}

std::optional<int> connect_with_spawn(const std::string& account, const std::string& socket_path,
                                      bool auto_spawn, std::string& error) {
    int fd = connect_socket(socket_path);
    if (fd >= 0) {
        return fd;
    }
    if (!auto_spawn) {
        error = "cannot connect to " + socket_path + ": " + std::strerror(errno);
        return std::nullopt;
    }
    if (!spawn_daemon(account, error)) {
        return std::nullopt;
    }
    // The daemon binds its socket after tdlib init; poll with a deadline.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        fd = connect_socket(socket_path);
        if (fd >= 0) {
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "daemon did not come up within 10s (socket " + socket_path + ")";
    return std::nullopt;
}

struct Session {
    int fd = -1;
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

enum class HandshakeOutcome { Ok, Mismatch, Failed };

HandshakeOutcome handshake(int fd, proto::FrameReader& reader, std::string& error) {
    if (std::string io_error;
        !proto::write_frame(fd, proto::Hello{kVersion, proto::kProtocolVersion}, io_error)) {
        error = io_error;
        return HandshakeOutcome::Failed;
    }
    std::string io_error;
    const auto line = reader.read_line(io_error);
    if (!line) {
        error = io_error.empty() ? "daemon closed the connection during handshake" : io_error;
        return HandshakeOutcome::Failed;
    }
    std::string parse_error;
    const auto frame = proto::parse(*line, parse_error);
    if (!frame) {
        error = "malformed handshake frame: " + parse_error;
        return HandshakeOutcome::Failed;
    }
    const auto* hello = std::get_if<proto::Hello>(&*frame);
    if (hello == nullptr) {
        error = "daemon did not open with a hello frame";
        return HandshakeOutcome::Failed;
    }
    if (hello->protocol_version != proto::kProtocolVersion || hello->binary_version != kVersion) {
        return HandshakeOutcome::Mismatch;
    }
    return HandshakeOutcome::Ok;
}

// The freshly exec'd client is authoritative on a version mismatch
// (DESIGN.md §10): ask the old daemon to stop, wait for the socket to die,
// respawn from this binary.
bool restart_stale_daemon(int fd, proto::FrameReader& reader, std::string& error) {
    proto::Request stop_request;
    stop_request.id = 1;
    stop_request.command = {"daemon", "stop"};
    stop_request.context.cwd = "/";
    if (std::string io_error; !proto::write_frame(fd, stop_request, io_error)) {
        error = io_error;
        return false;
    }
    // Drain until EOF; the old daemon may speak an older frame dialect, so
    // nothing here is interpreted.
    std::string io_error;
    while (reader.read_line(io_error)) {
    }
    return true;
}

int run_local_doctor(const std::string& account, const std::string& socket_path, bool json_mode,
                     const std::string& reason) {
    std::fprintf(stderr, "warning: daemon unreachable (%s); local diagnostics only\n",
                 reason.c_str());
    const auto env = paths::real_environment();
    const auto config = paths::config_file(env);
    const json data{{"account", account},
                    {"daemon", {{"running", false}, {"socket", socket_path}}},
                    {"config", {{"path", config}, {"exists", std::filesystem::exists(config)}}}};
    FrameRenderer renderer("doctor", json_mode);
    renderer.on_result(data);
    return renderer.exit_code();
}

// Sends the request and renders response frames until the terminal one.
int exchange(int fd, proto::FrameReader& reader, const proto::Request& request,
             const RunOptions& options) {
    if (std::string io_error; !proto::write_frame(fd, request, io_error)) {
        print_error("GENERIC", "cannot send request: " + io_error, json::object());
        return kGeneric;
    }
    FrameRenderer renderer(command_key(request.command), options.json);
    while (!renderer.done()) {
        std::string io_error;
        const auto line = reader.read_line(io_error);
        if (!line) {
            print_error("GENERIC", io_error.empty() ? "daemon closed the connection" : io_error,
                        json::object());
            return kGeneric;
        }
        std::string parse_error;
        const auto frame = proto::parse(*line, parse_error);
        if (!frame) {
            print_error("GENERIC", "malformed frame from daemon: " + parse_error, json::object());
            return kGeneric;
        }
        std::visit(
            [&renderer](const auto& f) {
                using T = std::decay_t<decltype(f)>;
                if constexpr (std::is_same_v<T, proto::Item>) {
                    FrameRenderer::on_item(f.data);
                } else if constexpr (std::is_same_v<T, proto::Progress>) {
                    renderer.on_progress(f.data);
                } else if constexpr (std::is_same_v<T, proto::Result>) {
                    renderer.on_result(f.data);
                } else if constexpr (std::is_same_v<T, proto::Error>) {
                    renderer.on_error(f.code, f.message, f.details, f.exit_code);
                }
                // Hello/Request/Challenge/Answer: nothing to render in M0;
                // challenges arrive with M1.
            },
            *frame);
    }
    return renderer.exit_code();
}

int run_in_process(const proto::Request& request, const RunOptions& options) {
    FrameRenderer renderer(command_key(request.command), options.json);
    daemon::CallbackSink sink(
        [](const json& data) { FrameRenderer::on_item(data); },
        [&renderer](const json& data) { renderer.on_progress(data); },
        [&renderer](const json& data) { renderer.on_result(data); },
        [&renderer](const std::string& code, const std::string& message, const json& details,
                    int exit_code) { renderer.on_error(code, message, details, exit_code); });
    std::string error;
    if (!daemon::run_no_daemon(request, sink, options.account, error)) {
        print_error("GENERIC", error, json::object());
        return kGeneric;
    }
    return renderer.exit_code();
}

} // namespace

int run_command(const proto::Request& request, const RunOptions& options) {
    if (options.no_daemon) {
        return run_in_process(request, options);
    }

    const auto env = paths::real_environment();
    std::string error;
    const auto socket_path = paths::socket_path(options.account, env, error);
    if (!socket_path) {
        print_error("USAGE", error, json::object());
        return kUsage;
    }
    const bool is_doctor = command_key(request.command) == "doctor";

    for (int attempt = 0; attempt < 2; ++attempt) {
        Session session;
        {
            auto fd = connect_with_spawn(options.account, *socket_path, options.auto_spawn, error);
            if (!fd) {
                if (is_doctor) {
                    return run_local_doctor(options.account, *socket_path, options.json, error);
                }
                print_error("GENERIC", error, json::object());
                return kGeneric;
            }
            session.fd = *fd;
        }
        proto::FrameReader reader(session.fd);
        switch (handshake(session.fd, reader, error)) {
        case HandshakeOutcome::Failed:
            print_error("GENERIC", "handshake failed: " + error, json::object());
            return kGeneric;
        case HandshakeOutcome::Mismatch:
            if (attempt > 0 || !restart_stale_daemon(session.fd, reader, error)) {
                print_error("GENERIC", "daemon version mismatch persists after restart",
                            json::object());
                return kGeneric;
            }
            continue;
        case HandshakeOutcome::Ok:
            break;
        }

        return exchange(session.fd, reader, request, options);
    }
    print_error("GENERIC", "daemon restart loop; giving up", json::object());
    return kGeneric;
}

} // namespace tgcli::cli
