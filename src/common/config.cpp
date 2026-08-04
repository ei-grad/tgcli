#include "common/config.hpp"

#include "common/config_test_support.hpp"
#include "common/paths.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#include <thread>
#include <unistd.h>
#include <utility>

#include <toml++/toml.hpp>

namespace tgcli::config {

namespace {

constexpr std::string_view kConfigName = "config.toml";
constexpr std::string_view kLockName = "config.lock";
constexpr std::string_view kTransactionName = ".config.toml.transaction";
constexpr std::string_view kCommittedTransactionName = ".config.toml.transaction.committed";
constexpr std::string_view kReplacementName = ".config.toml.replacement";
std::atomic<std::uint64_t>& temporary_sequence() {
    static std::atomic<std::uint64_t> sequence = 0;
    return sequence;
}

struct FileIdentity {
    dev_t device{};
    ino_t inode{};
    std::uint64_t size{};
    std::uint64_t ctime_nanoseconds{};

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

struct ParsedConfig {
    std::shared_ptr<const ConfigSnapshot> snapshot;
    toml::table document;
};

struct ParseResult {
    std::optional<ParsedConfig> parsed;
    std::optional<ConfigError> error;
};

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
    explicit operator bool() const {
        return fd_ >= 0;
    }

  private:
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
    }
    int fd_;
};

struct DirectoryContext {
    Descriptor canonical_parent;
    Descriptor directory;
    std::string canonical_parent_path;
    std::string directory_name;
    struct stat canonical_parent_status {};
    struct stat directory_status {};

    [[nodiscard]] int get() const {
        return directory.get();
    }
};

ConfigError make_error(ConfigReason reason, std::string message) {
    return ConfigError{reason, std::move(message)};
}

int nofollow_flags(int flags) {
#ifdef O_NOFOLLOW
    return flags | O_NOFOLLOW | O_CLOEXEC;
#else
    return flags | O_CLOEXEC;
#endif
}

std::uint64_t ctime_nanoseconds(const struct stat& status) {
#if defined(__APPLE__)
    const auto seconds = status.st_ctimespec.tv_sec;
    const auto nanoseconds = status.st_ctimespec.tv_nsec;
#else
    const auto seconds = status.st_ctim.tv_sec;
    const auto nanoseconds = status.st_ctim.tv_nsec;
#endif
    return static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(nanoseconds);
}

FileIdentity file_identity(const struct stat& status) {
    return {status.st_dev, status.st_ino, static_cast<std::uint64_t>(status.st_size),
            ctime_nanoseconds(status)};
}

bool same_file(const struct stat& left, const struct stat& right) {
    return file_identity(left) == file_identity(right);
}

bool same_inode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool same_directory_metadata(const struct stat& left, const struct stat& right) {
    return same_inode(left, right) && left.st_uid == right.st_uid &&
           (left.st_mode & (S_IFMT | 07777)) == (right.st_mode & (S_IFMT | 07777));
}

bool injected(const std::shared_ptr<const testing::StoreHooks>& hooks,
              testing::MutationFault fault) {
    return hooks && hooks->should_fail && hooks->should_fail(fault);
}

void notify_stage(const std::shared_ptr<const testing::StoreHooks>& hooks,
                  testing::MutationStage stage) {
    if (hooks && hooks->at_stage) {
        hooks->at_stage(stage);
    }
}

bool lock_entry_matches(int directory_fd, const struct stat& opened, uid_t expected_uid) {
    struct stat entry {};
    return ::fstatat(directory_fd, kLockName.data(), &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
           same_inode(opened, entry) && S_ISREG(entry.st_mode) && entry.st_uid == expected_uid &&
           (entry.st_mode & 07777) == 0600 && entry.st_nlink == 1;
}

bool regular_entry_matches(int directory_fd, std::string_view name, const struct stat& opened,
                           uid_t expected_uid) {
    struct stat entry {};
    return ::fstatat(directory_fd, name.data(), &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
           same_inode(opened, entry) && S_ISREG(entry.st_mode) && entry.st_uid == expected_uid &&
           (entry.st_mode & 07777) == 0600 && entry.st_nlink == 1;
}

bool validate_directory(int fd, uid_t expected_uid, ConfigError& error) {
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        error = make_error(ConfigReason::IoError,
                           std::string("cannot stat config directory: ") + std::strerror(errno));
        return false;
    }
    if (!S_ISDIR(status.st_mode)) {
        error = make_error(ConfigReason::WrongType, "config path parent is not a directory");
        return false;
    }
    if (status.st_uid != expected_uid) {
        error = make_error(ConfigReason::WrongOwner, "config directory has the wrong owner");
        return false;
    }
    if ((status.st_mode & 07777) != 0700) {
        error = make_error(ConfigReason::WrongMode,
                           "config directory has unsafe permissions; expected mode 0700");
        return false;
    }
    return true;
}

bool validate_regular_file(int fd, uid_t expected_uid, std::string_view label, struct stat& status,
                           ConfigError& error) {
    if (::fstat(fd, &status) != 0) {
        error = make_error(ConfigReason::IoError,
                           "cannot stat " + std::string(label) + ": " + std::strerror(errno));
        return false;
    }
    if (!S_ISREG(status.st_mode)) {
        error = make_error(ConfigReason::WrongType, std::string(label) + " is not a regular file");
        return false;
    }
    if (status.st_uid != expected_uid) {
        error = make_error(ConfigReason::WrongOwner, std::string(label) + " has the wrong owner");
        return false;
    }
    if ((status.st_mode & 07777) != 0600) {
        error = make_error(ConfigReason::WrongMode,
                           std::string(label) + " has unsafe permissions; expected mode 0600");
        return false;
    }
    if (status.st_nlink != 1) {
        error = make_error(ConfigReason::WrongLinkCount,
                           std::string(label) + " has an unexpected hard-link count");
        return false;
    }
    return true;
}

// Compact SHA-256 implementation used so the config identity does not acquire
// a platform crypto-library dependency.
std::string sha256(std::string_view input) {
    constexpr std::array<std::uint32_t, 64> round_constants = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U};
    std::array<std::uint32_t, 8> digest = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                           0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::string padded(input);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(padded.size()) * 8U;
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % 64 != 56) {
        padded.push_back('\0');
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<char>((bit_length >> shift) & 0xffU));
    }

    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto byte = [&, index](std::size_t within) {
                return static_cast<std::uint32_t>(
                    static_cast<unsigned char>(padded.at(offset + index * 4 + within)));
            };
            words.at(index) = (byte(0) << 24U) | (byte(1) << 16U) | (byte(2) << 8U) | byte(3);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto prior15 = words.at(index - 15);
            const auto prior2 = words.at(index - 2);
            const auto small0 = std::rotr(prior15, 7) ^ std::rotr(prior15, 18) ^ (prior15 >> 3U);
            const auto small1 = std::rotr(prior2, 17) ^ std::rotr(prior2, 19) ^ (prior2 >> 10U);
            words.at(index) = words.at(index - 16) + small0 + words.at(index - 7) + small1;
        }
        auto [a, b, c, d, e, f, g, h] = digest;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temporary1 = h + sum1 + choose + round_constants.at(index) + words.at(index);
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        digest.at(0) += a;
        digest.at(1) += b;
        digest.at(2) += c;
        digest.at(3) += d;
        digest.at(4) += e;
        digest.at(5) += f;
        digest.at(6) += g;
        digest.at(7) += h;
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : digest) {
        output << std::setw(8) << word;
    }
    return output.str();
}

std::string serialize_identity(std::string_view bytes, const struct stat& status) {
    return "sha256:" + sha256(bytes) +
           ";dev:" + std::to_string(static_cast<std::uint64_t>(status.st_dev)) +
           ";ino:" + std::to_string(static_cast<std::uint64_t>(status.st_ino)) +
           ";size:" + std::to_string(static_cast<std::uint64_t>(status.st_size)) +
           ";ctime_ns:" + std::to_string(ctime_nanoseconds(status));
}

std::optional<std::string> optional_hook(const toml::table& table, std::string_view key,
                                         ConfigError& error) {
    const auto node = table[key];
    if (!node) {
        return std::nullopt;
    }
    auto value = node.value<std::string>();
    if (!value) {
        error = make_error(ConfigReason::TypeError,
                           "account field " + std::string(key) + " must be a string");
        return std::nullopt;
    }
    if (value->empty()) {
        return std::nullopt;
    }
    return value;
}

// Account parsing is a closed sequence of independent schema checks; keeping
// the field names beside their validation makes omissions visible in review.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool parse_account(std::string_view name, const toml::table& table, AccountConfig& account,
                   ConfigError& error) {
    if (!paths::valid_account_name(std::string(name))) {
        error = make_error(ConfigReason::TypeError, "invalid account table name");
        return false;
    }
    for (const std::string_view forbidden : {"bot_token", "password", "database_key", "db_key"}) {
        if (table.contains(forbidden)) {
            error = make_error(ConfigReason::TypeError,
                               "real secrets cannot be stored in plain config fields");
            return false;
        }
    }

    if (const auto node = table["api_id"]; node) {
        const auto value = node.value<std::int64_t>();
        if (!value || *value <= 0 || *value > std::numeric_limits<std::int32_t>::max()) {
            error = make_error(ConfigReason::TypeError,
                               "account field api_id must be a positive signed-32-bit integer");
            return false;
        }
        account.api_id = static_cast<std::int32_t>(*value);
    }
    if (const auto node = table["api_hash"]; node) {
        const auto value = node.value<std::string>();
        if (!value || value->empty()) {
            error = make_error(ConfigReason::TypeError,
                               "account field api_hash must be a non-empty string");
            return false;
        }
        account.api_hash = *value;
    }

    ConfigError hook_error;
    account.api_id_cmd = optional_hook(table, "api_id_cmd", hook_error);
    if (!hook_error.message.empty()) {
        error = std::move(hook_error);
        return false;
    }
    account.api_hash_cmd = optional_hook(table, "api_hash_cmd", hook_error);
    if (!hook_error.message.empty()) {
        error = std::move(hook_error);
        return false;
    }
    account.db_key_cmd = optional_hook(table, "db_key_cmd", hook_error);
    if (!hook_error.message.empty()) {
        error = std::move(hook_error);
        return false;
    }
    account.password_cmd = optional_hook(table, "password_cmd", hook_error);
    if (!hook_error.message.empty()) {
        error = std::move(hook_error);
        return false;
    }
    account.bot_token_cmd = optional_hook(table, "bot_token_cmd", hook_error);
    if (!hook_error.message.empty()) {
        error = std::move(hook_error);
        return false;
    }
    if ((account.api_id && account.api_id_cmd) || (account.api_hash && account.api_hash_cmd)) {
        error = make_error(ConfigReason::ConflictingCredentials,
                           "plain app credentials conflict with configured commands");
        return false;
    }

    if (const auto node = table["allow_write"]; node) {
        const auto value = node.value<bool>();
        if (!value) {
            error =
                make_error(ConfigReason::TypeError, "account field allow_write must be a boolean");
            return false;
        }
        account.allow_write = *value;
    }
    if (const auto node = table["idle_exit"]; node) {
        const auto value = node.value<std::int64_t>();
        if (!value || *value <= 0) {
            error = make_error(ConfigReason::TypeError,
                               "account field idle_exit must be a positive integer");
            return false;
        }
        account.idle_exit = std::chrono::seconds(*value);
    }
    return true;
}

ParseResult parse_document(std::string bytes, std::string identity) {
    toml::table document;
    try {
        document = bytes.empty() ? toml::table{} : toml::parse(bytes);
    } catch (const toml::parse_error&) {
        return {{}, make_error(ConfigReason::ParseError, "config.toml is not valid TOML")};
    }

    auto snapshot = std::make_shared<ConfigSnapshot>();
    snapshot->identity = std::move(identity);
    snapshot->raw_bytes = std::move(bytes);
    if (const auto default_node = document["default_account"]; default_node) {
        const auto value = default_node.value<std::string>();
        if (!value || !paths::valid_account_name(*value)) {
            return {{},
                    make_error(ConfigReason::TypeError,
                               "default_account must be a valid account name")};
        }
        snapshot->default_account = *value;
    }

    if (const auto accounts_node = document["accounts"]; accounts_node) {
        const auto* accounts_table = accounts_node.as_table();
        if (accounts_table == nullptr) {
            return {{}, make_error(ConfigReason::TypeError, "accounts must be a table")};
        }
        for (const auto& [key, node] : *accounts_table) {
            const auto* account_table = node.as_table();
            if (account_table == nullptr) {
                return {{},
                        make_error(ConfigReason::TypeError, "each account entry must be a table")};
            }
            AccountConfig account;
            ConfigError error;
            if (!parse_account(key.str(), *account_table, account, error)) {
                return {{}, std::move(error)};
            }
            snapshot->accounts.emplace(key.str(), std::move(account));
        }
    }
    const std::string default_account = snapshot->default_account.value_or("");
    if (!default_account.empty() && !snapshot->accounts.contains(default_account)) {
        return {{},
                make_error(ConfigReason::InvalidDefault,
                           "default_account does not name a configured account")};
    }
    return {ParsedConfig{std::move(snapshot), std::move(document)}, {}};
}

LoadResult to_load_result(ParseResult parsed) {
    if (!parsed.parsed) {
        return {{}, std::move(parsed.error)};
    }
    return {std::move(parsed.parsed->snapshot), {}};
}

std::optional<std::string> split_parent(const std::string& config_path, ConfigError& error) {
    if (config_path.empty() || config_path.front() != '/') {
        error = make_error(ConfigReason::PathInvalid, "config path must be absolute");
        return std::nullopt;
    }
    const auto separator = config_path.rfind('/');
    if (separator == std::string::npos || separator == 0 ||
        config_path.substr(separator + 1) != kConfigName) {
        error = make_error(ConfigReason::PathInvalid, "config path must end in config.toml");
        return std::nullopt;
    }
    return config_path.substr(0, separator);
}

// Directory creation and retained/fresh identity checks form one ordered security boundary.
// NOLINTBEGIN(readability-function-cognitive-complexity)
std::optional<DirectoryContext>
open_config_directory(const std::string& config_path, uid_t expected_uid, bool create,
                      const std::shared_ptr<const testing::StoreHooks>& hooks, bool& missing,
                      ConfigError& error) {
    const auto directory_path = split_parent(config_path, error);
    if (!directory_path) {
        return std::nullopt;
    }
    const auto separator = directory_path->rfind('/');
    if (separator == std::string::npos || directory_path->substr(separator + 1).empty()) {
        error =
            make_error(ConfigReason::PathInvalid, "config directory must have a canonical parent");
        return std::nullopt;
    }
    DirectoryContext context;
    context.canonical_parent_path = separator == 0 ? "/" : directory_path->substr(0, separator);
    context.directory_name = directory_path->substr(separator + 1);
    missing = false;
    const int parent_fd =
        ::open(context.canonical_parent_path.c_str(), nofollow_flags(O_RDONLY | O_DIRECTORY));
    if (parent_fd < 0 && errno == ENOENT && !create) {
        missing = true;
        return context;
    }
    if (parent_fd < 0) {
        const auto reason =
            errno == ELOOP || errno == ENOTDIR ? ConfigReason::WrongType : ConfigReason::IoError;
        error = make_error(reason, "cannot open canonical config parent: " +
                                       std::string(std::strerror(errno)));
        return std::nullopt;
    }
    context.canonical_parent = Descriptor(parent_fd);
    if (::fstat(context.canonical_parent.get(), &context.canonical_parent_status) != 0) {
        error = make_error(ConfigReason::IoError, "cannot stat canonical config parent");
        return std::nullopt;
    }

    int directory_fd = ::openat(context.canonical_parent.get(), context.directory_name.c_str(),
                                nofollow_flags(O_RDONLY | O_DIRECTORY));
    if (directory_fd < 0 && errno == ENOENT && !create) {
        missing = true;
        return context;
    }
    if (directory_fd < 0 && errno == ENOENT && create) {
        if (::mkdirat(context.canonical_parent.get(), context.directory_name.c_str(), 0700) != 0 &&
            errno != EEXIST) {
            error = make_error(ConfigReason::IoError, "cannot create config directory: " +
                                                          std::string(std::strerror(errno)));
            return std::nullopt;
        }
        if (injected(hooks, testing::MutationFault::ParentDirectorySync) ||
            ::fsync(context.canonical_parent.get()) != 0) {
            error = make_error(ConfigReason::SyncError,
                               "cannot sync canonical parent after creating config directory");
            return std::nullopt;
        }
        directory_fd = ::openat(context.canonical_parent.get(), context.directory_name.c_str(),
                                nofollow_flags(O_RDONLY | O_DIRECTORY));
    }
    if (directory_fd < 0) {
        const auto reason =
            errno == ELOOP || errno == ENOTDIR ? ConfigReason::WrongType : ConfigReason::IoError;
        error = make_error(reason,
                           "cannot open config directory: " + std::string(std::strerror(errno)));
        return std::nullopt;
    }
    context.directory = Descriptor(directory_fd);
    if (!validate_directory(context.directory.get(), expected_uid, error) ||
        ::fstat(context.directory.get(), &context.directory_status) != 0) {
        if (error.message.empty()) {
            error = make_error(ConfigReason::IoError, "cannot retain config directory identity");
        }
        return std::nullopt;
    }
    return context;
}
// NOLINTEND(readability-function-cognitive-complexity)

bool canonical_directory_matches(const DirectoryContext& context, uid_t expected_uid,
                                 ConfigError& error) {
    struct stat retained_parent {};
    struct stat retained_directory {};
    struct stat directory_entry {};
    if (::fstat(context.canonical_parent.get(), &retained_parent) != 0 ||
        ::fstat(context.directory.get(), &retained_directory) != 0 ||
        ::fstatat(context.canonical_parent.get(), context.directory_name.c_str(), &directory_entry,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        error =
            make_error(ConfigReason::PathInvalid, "canonical config directory became inaccessible");
        return false;
    }
    const int fresh_parent_fd =
        ::open(context.canonical_parent_path.c_str(), nofollow_flags(O_RDONLY | O_DIRECTORY));
    if (fresh_parent_fd < 0) {
        error = make_error(ConfigReason::PathInvalid, "canonical config parent was replaced");
        return false;
    }
    const Descriptor fresh_parent(fresh_parent_fd);
    struct stat fresh_parent_status {};
    if (::fstat(fresh_parent.get(), &fresh_parent_status) != 0 ||
        !same_directory_metadata(context.canonical_parent_status, retained_parent) ||
        !same_directory_metadata(context.canonical_parent_status, fresh_parent_status) ||
        !same_directory_metadata(context.directory_status, retained_directory) ||
        !same_directory_metadata(context.directory_status, directory_entry) ||
        retained_directory.st_uid != expected_uid || (retained_directory.st_mode & 07777) != 0700) {
        error = make_error(ConfigReason::PathInvalid,
                           "canonical config directory identity or permissions changed");
        return false;
    }
    return true;
}

ParseResult read_from_directory(int directory_fd, uid_t expected_uid) {
    const int raw_fd = ::openat(directory_fd, kConfigName.data(), nofollow_flags(O_RDONLY));
    if (raw_fd < 0 && errno == ENOENT) {
        auto snapshot = std::make_shared<ConfigSnapshot>();
        snapshot->identity = "missing";
        return {ParsedConfig{std::move(snapshot), toml::table{}}, {}};
    }
    if (raw_fd < 0) {
        const auto reason = errno == ELOOP ? ConfigReason::WrongType : ConfigReason::IoError;
        return {
            {},
            make_error(reason, "cannot open config.toml: " + std::string(std::strerror(errno)))};
    }
    const Descriptor file(raw_fd);
    struct stat before {};
    ConfigError error;
    if (!validate_regular_file(file.get(), expected_uid, "config.toml", before, error)) {
        if (error.reason == ConfigReason::WrongLinkCount && before.st_nlink == 0) {
            return {{},
                    make_error(ConfigReason::PathInvalid,
                               "config.toml was atomically replaced while opening")};
        }
        return {{}, std::move(error)};
    }
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > kMaxConfigBytes) {
        return {{}, make_error(ConfigReason::TooLarge, "config.toml exceeds 1 MiB")};
    }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::pread(file.get(), bytes.data() + offset, bytes.size() - offset,
                                   static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return {{}, make_error(ConfigReason::IoError, "cannot read complete config.toml")};
        }
        offset += static_cast<std::size_t>(count);
    }
    struct stat after {};
    struct stat entry {};
    if (::fstat(file.get(), &after) != 0 ||
        ::fstatat(directory_fd, kConfigName.data(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
        return {{}, make_error(ConfigReason::IoError, "cannot validate config.toml after reading")};
    }
    if (!same_file(before, after) || !same_file(after, entry)) {
        return {{},
                make_error(ConfigReason::PathInvalid,
                           "config.toml changed or was replaced while reading")};
    }
    const std::string identity = serialize_identity(bytes, after);
    return parse_document(std::move(bytes), identity);
}

std::string serialize_document(const toml::table& document) {
    std::ostringstream output;
#if defined(__clang_analyzer__)
    // toml++ bitmask operators intentionally combine values outside named enum constants.
    (void)document;
    output << std::string_view{};
#else
    output << document;
#endif
    std::string bytes = output.str();
    if (bytes.empty() || bytes.back() != '\n') {
        bytes.push_back('\n');
    }
    return bytes;
}

bool write_all(int fd, std::string_view bytes, ConfigError& error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error = make_error(ConfigReason::IoError, "cannot write replacement config.toml");
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool descriptor_content_matches(int fd, const struct stat& retained, uid_t expected_uid,
                                std::string_view expected_hash, bool require_same_metadata,
                                ConfigError& error) {
    struct stat before {};
    if (!validate_regular_file(fd, expected_uid, "staged replacement config.toml", before, error) ||
        (require_same_metadata ? !same_file(retained, before) : !same_inode(retained, before))) {
        error =
            make_error(ConfigReason::PathInvalid, "staged replacement descriptor identity changed");
        return false;
    }
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > kMaxConfigBytes) {
        error = make_error(ConfigReason::PathInvalid, "staged replacement descriptor size changed");
        return false;
    }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count =
            ::pread(fd, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error = make_error(ConfigReason::PathInvalid,
                               "cannot read complete staged replacement config.toml");
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    struct stat after {};
    if (::fstat(fd, &after) != 0 || !same_file(before, after) || sha256(bytes) != expected_hash) {
        error =
            make_error(ConfigReason::PathInvalid, "staged replacement content or identity changed");
        return false;
    }
    return true;
}

bool exchange_entries(int directory_fd, std::string_view first, std::string_view second) {
#if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int kRenameExchange = 2U;
    return ::syscall(SYS_renameat2, directory_fd, first.data(), directory_fd, second.data(),
                     kRenameExchange) == 0;
#elif defined(__APPLE__)
    return ::renameatx_np(directory_fd, first.data(), directory_fd, second.data(), RENAME_SWAP) ==
           0;
#else
    (void)directory_fd;
    (void)first;
    (void)second;
    errno = ENOTSUP;
    return false;
#endif
}

std::optional<std::string> read_secure_named_file(int directory_fd, std::string_view name,
                                                  uid_t expected_uid, std::size_t maximum,
                                                  bool& missing, ConfigError& error) {
    missing = false;
    const int raw_fd = ::openat(directory_fd, name.data(), nofollow_flags(O_RDONLY));
    if (raw_fd < 0 && errno == ENOENT) {
        missing = true;
        return std::nullopt;
    }
    if (raw_fd < 0) {
        error = make_error(ConfigReason::SyncError,
                           "cannot open config transaction file " + std::string(name));
        return std::nullopt;
    }
    const Descriptor file(raw_fd);
    struct stat status {};
    if (!validate_regular_file(file.get(), expected_uid, name, status, error)) {
        error.reason = ConfigReason::SyncError;
        return std::nullopt;
    }
    if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > maximum) {
        error = make_error(ConfigReason::SyncError, "config transaction file has an invalid size");
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::pread(file.get(), bytes.data() + offset, bytes.size() - offset,
                                      static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error =
                make_error(ConfigReason::SyncError, "cannot read complete config transaction file");
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(count);
    }
    return bytes;
}

bool write_transaction_file(int directory_fd, std::string_view name, std::string_view bytes,
                            uid_t expected_uid, ConfigError& error) {
    const int raw_fd =
        ::openat(directory_fd, name.data(), nofollow_flags(O_WRONLY | O_CREAT | O_EXCL), 0600);
    if (raw_fd < 0) {
        error = make_error(ConfigReason::SyncError, "cannot create config transaction marker: " +
                                                        std::string(std::strerror(errno)));
        return false;
    }
    const Descriptor file(raw_fd);
    struct stat status {};
    if (!validate_regular_file(file.get(), expected_uid, name, status, error) ||
        !write_all(file.get(), bytes, error) || ::fsync(file.get()) != 0) {
        error = make_error(ConfigReason::SyncError, "cannot sync config transaction marker");
        return false;
    }
    return true;
}

bool unlink_if_present(int directory_fd, std::string_view name) {
    return ::unlinkat(directory_fd, name.data(), 0) == 0 || errno == ENOENT;
}

enum class TransactionState { PendingPresent, PendingMissing, CommittedPresent, CommittedMissing };

struct TransactionRecord {
    TransactionState state;
    std::string old_hash;
    std::string new_hash;
};

std::optional<TransactionRecord> parse_transaction(std::string_view bytes) {
    std::istringstream input{std::string(bytes)};
    std::string state;
    TransactionRecord record{};
    std::string extra;
    if (!std::getline(input, state) || !std::getline(input, record.old_hash) ||
        !std::getline(input, record.new_hash) || std::getline(input, extra)) {
        return std::nullopt;
    }
    if (record.new_hash.size() != 64 ||
        (!record.old_hash.empty() && record.old_hash.size() != 64)) {
        return std::nullopt;
    }
    if (state == "pending-present") {
        record.state = TransactionState::PendingPresent;
    } else if (state == "pending-missing") {
        record.state = TransactionState::PendingMissing;
    } else if (state == "committed-present") {
        record.state = TransactionState::CommittedPresent;
    } else if (state == "committed-missing") {
        record.state = TransactionState::CommittedMissing;
    } else {
        return std::nullopt;
    }
    return record;
}

std::string transaction_bytes(bool committed, bool old_present, std::string_view old_hash,
                              std::string_view new_hash) {
    return std::string(committed ? "committed-" : "pending-") +
           (old_present ? "present\n" : "missing\n") + std::string(old_hash) + "\n" +
           std::string(new_hash) + "\n";
}

bool transaction_marker_present(int directory_fd) {
    struct stat status {};
    return ::fstatat(directory_fd, kTransactionName.data(), &status, AT_SYMLINK_NOFOLLOW) == 0 ||
           errno != ENOENT;
}

bool transaction_is_active(int directory_fd, uid_t expected_uid, ConfigError& error) {
    const int raw_lock = ::openat(directory_fd, kLockName.data(), nofollow_flags(O_RDONLY));
    if (raw_lock < 0) {
        error = make_error(ConfigReason::SyncError,
                           "config transaction exists without a valid config.lock");
        return false;
    }
    const Descriptor lock(raw_lock);
    struct stat lock_status {};
    if (!validate_regular_file(lock.get(), expected_uid, kLockName, lock_status, error)) {
        error.reason = ConfigReason::SyncError;
        return false;
    }
    if (::flock(lock.get(), LOCK_SH | LOCK_NB) == 0) {
        ::flock(lock.get(), LOCK_UN);
        return false;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return true;
    }
    error = make_error(ConfigReason::SyncError,
                       "cannot determine whether config transaction is active");
    return false;
}

bool cleanup_transaction(int directory_fd, ConfigError& error) {
    if (!unlink_if_present(directory_fd, kReplacementName) ||
        !unlink_if_present(directory_fd, kCommittedTransactionName) ||
        !unlink_if_present(directory_fd, kTransactionName) || ::fsync(directory_fd) != 0) {
        error = make_error(ConfigReason::SyncError, "cannot clean config transaction state");
        return false;
    }
    return true;
}

// Recovery exhaustively distinguishes every durable pending/committed file layout.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool recover_transaction(int directory_fd, uid_t expected_uid, ConfigError& error) {
    bool marker_missing = false;
    const auto marker = read_secure_named_file(directory_fd, kTransactionName, expected_uid, 256,
                                               marker_missing, error);
    if (marker_missing) {
        if (!unlink_if_present(directory_fd, kReplacementName) ||
            !unlink_if_present(directory_fd, kCommittedTransactionName) ||
            ::fsync(directory_fd) != 0) {
            error = make_error(ConfigReason::SyncError,
                               "cannot remove orphaned config transaction state");
            return false;
        }
        return true;
    }
    if (!marker) {
        return false;
    }
    const auto record = parse_transaction(*marker);
    if (!record) {
        error = make_error(ConfigReason::SyncError, "config transaction marker is invalid");
        return false;
    }

    bool config_missing = false;
    const auto config = read_secure_named_file(directory_fd, kConfigName, expected_uid,
                                               kMaxConfigBytes, config_missing, error);
    bool replacement_missing = false;
    const auto replacement = read_secure_named_file(directory_fd, kReplacementName, expected_uid,
                                                    kMaxConfigBytes, replacement_missing, error);
    if ((!config && !config_missing) || (!replacement && !replacement_missing)) {
        return false;
    }

    const bool committed = record->state == TransactionState::CommittedPresent ||
                           record->state == TransactionState::CommittedMissing;
    if (committed) {
        if (!config || sha256(*config) != record->new_hash) {
            error = make_error(ConfigReason::SyncError,
                               "committed config transaction does not match config.toml");
            return false;
        }
        return cleanup_transaction(directory_fd, error);
    }

    if (record->state == TransactionState::PendingPresent) {
        if (!config || !replacement) {
            error = make_error(ConfigReason::SyncError,
                               "pending config transaction is missing a rollback entry");
            return false;
        }
        if (sha256(*config) == record->new_hash && sha256(*replacement) == record->old_hash) {
            if (!exchange_entries(directory_fd, kConfigName, kReplacementName)) {
                error = make_error(ConfigReason::SyncError,
                                   "cannot restore pending config transaction");
                return false;
            }
        } else if (sha256(*config) != record->old_hash ||
                   sha256(*replacement) != record->new_hash) {
            error = make_error(ConfigReason::SyncError,
                               "pending config transaction identity is ambiguous");
            return false;
        }
    } else {
        if (config && sha256(*config) == record->new_hash && replacement_missing) {
            if (::unlinkat(directory_fd, kConfigName.data(), 0) != 0) {
                error =
                    make_error(ConfigReason::SyncError, "cannot remove an uncommitted config.toml");
                return false;
            }
        } else if (!(config_missing && replacement_missing) &&
                   !(config_missing && replacement && sha256(*replacement) == record->new_hash)) {
            error = make_error(ConfigReason::SyncError,
                               "pending first-run config transaction is ambiguous");
            return false;
        }
    }
    if (::fsync(directory_fd) != 0) {
        error = make_error(ConfigReason::SyncError, "cannot sync recovered config transaction");
        return false;
    }
    return cleanup_transaction(directory_fd, error);
}

// Each mutation variant carries separate contract preconditions. The switch is
// intentionally centralized so no transform can bypass common validation.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool apply_mutation(toml::table& document, const Store::Mutation& mutation, ConfigError& error) {
    auto* accounts = document["accounts"].as_table();
    if (accounts == nullptr) {
        document.insert_or_assign("accounts", toml::table{});
        accounts = document["accounts"].as_table();
    }
    const bool exists = accounts->contains(mutation.account);
    switch (mutation.kind) {
    case Store::MutationKind::Add: {
        if (exists) {
            error = make_error(ConfigReason::TypeError, "account already exists");
            return false;
        }
        const bool was_empty = accounts->empty();
        toml::table account;
        account.insert("allow_write", false);
        accounts->insert(mutation.account, std::move(account));
        if (was_empty && !document.contains("default_account")) {
            document.insert_or_assign("default_account", mutation.account);
        }
        return true;
    }
    case Store::MutationKind::Use:
        if (!exists) {
            error = make_error(ConfigReason::TypeError, "account does not exist");
            return false;
        }
        document.insert_or_assign("default_account", mutation.account);
        return true;
    case Store::MutationKind::Remove: {
        if (!exists) {
            error = make_error(ConfigReason::TypeError, "account does not exist");
            return false;
        }
        const auto current_default = document["default_account"].value<std::string>();
        if (mutation.reassign_default &&
            (!current_default || *current_default != mutation.account)) {
            error = make_error(ConfigReason::TypeError,
                               "default reassignment requires removing the current default");
            return false;
        }
        if (current_default && *current_default == mutation.account) {
            if (accounts->size() == 1) {
                if (mutation.reassign_default) {
                    error = make_error(ConfigReason::TypeError,
                                       "cannot reassign the sole configured account");
                    return false;
                }
                document.erase("default_account");
            } else {
                if (!mutation.reassign_default || *mutation.reassign_default == mutation.account ||
                    !accounts->contains(*mutation.reassign_default)) {
                    error = make_error(ConfigReason::InvalidDefault,
                                       "removing the default requires an existing replacement");
                    return false;
                }
                document.insert_or_assign("default_account", *mutation.reassign_default);
            }
        }
        accounts->erase(mutation.account);
        return true;
    }
    case Store::MutationKind::MaterializeMain: {
        if (exists) {
            error = make_error(ConfigReason::TypeError, "implicit main is already configured");
            return false;
        }
        if (mutation.prompted.api_id.has_value() != mutation.prompted.api_hash.has_value()) {
            error = make_error(ConfigReason::TypeError,
                               "prompted app credentials must be persisted together");
            return false;
        }
        toml::table account;
        account.insert("allow_write", false);
        if (mutation.prompted.api_id) {
            account.insert("api_id", *mutation.prompted.api_id);
            account.insert("api_hash", *mutation.prompted.api_hash);
        }
        accounts->insert("main", std::move(account));
        if (!document.contains("default_account")) {
            document.insert_or_assign("default_account", "main");
        }
        return true;
    }
    }
    throw std::logic_error("unhandled config mutation");
}

} // namespace

std::string_view reason_name(ConfigReason reason) {
    switch (reason) {
    case ConfigReason::PathInvalid:
        return "path_invalid";
    case ConfigReason::WrongOwner:
        return "wrong_owner";
    case ConfigReason::WrongType:
        return "wrong_type";
    case ConfigReason::WrongMode:
        return "wrong_mode";
    case ConfigReason::WrongLinkCount:
        return "wrong_link_count";
    case ConfigReason::TooLarge:
        return "too_large";
    case ConfigReason::ParseError:
        return "parse_error";
    case ConfigReason::TypeError:
        return "type_error";
    case ConfigReason::InvalidDefault:
        return "invalid_default";
    case ConfigReason::ConflictingCredentials:
        return "conflicting_credentials";
    case ConfigReason::IoError:
        return "io_error";
    case ConfigReason::SyncError:
        return "sync_error";
    }
    throw std::logic_error("unhandled config reason");
}

Store::Store(std::string config_path, uid_t expected_uid)
    : config_path_(std::move(config_path)),
      expected_uid_(expected_uid == static_cast<uid_t>(-1) ? ::getuid() : expected_uid) {}

Store::Store(std::string config_path, std::shared_ptr<const testing::StoreHooks> hooks,
             uid_t expected_uid)
    : config_path_(std::move(config_path)),
      expected_uid_(expected_uid == static_cast<uid_t>(-1) ? ::getuid() : expected_uid),
      hooks_(std::move(hooks)) {}

LoadResult Store::load(const MutationControl& control) const {
    const auto interrupted = [&control]() -> std::optional<LoadResult> {
        if (control.cancellation.stop_requested()) {
            return LoadResult{{}, {}, false, true};
        }
        if (std::chrono::steady_clock::now() >= control.deadline) {
            return LoadResult{{}, {}, true, false};
        }
        return std::nullopt;
    };
    if (auto result = interrupted()) {
        return std::move(*result);
    }
    ConfigError error;
    bool missing = false;
    auto directory =
        open_config_directory(config_path_, expected_uid_, false, hooks_, missing, error);
    if (!directory) {
        return {{}, std::move(error)};
    }
    if (missing) {
        auto snapshot = std::make_shared<ConfigSnapshot>();
        snapshot->identity = "missing";
        return {std::move(snapshot), {}};
    }
    for (int attempt = 0; attempt < 256; ++attempt) {
        if (auto result = interrupted()) {
            return std::move(*result);
        }
        if (transaction_marker_present(directory->get())) {
            if (transaction_is_active(directory->get(), expected_uid_, error)) {
                const auto wake = std::min(control.deadline, std::chrono::steady_clock::now() +
                                                                 std::chrono::milliseconds(1));
                std::this_thread::sleep_until(wake);
                continue;
            }
            if (error.message.empty()) {
                error = make_error(ConfigReason::SyncError,
                                   "config transaction requires recovery under config.lock");
            }
            return {{}, std::move(error)};
        }
        auto loaded = read_from_directory(directory->get(), expected_uid_);
        if (auto result = interrupted()) {
            return std::move(*result);
        }
        if (loaded.parsed || !loaded.error || loaded.error->reason != ConfigReason::PathInvalid) {
            return to_load_result(std::move(loaded));
        }
    }
    return {{},
            make_error(ConfigReason::IoError,
                       "config.toml kept changing while a stable snapshot was read")};
}

MutationResult Store::add_account(std::string_view expected_identity, std::string_view account,
                                  const MutationControl& control) const {
    return mutate(expected_identity, Mutation{MutationKind::Add, std::string(account), {}, {}},
                  control);
}

MutationResult Store::use_account(std::string_view expected_identity, std::string_view account,
                                  const MutationControl& control) const {
    return mutate(expected_identity, Mutation{MutationKind::Use, std::string(account), {}, {}},
                  control);
}

MutationResult Store::remove_account(std::string_view expected_identity, std::string_view account,
                                     std::optional<std::string_view> reassign_default,
                                     const MutationControl& control) const {
    return mutate(
        expected_identity,
        Mutation{MutationKind::Remove,
                 std::string(account),
                 reassign_default ? std::optional<std::string>(*reassign_default) : std::nullopt,
                 {}},
        control);
}

MutationResult Store::materialize_implicit_main(std::string_view expected_identity,
                                                const PromptedAppCredentials& prompted,
                                                const MutationControl& control) const {
    return mutate(expected_identity, Mutation{MutationKind::MaterializeMain, "main", {}, prompted},
                  control);
}

// This ordered verify-write-verify-rename transaction is kept linear so every
// failure is visibly before or after the atomic replacement boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
MutationResult Store::mutate(std::string_view expected_identity, const Mutation& mutation,
                             const MutationControl& control) const {
    if (control.cancellation.stop_requested()) {
        return {MutationStatus::Cancelled, {}, {}};
    }
    if (std::chrono::steady_clock::now() >= control.deadline) {
        return {MutationStatus::TimedOut, {}, {}};
    }
    if (!paths::valid_account_name(mutation.account) ||
        (mutation.reassign_default && !paths::valid_account_name(*mutation.reassign_default))) {
        return {MutationStatus::Invalid,
                {},
                make_error(ConfigReason::PathInvalid, "invalid account name")};
    }
    ConfigError error;
    bool missing = false;
    auto directory =
        open_config_directory(config_path_, expected_uid_, true, hooks_, missing, error);
    if (!directory) {
        return {MutationStatus::IoError, {}, std::move(error)};
    }

    const int raw_lock =
        ::openat(directory->get(), kLockName.data(), nofollow_flags(O_RDWR | O_CREAT), 0600);
    if (raw_lock < 0) {
        const auto reason = errno == ELOOP ? ConfigReason::WrongType : ConfigReason::IoError;
        return {
            MutationStatus::IoError,
            {},
            make_error(reason, "cannot open config.lock: " + std::string(std::strerror(errno)))};
    }
    const Descriptor lock(raw_lock);
    struct stat lock_status {};
    if (!validate_regular_file(lock.get(), expected_uid_, "config.lock", lock_status, error)) {
        return {MutationStatus::IoError, {}, std::move(error)};
    }
    while (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        const int lock_error = errno;
        if (control.cancellation.stop_requested()) {
            return {MutationStatus::Cancelled, {}, {}};
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= control.deadline) {
            return {MutationStatus::TimedOut, {}, {}};
        }
        if (lock_error != EINTR && lock_error != EWOULDBLOCK && lock_error != EAGAIN) {
            return {MutationStatus::IoError,
                    {},
                    make_error(ConfigReason::IoError, "cannot lock config.lock: " +
                                                          std::string(std::strerror(lock_error)))};
        }
        const auto remaining = control.deadline - now;
        std::this_thread::sleep_for(
            std::min(std::chrono::milliseconds(1),
                     std::chrono::duration_cast<std::chrono::milliseconds>(remaining)));
    }
    struct stat lock_entry {};
    if (::fstatat(directory->get(), kLockName.data(), &lock_entry, AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_inode(lock_status, lock_entry) || !S_ISREG(lock_entry.st_mode) ||
        lock_entry.st_uid != expected_uid_ || (lock_entry.st_mode & 07777) != 0600 ||
        lock_entry.st_nlink != 1) {
        return {MutationStatus::IoError,
                {},
                make_error(ConfigReason::PathInvalid,
                           "config.lock changed or was replaced while acquiring it")};
    }
    notify_stage(hooks_, testing::MutationStage::AfterLock);
    if (!canonical_directory_matches(*directory, expected_uid_, error)) {
        return {MutationStatus::IoError, {}, std::move(error)};
    }
    if (!recover_transaction(directory->get(), expected_uid_, error)) {
        return {MutationStatus::DurabilityUnknown, {}, std::move(error)};
    }

    auto current = read_from_directory(directory->get(), expected_uid_);
    if (!current.parsed) {
        return {MutationStatus::Invalid, {}, std::move(current.error)};
    }
    ParsedConfig current_config = std::move(*current.parsed);
    if (current_config.snapshot->identity != expected_identity) {
        return {MutationStatus::Conflict, std::move(current_config.snapshot), {}};
    }
    if (!apply_mutation(current_config.document, mutation, error)) {
        return {MutationStatus::Invalid, std::move(current_config.snapshot), std::move(error)};
    }
    const std::string bytes = serialize_document(current_config.document);
    if (bytes.size() > kMaxConfigBytes) {
        return {MutationStatus::Invalid, std::move(current_config.snapshot),
                make_error(ConfigReason::TooLarge, "config mutation would exceed the 1 MiB limit")};
    }
    auto validated = parse_document(bytes, "pending");
    if (!validated.parsed) {
        return {MutationStatus::Invalid, {}, std::move(validated.error)};
    }

    Descriptor temporary;
    std::string temporary_name;
    for (int attempt = 0; attempt < 128; ++attempt) {
        temporary_name = ".config.toml.tmp." + std::to_string(::getpid()) + "." +
                         std::to_string(temporary_sequence().fetch_add(1));
        const int fd = ::openat(directory->get(), temporary_name.c_str(),
                                nofollow_flags(O_RDWR | O_CREAT | O_EXCL), 0600);
        if (fd >= 0) {
            temporary = Descriptor(fd);
            break;
        }
        if (errno != EEXIST) {
            return {MutationStatus::IoError,
                    {},
                    make_error(ConfigReason::IoError, "cannot create replacement config.toml: " +
                                                          std::string(std::strerror(errno)))};
        }
    }
    if (!temporary) {
        return {
            MutationStatus::IoError,
            {},
            make_error(ConfigReason::IoError, "cannot allocate a unique replacement config.toml")};
    }
    const auto discard_temporary = [&] { ::unlinkat(directory->get(), temporary_name.c_str(), 0); };
    struct stat temporary_status {};
    if (::fchmod(temporary.get(), 0600) != 0 ||
        !validate_regular_file(temporary.get(), expected_uid_, "temporary config.toml",
                               temporary_status, error) ||
        !write_all(temporary.get(), bytes, error)) {
        discard_temporary();
        if (error.message.empty()) {
            error =
                make_error(ConfigReason::IoError, "cannot set replacement config.toml permissions");
        }
        return {MutationStatus::IoError, {}, std::move(error)};
    }
    if (injected(hooks_, testing::MutationFault::TemporaryFileSync) ||
        ::fsync(temporary.get()) != 0) {
        discard_temporary();
        return {MutationStatus::IoError,
                {},
                make_error(ConfigReason::SyncError, "cannot sync replacement config.toml")};
    }

    if (!validate_regular_file(temporary.get(), expected_uid_, "temporary config.toml",
                               temporary_status, error)) {
        discard_temporary();
        return {MutationStatus::IoError, {}, std::move(error)};
    }

    // Detect a path replacement after planning/writing but before rename.
    auto final_check = read_from_directory(directory->get(), expected_uid_);
    if (!final_check.parsed) {
        discard_temporary();
        return {MutationStatus::Invalid, {}, std::move(final_check.error)};
    }
    if (final_check.parsed->snapshot->identity != expected_identity) {
        discard_temporary();
        return {MutationStatus::Conflict, std::move(final_check.parsed->snapshot), {}};
    }
    notify_stage(hooks_, testing::MutationStage::BeforeCommit);
    if (control.commit_admission && !control.commit_admission()) {
        discard_temporary();
        return {MutationStatus::PreconditionFailed, {}, {}};
    }
    if (!lock_entry_matches(directory->get(), lock_status, expected_uid_)) {
        discard_temporary();
        return {MutationStatus::IoError,
                {},
                make_error(ConfigReason::PathInvalid,
                           "config.lock changed or was replaced before commit")};
    }
    if (!regular_entry_matches(directory->get(), temporary_name, temporary_status, expected_uid_)) {
        discard_temporary();
        return {
            MutationStatus::IoError,
            {},
            make_error(ConfigReason::PathInvalid, "replacement config.toml changed before commit")};
    }
    if (!canonical_directory_matches(*directory, expected_uid_, error)) {
        discard_temporary();
        return {MutationStatus::IoError, {}, std::move(error)};
    }
    const bool old_present = current_config.snapshot->identity != "missing";
    const std::string old_hash = old_present ? sha256(current_config.snapshot->raw_bytes) : "";
    const std::string new_hash = sha256(bytes);
    if (::renameat(directory->get(), temporary_name.c_str(), directory->get(),
                   kReplacementName.data()) != 0) {
        discard_temporary();
        return {
            MutationStatus::IoError,
            {},
            make_error(ConfigReason::IoError, "cannot stage replacement config.toml transaction")};
    }
    temporary_name.clear();
    const std::string pending = transaction_bytes(false, old_present, old_hash, new_hash);
    if (!write_transaction_file(directory->get(), kTransactionName, pending, expected_uid_,
                                error) ||
        ::fsync(directory->get()) != 0) {
        cleanup_transaction(directory->get(), error);
        return {MutationStatus::IoError,
                {},
                make_error(ConfigReason::SyncError, "cannot prepare durable config transaction")};
    }

    struct stat staged_status {};
    if (!validate_regular_file(temporary.get(), expected_uid_, "staged replacement config.toml",
                               staged_status, error)) {
        if (!cleanup_transaction(directory->get(), error)) {
            return {MutationStatus::DurabilityUnknown, {}, std::move(error)};
        }
        return {MutationStatus::IoError, {}, std::move(error)};
    }
    notify_stage(hooks_, testing::MutationStage::AfterPrepare);
    if (!regular_entry_matches(directory->get(), kReplacementName, staged_status, expected_uid_) ||
        !descriptor_content_matches(temporary.get(), staged_status, expected_uid_, new_hash, true,
                                    error)) {
        if (!cleanup_transaction(directory->get(), error)) {
            return {MutationStatus::DurabilityUnknown, {}, std::move(error)};
        }
        return {MutationStatus::IoError,
                {},
                make_error(ConfigReason::PathInvalid,
                           "staged replacement changed after transaction preparation")};
    }

    const bool commit_rename_failed =
        injected(hooks_, testing::MutationFault::ReplacementRename) ||
        (old_present ? !exchange_entries(directory->get(), kConfigName, kReplacementName)
                     : ::renameat(directory->get(), kReplacementName.data(), directory->get(),
                                  kConfigName.data()) != 0);
    if (commit_rename_failed) {
        cleanup_transaction(directory->get(), error);
        return {MutationStatus::IoError,
                {},
                make_error(ConfigReason::IoError, "cannot atomically replace config.toml: " +
                                                      std::string(std::strerror(errno)))};
    }
    notify_stage(hooks_, testing::MutationStage::AfterExchange);

    const bool commit_sync_failed = injected(hooks_, testing::MutationFault::CommitDirectorySync) ||
                                    ::fsync(directory->get()) != 0;
    if (commit_sync_failed) {
        const bool rollback_rename_failed =
            injected(hooks_, testing::MutationFault::RollbackRename) ||
            (old_present ? !exchange_entries(directory->get(), kConfigName, kReplacementName)
                         : ::unlinkat(directory->get(), kConfigName.data(), 0) != 0);
        if (rollback_rename_failed ||
            injected(hooks_, testing::MutationFault::RollbackDirectorySync) ||
            ::fsync(directory->get()) != 0) {
            return {MutationStatus::DurabilityUnknown,
                    {},
                    make_error(ConfigReason::SyncError,
                               "config commit and rollback durability are unknown")};
        }
        return {MutationStatus::DurabilityUnknown,
                {},
                make_error(ConfigReason::SyncError,
                           "config directory sync failed; rollback requires locked recovery")};
    }

    bool canonical_missing = false;
    const auto canonical = read_secure_named_file(directory->get(), kConfigName, expected_uid_,
                                                  kMaxConfigBytes, canonical_missing, error);
    const bool replacement_matches =
        !canonical_missing && canonical && sha256(*canonical) == new_hash &&
        regular_entry_matches(directory->get(), kConfigName, staged_status, expected_uid_) &&
        descriptor_content_matches(temporary.get(), staged_status, expected_uid_, new_hash, false,
                                   error);
    if (!replacement_matches) {
        const bool rollback_failed =
            old_present ? !exchange_entries(directory->get(), kConfigName, kReplacementName)
                        : ::unlinkat(directory->get(), kConfigName.data(), 0) != 0;
        if (rollback_failed || ::fsync(directory->get()) != 0) {
            return {MutationStatus::DurabilityUnknown,
                    {},
                    make_error(ConfigReason::SyncError,
                               "replacement identity mismatch could not be durably rolled back")};
        }

        bool restored_missing = false;
        const auto restored = read_secure_named_file(directory->get(), kConfigName, expected_uid_,
                                                     kMaxConfigBytes, restored_missing, error);
        const bool old_restored =
            old_present ? restored && sha256(*restored) == old_hash : restored_missing;
        if (!old_restored || !cleanup_transaction(directory->get(), error)) {
            return {MutationStatus::DurabilityUnknown,
                    {},
                    make_error(ConfigReason::SyncError,
                               "replacement identity mismatch left uncertain transaction state")};
        }
        return {MutationStatus::IoError,
                {},
                make_error(ConfigReason::PathInvalid,
                           "replacement config.toml identity or content changed during commit")};
    }

    const std::string committed = transaction_bytes(true, old_present, old_hash, new_hash);
    if (!write_transaction_file(directory->get(), kCommittedTransactionName, committed,
                                expected_uid_, error) ||
        ::renameat(directory->get(), kCommittedTransactionName.data(), directory->get(),
                   kTransactionName.data()) != 0 ||
        ::fsync(directory->get()) != 0 || !cleanup_transaction(directory->get(), error)) {
        return {MutationStatus::DurabilityUnknown,
                {},
                make_error(ConfigReason::SyncError,
                           "config commit is durable but transaction finalization is uncertain")};
    }
    auto applied = read_from_directory(directory->get(), expected_uid_);
    if (!applied.parsed) {
        return {MutationStatus::IoError, {}, std::move(applied.error)};
    }
    return {MutationStatus::Applied, std::move(applied.parsed->snapshot), {}};
}

SnapshotManager::SnapshotManager(const Store& store) : store_(store) {
    current_.store(std::make_shared<PublishedSnapshot>());
}

bool SnapshotManager::initialize(Clock::time_point now) {
    const std::lock_guard lock(reload_mutex_);
    const auto loaded = store_.load();
    next_poll_ = now + kPollInterval;
    if (!loaded) {
        current_.store(
            std::make_shared<PublishedSnapshot>(PublishedSnapshot{nullptr, false, loaded.error}));
        return false;
    }
    current_.store(std::make_shared<PublishedSnapshot>(
        PublishedSnapshot{loaded.snapshot, true, std::nullopt}));
    return true;
}

ReloadStatus SnapshotManager::reload() {
    const std::lock_guard lock(reload_mutex_);
    const auto loaded = store_.load();
    const auto previous = current_.load();
    if (!loaded) {
        current_.store(std::make_shared<PublishedSnapshot>(
            PublishedSnapshot{previous ? previous->snapshot : nullptr, false, loaded.error}));
        return ReloadStatus::Invalid;
    }
    if (previous && previous->snapshot &&
        previous->snapshot->identity == loaded.snapshot->identity && !previous->error) {
        return ReloadStatus::Unchanged;
    }
    current_.store(std::make_shared<PublishedSnapshot>(
        PublishedSnapshot{loaded.snapshot, true, std::nullopt}));
    return ReloadStatus::Published;
}

ReloadStatus SnapshotManager::poll(Clock::time_point now) {
    {
        const std::lock_guard lock(reload_mutex_);
        if (now < next_poll_) {
            return ReloadStatus::NotDue;
        }
        next_poll_ = now + kPollInterval;
    }
    return reload();
}

std::shared_ptr<const PublishedSnapshot> SnapshotManager::current() const {
    return current_.load();
}

} // namespace tgcli::config
