#!/usr/bin/env python3

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import NoReturn

DIALECT = "https://json-schema.org/draft/2020-12/schema"
ASCII_WHITESPACE = re.compile(r"[ \t\n\r\f\v]+")
COMMAND_KEY = re.compile(r"^[a-z0-9][a-z0-9-]*( [a-z0-9][a-z0-9-]*)*$")
PORTABLE_SCHEMA_STEM = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
WINDOWS_RESERVED_NAMES = frozenset(
    {"con", "prn", "aux", "nul"}
    | {f"com{index}" for index in range(1, 10)}
    | {f"lpt{index}" for index in range(1, 10)}
)
KIND_ORDER = {"result": 0, "item": 1, "error": 2}
CATALOG_RULES = {
    "manifest.json": frozenset({"result"}),
    "stream-manifest.json": frozenset({"result", "item", "error"}),
    "error-manifest.json": frozenset({"error"}),
}
DIRECTORY_OPEN_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
REGULAR_OPEN_FLAGS = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
READ_CHUNK_SIZE = 64 * 1024
# Keep every raw-literal body well below the C++ support floor of 65,536 bytes.
CPP_LITERAL_CHUNK_BYTES = 16 * 1024


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


@dataclass
class TrustedSchemaSource:
    root_fd: int
    docs_fd: int
    schemas_fd: int

    @classmethod
    def open(cls, source_root_argument: Path) -> TrustedSchemaSource:
        source_root = source_root_argument.absolute()
        descriptors: list[int] = []
        try:
            root_fd = open_trusted_component(
                source_root, DIRECTORY_OPEN_FLAGS, "directory", "source root"
            )
            descriptors.append(root_fd)
            docs_fd = open_trusted_component(
                "docs",
                DIRECTORY_OPEN_FLAGS,
                "directory",
                "source docs",
                dir_fd=root_fd,
            )
            descriptors.append(docs_fd)
            schemas_fd = open_trusted_component(
                "schemas",
                DIRECTORY_OPEN_FLAGS,
                "directory",
                "source docs/schemas",
                dir_fd=docs_fd,
            )
            descriptors.append(schemas_fd)
        except GenerationError:
            for descriptor in reversed(descriptors):
                os.close(descriptor)
            raise
        return cls(root_fd, docs_fd, schemas_fd)

    def close(self) -> None:
        os.close(self.schemas_fd)
        os.close(self.docs_fd)
        os.close(self.root_fd)

    def __enter__(self):
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def verify_edges(self) -> None:
        # Descriptor identity proves retained containment across directory renames.
        verify_directory_edge(self.docs_fd, self.root_fd, "source docs")
        verify_directory_edge(self.schemas_fd, self.docs_fd, "source docs/schemas")

    def read_leaf(self, filename: str, owner: str) -> bytes:
        self.verify_edges()
        descriptor = open_trusted_component(
            filename,
            REGULAR_OPEN_FLAGS,
            "file",
            owner,
            dir_fd=self.schemas_fd,
        )
        try:
            data = read_descriptor_bytes(descriptor, owner)
            # Reject a parent escape that raced the opened leaf read.
            self.verify_edges()
            return data
        finally:
            os.close(descriptor)


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


def component_mode(
    candidate: str | Path,
    dir_fd: int | None,
) -> int | None:
    try:
        return os.stat(candidate, dir_fd=dir_fd, follow_symlinks=False).st_mode
    except OSError:
        return None


def open_trusted_component(
    candidate: str | Path,
    flags: int,
    expected: str,
    owner: str,
    *,
    dir_fd: int | None = None,
) -> int:
    try:
        descriptor = os.open(candidate, flags, dir_fd=dir_fd)
    except OSError as error:
        mode = component_mode(candidate, dir_fd)
        if mode is not None and stat.S_ISLNK(mode):
            raise GenerationError(f"{owner}: cannot be a symlink") from error
        if mode is not None and expected == "directory" and not stat.S_ISDIR(mode):
            raise GenerationError(f"{owner}: must be a directory") from error
        if mode is not None and expected == "file" and not stat.S_ISREG(mode):
            raise GenerationError(f"{owner}: must be a regular file") from error
        if error.errno in {errno.ENOENT, errno.ENOTDIR}:
            raise GenerationError(
                f"{owner}: missing or inaccessible: {error}"
            ) from error
        raise GenerationError(
            f"{owner}: cannot open trusted component: {error}"
        ) from error
    try:
        mode = os.fstat(descriptor).st_mode
    except OSError as error:
        os.close(descriptor)
        raise GenerationError(
            f"{owner}: cannot inspect opened component: {error}"
        ) from error
    valid_type = stat.S_ISDIR(mode) if expected == "directory" else stat.S_ISREG(mode)
    if not valid_type:
        os.close(descriptor)
        type_name = "directory" if expected == "directory" else "regular file"
        fail(f"{owner}: must be a {type_name}")
    return descriptor


def read_descriptor_bytes(descriptor: int, owner: str) -> bytes:
    chunks: list[bytes] = []
    try:
        while True:
            chunk = os.read(descriptor, READ_CHUNK_SIZE)
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)
    except OSError as error:
        raise GenerationError(
            f"{owner}: cannot read opened component: {error}"
        ) from error


def verify_directory_edge(child_fd: int, parent_fd: int, owner: str) -> None:
    observed_parent = open_trusted_component(
        "..",
        DIRECTORY_OPEN_FLAGS,
        "directory",
        f"{owner} parent edge",
        dir_fd=child_fd,
    )
    try:
        observed = os.fstat(observed_parent)
        expected = os.fstat(parent_fd)
    except OSError as error:
        raise GenerationError(
            f"{owner}: cannot verify retained edge: {error}"
        ) from error
    finally:
        os.close(observed_parent)
    require(
        (observed.st_dev, observed.st_ino) == (expected.st_dev, expected.st_ino),
        f"{owner}: retained directory is outside its trusted parent",
    )


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
    posix_reference = PurePosixPath(value)
    windows_reference = PureWindowsPath(value)
    suffix = ".schema.json"
    stem = value[: -len(suffix)] if value.endswith(suffix) else ""
    require(
        posix_reference.name == value
        and windows_reference.name == value
        and stem not in {"", ".", ".."}
        and PORTABLE_SCHEMA_STEM.fullmatch(stem) is not None
        and stem.split(".", 1)[0] not in WINDOWS_RESERVED_NAMES,
        f"{owner}: unsafe schema reference",
    )
    return value


def load_catalogs(source: TrustedSchemaSource) -> tuple[CatalogData, ...]:
    loaded: list[CatalogData] = []
    for name in CATALOG_RULES:
        data = source.read_leaf(name, f"source catalog {name}")
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
    with TrustedSchemaSource.open(source_root_argument) as source:
        catalogs = load_catalogs(source)
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
                        kinds <= allowed,
                        f"catalog {catalog.name}/{command}: invalid kind",
                    )
                else:
                    require(
                        kinds == allowed,
                        f"catalog {catalog.name}/{command}: invalid kinds",
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
            data = source.read_leaf(filename, f"source schema {filename}")
            document = parse_json_object(data, f"schema {filename}")
            require(
                document.get("$schema") == DIALECT,
                f"schema {filename}: invalid dialect",
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


def literal_chunks(data: bytes, owner: str) -> tuple[bytes, ...]:
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise GenerationError(f"{owner}: invalid UTF-8: {error}") from error
    if not data:
        return (b"",)

    chunks: list[bytes] = []
    start = 0
    while start < len(data):
        end = min(start + CPP_LITERAL_CHUNK_BYTES, len(data))
        while end < len(data) and data[end] & 0xC0 == 0x80:
            end -= 1
        require(end > start, f"{owner}: UTF-8 code point exceeds literal chunk bound")
        chunk = data[start:end]
        chunk.decode("utf-8")
        chunks.append(chunk)
        start = end
    return tuple(chunks)


def cpp_identifier(prefix: str, index: int) -> str:
    return f"k{prefix}{index}"


def cpp_string_literal(value: str) -> str:
    encoded: list[str] = []
    for byte in value.encode("utf-8"):
        if 0x20 <= byte <= 0x7E and byte not in {ord('"'), ord("\\")}:
            encoded.append(chr(byte))
        elif byte == ord('"'):
            encoded.append('\\"')
        elif byte == ord("\\"):
            encoded.append("\\\\")
        else:
            encoded.append(f"\\{byte:03o}")
    return f'"{"".join(encoded)}"'


def render_asset(identifier: str, data: bytes, owner: str) -> list[str]:
    chunks = literal_chunks(data, owner)
    lines = [
        f"constexpr auto {identifier}Storage = assemble_asset<{len(data)}>(",
        f"    std::array<std::string_view, {len(chunks)}>{{{{",
    ]
    for index, chunk in enumerate(chunks):
        lines.append(f"        {raw_literal(chunk, f'{owner} chunk {index}')},")
    lines.extend(
        [
            "    }});",
            (
                f"constexpr std::string_view {identifier}{{{identifier}Storage.data(), "
                f"{identifier}Storage.size()}};"
            ),
        ]
    )
    return lines


def render_cpp(inventory: AssetInventory) -> bytes:
    asset_names: dict[str, str] = {}
    lines = [
        '#include "cli/schema_catalog.hpp"',
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <string_view>",
        "",
        "namespace tgcli::cli {",
        "namespace {",
        "",
        "template <std::size_t Size, std::size_t ChunkCount>",
        "consteval std::array<char, Size>",
        "assemble_asset(const std::array<std::string_view, ChunkCount>& chunks) {",
        "    std::array<char, Size> bytes{};",
        "    std::size_t offset = 0;",
        "    for (const auto chunk : chunks) {",
        "        for (const char value : chunk) {",
        "            bytes[offset++] = value;",
        "        }",
        "    }",
        "    if (offset != Size) {",
        '        throw "schema asset size mismatch";',
        "    }",
        "    return bytes;",
        "}",
        "",
    ]
    for index, (filename, data) in enumerate(inventory.assets):
        identifier = cpp_identifier("Schema", index)
        asset_names[filename] = identifier
        lines.extend(render_asset(identifier, data, filename))
    lines.append("")
    for index, catalog in enumerate(inventory.catalogs):
        identifier = cpp_identifier("Catalog", index)
        lines.extend(render_asset(identifier, catalog.bytes, catalog.name))
    lines.extend(
        [
            "",
            f"constexpr std::array<EmbeddedCatalogView, {len(inventory.catalogs)}> kCatalogs{{{{",
        ]
    )
    for index, catalog in enumerate(inventory.catalogs):
        lines.append(
            f"    {{{cpp_string_literal(catalog.name)}, "
            f"{cpp_identifier('Catalog', index)}}},"
        )
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
            f"    {{{cpp_string_literal(mapping.command)}, SchemaPayloadKind::{kind}, "
            f"{cpp_string_literal(mapping.filename)}, "
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
        lines.append(f"    {cpp_string_literal(command)},")
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
