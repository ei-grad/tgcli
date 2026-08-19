#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tgcli::proto {

// Neutral M3/M4 operation identity shared by frame admission and daemon
// policy. Persistent and wire contracts use the names below, never enum
// ordinals.
enum class M3Operation {
    Send,
    MsgEdit,
    MsgDelete,
    MsgForward,
    MsgReact,
    MsgPin,
    MsgUnpin,
    ChatMarkRead,
    ChatMute,
    ChatUnmute,
    ChatPin,
    ChatUnpin,
    ChatArchive,
    ChatUnarchive,
    ChatJoin,
    ChatLeave,
    SavedAttach,
};

struct M3OperationIdentity {
    M3Operation operation;
    std::string_view canonical_name;
    std::string_view command_path;
};

inline constexpr std::array<M3OperationIdentity, 17> kM3OperationIdentities{{
    {M3Operation::Send, "send", "send"},
    {M3Operation::MsgEdit, "msg_edit", "msg edit"},
    {M3Operation::MsgDelete, "msg_delete", "msg delete"},
    {M3Operation::MsgForward, "msg_forward", "msg forward"},
    {M3Operation::MsgReact, "msg_react", "msg react"},
    {M3Operation::MsgPin, "msg_pin", "msg pin"},
    {M3Operation::MsgUnpin, "msg_unpin", "msg unpin"},
    {M3Operation::ChatMarkRead, "chat_mark_read", "chat mark-read"},
    {M3Operation::ChatMute, "chat_mute", "chat mute"},
    {M3Operation::ChatUnmute, "chat_unmute", "chat unmute"},
    {M3Operation::ChatPin, "chat_pin", "chat pin"},
    {M3Operation::ChatUnpin, "chat_unpin", "chat unpin"},
    {M3Operation::ChatArchive, "chat_archive", "chat archive"},
    {M3Operation::ChatUnarchive, "chat_unarchive", "chat unarchive"},
    {M3Operation::ChatJoin, "chat_join", "chat join"},
    {M3Operation::ChatLeave, "chat_leave", "chat leave"},
    {M3Operation::SavedAttach, "saved_attach", "saved attach"},
}};

constexpr std::span<const M3OperationIdentity> m3_operation_identities() noexcept {
    return kM3OperationIdentities;
}

constexpr const M3OperationIdentity* m3_operation_identity(M3Operation operation) noexcept {
    for (const auto& identity : kM3OperationIdentities) {
        if (identity.operation == operation) {
            return &identity;
        }
    }
    return nullptr;
}

constexpr std::optional<M3Operation> parse_m3_operation(std::string_view canonical_name) noexcept {
    for (const auto& identity : kM3OperationIdentities) {
        if (identity.canonical_name == canonical_name) {
            return identity.operation;
        }
    }
    return std::nullopt;
}

constexpr std::optional<M3Operation>
m3_operation_for_command(std::string_view command_path) noexcept {
    for (const auto& identity : kM3OperationIdentities) {
        if (identity.command_path == command_path) {
            return identity.operation;
        }
    }
    return std::nullopt;
}

inline bool command_matches(std::span<const std::string> command,
                            std::string_view command_path) noexcept {
    std::size_t part = 0;
    while (!command_path.empty()) {
        if (part == command.size()) {
            return false;
        }
        const auto separator = command_path.find(' ');
        const auto token = command_path.substr(0, separator);
        if (command[part++] != token) {
            return false;
        }
        if (separator == std::string_view::npos) {
            command_path = {};
        } else {
            command_path.remove_prefix(separator + 1);
        }
    }
    return part == command.size();
}

inline std::optional<M3Operation>
m3_operation_for_command(std::span<const std::string> command) noexcept {
    for (const auto& identity : kM3OperationIdentities) {
        if (command_matches(command, identity.command_path)) {
            return identity.operation;
        }
    }
    return std::nullopt;
}

constexpr bool valid_idempotency_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > 128) {
        return false;
    }
    const auto alphanumeric = [](char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9');
    };
    if (!alphanumeric(key.front())) {
        return false;
    }
    return std::ranges::all_of(key.substr(1), [alphanumeric](char value) {
        return alphanumeric(value) || value == '.' || value == '_' || value == ':' || value == '-';
    });
}

} // namespace tgcli::proto
