#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import pathlib
import stat
import subprocess
import sys


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
EXPECTED_GIT_TREE = "27af7989c2c65dec54eb7860fb6932d5e422c7b5"
EXPECTED_ASSERTIONS = {
    "current_state_is_a_closed_updates_query": {
        "file_sha256": "a8166ef37efb1a1440357b81e8e26c68ea45a35901c0bcc8d69964487c98476f",
        "fragment_sha256": "ba04d55e01ffbf6f1ea9bb47f798d5888b385bee254e490f5053c6d4cc077f67",
        "path": "td/generate/scheme/td_api.tl",
        "source_lines": ("getCurrentState = Updates;",),
    },
    "chat_folder_count_is_clamped_to_one_hundred": {
        "file_sha256": "4d002f748f78110fd16e0091df5f52e9f101f578d3c9ea414c8668d77012c590",
        "fragment_sha256": "3c3a196bbce9786d5cfe7b14bd48a0db9bccb53d4b188a7bd7dab8fca32c793b",
        "path": "td/telegram/DialogFilterManager.cpp",
        "source_lines": (
            '  auto max_dialog_filters = clamp(td_->option_manager_->get_option_integer("chat_folder_count_max"),',
            "                                  static_cast<int64>(0), static_cast<int64>(100));",
            "  if (dialog_filters_.size() >= narrow_cast<size_t>(max_dialog_filters)) {",
            '    return promise.set_error(400, "The maximum number of chat folders exceeded");',
        ),
    },
    "chat_list_ids_map_one_to_one_to_public_lists": {
        "file_sha256": "94f05fefbe9f43b6f28a1abc0394dadbd4d35676540f38f69baa9b94101ea7d7",
        "fragment_sha256": "6e0f4924792408d11cb82ad6dc840ba075eb9380cc4343917ba798a8c1e21a3a",
        "path": "td/telegram/DialogListId.cpp",
        "source_lines": (
            "td_api::object_ptr<td_api::ChatList> DialogListId::get_chat_list_object() const {",
            "  if (is_folder()) {",
            "    auto folder_id = get_folder_id();",
            "    if (folder_id == FolderId::archive()) {",
            "      return td_api::make_object<td_api::chatListArchive>();",
            "    }",
            "    return td_api::make_object<td_api::chatListMain>();",
            "  }",
            "  if (is_filter()) {",
            "    return td_api::make_object<td_api::chatListFolder>(get_filter_id().get());",
            "  }",
            "  UNREACHABLE();",
            "  return nullptr;",
            "}",
            "",
            "vector<td_api::object_ptr<td_api::ChatList>> DialogListId::get_chat_lists_object(",
            "    const vector<DialogListId> &dialog_list_ids) {",
            "  return transform(dialog_list_ids, [](DialogListId dialog_list_id) { return dialog_list_id.get_chat_list_object(); });",
            "}",
        ),
    },
    "chat_list_membership_rejects_duplicate_ids": {
        "file_sha256": "7e9f8dd8b9d13821d40a807e442d3da535c48f0676ab0a1323a5d07e184e80da",
        "fragment_sha256": "d026dc8c0b1736acd991bfdfa04ffd0ab40e7d70eb4985115345d0608ef944a4",
        "path": "td/telegram/MessagesManager.cpp",
        "source_lines": (
            "bool MessagesManager::is_dialog_in_list(const Dialog *d, DialogListId dialog_list_id) {",
            "  return td::contains(d->dialog_list_ids, dialog_list_id);",
            "}",
            "",
            "void MessagesManager::add_dialog_to_list(Dialog *d, DialogListId dialog_list_id) {",
            '  LOG(INFO) << "Add " << d->dialog_id << " to " << dialog_list_id;',
            "  CHECK(!is_dialog_in_list(d, dialog_list_id));",
            "  d->dialog_list_ids.push_back(dialog_list_id);",
            "  CHECK(d->is_update_new_chat_sent);",
            "  send_closure(G()->td(), &Td::send_update,",
            "               td_api::make_object<td_api::updateChatAddedToList>(",
            '                   get_chat_id_object(d->dialog_id, "updateChatAddedToList"), dialog_list_id.get_chat_list_object()));',
            "}",
        ),
    },
}
ASSERTION_KEYS = {"file_sha256", "fragment_sha256", "id", "path", "source_lines"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def lstat_path(source: pathlib.Path, label: str) -> os.stat_result:
    try:
        result = source.lstat()
    except OSError as error:
        raise VerificationError(f"{label} is missing: {error}") from error
    require(not stat.S_ISLNK(result.st_mode), f"{label} is a symlink")
    return result


def safe_directory(source: pathlib.Path, label: str) -> pathlib.Path:
    lexical = source if source.is_absolute() else pathlib.Path.cwd() / source
    current = pathlib.Path(lexical.anchor)
    result = current.lstat()
    for component in lexical.parts[1:]:
        current /= component
        result = lstat_path(current, label)
    require(stat.S_ISDIR(result.st_mode), f"{label} is not a directory")
    try:
        return lexical.resolve(strict=True)
    except OSError as error:
        raise VerificationError(f"cannot resolve {label}: {error}") from error


def load_json(source: pathlib.Path, label: str) -> object:
    result = lstat_path(source, label)
    require(stat.S_ISREG(result.st_mode), f"{label} is not a regular file")
    try:
        return json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read {label}: {error}") from error


def git_output(source: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", os.fspath(source), *arguments],
        capture_output=True,
        check=False,
        text=True,
    )
    require(
        completed.returncode == 0,
        f"cannot inspect pinned TDLib checkout with git {' '.join(arguments)}",
    )
    return completed.stdout.strip()


def verify_metadata(root: pathlib.Path) -> dict[str, bytes]:
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
        set(contract)
        == {"assertions", "component", "schema_version", "source_git_tree"},
        "stream source contract keys differ",
    )
    require(
        contract["schema_version"] == 1, "unsupported stream source contract version"
    )
    require(
        contract["component"] == EXPECTED_COMPONENT, "stream TDLib identity differs"
    )
    require(
        contract["source_git_tree"] == EXPECTED_GIT_TREE, "stream source tree differs"
    )
    assertions = contract["assertions"]
    require(
        isinstance(assertions, list) and len(assertions) == len(EXPECTED_ASSERTIONS),
        "stream source assertion set differs",
    )
    fragments: dict[str, bytes] = {}
    for assertion in assertions:
        require(
            isinstance(assertion, dict) and set(assertion) == ASSERTION_KEYS,
            "stream source assertion shape differs",
        )
        assertion_id = assertion["id"]
        require(
            isinstance(assertion_id, str) and assertion_id in EXPECTED_ASSERTIONS,
            "unknown stream source assertion",
        )
        require(assertion_id not in fragments, "duplicate stream source assertion")
        expected = EXPECTED_ASSERTIONS[assertion_id]
        require(
            assertion["path"] == expected["path"]
            and assertion["file_sha256"] == expected["file_sha256"]
            and assertion["fragment_sha256"] == expected["fragment_sha256"],
            f"{assertion_id} source assertion differs",
        )
        lines = assertion["source_lines"]
        require(
            isinstance(lines, list)
            and all(
                isinstance(line, str) and "\n" not in line and "\r" not in line
                for line in lines
            )
            and tuple(lines) == expected["source_lines"],
            f"{assertion_id} source assertion differs",
        )
        fragment = ("\n".join(lines) + "\n").encode()
        require(
            sha256_bytes(fragment) == expected["fragment_sha256"],
            f"{assertion_id} source fragment digest differs",
        )
        fragments[assertion_id] = fragment
    require(
        set(fragments) == set(EXPECTED_ASSERTIONS), "stream source assertion IDs differ"
    )
    return fragments


def verify_source_file(
    source: pathlib.Path, relative: pathlib.PurePosixPath
) -> pathlib.Path:
    require(
        not relative.is_absolute() and ".." not in relative.parts,
        "source path is unsafe",
    )
    current = source
    result = source.lstat()
    for component in relative.parts:
        current /= component
        result = lstat_path(current, f"pinned stream source component {relative}")
    require(
        stat.S_ISREG(result.st_mode),
        f"pinned stream source file {relative} is not regular",
    )
    return current


def verify_checkout(source_argument: pathlib.Path, fragments: dict[str, bytes]) -> None:
    source = safe_directory(source_argument, "TDLib source argument")
    require(
        pathlib.Path(git_output(source, "rev-parse", "--show-toplevel")).resolve()
        == source,
        "TDLib source argument is not the checkout root",
    )
    require(
        git_output(source, "rev-parse", "--verify", "HEAD^{commit}")
        == EXPECTED_COMPONENT["immutable_ref"],
        "TDLib checkout is at the wrong revision",
    )
    require(
        git_output(source, "rev-parse", "HEAD^{tree}") == EXPECTED_GIT_TREE,
        "TDLib checkout tree differs",
    )
    require(
        not git_output(source, "status", "--porcelain", "--untracked-files=all"),
        "TDLib checkout has tracked or untracked changes",
    )
    for assertion_id, fragment in fragments.items():
        expected = EXPECTED_ASSERTIONS[assertion_id]
        source_file = verify_source_file(
            source, pathlib.PurePosixPath(expected["path"])
        )
        try:
            content = source_file.read_bytes()
        except OSError as error:
            raise VerificationError(
                f"cannot read pinned stream source: {error}"
            ) from error
        require(
            sha256_bytes(content) == expected["file_sha256"],
            f"{assertion_id} pinned source file digest differs",
        )
        require(
            content.count(fragment) == 1,
            f"{assertion_id} source fragment is not unique",
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify the pinned TDLib stream source contract"
    )
    parser.add_argument("--repo-root", type=pathlib.Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--tdlib-source", type=pathlib.Path)
    mode.add_argument("--metadata-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    try:
        root = safe_directory(arguments.repo_root, "repository root")
        fragments = verify_metadata(root)
        if arguments.metadata_only:
            print("TDLib stream contract metadata verified; source not inspected")
            return 0
        require(arguments.tdlib_source is not None, "--tdlib-source is required")
        verify_checkout(arguments.tdlib_source, fragments)
    except (OSError, UnicodeError, VerificationError) as error:
        print(
            f"TDLib stream source contract verification failed: {error}",
            file=sys.stderr,
        )
        return 1
    print("TDLib stream contract authenticated source verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
