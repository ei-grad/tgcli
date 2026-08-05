#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys


class Re2VerificationError(RuntimeError):
    pass


RE2_RUNTIME_SOURCES = {
    "re2/bitstate.cc",
    "re2/compile.cc",
    "re2/dfa.cc",
    "re2/filtered_re2.cc",
    "re2/mimics_pcre.cc",
    "re2/nfa.cc",
    "re2/onepass.cc",
    "re2/parse.cc",
    "re2/perl_groups.cc",
    "re2/prefilter.cc",
    "re2/prefilter_tree.cc",
    "re2/prog.cc",
    "re2/re2.cc",
    "re2/regexp.cc",
    "re2/set.cc",
    "re2/simplify.cc",
    "re2/stringpiece.cc",
    "re2/tostring.cc",
    "re2/unicode_casefold.cc",
    "re2/unicode_groups.cc",
    "util/rune.cc",
    "util/strutil.cc",
}
RE2_TEST_SOURCES = {
    "util/benchmark.cc",
    "util/pcre.cc",
    "util/test.cc",
}
RE2_TEST_TARGETS = {
    "charclass_test",
    "compile_test",
    "dfa_test",
    "exhaustive1_test",
    "exhaustive2_test",
    "exhaustive3_test",
    "exhaustive_test",
    "filtered_re2_test",
    "mimics_pcre_test",
    "parse_test",
    "possible_match_test",
    "random_test",
    "re2_arg_test",
    "re2_test",
    "regexp_benchmark",
    "regexp_test",
    "required_prefix_test",
    "search_test",
    "set_test",
    "simplify_test",
    "string_generator_test",
}
FORBIDDEN_DEFINITION = re.compile(
    r"(?:^|[\s\"'])(?:-D|/D)(?:RE2_USE_ICU|USEPCRE)(?:=[^\s\"']+)?"
    r"(?=$|[\s\"'])"
)
FORBIDDEN_LIBRARY = re.compile(
    r"(?:^|[\s\"'])(?:-l(?:icu|pcre|absl)[^\s\"']*|[^\s\"']*/"
    r"lib(?:icu|pcre|absl)[^\s\"']*\.(?:a|dylib|so(?:\.[0-9]+)*))"
    r"(?=$|[\s\"'])",
    re.IGNORECASE,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Re2VerificationError(message)


def read_regular_text(file: pathlib.Path, owner: str) -> str:
    require(file.is_file(), f"{owner} is missing: {file}")
    require(not file.is_symlink(), f"{owner} cannot be a symlink: {file}")
    try:
        return file.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise Re2VerificationError(f"cannot read {owner}: {error}") from error


def validate_cache(text: str) -> None:
    entries: dict[str, tuple[str, str]] = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([^:#=]+):([^=]+)=(.*)", line)
        if match is None:
            continue
        name, value_type, value = match.groups()
        require(name not in entries, f"duplicate CMake cache entry: {name}")
        entries[name] = (value_type, value)
    require(
        "RE2_USE_ICU" not in entries,
        "RE2 build contains the nonexistent RE2_USE_ICU cache option",
    )


def load_json(file: pathlib.Path, owner: str) -> dict:
    try:
        document = json.loads(read_regular_text(file, owner))
    except json.JSONDecodeError as error:
        raise Re2VerificationError(f"cannot parse {owner}: {error}") from error
    require(isinstance(document, dict), f"{owner} must contain a JSON object")
    return document


def expected_archive(build_directory: pathlib.Path) -> pathlib.Path:
    return (build_directory / "_deps/re2-build/libre2.a").resolve()


def validate_target_evidence(build_directory: pathlib.Path) -> pathlib.Path:
    evidence = load_json(
        build_directory / "re2-target-evidence.json", "RE2 target evidence"
    )
    require(
        set(evidence)
        == {
            "archive",
            "archive_name",
            "configured_options",
            "runtime_libraries",
            "schema_version",
            "target",
            "target_type",
        }
        and evidence["schema_version"] == 1,
        "RE2 target evidence has an invalid schema",
    )
    require(
        evidence["configured_options"]
        == {"BUILD_SHARED_LIBS": False, "RE2_BUILD_TESTING": False, "USEPCRE": False}
        and evidence["runtime_libraries"] == ["Threads::Threads"]
        and evidence["target"] == "re2::re2"
        and evidence["target_type"] == "STATIC_LIBRARY"
        and evidence["archive_name"] == "libre2.a",
        "RE2 target evidence differs from the required static configuration",
    )
    archive_value = evidence["archive"]
    require(
        isinstance(archive_value, str) and pathlib.Path(archive_value).is_absolute(),
        "RE2 target evidence archive path is invalid",
    )
    archive = pathlib.Path(archive_value)
    require(
        archive.is_file() and not archive.is_symlink(),
        "RE2 target evidence archive is missing or unsafe",
    )
    require(
        archive.resolve() == expected_archive(build_directory),
        "RE2 target evidence does not identify the canonical pinned archive",
    )
    return archive.resolve()


def normalized_source_suffix(file: str) -> str | None:
    normalized = file.replace("\\", "/")
    for suffix in RE2_RUNTIME_SOURCES | RE2_TEST_SOURCES:
        if normalized.endswith(f"/{suffix}"):
            return suffix
    if "/re2/testing/" in normalized:
        return "re2/testing/"
    return None


def compile_command(entry: dict) -> str:
    if isinstance(entry.get("command"), str):
        return entry["command"]
    arguments = entry.get("arguments")
    require(
        isinstance(arguments, list)
        and all(isinstance(argument, str) for argument in arguments),
        "RE2 compile database entry has no command",
    )
    return " ".join(arguments)


def validate_compile_commands(document: object) -> None:
    require(isinstance(document, list), "compile_commands.json must contain an array")
    runtime_sources: set[str] = set()
    for entry in document:
        require(isinstance(entry, dict), "compile database entry must be an object")
        source = entry.get("file")
        require(
            isinstance(source, str) and source, "compile database entry has no file"
        )
        suffix = normalized_source_suffix(source)
        if suffix is None:
            continue
        require(
            suffix in RE2_RUNTIME_SOURCES,
            f"RE2 test or benchmark source entered the build: {source}",
        )
        require(
            suffix not in runtime_sources, f"duplicate RE2 runtime source: {suffix}"
        )
        runtime_sources.add(suffix)
        command = compile_command(entry)
        require(
            FORBIDDEN_DEFINITION.search(command) is None,
            f"forbidden RE2 compile definition for {suffix}",
        )
    require(
        runtime_sources == RE2_RUNTIME_SOURCES,
        "RE2 runtime source inventory differs from the pinned upstream target",
    )


def validate_build_artifacts(build_directory: pathlib.Path) -> pathlib.Path:
    re2_build_directory = build_directory / "_deps/re2-build"
    require(
        re2_build_directory.is_dir() and not re2_build_directory.is_symlink(),
        "RE2 sub-build directory is missing or unsafe",
    )
    static_libraries: list[pathlib.Path] = []
    for entry in re2_build_directory.rglob("*"):
        name = entry.name
        lower_name = name.lower()
        if lower_name == "libre2.a":
            require(entry.is_file() and not entry.is_symlink(), "libre2.a is unsafe")
            static_libraries.append(entry)
        if lower_name.startswith("libre2.") and lower_name != "libre2.a":
            raise Re2VerificationError(f"shared or unexpected RE2 artifact: {entry}")
        if lower_name.startswith(("libabsl", "libicu", "libpcre")):
            raise Re2VerificationError(f"forbidden RE2 transitive artifact: {entry}")
        stem = entry.stem.removesuffix(".exe")
        if entry.is_file() and stem in RE2_TEST_TARGETS:
            raise Re2VerificationError(f"RE2 test or benchmark artifact: {entry}")
    require(
        len(static_libraries) == 1, "build must contain exactly one static libre2.a"
    )
    require(
        static_libraries[0].resolve() == expected_archive(build_directory),
        "static RE2 archive is not at the canonical build path",
    )
    for entry in build_directory.rglob("*"):
        if entry.is_file() and entry.name.lower().startswith(
            ("libabsl", "libicu", "libpcre")
        ):
            raise Re2VerificationError(f"forbidden RE2 transitive artifact: {entry}")
    return static_libraries[0].resolve()


def token_path(token: str, build_directory: pathlib.Path) -> pathlib.Path:
    candidate = pathlib.Path(token)
    if not candidate.is_absolute():
        candidate = build_directory / candidate
    return candidate.resolve()


def archive_tokens(tokens: list[str]) -> list[str]:
    return [
        token
        for token in tokens
        if pathlib.PurePosixPath(token.replace("\\", "/")).name == "libre2.a"
    ]


def parse_command(line: str) -> list[str]:
    try:
        return shlex.split(line, posix=True)
    except ValueError as error:
        raise Re2VerificationError(f"cannot parse Ninja command: {error}") from error


def validate_link_commands(
    text: str, build_directory: pathlib.Path, archive: pathlib.Path
) -> str:
    require(
        FORBIDDEN_DEFINITION.search(text) is None,
        "RE2 link graph contains ICU or PCRE definitions",
    )
    require(
        FORBIDDEN_LIBRARY.search(text) is None,
        "RE2 link graph contains a forbidden runtime library",
    )
    executable_links: list[tuple[str, list[str]]] = []
    for line in text.splitlines():
        tokens = parse_command(line)
        outputs = [
            tokens[index + 1]
            for index, token in enumerate(tokens[:-1])
            if token == "-o"
        ]
        if any(
            pathlib.PurePosixPath(output.replace("\\", "/")).name == "tgcli"
            for output in outputs
        ):
            executable_links.append((line, tokens))
    require(len(executable_links) == 1, "cannot identify the final tgcli link command")
    line, tokens = executable_links[0]
    candidates = archive_tokens(tokens)
    require(
        len(candidates) == 1
        and token_path(candidates[0], build_directory) == archive.resolve(),
        "final tgcli link command does not identify only the canonical RE2 archive",
    )
    return line


def validate_link_map(
    text: str, build_directory: pathlib.Path, archive: pathlib.Path
) -> None:
    require(
        FORBIDDEN_LIBRARY.search(text) is None,
        "final link map contains a forbidden RE2 runtime library",
    )
    loaded_archives: list[str] = []
    for line in text.splitlines():
        tokens = parse_command(line)
        if tokens and tokens[0] == "LOAD":
            loaded_archives.extend(archive_tokens(tokens[1:]))
    require(
        len(loaded_archives) == 1
        and token_path(loaded_archives[0], build_directory) == archive.resolve(),
        "final link map does not identify only the canonical RE2 archive",
    )


def sha256_file(file: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with file.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def validate_build_evidence(document: object) -> dict:
    require(isinstance(document, dict), "RE2 build evidence must be a JSON object")
    require(
        set(document)
        == {
            "archive",
            "checks",
            "compile_sources",
            "configured_options",
            "final_link",
            "link_map",
            "runtime_libraries",
            "schema_version",
            "target",
            "target_type",
        }
        and document["schema_version"] == 1,
        "RE2 build evidence has an invalid schema",
    )
    archive = document["archive"]
    require(
        isinstance(archive, dict)
        and set(archive) == {"path", "sha256", "size"}
        and archive["path"] == "_deps/re2-build/libre2.a"
        and isinstance(archive["sha256"], str)
        and re.fullmatch(r"[0-9a-f]{64}", archive["sha256"]) is not None
        and type(archive["size"]) is int
        and archive["size"] > 0,
        "RE2 build evidence archive identity is invalid",
    )
    require(
        document["checks"]
        == {
            "abseil_absent": True,
            "benchmarks_absent": True,
            "icu_absent": True,
            "pcre_absent": True,
            "shared_re2_absent": True,
            "tests_absent": True,
        }
        and document["compile_sources"] == sorted(RE2_RUNTIME_SOURCES)
        and document["configured_options"]
        == {"BUILD_SHARED_LIBS": False, "RE2_BUILD_TESTING": False, "USEPCRE": False}
        and document["runtime_libraries"] == ["Threads::Threads"]
        and document["target"] == "re2::re2"
        and document["target_type"] == "STATIC_LIBRARY",
        "RE2 build evidence does not prove the required static runtime closure",
    )
    final_link = document["final_link"]
    require(
        isinstance(final_link, dict)
        and set(final_link) == {"archive", "command_sha256"}
        and final_link["archive"] == "_deps/re2-build/libre2.a"
        and isinstance(final_link["command_sha256"], str)
        and re.fullmatch(r"[0-9a-f]{64}", final_link["command_sha256"]) is not None,
        "RE2 final-link evidence is invalid",
    )
    link_map = document["link_map"]
    require(
        link_map is None or isinstance(link_map, dict),
        "RE2 link-map evidence is invalid",
    )
    if isinstance(link_map, dict):
        require(
            set(link_map) == {"archive", "sha256", "size"}
            and link_map["archive"] == "_deps/re2-build/libre2.a"
            and isinstance(link_map["sha256"], str)
            and re.fullmatch(r"[0-9a-f]{64}", link_map["sha256"]) is not None
            and type(link_map["size"]) is int
            and link_map["size"] > 0,
            "RE2 link-map evidence is invalid",
        )
    return document


def write_build_evidence(
    output: pathlib.Path,
    archive: pathlib.Path,
    final_link: str,
    link_map: pathlib.Path | None,
) -> None:
    require(
        not output.exists() and not output.is_symlink(),
        f"RE2 build evidence output already exists: {output}",
    )
    require(output.parent.is_dir(), "RE2 build evidence output parent is missing")
    link_map_identity = None
    if link_map is not None:
        link_map_identity = {
            "archive": "_deps/re2-build/libre2.a",
            "sha256": sha256_file(link_map),
            "size": link_map.stat().st_size,
        }
    evidence = {
        "archive": {
            "path": "_deps/re2-build/libre2.a",
            "sha256": sha256_file(archive),
            "size": archive.stat().st_size,
        },
        "checks": {
            "abseil_absent": True,
            "benchmarks_absent": True,
            "icu_absent": True,
            "pcre_absent": True,
            "shared_re2_absent": True,
            "tests_absent": True,
        },
        "compile_sources": sorted(RE2_RUNTIME_SOURCES),
        "configured_options": {
            "BUILD_SHARED_LIBS": False,
            "RE2_BUILD_TESTING": False,
            "USEPCRE": False,
        },
        "final_link": {
            "archive": "_deps/re2-build/libre2.a",
            "command_sha256": hashlib.sha256(final_link.encode("utf-8")).hexdigest(),
        },
        "link_map": link_map_identity,
        "runtime_libraries": ["Threads::Threads"],
        "schema_version": 1,
        "target": "re2::re2",
        "target_type": "STATIC_LIBRARY",
    }
    validate_build_evidence(evidence)
    try:
        with output.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    except OSError as error:
        raise Re2VerificationError(
            f"cannot write RE2 build evidence: {error}"
        ) from error


def run_ninja_commands(build_directory: pathlib.Path, ninja: str) -> str:
    try:
        completed = subprocess.run(
            [ninja, "-C", str(build_directory), "-t", "commands", "tgcli"],
            capture_output=True,
            check=False,
            text=True,
        )
    except OSError as error:
        raise Re2VerificationError(f"cannot execute Ninja: {error}") from error
    require(completed.returncode == 0, "cannot inspect the tgcli Ninja link graph")
    require(not completed.stderr, "Ninja link graph inspection wrote to stderr")
    return completed.stdout


def verify(args: argparse.Namespace) -> None:
    build_directory = args.build_directory.resolve()
    require(
        build_directory.is_dir() and not build_directory.is_symlink(),
        f"RE2 build directory is missing or unsafe: {build_directory}",
    )
    validate_cache(read_regular_text(build_directory / "CMakeCache.txt", "CMake cache"))
    try:
        compile_commands = json.loads(
            read_regular_text(
                build_directory / "compile_commands.json", "compile database"
            )
        )
    except json.JSONDecodeError as error:
        raise Re2VerificationError(f"cannot parse compile database: {error}") from error
    validate_compile_commands(compile_commands)
    target_archive = validate_target_evidence(build_directory)
    build_archive = validate_build_artifacts(build_directory)
    require(
        target_archive == build_archive, "RE2 build evidence archive identities differ"
    )
    final_link = validate_link_commands(
        run_ninja_commands(build_directory, args.ninja), build_directory, build_archive
    )
    link_map = None
    if args.link_map is not None:
        link_map = args.link_map.resolve()
        validate_link_map(
            read_regular_text(link_map, "final link map"),
            build_directory,
            build_archive,
        )
    if args.output is not None:
        write_build_evidence(args.output.resolve(), build_archive, final_link, link_map)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify the pinned static RE2 build")
    parser.add_argument("--build-directory", type=pathlib.Path, required=True)
    parser.add_argument("--link-map", type=pathlib.Path)
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--output", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    try:
        verify(parse_args())
    except (OSError, Re2VerificationError) as error:
        print(f"RE2 build verification failed: {error}", file=sys.stderr)
        return 1
    print("RE2 build integration verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
