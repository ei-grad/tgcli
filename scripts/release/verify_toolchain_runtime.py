#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys


class RuntimeVerificationError(RuntimeError):
    pass


CRT_NAMES = {"crt1.o", "crti.o", "crtbeginT.o", "crtend.o", "crtn.o"}
LIBRARY_FLAGS = {
    "-latomic_asneeded": ("libatomic_asneeded.a", "libatomic.a"),
    "-lc": ("libc.a", "libc.a"),
    "-ldl": ("libdl.a", "libdl.a"),
    "-lgcc": ("libgcc.a", "libgcc.a"),
    "-lgcc_eh": ("libgcc_eh.a", "libgcc_eh.a"),
    "-lm": ("libm.a", "libm.a"),
    "-lstdc++": ("libstdc++.a", "libstdc++.a"),
}
RUNTIME_NAMES = CRT_NAMES | {locked_name for _, locked_name in LIBRARY_FLAGS.values()}
MAP_NAMES = {name: name for name in CRT_NAMES}
MAP_NAMES.update(
    {query_name: locked_name for query_name, locked_name in LIBRARY_FLAGS.values()}
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeVerificationError(message)


def sha256_file(file: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with file.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def load_lock(file: pathlib.Path) -> tuple[dict, dict[str, dict]]:
    try:
        document = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RuntimeVerificationError(
            f"cannot read dependency lock: {error}"
        ) from error
    require(isinstance(document, dict), "dependency lock must be an object")
    toolchains = document.get("release_toolchains")
    require(
        isinstance(toolchains, list) and len(toolchains) == 1,
        "dependency lock must contain one release toolchain",
    )
    entries = toolchains[0].get("runtime_files")
    require(isinstance(entries, list), "toolchain runtime inventory is missing")
    by_name: dict[str, dict] = {}
    for entry in entries:
        require(isinstance(entry, dict), "toolchain runtime entry must be an object")
        name = pathlib.PurePosixPath(entry.get("path", "")).name
        require(name in RUNTIME_NAMES, f"unexpected locked runtime input: {name}")
        require(name not in by_name, f"duplicate locked runtime basename: {name}")
        by_name[name] = entry
    require(set(by_name) == RUNTIME_NAMES, "locked runtime input set is incomplete")
    return toolchains[0], by_name


def canonical_relative(
    candidate: pathlib.Path, toolchain_root: pathlib.Path, owner: str
) -> str:
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as error:
        raise RuntimeVerificationError(f"{owner} is missing: {candidate}") from error
    require(
        resolved.is_file() and not resolved.is_symlink(),
        f"{owner} is not a regular file: {candidate}",
    )
    require(
        resolved.is_relative_to(toolchain_root),
        f"{owner} resolves outside the pinned toolchain: {candidate}",
    )
    return resolved.relative_to(toolchain_root).as_posix()


def run_compiler(
    compiler: pathlib.Path, arguments: list[str]
) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            [str(compiler), *arguments],
            capture_output=True,
            check=False,
            input=b"",
        )
    except OSError as error:
        raise RuntimeVerificationError(
            f"cannot execute cross compiler: {error}"
        ) from error


def driver_selection(
    compiler: pathlib.Path, toolchain_root: pathlib.Path
) -> tuple[dict[str, str], list[str]]:
    arguments = [
        "-###",
        "-static",
        "-Wl,--build-id=none",
        "-x",
        "c++",
        "-",
        "-ldl",
        "-o",
        "tgcli-runtime-probe",
    ]
    completed = run_compiler(compiler, arguments)
    require(completed.returncode == 0, "cross compiler -### query failed")
    try:
        driver_output = completed.stderr.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise RuntimeVerificationError(
            "cross compiler -### output is not UTF-8"
        ) from error
    collect_lines = [
        line for line in driver_output.splitlines() if "/collect2 " in line
    ]
    require(len(collect_lines) == 1, "cross compiler emitted an ambiguous link command")
    try:
        tokens = shlex.split(collect_lines[0])
    except ValueError as error:
        raise RuntimeVerificationError(
            "cannot parse cross compiler link command"
        ) from error

    selected: dict[str, str] = {}
    for token in tokens:
        name = pathlib.PurePosixPath(token).name
        if re.fullmatch(r"crt[A-Za-z0-9_+.~-]*\.o", name) is None:
            continue
        require(
            name not in selected, f"driver selected duplicate startup object: {name}"
        )
        selected[name] = canonical_relative(
            pathlib.Path(token), toolchain_root, f"driver-selected {name}"
        )

    token_set = set(tokens)
    for flag, (query_name, locked_name) in LIBRARY_FLAGS.items():
        require(
            flag in token_set, f"driver did not select required runtime flag: {flag}"
        )
        completed = run_compiler(compiler, [f"-print-file-name={query_name}"])
        require(
            completed.returncode == 0 and not completed.stderr,
            f"cross compiler cannot resolve {query_name}",
        )
        try:
            resolved_name = completed.stdout.decode("utf-8", errors="strict").strip()
        except UnicodeError as error:
            raise RuntimeVerificationError(
                f"cross compiler path for {query_name} is not UTF-8"
            ) from error
        require(
            resolved_name and resolved_name != query_name,
            f"cross compiler cannot resolve {query_name}",
        )
        selected[locked_name] = canonical_relative(
            pathlib.Path(resolved_name),
            toolchain_root,
            f"driver-selected {query_name}",
        )
    require(
        set(selected) == RUNTIME_NAMES,
        "driver-selected runtime input set is incomplete",
    )
    return selected, arguments


def map_selection(
    link_map: pathlib.Path, toolchain_root: pathlib.Path
) -> dict[str, set[str]]:
    require(link_map.is_file(), f"link map is missing: {link_map}")
    require(not link_map.is_symlink(), f"link map cannot be a symlink: {link_map}")
    try:
        text = link_map.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as error:
        raise RuntimeVerificationError(f"cannot read link map: {error}") from error
    name_pattern = "|".join(re.escape(name) for name in sorted(MAP_NAMES))
    candidates = re.findall(rf"([^\s()]*\/(?:{name_pattern}))(?=[\s(]|$)", text)
    observed: dict[str, set[str]] = {name: set() for name in RUNTIME_NAMES}
    for raw_candidate in candidates:
        candidate = pathlib.Path(raw_candidate)
        if not candidate.is_absolute():
            candidate = link_map.parent / candidate
        map_name = candidate.name
        locked_name = MAP_NAMES[map_name]
        observed[locked_name].add(
            canonical_relative(
                candidate, toolchain_root, f"link-map-selected {map_name}"
            )
        )
    for load_value in re.findall(r"(?m)^LOAD (.+)$", text):
        candidate = pathlib.Path(load_value)
        if not candidate.is_absolute():
            candidate = link_map.parent / candidate
        try:
            resolved = candidate.resolve(strict=True)
        except OSError:
            continue
        name = candidate.name
        if resolved.is_relative_to(toolchain_root) and (
            name.endswith(".a") or re.fullmatch(r"crt[A-Za-z0-9_+.~-]*\.o", name)
        ):
            require(
                name in MAP_NAMES, f"link map selected unlocked runtime input: {name}"
            )
    for name, relative_paths in observed.items():
        require(relative_paths, f"link map omits selected runtime input: {name}")
    return observed


def verify(args: argparse.Namespace) -> None:
    lock_file = args.lock.resolve()
    toolchain_root = args.toolchain_root.resolve()
    compiler = args.compiler.resolve()
    output = args.output.resolve()
    require(toolchain_root.is_dir(), f"toolchain root is missing: {toolchain_root}")
    require(not toolchain_root.is_symlink(), "toolchain root cannot be a symlink")
    require(compiler.is_file(), f"cross compiler is missing: {compiler}")
    require(not compiler.is_symlink(), "cross compiler cannot be a symlink")
    require(
        not output.exists() and not output.is_symlink(),
        f"runtime verification output already exists: {output}",
    )
    toolchain, locked_by_name = load_lock(lock_file)
    driver_selected, driver_arguments = driver_selection(compiler, toolchain_root)
    map_selected = map_selection(args.link_map.resolve(), toolchain_root)

    selected_files: list[dict] = []
    for name in sorted(RUNTIME_NAMES):
        locked = locked_by_name[name]
        locked_path = locked["path"]
        require(
            driver_selected[name] == locked_path,
            f"driver selected unexpected canonical path for {name}: "
            f"{driver_selected[name]}",
        )
        require(
            map_selected[name] == {locked_path},
            f"link map selected unexpected canonical path for {name}: "
            f"{sorted(map_selected[name])}",
        )
        runtime_file = toolchain_root / locked_path
        require(
            sha256_file(runtime_file) == locked["sha256"],
            f"locked runtime input checksum mismatch: {locked_path}",
        )
        selected_files.append(
            {
                "component_id": locked["component_id"],
                "path": locked_path,
                "sha256": locked["sha256"],
            }
        )

    document = {
        "compiler_driver": {
            "argv": [compiler.name, *driver_arguments],
            "target": toolchain["target"],
        },
        "link_map": {
            "path": ".tgcli-build/app/tgcli.link.map",
            "selected_paths": [entry["path"] for entry in selected_files],
        },
        "schema_version": 1,
        "selected_runtime_files": selected_files,
    }
    output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify the exact pinned runtime inputs selected by the linker"
    )
    parser.add_argument("--lock", type=pathlib.Path, required=True)
    parser.add_argument("--toolchain-root", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", type=pathlib.Path, required=True)
    parser.add_argument("--link-map", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser.parse_args()


def main() -> int:
    try:
        verify(parse_args())
    except (RuntimeVerificationError, OSError, json.JSONDecodeError) as error:
        print(f"toolchain runtime verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
