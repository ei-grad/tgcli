#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import NoReturn

PINNED_TDLIB_SHA = "a17f87c4cff7b90b278d12b91ba0614383aaee82"
PINNED_FUNCTION_COUNT = 1001
ACCEPTED_POLICY_SHA256 = (
    "sha256:4fcfa4c3dc1f81486382351db8b6a6f744e0b2116383e9705a8046245229f4ce"
)
SCHEMA_VERSION = 1
TD_API_HEADER_EVIDENCE_CONTRACT = "doxygen-normalized-v1"
FUNCTION_MARKER = "---functions---"
TL_FUNCTION = re.compile(
    r"^(?P<name>[A-Za-z][A-Za-z0-9_]*)"
    r"(?: (?P<fields>[^=]*?))? = (?P<result>[A-Za-z][A-Za-z0-9_]*);$"
)
HEADER_CONSTRUCTOR = re.compile(
    r"^class (?P<name>[A-Za-z][A-Za-z0-9_]*) final : public [A-Za-z][A-Za-z0-9_]* \{"
    r"(?P<body>.*?)^\};$",
    re.MULTILINE | re.DOTALL,
)
HEADER_ID = re.compile(r"static const std::int32_t ID = (?P<id>-?[0-9]+);")
POLICY_KEYS = {
    "name",
    "constructor_id",
    "result_type",
    "fields_sha256",
    "principal",
    "principal_evidence",
    "admission",
    "body_validator",
    "target_fields",
    "sensitive_input",
    "sensitive_output",
    "reviewed",
    "evidence_category",
    "review_reason",
}
COMPILED_VALIDATORS = {
    "chat_member_target": (
        "validate_raw_body_chat_member_target",
        "plan_raw_body_chat_targets",
    ),
    "chat_optional_sender_target": (
        "validate_raw_body_chat_optional_sender_target",
        "plan_raw_body_chat_targets",
    ),
    "chat_targets": (
        "validate_raw_body_chat_targets",
        "plan_raw_body_chat_targets",
    ),
    "deny": ("validate_raw_body_deny", "plan_raw_body_none"),
    "none": ("validate_raw_body_none", "plan_raw_body_none"),
    "raise_destructive": (
        "validate_raw_body_raise_destructive",
        "plan_raw_body_none",
    ),
    "raise_write": ("validate_raw_body_raise_write", "plan_raw_body_none"),
}

ADMITTED_FUNCTIONS = {
    "cleanFileName": ("read", "none", "local_pure_read"),
    "getCountryFlagEmoji": ("read", "none", "local_pure_read"),
    "getFileExtension": ("read", "none", "local_pure_read"),
    "getFileMimeType": ("read", "none", "local_pure_read"),
    "getMarkdownText": ("read", "none", "local_pure_read"),
    "getTextEntities": ("read", "none", "local_pure_read"),
    "parseMarkdown": ("read", "none", "local_pure_read"),
    "parseTextEntities": ("read", "none", "local_pure_read"),
    "getChatAdministrators": ("read", "chat_targets", "non_secret_chat_read"),
    "getChatHistory": ("read", "chat_targets", "non_secret_chat_read"),
    "getChatMember": ("read", "chat_member_target", "non_secret_chat_read"),
    "getChatMessageByDate": ("read", "chat_targets", "non_secret_chat_read"),
    "getChatMessageCalendar": ("read", "chat_targets", "non_secret_chat_read"),
    "getChatMessageCount": ("read", "chat_targets", "non_secret_chat_read"),
    "getChatMessagePosition": ("read", "chat_targets", "non_secret_chat_read"),
    "getChatPinnedMessage": ("read", "chat_targets", "non_secret_chat_read"),
    "getChatScheduledMessages": ("read", "chat_targets", "non_secret_chat_read"),
    "getChatSparseMessagePositions": ("read", "chat_targets", "non_secret_chat_read"),
    "getForumTopic": ("read", "chat_targets", "non_secret_chat_read"),
    "getForumTopicHistory": ("read", "chat_targets", "non_secret_chat_read"),
    "getForumTopicLink": ("read", "chat_targets", "non_secret_chat_read"),
    "getForumTopics": ("read", "chat_targets", "non_secret_chat_read"),
    "getMessage": ("read", "chat_targets", "non_secret_chat_read"),
    "getMessageAddedReactions": ("read", "chat_targets", "non_secret_chat_read"),
    "getMessageAvailableReactions": ("read", "chat_targets", "non_secret_chat_read"),
    "getMessageProperties": ("read", "chat_targets", "non_secret_chat_read"),
    "getMessages": ("read", "chat_targets", "non_secret_chat_read"),
    "searchChatMembers": ("read", "chat_targets", "non_secret_chat_read"),
    "searchChatMessages": (
        "read",
        "chat_optional_sender_target",
        "non_secret_chat_read",
    ),
    "addMessageReaction": ("write", "chat_targets", "non_secret_chat_write"),
    "createForumTopic": ("write", "chat_targets", "non_secret_chat_write"),
    "editForumTopic": ("write", "chat_targets", "non_secret_chat_write"),
    "readAllChatMentions": ("write", "chat_targets", "non_secret_chat_write"),
    "readAllChatPollVotes": ("write", "chat_targets", "non_secret_chat_write"),
    "readAllChatReactions": ("write", "chat_targets", "non_secret_chat_write"),
    "removeMessageReaction": ("write", "chat_targets", "non_secret_chat_write"),
    "setChatDescription": ("write", "chat_targets", "non_secret_chat_write"),
    "setChatMessageAutoDeleteTime": ("write", "chat_targets", "non_secret_chat_write"),
    "setChatTitle": ("write", "chat_targets", "non_secret_chat_write"),
    "toggleChatHasProtectedContent": ("write", "chat_targets", "non_secret_chat_write"),
    "toggleChatIsMarkedAsUnread": ("write", "chat_targets", "non_secret_chat_write"),
    "toggleChatIsPinned": ("write", "chat_targets", "non_secret_chat_write"),
    "toggleChatIsTranslatable": ("write", "chat_targets", "non_secret_chat_write"),
    "toggleChatViewAsTopics": ("write", "chat_targets", "non_secret_chat_write"),
    "toggleForumTopicIsClosed": ("write", "chat_targets", "non_secret_chat_write"),
    "toggleForumTopicIsPinned": ("write", "chat_targets", "non_secret_chat_write"),
    "toggleGeneralForumTopicIsHidden": (
        "write",
        "chat_targets",
        "non_secret_chat_write",
    ),
    "viewMessages": ("write", "chat_targets", "non_secret_chat_write"),
    "banChatMember": (
        "destructive",
        "chat_member_target",
        "non_secret_chat_destructive",
    ),
    "deleteChatHistory": ("destructive", "chat_targets", "non_secret_chat_destructive"),
    "deleteForumTopic": ("destructive", "chat_targets", "non_secret_chat_destructive"),
    "deleteMessages": ("destructive", "chat_targets", "non_secret_chat_destructive"),
    "leaveChat": ("destructive", "chat_targets", "non_secret_chat_destructive"),
    "setChatMemberStatus": (
        "destructive",
        "chat_member_target",
        "non_secret_chat_destructive",
    ),
}
EVIDENCE_CATEGORIES = {
    "local_pure_read",
    "non_secret_chat_read",
    "non_secret_chat_write",
    "non_secret_chat_destructive",
    "denied_not_in_v1_allowlist",
}
INDIRECT_TARGET_DENIAL_EVIDENCE = {
    "getMessageThread": (
        "Requests.cpp:629-654 returns MessageThreadInfo whose result chat is "
        "MessagesManager.cpp:14607-14663 info.dialog_id"
    ),
    "getMessageThreadHistory": (
        "Requests.cpp:980-1011 renders messages_.first, the dialog returned by "
        "get_message_thread_history"
    ),
    "getRepliedMessage": (
        "Requests.cpp:607-626 renders replied_message_full_id_, including its returned dialog"
    ),
}
USER_PRINCIPAL_OVERRIDES = {
    "banChatMember",
    "createForumTopic",
    "deleteForumTopic",
    "deleteMessages",
    "editForumTopic",
    "getChatAdministrators",
    "getChatMember",
    "getChatPinnedMessage",
    "getForumTopic",
    "getForumTopicLink",
    "getMessage",
    "getMessageProperties",
    "getMessages",
    "getRepliedMessage",
    "leaveChat",
    "searchChatMembers",
    "setChatDescription",
    "setChatMemberStatus",
    "setChatTitle",
    "toggleForumTopicIsClosed",
    "toggleGeneralForumTopicIsHidden",
}


class PolicyError(RuntimeError):
    pass


def fail(message: str) -> NoReturn:
    raise PolicyError(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def duplicate_rejecting_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(source: Path) -> dict[str, object]:
    require(
        source.is_file() and not source.is_symlink(), f"unsafe JSON asset: {source}"
    )
    data = source.read_bytes()
    require(
        data.endswith(b"\n") and not data.endswith(b"\n\n"),
        f"invalid final LF: {source}",
    )
    try:
        document = json.loads(
            data,
            object_pairs_hook=duplicate_rejecting_object,
            parse_constant=lambda value: fail(f"invalid JSON constant: {value}"),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PolicyError(f"invalid JSON asset {source}: {error}") from error
    require(isinstance(document, dict), f"JSON root is not an object: {source}")
    return document


def json_bytes(document: object) -> bytes:
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode()


def rows_digest(rows: object) -> str:
    encoded = json.dumps(rows, separators=(",", ":"), ensure_ascii=False).encode()
    return f"sha256:{hashlib.sha256(encoded).hexdigest()}"


def file_digest(source: Path) -> str:
    return f"sha256:{hashlib.sha256(source.read_bytes()).hexdigest()}"


def git_output(repository: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def verify_tdlib(tdlib_source: Path) -> tuple[Path, Path]:
    require(
        tdlib_source.is_dir() and not tdlib_source.is_symlink(), "unsafe TDLib source"
    )
    require(
        git_output(tdlib_source, "rev-parse", "HEAD") == PINNED_TDLIB_SHA,
        "TDLib pin differs",
    )
    require(
        not git_output(tdlib_source, "status", "--porcelain"), "TDLib source is dirty"
    )
    scheme = tdlib_source / "td" / "generate" / "scheme" / "td_api.tl"
    header = tdlib_source / "td" / "generate" / "auto" / "td" / "telegram" / "td_api.h"
    require(scheme.is_file() and not scheme.is_symlink(), "unsafe td_api.tl")
    require(header.is_file() and not header.is_symlink(), "unsafe td_api.h")
    return scheme, header


def parse_tl_rows(text: str) -> dict[str, tuple[str, list[tuple[str, str]]]]:
    rows: dict[str, tuple[str, list[tuple[str, str]]]] = {}
    for line in text.splitlines():
        if not line or line.startswith("//"):
            continue
        if line.startswith("vector "):
            continue
        match = TL_FUNCTION.fullmatch(line)
        require(match is not None, f"unparsed TDLib definition: {line}")
        name = match.group("name")
        if name in {
            "double",
            "string",
            "int32",
            "int53",
            "int64",
            "bytes",
            "boolFalse",
            "boolTrue",
        }:
            continue
        fields_text = match.group("fields") or ""
        fields: list[tuple[str, str]] = []
        for token in fields_text.split() if fields_text else []:
            require(token.count(":") == 1, f"invalid field token for {name}: {token}")
            field_name, field_type = token.split(":", 1)
            require(field_name and field_type, f"empty field token for {name}")
            fields.append((field_name, field_type))
        require(name not in rows, f"duplicate TDLib constructor: {name}")
        rows[name] = (match.group("result"), fields)
    return rows


def parse_scheme(source: Path) -> dict[str, tuple[str, str]]:
    text = source.read_text(encoding="utf-8")
    require(text.count(FUNCTION_MARKER) == 1, "function marker differs")
    function_text = text.split(FUNCTION_MARKER, 1)[1]
    rows: dict[str, tuple[str, str]] = {}
    for name, (result_type, fields) in parse_tl_rows(function_text).items():
        canonical_fields = "\n".join(
            f"{field}:{field_type}" for field, field_type in fields
        ).encode()
        rows[name] = (
            result_type,
            f"sha256:{hashlib.sha256(canonical_fields).hexdigest()}",
        )
    return rows


def normalize_doxygen_header(text: str) -> str:
    output: list[str] = []
    index = 0
    state = "normal"
    line_prefix = True
    line_output_start = 0
    while index < len(text):
        if state == "normal" and line_prefix and text.startswith("/**", index):
            del output[line_output_start:]
            close = text.find("*/", index + 3)
            nested = text.find("/**", index + 3)
            require(close != -1, "unterminated Doxygen block in td_api.h")
            require(nested == -1 or close < nested, "nested Doxygen block in td_api.h")
            index = close + 2
            while index < len(text) and text[index] in " \t":
                index += 1
            if index == len(text):
                break
            if text.startswith("\r\n", index):
                index += 2
            else:
                require(text[index] == "\n", "junk after Doxygen block in td_api.h")
                index += 1
            line_prefix = True
            line_output_start = len(output)
            continue

        if state == "normal":
            require(not text.startswith("*/", index), "stray comment close in td_api.h")
            if text.startswith("//", index):
                output.extend(("/", "/"))
                index += 2
                line_prefix = False
                state = "line_comment"
                continue
            if text.startswith("/*", index):
                output.extend(("/", "*"))
                index += 2
                line_prefix = False
                state = "block_comment"
                continue
            if text[index] == '"':
                state = "string"
            elif text[index] == "'":
                state = "character"
        elif state == "line_comment" and text[index] == "\n":
            state = "normal"
        elif state == "block_comment" and text.startswith("*/", index):
            output.extend(("*", "/"))
            index += 2
            state = "normal"
            continue
        elif state in {"string", "character"}:
            quote = '"' if state == "string" else "'"
            if text[index] == "\\" and index + 1 < len(text):
                output.extend((text[index], text[index + 1]))
                index += 2
                line_prefix = False
                continue
            if text[index] == quote:
                state = "normal"

        character = text[index]
        output.append(character)
        index += 1
        if character == "\n":
            line_prefix = True
            line_output_start = len(output)
        elif character not in " \t\r" or not line_prefix:
            line_prefix = False
    return "".join(output)


def normalized_header_text(source: Path) -> str:
    return normalize_doxygen_header(source.read_bytes().decode("utf-8"))


def parse_header(source: Path) -> dict[str, int]:
    text = normalized_header_text(source)
    rows: dict[str, int] = {}
    for match in HEADER_CONSTRUCTOR.finditer(text):
        name = match.group("name")
        identifier = HEADER_ID.search(match.group("body"))
        require(identifier is not None, f"function ID missing: {name}")
        require(name not in rows, f"duplicate header function: {name}")
        rows[name] = int(identifier.group("id"))
    return rows


def source_function_contracts(source: Path) -> dict[str, dict[str, object]]:
    contracts: dict[str, dict[str, object]] = {}
    comments: list[str] = []
    in_functions = False
    for line_number, line in enumerate(
        source.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if line == FUNCTION_MARKER:
            in_functions = True
            comments.clear()
            continue
        if not in_functions:
            continue
        if line.startswith("//"):
            comments.append(line.removeprefix("//").strip())
            continue
        if not line:
            continue
        match = TL_FUNCTION.fullmatch(line)
        require(match is not None, f"unparsed function contract: {line}")
        name = match.group("name")
        contracts[name] = {
            "line": line_number,
            "contract": " ".join(comments),
            "signature": line,
        }
        comments.clear()
    require(len(contracts) == PINNED_FUNCTION_COUNT, "function contract count differs")
    return contracts


def source_principal_evidence(
    tdlib_source: Path, contracts: dict[str, dict[str, object]]
) -> dict[str, tuple[str, str] | None]:
    requests_source = tdlib_source / "td" / "telegram" / "Requests.cpp"
    synchronous_source = tdlib_source / "td" / "telegram" / "SynchronousRequests.cpp"
    require(
        requests_source.is_file()
        and not requests_source.is_symlink()
        and synchronous_source.is_file()
        and not synchronous_source.is_symlink(),
        "unsafe TDLib principal evidence source",
    )
    request_text = requests_source.read_text(encoding="utf-8")
    handler = re.compile(
        r"^void Requests::on_request\([^\n]*td_api::(?P<name>[A-Za-z][A-Za-z0-9_]*) "
        r"&request\) \{$",
        re.MULTILINE,
    )
    matches = list(handler.finditer(request_text))
    request_principals: dict[str, tuple[str, str]] = {}
    request_handler_lines: dict[str, int] = {}
    for index, match in enumerate(matches):
        end = (
            matches[index + 1].start()
            if index + 1 < len(matches)
            else len(request_text)
        )
        body = request_text[match.end() : end]
        line = request_text.count("\n", 0, match.start()) + 1
        request_handler_lines[match.group("name")] = line
        if "CHECK_IS_BOT();" in body:
            request_principals[match.group("name")] = (
                "bot",
                f"Requests.cpp:{line}:CHECK_IS_BOT",
            )
        elif "CHECK_IS_USER();" in body or "CHECK_IS_USER_OR_BUSINESS();" in body:
            request_principals[match.group("name")] = (
                "user",
                f"Requests.cpp:{line}:explicit user admission guard",
            )

    synchronous_text = synchronous_source.read_text(encoding="utf-8")
    synchronous_functions = set(
        re.findall(r"case td_api::([A-Za-z][A-Za-z0-9_]*)::ID:", synchronous_text)
    )
    evidence: dict[str, tuple[str, str] | None] = {}
    for name, contract in contracts.items():
        text = str(contract["contract"]).lower()
        line = int(contract["line"])
        if any(
            marker in text
            for marker in (
                "for bots only",
                "only for bots",
                "can be called only by bots",
            )
        ):
            evidence[name] = ("bot", f"td_api.tl:{line}:explicit bot-only contract")
        elif any(
            marker in text
            for marker in (
                "not supported for bots",
                "not supported for bot accounts",
                "for non-bot users only",
                "only for non-bot users",
            )
        ):
            evidence[name] = ("user", f"td_api.tl:{line}:explicit non-bot contract")
        elif name in synchronous_functions:
            evidence[name] = (
                "both",
                "SynchronousRequests.cpp:account-independent static dispatcher",
            )
        elif name in USER_PRINCIPAL_OVERRIDES:
            require(
                name in request_handler_lines,
                f"principal override handler missing: {name}",
            )
            evidence[name] = (
                "user",
                (
                    f"td_api.tl:{line}+Requests.cpp:{request_handler_lines[name]}:"
                    "direct typed user path; raw v1 restricts principal to user"
                ),
            )
        else:
            evidence[name] = request_principals.get(name)
    return evidence


def reviewed_principal(
    name: str,
    contract: dict[str, object],
    evidence: dict[str, tuple[str, str] | None],
) -> tuple[str, str, bool]:
    resolved = evidence[name]
    if resolved is not None:
        return resolved[0], resolved[1], True
    line = int(contract["line"])
    return (
        "user",
        (
            f"td_api.tl:{line}+Requests.cpp:ambiguous principal; whole function denied and "
            "principal conservatively restricted to user"
        ),
        False,
    )


def source_type_graph(tdlib_source: Path) -> dict[str, object]:
    scheme, header = verify_tdlib(tdlib_source)
    text = scheme.read_text(encoding="utf-8")
    type_text, function_text = text.split(FUNCTION_MARKER, 1)
    type_rows = parse_tl_rows(type_text)
    function_rows = parse_tl_rows(function_text)
    constructors = {**type_rows, **function_rows}
    require(len(function_rows) == PINNED_FUNCTION_COUNT, "graph function count differs")
    header_rows = parse_header(header)
    require(
        set(constructors) == set(header_rows), "graph/header constructor set differs"
    )
    require(
        len(set(header_rows.values())) == len(header_rows),
        "duplicate generated constructor ID",
    )
    rows = [
        {
            "name": name,
            "constructor_id": header_rows[name],
            "result_type": result_type,
            "kind": "function" if name in function_rows else "object",
            "fields": [
                {"name": field_name, "type": field_type}
                for field_name, field_type in fields
            ],
        }
        for name, (result_type, fields) in sorted(constructors.items())
    ]
    result_types: dict[str, list[str]] = {}
    for row in rows:
        if row["kind"] == "object":
            result_types.setdefault(str(row["result_type"]), []).append(
                str(row["name"])
            )
    primitive_types = {"Bool", "int32", "int53", "int64", "double", "string", "bytes"}

    def validate_type(type_name: str) -> None:
        if (
            type_name in primitive_types
            or type_name in type_rows
            or type_name in result_types
        ):
            return
        if type_name.startswith("vector<") and type_name.endswith(">"):
            validate_type(type_name[7:-1])
            return
        fail(f"unknown graph field/result type: {type_name}")

    for result_type, fields in constructors.values():
        validate_type(result_type)
        field_names = [name for name, unused_type in fields]
        require(len(set(field_names)) == len(field_names), "duplicate generated field")
        for unused_name, field_type in fields:
            validate_type(field_type)
    result_rows = [
        {"name": name, "constructors": constructors_for_type}
        for name, constructors_for_type in sorted(result_types.items())
    ]
    return {
        "schema_version": SCHEMA_VERSION,
        "tdlib_sha": PINNED_TDLIB_SHA,
        "constructor_count": len(rows),
        "function_count": len(function_rows),
        "graph_sha256": rows_digest(
            {"constructors": rows, "result_types": result_rows}
        ),
        "constructors": rows,
        "result_types": result_rows,
    }


def cpp_string(value: str) -> str:
    require(
        re.fullmatch(r"[A-Za-z0-9_<>]+", value) is not None,
        "invalid C++ graph token",
    )
    return f'"{value}"'


def generated_graph_include(
    graph: dict[str, object], policy: dict[str, object]
) -> bytes:
    constructors = graph["constructors"]
    assert isinstance(constructors, list)
    fields: list[dict[str, object]] = []
    constructor_rows: list[str] = []
    for row in constructors:
        assert isinstance(row, dict)
        row_fields = row["fields"]
        assert isinstance(row_fields, list)
        offset = len(fields)
        fields.extend(row_fields)
        constructor_rows.append(
            "    RawTdConstructorSpec{"
            + ", ".join(
                [
                    cpp_string(str(row["name"])),
                    cpp_string(str(row["result_type"])),
                    str(row["constructor_id"]),
                    "RawTdConstructorKind::Function"
                    if row["kind"] == "function"
                    else "RawTdConstructorKind::Object",
                    str(offset),
                    str(len(row_fields)),
                ]
            )
            + "},"
        )
    field_rows = [
        f"    RawTdFieldSpec{{{cpp_string(str(field['name']))}, {cpp_string(str(field['type']))}}},"
        for field in fields
    ]
    policy_rows = policy["functions"]
    assert isinstance(policy_rows, list)
    graph_by_name = {
        str(row["name"]): row for row in constructors if isinstance(row, dict)
    }
    validator_symbols = sorted(
        {str(row["body_validator"]) for row in policy_rows if isinstance(row, dict)}
    )
    chat_target_cases: list[str] = []
    for row in policy_rows:
        assert isinstance(row, dict)
        if row["body_validator"] not in {
            "chat_member_target",
            "chat_optional_sender_target",
            "chat_targets",
        }:
            continue
        name = str(row["name"])
        target_fields = row["target_fields"]
        assert isinstance(target_fields, list)
        graph_fields = graph_by_name[name]["fields"]
        assert isinstance(graph_fields, list)
        field_types = {
            str(field["name"]): str(field["type"])
            for field in graph_fields
            if isinstance(field, dict)
        }
        planner_lines: list[str] = []
        for field in target_fields:
            if field == "chat_id":
                require(
                    field_types[field] == "int53", f"chat target type differs: {name}"
                )
                planner_lines.append(
                    f"    if (!append_raw_chat_target(typed.{field}_, plan)) {{ return false; }}"
                )
            elif field == "member_id.chat_id|required":
                require(
                    field_types["member_id"] == "MessageSender",
                    f"member target type differs: {name}",
                )
                planner_lines.append(
                    "    if (!append_raw_message_sender_target(typed.member_id_, plan, true)) "
                    "{ return false; }"
                )
            elif field == "sender_id.chat_id|optional":
                require(
                    field_types["sender_id"] == "MessageSender",
                    f"sender target type differs: {name}",
                )
                planner_lines.append(
                    "    if (!append_raw_message_sender_target(typed.sender_id_, plan, false)) "
                    "{ return false; }"
                )
            else:
                raise PolicyError(f"unsupported admitted raw target: {name}.{field}")
        chat_target_cases.extend(
            [
                f"case td::td_api::{name}::ID: {{",
                f"    const auto& typed = static_cast<const td::td_api::{name}&>(function);",
                *planner_lines,
                "    return true;",
                "}",
            ]
        )
    policy_descriptors: list[str] = []
    for row in policy_rows:
        assert isinstance(row, dict)
        principal = {
            "user": "RawPrincipal::User",
            "bot": "RawPrincipal::Bot",
            "both": "RawPrincipal::Both",
        }[str(row["principal"])]
        admission = {
            "denied": "AdmissionTier::Denied",
            "read": "AdmissionTier::Read",
            "write": "AdmissionTier::Write",
            "destructive": "AdmissionTier::Destructive",
        }[str(row["admission"])]
        policy_descriptors.append(
            "    RawPolicyDescriptor{"
            + ", ".join(
                [
                    cpp_string(str(row["name"])),
                    str(row["constructor_id"]),
                    principal,
                    admission,
                    cpp_string(str(row["body_validator"])),
                    "true" if row["sensitive_input"] else "false",
                    "true" if row["sensitive_output"] else "false",
                    "true" if row["reviewed"] else "false",
                ]
            )
            + "},"
        )
    output = [
        "// Generated from pinned td_api.tl. Do not edit.",
        f"inline constexpr std::string_view kRawTdGraphSha256 = {cpp_string(str(graph['graph_sha256']).removeprefix('sha256:'))};",
        f"inline constexpr std::string_view kRawPolicySha256 = {cpp_string(str(policy['policy_sha256']).removeprefix('sha256:'))};",
        f"inline constexpr bool kRawPolicyActivationReady = {'true' if policy['activation_ready'] else 'false'};",
        f"inline constexpr std::array<RawTdFieldSpec, {len(field_rows)}> kRawTdFields{{",
        *field_rows,
        "};",
        (
            f"inline constexpr std::array<RawTdConstructorSpec, {len(constructor_rows)}> "
            "kRawTdConstructors{"
        ),
        *constructor_rows,
        "};",
        "bool plan_raw_body_chat_targets(const td::td_api::Function& function,",
        "                                RawPreflightPlan& plan) noexcept {",
        "    switch (function.get_id()) {",
        *chat_target_cases,
        "    default:",
        "        return false;",
        "    }",
        "}",
        f"inline constexpr std::array<RawBodyValidatorDescriptor, {len(validator_symbols)}> "
        "kRawBodyValidators{"
        + ", ".join(
            "RawBodyValidatorDescriptor{"
            + cpp_string(symbol)
            + ", &"
            + COMPILED_VALIDATORS[symbol][0]
            + ", &"
            + COMPILED_VALIDATORS[symbol][1]
            + "}"
            for symbol in validator_symbols
        )
        + "};",
        (
            f"inline constexpr std::array<RawPolicyDescriptor, {len(policy_descriptors)}> "
            "kRawPolicies{"
        ),
        *policy_descriptors,
        "};",
        "",
    ]
    return "\n".join(output).encode()


def generated_wipe_include(graph: dict[str, object]) -> bytes:
    constructors = graph["constructors"]
    assert isinstance(constructors, list)

    def cases(kind: str) -> list[str]:
        rows: list[str] = []
        for row in constructors:
            assert isinstance(row, dict)
            if row["kind"] != kind:
                continue
            name = str(row["name"])
            fields = row["fields"]
            assert isinstance(fields, list)
            rows.extend(
                [
                    f"case td::td_api::{name}::ID: {{",
                    *(
                        [f"    auto& typed = static_cast<td::td_api::{name}&>(value);"]
                        if fields
                        else []
                    ),
                    *[
                        f"    wipe_native_value(typed.{field['name']}_, observer);"
                        for field in fields
                        if isinstance(field, dict)
                    ],
                    "    return;",
                    "}",
                ]
            )
        return rows

    output = [
        "// Generated from pinned td_api.tl. Do not edit.",
        "void wipe_native_object(td::td_api::Object& value,",
        "                        const secure::WipeObserver& observer) noexcept {",
        "    switch (value.get_id()) {",
        *cases("object"),
        "    default:",
        "        return;",
        "    }",
        "}",
        "",
        "void wipe_native_function(td::td_api::Function& value,",
        "                          const secure::WipeObserver& observer) noexcept {",
        "    switch (value.get_id()) {",
        *cases("function"),
        "    default:",
        "        return;",
        "    }",
        "}",
        "",
    ]
    return "\n".join(output).encode()


def generated_canonical_include(graph: dict[str, object]) -> bytes:
    constructors = graph["constructors"]
    assert isinstance(constructors, list)

    def cases(kind: str) -> list[str]:
        rows: list[str] = []
        for row in constructors:
            assert isinstance(row, dict)
            if row["kind"] != kind:
                continue
            name = str(row["name"])
            fields = row["fields"]
            assert isinstance(fields, list)
            field_lines: list[str] = []
            for field in fields:
                assert isinstance(field, dict)
                field_lines.extend(
                    [
                        (
                            "    if (!append_native_field("
                            f"{cpp_string(str(field['name']))}, typed.{field['name']}_, "
                            f"{cpp_string(str(field['type']))}, output)) {{"
                        ),
                        "        return false;",
                        "    }",
                    ]
                )
            rows.extend(
                [
                    f"case td::td_api::{name}::ID: {{",
                    *(
                        [
                            f"    const auto& typed = static_cast<const td::td_api::{name}&>(value);"
                        ]
                        if fields
                        else []
                    ),
                    f"    append_native_begin({cpp_string(name)}, output);",
                    *field_lines,
                    "    output.push_back('}');",
                    "    return true;",
                    "}",
                ]
            )
        return rows

    output = [
        "// Generated from pinned td_api.tl. Do not edit.",
        "bool append_native_object(const td::td_api::Object& value, CanonicalBuffer& output) {",
        "    switch (value.get_id()) {",
        *cases("object"),
        "    default:",
        "        return false;",
        "    }",
        "}",
        "",
        "bool append_native_function(const td::td_api::Function& value, CanonicalBuffer& output) {",
        "    switch (value.get_id()) {",
        *cases("function"),
        "    default:",
        "        return false;",
        "    }",
        "}",
        "",
    ]
    return "\n".join(output).encode()


def source_inventory(tdlib_source: Path) -> tuple[dict[str, object], dict[str, object]]:
    scheme, header = verify_tdlib(tdlib_source)
    tl_rows = parse_scheme(scheme)
    header_rows = parse_header(header)
    require(set(tl_rows) <= set(header_rows), "tl/header function set differs")
    require(len(tl_rows) == PINNED_FUNCTION_COUNT, "pin-derived function count differs")
    functions = [
        {
            "name": name,
            "constructor_id": header_rows[name],
            "result_type": tl_rows[name][0],
            "fields_sha256": tl_rows[name][1],
        }
        for name in sorted(tl_rows)
    ]
    inventory = {
        "schema_version": SCHEMA_VERSION,
        "tdlib_sha": PINNED_TDLIB_SHA,
        "function_count": len(functions),
        "functions_sha256": rows_digest(functions),
        "functions": functions,
    }
    evidence = {
        "td_api_tl_sha256": file_digest(scheme),
        "td_api_header_contract": TD_API_HEADER_EVIDENCE_CONTRACT,
        "td_api_header_normalized_sha256": (
            f"sha256:{hashlib.sha256(normalized_header_text(header).encode()).hexdigest()}"
        ),
    }
    return inventory, evidence


def candidate_policy(
    inventory: dict[str, object], graph: dict[str, object], tdlib_source: Path
) -> dict[str, object]:
    functions = inventory["functions"]
    constructors = graph["constructors"]
    assert isinstance(functions, list) and isinstance(constructors, list)
    graph_functions = {
        str(row["name"]): row
        for row in constructors
        if isinstance(row, dict) and row["kind"] == "function"
    }
    scheme, _ = verify_tdlib(tdlib_source)
    contracts = source_function_contracts(scheme)
    principal_contracts = source_principal_evidence(tdlib_source, contracts)
    require(
        set(ADMITTED_FUNCTIONS) <= set(graph_functions),
        "admitted raw function is absent from pin",
    )
    rows: list[dict[str, object]] = []
    unfinished_functions: list[str] = []
    for inventory_row in functions:
        assert isinstance(inventory_row, dict)
        name = str(inventory_row["name"])
        graph_row = graph_functions[name]
        contract = contracts[name]
        fields = graph_row["fields"]
        assert isinstance(fields, list)
        principal, principal_evidence, principal_reviewed = reviewed_principal(
            name, contract, principal_contracts
        )
        admitted = ADMITTED_FUNCTIONS.get(name)
        if admitted is not None:
            require(principal_reviewed, f"admitted principal evidence missing: {name}")
            admission, body_validator, category = admitted
            target_fields = {
                "chat_member_target": ["chat_id", "member_id.chat_id|required"],
                "chat_optional_sender_target": [
                    "chat_id",
                    "sender_id.chat_id|optional",
                ],
                "chat_targets": ["chat_id"],
                "none": [],
            }[body_validator]
            if target_fields:
                matching_fields = [
                    field
                    for field in fields
                    if isinstance(field, dict) and field["name"] == "chat_id"
                ]
                require(
                    matching_fields == [{"name": "chat_id", "type": "int53"}],
                    f"admitted chat target differs: {name}",
                )
                if body_validator == "chat_member_target":
                    require(
                        {
                            str(field["name"]): str(field["type"])
                            for field in fields
                            if isinstance(field, dict)
                        }.get("member_id")
                        == "MessageSender",
                        f"admitted member selector differs: {name}",
                    )
                if body_validator == "chat_optional_sender_target":
                    require(
                        {
                            str(field["name"]): str(field["type"])
                            for field in fields
                            if isinstance(field, dict)
                        }.get("sender_id")
                        == "MessageSender",
                        f"admitted sender selector differs: {name}",
                    )
                reason = (
                    f"reviewed:td_api.tl:{contract['line']}; {name} has exact generated "
                    f"chat target metadata {','.join(target_fields)} requiring generation-bound "
                    f"non-secret getChat preflight; static {admission} is worst-case"
                )
            else:
                reason = (
                    f"reviewed:td_api.tl:{contract['line']}; {name} is a local typed "
                    f"transform with result {inventory_row['result_type']} and no account, "
                    "chat, file, credential, logging, payment, or lifecycle target"
                )
            sensitive_input = False
            sensitive_output = False
            reviewed = True
        else:
            category = "denied_not_in_v1_allowlist"
            admission = "denied"
            body_validator = "deny"
            target_fields = []
            sensitive_input = True
            sensitive_output = True
            reviewed = True
            indirect_evidence = INDIRECT_TARGET_DENIAL_EVIDENCE.get(name)
            evidence_suffix = (
                f"; indirect target evidence {indirect_evidence}"
                if indirect_evidence is not None
                else ""
            )
            reason = (
                f"reviewed:td_api.tl:{contract['line']} exact signature "
                f"[{contract['signature']}] inventory constructor_id "
                f"{inventory_row['constructor_id']}, result_type "
                f"{inventory_row['result_type']}, fields_sha256 "
                f"{inventory_row['fields_sha256']}; outside the frozen reviewed v1 allowlist; "
                f"denied whole{evidence_suffix}"
            )
        rows.append(
            {
                "name": name,
                "constructor_id": inventory_row["constructor_id"],
                "result_type": inventory_row["result_type"],
                "fields_sha256": inventory_row["fields_sha256"],
                "principal": principal,
                "principal_evidence": principal_evidence,
                "admission": admission,
                "body_validator": body_validator,
                "target_fields": target_fields,
                "sensitive_input": sensitive_input,
                "sensitive_output": sensitive_output,
                "reviewed": reviewed,
                "evidence_category": category,
                "review_reason": reason,
            }
        )
    policy_digest = rows_digest(rows)
    accepted = not unfinished_functions and policy_digest == ACCEPTED_POLICY_SHA256
    return {
        "schema_version": SCHEMA_VERSION,
        "tdlib_sha": PINNED_TDLIB_SHA,
        "activation_ready": accepted,
        "activation_blockers": [
            "independent_policy_acceptance",
            "unfinished_function_reviews",
        ]
        if unfinished_functions
        else ([] if accepted else ["independent_policy_acceptance"]),
        "unfinished_functions": unfinished_functions,
        "function_count": len(rows),
        "inventory_sha256": inventory["functions_sha256"],
        "policy_sha256": policy_digest,
        "functions": rows,
    }


def lock_document(
    inventory: dict[str, object],
    policy: dict[str, object],
    graph: dict[str, object],
    evidence: dict[str, object],
) -> dict[str, object]:
    return {
        "schema_version": SCHEMA_VERSION,
        "tdlib_sha": PINNED_TDLIB_SHA,
        "function_count": inventory["function_count"],
        "inventory_sha256": inventory["functions_sha256"],
        "policy_sha256": policy["policy_sha256"],
        "constructor_count": graph["constructor_count"],
        "type_graph_sha256": graph["graph_sha256"],
        **evidence,
    }


def validate_policy(inventory: dict[str, object], policy: dict[str, object]) -> None:
    require(
        set(policy)
        == {
            "schema_version",
            "tdlib_sha",
            "activation_ready",
            "activation_blockers",
            "unfinished_functions",
            "function_count",
            "inventory_sha256",
            "policy_sha256",
            "functions",
        },
        "policy root keys differ",
    )
    require(policy["schema_version"] == SCHEMA_VERSION, "policy version differs")
    require(policy["tdlib_sha"] == PINNED_TDLIB_SHA, "policy pin differs")
    require(
        isinstance(policy["activation_ready"], bool), "activation_ready is not boolean"
    )
    blockers = policy["activation_blockers"]
    require(
        isinstance(blockers, list)
        and all(isinstance(blocker, str) and blocker for blocker in blockers)
        and blockers == sorted(set(blockers)),
        "activation blockers differ",
    )
    unfinished_functions = policy["unfinished_functions"]
    require(
        isinstance(unfinished_functions, list)
        and unfinished_functions == sorted(set(unfinished_functions))
        and all(isinstance(name, str) and name for name in unfinished_functions),
        "unfinished function set differs",
    )
    rows = policy["functions"]
    inventory_rows = inventory["functions"]
    require(
        isinstance(rows, list) and isinstance(inventory_rows, list),
        "policy rows differ",
    )
    require(
        len(rows) == len(inventory_rows) == PINNED_FUNCTION_COUNT,
        "policy count differs",
    )
    require(policy["function_count"] == len(rows), "policy count field differs")
    require(
        policy["inventory_sha256"] == inventory["functions_sha256"],
        "inventory link differs",
    )
    require(policy["policy_sha256"] == rows_digest(rows), "policy digest differs")
    for inventory_row, row in zip(inventory_rows, rows, strict=True):
        require(
            isinstance(row, dict) and set(row) == POLICY_KEYS, "policy row keys differ"
        )
        require(row["name"] == inventory_row["name"], "policy row order/name differs")
        for identity_field in (
            "constructor_id",
            "result_type",
            "fields_sha256",
        ):
            require(
                row[identity_field] == inventory_row[identity_field],
                f"policy {identity_field} differs",
            )
        require(row["principal"] in {"user", "bot", "both"}, "policy principal differs")
        require(
            isinstance(row["principal_evidence"], str)
            and re.match(
                r"^(?:td_api\.tl|Requests\.cpp|SynchronousRequests\.cpp):",
                row["principal_evidence"],
            )
            is not None,
            "policy principal evidence differs",
        )
        require(
            row["admission"] in {"read", "write", "destructive", "denied"},
            "policy admission differs",
        )
        require(isinstance(row["body_validator"], str), "policy validator differs")
        require(
            row["body_validator"] in COMPILED_VALIDATORS,
            "compiled policy validator missing",
        )
        target_fields = row["target_fields"]
        require(
            isinstance(target_fields, list)
            and target_fields == sorted(set(target_fields))
            and all(isinstance(field, str) and field for field in target_fields),
            "policy target fields differ",
        )
        require(isinstance(row["sensitive_input"], bool), "request sensitivity differs")
        require(
            isinstance(row["sensitive_output"], bool), "response sensitivity differs"
        )
        require(isinstance(row["reviewed"], bool), "review state differs")
        require(
            row["evidence_category"] in EVIDENCE_CATEGORIES,
            "policy evidence category differs",
        )
        require(
            isinstance(row["review_reason"], str) and row["review_reason"],
            "review reason missing",
        )
        if not row["reviewed"]:
            require(row["admission"] == "denied", "unreviewed function is not denied")
            require(
                row["body_validator"] == "deny", "unreviewed function validator differs"
            )
            require(
                row["review_reason"].startswith("unfinished:td_api.tl:"),
                "unreviewed function reason differs",
            )
        else:
            require(
                row["review_reason"].startswith("reviewed:")
                and len(row["review_reason"]) > len("reviewed:"),
                "reviewed function lacks concrete reasoning",
            )
        if row["admission"] == "denied":
            require(
                row["body_validator"] == "deny", "denied function validator differs"
            )
        else:
            require(row["reviewed"], "admitted function is not reviewed")
            require(
                not row["sensitive_output"], "response-sensitive function is admitted"
            )
            require(
                (
                    row["body_validator"]
                    in {
                        "chat_member_target",
                        "chat_optional_sender_target",
                        "chat_targets",
                    }
                )
                == bool(row["target_fields"]),
                "admitted preflight metadata differs",
            )
    expected_unfinished = [row["name"] for row in rows if not row["reviewed"]]
    require(
        unfinished_functions == expected_unfinished,
        "unfinished function names differ",
    )
    all_reviewed = not expected_unfinished
    if all_reviewed:
        require(
            blockers in ([], ["independent_policy_acceptance"]),
            "reviewed policy blockers differ",
        )
    else:
        require(not policy["activation_ready"], "unreviewed policy is activation-ready")
        require(
            blockers
            == ["independent_policy_acceptance", "unfinished_function_reviews"],
            "unfinished policy blockers differ",
        )
    ready = all_reviewed and not blockers
    require(
        policy["activation_ready"] == ready, "activation_ready/review relation differs"
    )


def validate_assets(tdlib_source: Path, output_root: Path, activation: bool) -> None:
    expected_inventory, evidence = source_inventory(tdlib_source)
    expected_graph = source_type_graph(tdlib_source)
    raw_root = output_root / "docs" / "raw"
    inventory = load_json(raw_root / f"td-functions.{PINNED_TDLIB_SHA}.json")
    graph = load_json(raw_root / f"td-types.{PINNED_TDLIB_SHA}.json")
    policy = load_json(raw_root / f"raw-policy.{PINNED_TDLIB_SHA}.json")
    lock = load_json(raw_root / "raw-policy.lock.json")
    require(
        inventory == expected_inventory,
        "committed inventory differs from pinned source",
    )
    require(graph == expected_graph, "committed type graph differs from pinned source")
    validate_policy(inventory, policy)
    require(
        policy == candidate_policy(expected_inventory, expected_graph, tdlib_source),
        "committed raw policy differs from reviewed candidate",
    )
    require(
        lock == lock_document(inventory, policy, graph, evidence),
        "committed raw policy lock differs",
    )
    generated_include = output_root / "src" / "daemon" / "raw_td_schema.generated.inc"
    require(
        generated_include.read_bytes() == generated_graph_include(graph, policy),
        "committed raw type metadata differs from pinned source",
    )
    generated_wipe = output_root / "src" / "daemon" / "raw_td_wipe.generated.inc"
    require(
        generated_wipe.read_bytes() == generated_wipe_include(graph),
        "committed raw native wipe visitor differs from pinned source",
    )
    generated_canonical = (
        output_root / "src" / "daemon" / "raw_td_canonical.generated.inc"
    )
    require(
        generated_canonical.read_bytes() == generated_canonical_include(graph),
        "committed raw native canonical visitor differs from pinned source",
    )
    if activation:
        require(
            policy["activation_ready"] is True, "raw policy is not activation-ready"
        )


def emit_candidate(tdlib_source: Path, output_root: Path) -> None:
    inventory, evidence = source_inventory(tdlib_source)
    graph = source_type_graph(tdlib_source)
    policy = candidate_policy(inventory, graph, tdlib_source)
    raw_root = output_root / "docs" / "raw"
    raw_root.mkdir(parents=True, exist_ok=True)
    (raw_root / f"td-functions.{PINNED_TDLIB_SHA}.json").write_bytes(
        json_bytes(inventory)
    )
    (raw_root / f"td-types.{PINNED_TDLIB_SHA}.json").write_bytes(json_bytes(graph))
    (raw_root / f"raw-policy.{PINNED_TDLIB_SHA}.json").write_bytes(json_bytes(policy))
    (raw_root / "raw-policy.lock.json").write_bytes(
        json_bytes(lock_document(inventory, policy, graph, evidence))
    )
    (output_root / "src" / "daemon" / "raw_td_schema.generated.inc").write_bytes(
        generated_graph_include(graph, policy)
    )
    (output_root / "src" / "daemon" / "raw_td_wipe.generated.inc").write_bytes(
        generated_wipe_include(graph)
    )
    (output_root / "src" / "daemon" / "raw_td_canonical.generated.inc").write_bytes(
        generated_canonical_include(graph)
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate and validate dormant raw policy assets"
    )
    parser.add_argument(
        "command", choices=("emit-candidate", "validate-dormant", "validate-activation")
    )
    parser.add_argument("--tdlib-source", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "emit-candidate":
            emit_candidate(args.tdlib_source, args.output_root)
        else:
            validate_assets(
                args.tdlib_source,
                args.output_root,
                activation=args.command == "validate-activation",
            )
    except (OSError, subprocess.CalledProcessError, PolicyError) as error:
        print(f"raw policy generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
