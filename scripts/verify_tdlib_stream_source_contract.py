#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Optional


class VerificationError(RuntimeError):
    pass


EXPECTED_COMPONENT = {
    "archive_sha256": "095a2e78ced222bbe4cf934d7a6796db6c13837b45f3c31e0a28df5d273770e7",
    "generated_source_tree_sha256": (
        "5999031e3cbfe0ebbd95f88b152fe2724df7042beb9e74843feb65b02ca6588d"
    ),
    "id": "tdlib",
    "immutable_ref": "a17f87c4cff7b90b278d12b91ba0614383aaee82",
    "source_tree_sha256": "b7b7a35caefdc6d083f42fc8f331344fd6e021bdf30fde7518326303c2bdca35",
    "version": "1.8.65",
}
EXPECTED_ASSERTION = {
    "file_sha256": "4d002f748f78110fd16e0091df5f52e9f101f578d3c9ea414c8668d77012c590",
    "fragment_sha256": "3c3a196bbce9786d5cfe7b14bd48a0db9bccb53d4b188a7bd7dab8fca32c793b",
    "id": "chat_folder_count_is_clamped_to_one_hundred",
    "path": "td/telegram/DialogFilterManager.cpp",
    "source_lines": [
        '  auto max_dialog_filters = clamp(td_->option_manager_->get_option_integer("chat_folder_count_max"),',
        "                                  static_cast<int64>(0), static_cast<int64>(100));",
        "  if (dialog_filters_.size() >= narrow_cast<size_t>(max_dialog_filters)) {",
        '    return promise.set_error(400, "The maximum number of chat folders exceeded");',
    ],
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def load_json(source: pathlib.Path, label: str) -> object:
    require(source.is_file() and not source.is_symlink(), f"{label} is missing or unsafe")
    try:
        return json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read {label}: {error}") from error


def git_output(source: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(source), *arguments],
        capture_output=True,
        check=False,
        text=True,
    )
    require(completed.returncode == 0, "cannot inspect pinned TDLib checkout")
    return completed.stdout.strip()


def verify(root: pathlib.Path, tdlib_source: Optional[pathlib.Path]) -> None:
    lock = load_json(root / "release/dependencies.lock.json", "dependency lock")
    require(isinstance(lock, dict), "dependency lock must be an object")
    components = lock.get("components")
    require(isinstance(components, list), "dependency lock components are missing")
    tdlib = [
        component
        for component in components
        if isinstance(component, dict) and component.get("id") == "tdlib"
    ]
    require(len(tdlib) == 1, "dependency lock must contain exactly one TDLib component")
    require(
        {key: tdlib[0].get(key) for key in EXPECTED_COMPONENT} == EXPECTED_COMPONENT,
        "dependency lock TDLib identity differs",
    )

    contract = load_json(
        root / "release/tdlib-stream-source-contract.json", "stream source contract"
    )
    require(isinstance(contract, dict), "stream source contract must be an object")
    require(
        set(contract) == {"assertions", "component", "schema_version"},
        "stream source contract keys differ",
    )
    require(contract["schema_version"] == 1, "unsupported stream source contract version")
    require(contract["component"] == EXPECTED_COMPONENT, "stream TDLib identity differs")
    require(
        contract["assertions"] == [EXPECTED_ASSERTION],
        "stream source assertion differs",
    )
    fragment = ("\n".join(EXPECTED_ASSERTION["source_lines"]) + "\n").encode()
    require(
        hashlib.sha256(fragment).hexdigest() == EXPECTED_ASSERTION["fragment_sha256"],
        "stream source fragment digest differs",
    )

    if tdlib_source is None:
        return
    source = tdlib_source.resolve()
    require(source.is_dir() and not source.is_symlink(), "TDLib checkout is missing or unsafe")
    require(
        git_output(source, "rev-parse", "HEAD") == EXPECTED_COMPONENT["immutable_ref"],
        "TDLib checkout is at the wrong revision",
    )
    require(
        not git_output(source, "status", "--porcelain", "--untracked-files=no"),
        "TDLib checkout has tracked changes",
    )
    source_file = source / EXPECTED_ASSERTION["path"]
    require(
        source_file.is_file() and not source_file.is_symlink(),
        "pinned stream source file is missing or unsafe",
    )
    content = source_file.read_bytes()
    require(
        hashlib.sha256(content).hexdigest() == EXPECTED_ASSERTION["file_sha256"],
        "pinned stream source file digest differs",
    )
    require(content.count(fragment) == 1, "pinned stream source fragment is not unique")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the pinned TDLib stream source contract")
    parser.add_argument("--repo-root", type=pathlib.Path, required=True)
    parser.add_argument("--tdlib-source", type=pathlib.Path)
    arguments = parser.parse_args()
    try:
        verify(arguments.repo_root.resolve(), arguments.tdlib_source)
    except (OSError, VerificationError) as error:
        print(f"TDLib stream source contract verification failed: {error}", file=sys.stderr)
        return 1
    print("TDLib stream source contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
