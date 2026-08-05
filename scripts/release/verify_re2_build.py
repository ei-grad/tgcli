#!/usr/bin/env python3

import argparse
import json
import pathlib
import re
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
    for option in ("BUILD_SHARED_LIBS", "RE2_BUILD_TESTING", "USEPCRE"):
        require(
            entries.get(option) == ("BOOL", "OFF"),
            f"RE2 build requires {option}:BOOL=OFF",
        )
    require(
        "RE2_USE_ICU" not in entries,
        "RE2 build contains the nonexistent RE2_USE_ICU cache option",
    )


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


def validate_build_artifacts(build_directory: pathlib.Path) -> None:
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
    for entry in build_directory.rglob("*"):
        if entry.is_file() and entry.name.lower().startswith(
            ("libabsl", "libicu", "libpcre")
        ):
            raise Re2VerificationError(f"forbidden RE2 transitive artifact: {entry}")


def validate_link_commands(text: str) -> None:
    require(
        FORBIDDEN_DEFINITION.search(text) is None,
        "RE2 link graph contains ICU or PCRE definitions",
    )
    require(
        FORBIDDEN_LIBRARY.search(text) is None,
        "RE2 link graph contains a forbidden runtime library",
    )
    executable_links = [
        line
        for line in text.splitlines()
        if re.search(r"(?:^|\s)-o\s+(?:[^\s]*/)?tgcli(?:\s|$)", line)
    ]
    require(len(executable_links) == 1, "cannot identify the final tgcli link command")
    require(
        "libre2.a" in executable_links[0],
        "final tgcli link command does not contain static RE2",
    )


def validate_link_map(text: str) -> None:
    require("libre2.a" in text, "final link map does not contain static RE2")
    require(
        FORBIDDEN_LIBRARY.search(text) is None,
        "final link map contains a forbidden RE2 runtime library",
    )


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
    validate_build_artifacts(build_directory)
    validate_link_commands(run_ninja_commands(build_directory, args.ninja))
    if args.link_map is not None:
        validate_link_map(read_regular_text(args.link_map.resolve(), "final link map"))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify the pinned static RE2 build")
    parser.add_argument("--build-directory", type=pathlib.Path, required=True)
    parser.add_argument("--link-map", type=pathlib.Path)
    parser.add_argument("--ninja", default="ninja")
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
