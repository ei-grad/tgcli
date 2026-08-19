#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import NoReturn

DIALECT = "https://json-schema.org/draft/2020-12/schema"
ASCII_WHITESPACE = re.compile(r"[ \t\n\r\f\v]+")
COMMAND_KEY = re.compile(r"^[a-z0-9][a-z0-9-]*( [a-z0-9][a-z0-9-]*)*$")
KIND_ORDER = {"result": 0, "item": 1, "error": 2}
CATALOG_RULES = {
    "manifest.json": frozenset({"result"}),
    "stream-manifest.json": frozenset({"result", "item", "error"}),
    "error-manifest.json": frozenset({"error"}),
}


class GenerationError(RuntimeError):
    pass


@dataclass(frozen=True, order=True)
class Mapping:
    command: str
    kind_rank: int
    kind: str
    filename: str


@dataclass(frozen=True)
class CatalogData:
    name: str
    bytes: bytes
    document: dict[str, object]


@dataclass(frozen=True)
class AssetInventory:
    catalogs: tuple[CatalogData, ...]
    mappings: tuple[Mapping, ...]
    assets: tuple[tuple[str, bytes], ...]
    completion_keys: tuple[str, ...]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GenerationError(message)


def fail(message: str) -> NoReturn:
    raise GenerationError(message)


def duplicate_rejecting_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_json_constant(value: str) -> NoReturn:
    fail(f"invalid JSON constant: {value}")


def parse_json_object(data: bytes, owner: str) -> dict[str, object]:
    require(
        data.endswith(b"\n") and not data.endswith(b"\n\n"),
        f"{owner}: invalid final LF",
    )
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise GenerationError(f"{owner}: invalid UTF-8: {error}") from error
    try:
        document = json.loads(
            text,
            object_pairs_hook=duplicate_rejecting_object,
            parse_constant=reject_json_constant,
        )
    except (json.JSONDecodeError, GenerationError) as error:
        raise GenerationError(f"{owner}: invalid JSON: {error}") from error
    require(isinstance(document, dict), f"{owner}: root must be an object")
    return document


def checked_component(
    candidate: Path,
    expected: str,
    source_root: Path,
    resolved_root: Path,
    owner: str,
) -> Path:
    try:
        mode = os.lstat(candidate).st_mode
    except OSError as error:
        raise GenerationError(f"{owner}: missing or inaccessible: {error}") from error
    require(not stat.S_ISLNK(mode), f"{owner}: cannot be a symlink")
    if expected == "directory":
        require(stat.S_ISDIR(mode), f"{owner}: must be a directory")
    else:
        require(stat.S_ISREG(mode), f"{owner}: must be a regular file")
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(resolved_root)
    except (OSError, ValueError) as error:
        raise GenerationError(f"{owner}: escapes {source_root}: {error}") from error
    return resolved


def trusted_schema_directory(source_root_argument: Path) -> tuple[Path, Path]:
    source_root = source_root_argument.absolute()
    try:
        root_mode = os.lstat(source_root).st_mode
    except OSError as error:
        raise GenerationError(
            f"source root: missing or inaccessible: {error}"
        ) from error
    require(not stat.S_ISLNK(root_mode), "source root: cannot be a symlink")
    require(stat.S_ISDIR(root_mode), "source root: must be a directory")
    try:
        resolved_root = source_root.resolve(strict=True)
    except OSError as error:
        raise GenerationError(f"source root: cannot resolve: {error}") from error
    checked_component(
        source_root / "docs", "directory", source_root, resolved_root, "source docs"
    )
    schemas = checked_component(
        source_root / "docs" / "schemas",
        "directory",
        source_root,
        resolved_root,
        "source docs/schemas",
    )
    return resolved_root, schemas


def normalize_target(tokens: list[str]) -> str:
    components: list[str] = []
    for token in tokens:
        components.extend(part for part in ASCII_WHITESPACE.split(token) if part)
    return " ".join(components)


def canonicalize_target(target: str) -> str:
    return "read" if target == "history" else target


def validate_command_collisions(catalogs: tuple[CatalogData, ...]) -> None:
    canonical_spellings: dict[str, str] = {}
    for catalog in catalogs:
        commands = catalog.document["commands"]
        assert isinstance(commands, dict)
        for raw_key in commands:
            normalized = normalize_target([raw_key])
            canonical = canonicalize_target(normalized)
            previous = canonical_spellings.get(canonical)
            if previous is not None and previous != raw_key:
                fail(
                    f"catalog command collision: {previous!r} and {raw_key!r} "
                    f"canonicalize to {canonical!r}"
                )
            canonical_spellings[canonical] = raw_key


def validate_command_key(command: object, owner: str) -> str:
    require(
        isinstance(command, str) and command, f"{owner}: command key must be nonempty"
    )
    require(
        command.isascii() and COMMAND_KEY.fullmatch(command) is not None,
        f"{owner}: noncanonical command key",
    )
    require(
        normalize_target([command]) == command,
        f"{owner}: command key changes under normalization",
    )
    require(
        canonicalize_target(command) == command,
        f"{owner}: alias is not a canonical command key",
    )
    return command


def validate_reference(value: object, owner: str) -> str:
    require(
        isinstance(value, str) and value, f"{owner}: schema reference must be nonempty"
    )
    require(
        "\0" not in value and "\\" not in value, f"{owner}: unsafe schema reference"
    )
    reference = PurePosixPath(value)
    require(
        not reference.is_absolute()
        and len(reference.parts) == 1
        and reference.parts[0] not in {"", ".", ".."}
        and value.endswith(".schema.json"),
        f"{owner}: unsafe schema reference",
    )
    return value


def load_catalogs(
    source_root: Path, schemas: Path, resolved_root: Path
) -> tuple[CatalogData, ...]:
    loaded: list[CatalogData] = []
    for name in CATALOG_RULES:
        catalog_file = checked_component(
            schemas / name,
            "file",
            source_root,
            resolved_root,
            f"source catalog {name}",
        )
        data = catalog_file.read_bytes()
        document = parse_json_object(data, f"catalog {name}")
        require(
            set(document) == {"schemaDialect", "commands"},
            f"catalog {name}: invalid root keys",
        )
        require(
            document["schemaDialect"] == DIALECT, f"catalog {name}: invalid dialect"
        )
        require(
            isinstance(document["commands"], dict),
            f"catalog {name}: commands must be an object",
        )
        loaded.append(CatalogData(name, data, document))
    return tuple(loaded)


def build_inventory(source_root_argument: Path) -> AssetInventory:
    resolved_root, schemas = trusted_schema_directory(source_root_argument)
    source_root = source_root_argument.absolute()
    catalogs = load_catalogs(source_root, schemas, resolved_root)
    validate_command_collisions(catalogs)

    merged: dict[tuple[str, str], str] = {}
    referenced: set[str] = set()
    for catalog in catalogs:
        commands = catalog.document["commands"]
        assert isinstance(commands, dict)
        allowed = CATALOG_RULES[catalog.name]
        for raw_command, raw_contract in commands.items():
            command = validate_command_key(raw_command, f"catalog {catalog.name}")
            require(
                isinstance(raw_contract, dict) and raw_contract,
                f"catalog {catalog.name}/{command}: contract must be nonempty",
            )
            kinds = set(raw_contract)
            if catalog.name == "stream-manifest.json":
                require(
                    kinds <= allowed, f"catalog {catalog.name}/{command}: invalid kind"
                )
            else:
                require(
                    kinds == allowed, f"catalog {catalog.name}/{command}: invalid kinds"
                )
            for kind, raw_filename in raw_contract.items():
                require(
                    kind in allowed,
                    f"catalog {catalog.name}/{command}: invalid kind {kind}",
                )
                filename = validate_reference(
                    raw_filename, f"catalog {catalog.name}/{command}/{kind}"
                )
                key = (command, kind)
                previous = merged.get(key)
                if previous is not None and previous != filename:
                    fail(
                        f"conflicting schema mapping for {command}/{kind}: "
                        f"{previous} versus {filename}"
                    )
                merged[key] = filename
                referenced.add(filename)

    assets: list[tuple[str, bytes]] = []
    for filename in sorted(referenced):
        schema_file = checked_component(
            schemas / filename,
            "file",
            source_root,
            resolved_root,
            f"source schema {filename}",
        )
        data = schema_file.read_bytes()
        document = parse_json_object(data, f"schema {filename}")
        require(
            document.get("$schema") == DIALECT, f"schema {filename}: invalid dialect"
        )
        assets.append((filename, data))

    mappings = tuple(
        sorted(
            Mapping(command, KIND_ORDER[kind], kind, filename)
            for (command, kind), filename in merged.items()
        )
    )
    commands = sorted({mapping.command for mapping in mappings})
    if "read" in commands:
        commands.append("history")
    completion_keys = tuple(sorted(commands))
    return AssetInventory(catalogs, mappings, tuple(assets), completion_keys)


def raw_literal(data: bytes, owner: str) -> str:
    text = data.decode("utf-8")
    seed = hashlib.sha256(owner.encode("utf-8") + b"\0" + data).hexdigest()
    for index in range(256):
        delimiter = f"TG{seed[:10]}{index:02x}"
        if f'){delimiter}"' not in text:
            return f'R"{delimiter}({text}){delimiter}"'
    fail(f"cannot choose a raw string delimiter for {owner}")


def cpp_identifier(prefix: str, index: int) -> str:
    return f"k{prefix}{index}"


def render_cpp(inventory: AssetInventory) -> bytes:
    asset_names: dict[str, str] = {}
    lines = [
        '#include "cli/schema_catalog.hpp"',
        "",
        "#include <array>",
        "",
        "namespace tgcli::cli {",
        "namespace {",
        "",
    ]
    for index, (filename, data) in enumerate(inventory.assets):
        identifier = cpp_identifier("Schema", index)
        asset_names[filename] = identifier
        lines.append(
            f"constexpr std::string_view {identifier} = {raw_literal(data, filename)};"
        )
    lines.append("")
    for index, catalog in enumerate(inventory.catalogs):
        identifier = cpp_identifier("Catalog", index)
        lines.append(
            f"constexpr std::string_view {identifier} = "
            f"{raw_literal(catalog.bytes, catalog.name)};"
        )
    lines.extend(
        [
            "",
            f"constexpr std::array<EmbeddedCatalogView, {len(inventory.catalogs)}> kCatalogs{{{{",
        ]
    )
    for index, catalog in enumerate(inventory.catalogs):
        lines.append(f'    {{"{catalog.name}", {cpp_identifier("Catalog", index)}}},')
    lines.extend(
        [
            "}};",
            "",
            f"constexpr std::array<SchemaMappingView, {len(inventory.mappings)}> kMappings{{{{",
        ]
    )
    for mapping in inventory.mappings:
        kind = mapping.kind.capitalize()
        lines.append(
            f'    {{"{mapping.command}", SchemaPayloadKind::{kind}, "{mapping.filename}", '
            f"{asset_names[mapping.filename]}}},"
        )
    lines.extend(
        [
            "}};",
            "",
            f"constexpr std::array<std::string_view, {len(inventory.completion_keys)}> kCompletionKeys{{{{",
        ]
    )
    for command in inventory.completion_keys:
        lines.append(f'    "{command}",')
    lines.extend(
        [
            "}};",
            "",
            "} // namespace",
            "",
            "std::span<const EmbeddedCatalogView> embedded_schema_catalogs() noexcept {",
            "    return kCatalogs;",
            "}",
            "",
            "std::span<const SchemaMappingView> embedded_schema_mappings() noexcept {",
            "    return kMappings;",
            "}",
            "",
            "std::span<const std::string_view> schema_target_completion_keys() noexcept {",
            "    return kCompletionKeys;",
            "}",
            "",
            "} // namespace tgcli::cli",
            "",
        ]
    )
    return "\n".join(lines).encode("utf-8")


def atomic_write(output: Path, data: bytes) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and output.is_symlink():
        fail(f"output cannot be a symlink: {output}")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", dir=output.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
    finally:
        if temporary.exists():
            temporary.unlink()


def emit_cpp(source_root: Path, output: Path) -> None:
    atomic_write(output, render_cpp(build_inventory(source_root)))


def package_files(source_root: Path) -> tuple[str, ...]:
    inventory = build_inventory(source_root)
    names = [f"docs/schemas/{catalog.name}" for catalog in inventory.catalogs]
    names.extend(f"docs/schemas/{filename}" for filename, _ in inventory.assets)
    return tuple(sorted(names))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate trusted embedded schema assets"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    emit = subparsers.add_parser("emit-cpp")
    emit.add_argument("--source-root", type=Path, required=True)
    emit.add_argument("--output", type=Path, required=True)
    listing = subparsers.add_parser("list-package-files")
    listing.add_argument("--source-root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "emit-cpp":
            emit_cpp(args.source_root, args.output)
        else:
            for filename in package_files(args.source_root):
                print(filename)
    except (GenerationError, OSError) as error:
        print(f"schema asset generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
