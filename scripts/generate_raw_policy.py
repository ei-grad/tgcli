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
SCHEMA_VERSION = 1
FUNCTION_MARKER = "---functions---"
TL_FUNCTION = re.compile(
    r"^(?P<name>[A-Za-z][A-Za-z0-9_]*)"
    r"(?: (?P<fields>[^=]*?))? = (?P<result>[A-Za-z][A-Za-z0-9_]*);$"
)
HEADER_FUNCTION = re.compile(
    r"^class (?P<name>[A-Za-z][A-Za-z0-9_]*) final : public Function \{"
    r"(?P<body>.*?)^\};$",
    re.MULTILINE | re.DOTALL,
)
HEADER_ID = re.compile(r"static const std::int32_t ID = (?P<id>-?[0-9]+);")
POLICY_KEYS = {
    "name",
    "principal",
    "admission",
    "body_validator",
    "sensitive_input",
    "sensitive_output",
    "reviewed",
    "review_reason",
}
COMPILED_VALIDATORS = {"deny", "none"}


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


def parse_scheme(source: Path) -> dict[str, tuple[str, str]]:
    text = source.read_text(encoding="utf-8")
    require(text.count(FUNCTION_MARKER) == 1, "function marker differs")
    function_text = text.split(FUNCTION_MARKER, 1)[1]
    rows: dict[str, tuple[str, str]] = {}
    for line in function_text.splitlines():
        if not line or line.startswith("//"):
            continue
        match = TL_FUNCTION.fullmatch(line)
        require(match is not None, f"unparsed function definition: {line}")
        name = match.group("name")
        fields = match.group("fields") or ""
        tokens = fields.split() if fields else []
        for token in tokens:
            require(token.count(":") == 1, f"invalid field token for {name}: {token}")
            field_name, field_type = token.split(":", 1)
            require(field_name and field_type, f"empty field token for {name}")
        require(name not in rows, f"duplicate tl function: {name}")
        canonical_fields = "\n".join(tokens).encode()
        rows[name] = (
            match.group("result"),
            f"sha256:{hashlib.sha256(canonical_fields).hexdigest()}",
        )
    return rows


def parse_header(source: Path) -> dict[str, int]:
    text = source.read_text(encoding="utf-8")
    rows: dict[str, int] = {}
    for match in HEADER_FUNCTION.finditer(text):
        name = match.group("name")
        identifier = HEADER_ID.search(match.group("body"))
        require(identifier is not None, f"function ID missing: {name}")
        require(name not in rows, f"duplicate header function: {name}")
        rows[name] = int(identifier.group("id"))
    return rows


def source_inventory(tdlib_source: Path) -> tuple[dict[str, object], dict[str, object]]:
    scheme, header = verify_tdlib(tdlib_source)
    tl_rows = parse_scheme(scheme)
    header_rows = parse_header(header)
    require(set(tl_rows) == set(header_rows), "tl/header function bijection differs")
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
        "td_api_header_sha256": file_digest(header),
    }
    return inventory, evidence


def dormant_policy(inventory: dict[str, object]) -> dict[str, object]:
    functions = inventory["functions"]
    assert isinstance(functions, list)
    rows = [
        {
            "name": row["name"],
            "principal": "both",
            "admission": "denied",
            "body_validator": "deny",
            "sensitive_input": True,
            "sensitive_output": True,
            "reviewed": False,
            "review_reason": "dormant_unreviewed_default_deny",
        }
        for row in functions
    ]
    return {
        "schema_version": SCHEMA_VERSION,
        "tdlib_sha": PINNED_TDLIB_SHA,
        "activation_ready": False,
        "function_count": len(rows),
        "inventory_sha256": inventory["functions_sha256"],
        "policy_sha256": rows_digest(rows),
        "functions": rows,
    }


def lock_document(
    inventory: dict[str, object], policy: dict[str, object], evidence: dict[str, object]
) -> dict[str, object]:
    return {
        "schema_version": SCHEMA_VERSION,
        "tdlib_sha": PINNED_TDLIB_SHA,
        "function_count": inventory["function_count"],
        "inventory_sha256": inventory["functions_sha256"],
        "policy_sha256": policy["policy_sha256"],
        **evidence,
    }


def validate_policy(inventory: dict[str, object], policy: dict[str, object]) -> None:
    require(
        set(policy)
        == {
            "schema_version",
            "tdlib_sha",
            "activation_ready",
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
        require(row["principal"] in {"user", "bot", "both"}, "policy principal differs")
        require(
            row["admission"] in {"read", "write", "destructive", "denied"},
            "policy admission differs",
        )
        require(isinstance(row["body_validator"], str), "policy validator differs")
        require(
            row["body_validator"] in COMPILED_VALIDATORS,
            "compiled policy validator missing",
        )
        require(isinstance(row["sensitive_input"], bool), "request sensitivity differs")
        require(
            isinstance(row["sensitive_output"], bool), "response sensitivity differs"
        )
        require(isinstance(row["reviewed"], bool), "review state differs")
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
                row["review_reason"] == "dormant_unreviewed_default_deny",
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
    ready = all(row["reviewed"] for row in rows)
    require(
        policy["activation_ready"] == ready, "activation_ready/review relation differs"
    )


def validate_assets(tdlib_source: Path, output_root: Path, activation: bool) -> None:
    expected_inventory, evidence = source_inventory(tdlib_source)
    raw_root = output_root / "docs" / "raw"
    inventory = load_json(raw_root / f"td-functions.{PINNED_TDLIB_SHA}.json")
    policy = load_json(raw_root / f"raw-policy.{PINNED_TDLIB_SHA}.json")
    lock = load_json(raw_root / "raw-policy.lock.json")
    require(
        inventory == expected_inventory,
        "committed inventory differs from pinned source",
    )
    validate_policy(inventory, policy)
    require(
        lock == lock_document(inventory, policy, evidence),
        "committed raw policy lock differs",
    )
    if activation:
        require(
            policy["activation_ready"] is True, "raw policy is not activation-ready"
        )


def emit_seed(tdlib_source: Path, output_root: Path) -> None:
    inventory, evidence = source_inventory(tdlib_source)
    policy = dormant_policy(inventory)
    raw_root = output_root / "docs" / "raw"
    raw_root.mkdir(parents=True, exist_ok=True)
    (raw_root / f"td-functions.{PINNED_TDLIB_SHA}.json").write_bytes(
        json_bytes(inventory)
    )
    (raw_root / f"raw-policy.{PINNED_TDLIB_SHA}.json").write_bytes(json_bytes(policy))
    (raw_root / "raw-policy.lock.json").write_bytes(
        json_bytes(lock_document(inventory, policy, evidence))
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate and validate dormant raw policy assets"
    )
    parser.add_argument(
        "command", choices=("emit-seed", "validate-dormant", "validate-activation")
    )
    parser.add_argument("--tdlib-source", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "emit-seed":
            emit_seed(args.tdlib_source, args.output_root)
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
