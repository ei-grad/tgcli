#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
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
EXPECTED_ASSERTIONS = {
    "reset_authorization_false_resolves_unit": {
        "file_sha256": "3176b4c615614610f3766c652e54d296defe61084882f5ab5bc8302db559c1a1",
        "fragment_sha256": "8c5735c4700a7e9306a20499a50c937fd02b24124d6c6c929b7f11c37755ef01",
        "path": "td/telegram/AccountManager.cpp",
        "source_lines": (
            "class ResetAuthorizationQuery final : public Td::ResultHandler {",
            "  Promise<Unit> promise_;",
            "",
            " public:",
            (
                "  explicit ResetAuthorizationQuery(Promise<Unit> &&promise) : "
                "promise_(std::move(promise)) {"
            ),
            "  }",
            "",
            "  void send(int64 authorization_id) {",
            (
                "    send_query(G()->net_query_creator().create("
                "telegram_api::account_resetAuthorization(authorization_id)));"
            ),
            "  }",
            "",
            "  void on_result(BufferSlice packet) final {",
            "    auto result_ptr = fetch_result<telegram_api::account_resetAuthorization>(packet);",
            "    if (result_ptr.is_error()) {",
            "      return on_error(result_ptr.move_as_error());",
            "    }",
            "",
            "    bool result = result_ptr.move_as_ok();",
            '    LOG_IF(WARNING, !result) << "Failed to terminate session";',
            "    promise_.set_value(Unit());",
            "  }",
            "",
            "  void on_error(Status status) final {",
            "    promise_.set_error(std::move(status));",
            "  }",
            "};",
        ),
    },
    "terminate_session_maps_unit_to_public_ok": {
        "file_sha256": "7b44e123158f145156934a7f245a7b487c995f125d2fd4eefa676e839d28dec6",
        "fragment_sha256": "33611c6309ab126885ec3bafde6b450960cac81e69323f08a368e1c492f6391d",
        "path": "td/telegram/Requests.cpp",
        "source_lines": (
            "void Requests::on_request(uint64 id, const td_api::terminateSession &request) {",
            "  CHECK_IS_USER();",
            "  CREATE_OK_REQUEST_PROMISE();",
            "  td_->account_manager_->terminate_session(request.session_id_, std::move(promise));",
            "}",
        ),
    },
}
COMPONENT_KEYS = set(EXPECTED_COMPONENT)
ASSERTION_KEYS = {"file_sha256", "fragment_sha256", "id", "path", "source_lines"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def load_json(source: pathlib.Path, label: str) -> object:
    require(
        source.is_file() and not source.is_symlink(), f"{label} is missing or unsafe"
    )
    try:
        return json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read {label}: {error}") from error


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def verify_component(lock: object, contract: object) -> None:
    require(isinstance(lock, dict), "dependency lock must be an object")
    components = lock.get("components")
    require(isinstance(components, list), "dependency lock components are missing")
    tdlib = [
        component
        for component in components
        if isinstance(component, dict) and component.get("id") == "tdlib"
    ]
    require(len(tdlib) == 1, "dependency lock must contain exactly one TDLib component")
    locked = {key: tdlib[0].get(key) for key in COMPONENT_KEYS}
    require(
        locked == EXPECTED_COMPONENT,
        "dependency lock TDLib identity differs from the accepted session contract",
    )

    require(isinstance(contract, dict), "session source contract must be an object")
    require(
        set(contract) == {"assertions", "component", "schema_version"},
        "session source contract keys differ",
    )
    require(
        contract["schema_version"] == 1, "unsupported session source contract version"
    )
    component = contract["component"]
    require(
        isinstance(component, dict) and set(component) == COMPONENT_KEYS,
        "session source component identity is invalid",
    )
    require(
        component == EXPECTED_COMPONENT,
        "session source contract is not bound to the accepted TDLib identity",
    )


def verify_assertions(contract: object) -> dict[str, bytes]:
    require(isinstance(contract, dict), "session source contract must be an object")
    assertions = contract["assertions"]
    require(isinstance(assertions, list), "session source assertions must be a list")
    require(
        len(assertions) == len(EXPECTED_ASSERTIONS),
        "session source assertion set differs",
    )
    verified: dict[str, bytes] = {}
    for assertion in assertions:
        require(
            isinstance(assertion, dict) and set(assertion) == ASSERTION_KEYS,
            "session source assertion shape is invalid",
        )
        assertion_id = assertion["id"]
        require(
            isinstance(assertion_id, str) and assertion_id in EXPECTED_ASSERTIONS,
            "unknown session source assertion",
        )
        require(assertion_id not in verified, "duplicate session source assertion")
        expected = EXPECTED_ASSERTIONS[assertion_id]
        require(
            assertion["path"] == expected["path"], f"{assertion_id} source path differs"
        )
        require(
            assertion["file_sha256"] == expected["file_sha256"],
            f"{assertion_id} whole-file identity differs",
        )
        require(
            assertion["fragment_sha256"] == expected["fragment_sha256"],
            f"{assertion_id} fragment identity differs",
        )
        source_lines = assertion["source_lines"]
        require(
            isinstance(source_lines, list)
            and all(
                isinstance(line, str) and "\n" not in line and "\r" not in line
                for line in source_lines
            ),
            f"{assertion_id} source lines are invalid",
        )
        require(
            tuple(source_lines) == expected["source_lines"],
            f"{assertion_id} source semantics differ",
        )
        fragment = ("\n".join(source_lines) + "\n").encode()
        require(
            sha256_bytes(fragment) == assertion["fragment_sha256"],
            f"{assertion_id} fragment digest differs",
        )
        verified[assertion_id] = fragment
    require(
        set(verified) == set(EXPECTED_ASSERTIONS), "session source assertion IDs differ"
    )
    return verified


def git_output(source: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(source), *arguments],
        capture_output=True,
        check=False,
        text=True,
    )
    require(
        completed.returncode == 0,
        f"cannot inspect pinned TDLib checkout with git {' '.join(arguments)}",
    )
    return completed.stdout.strip()


def verify_checkout(source: pathlib.Path, fragments: dict[str, bytes]) -> None:
    require(
        source.is_dir() and not source.is_symlink(),
        "pinned TDLib checkout is missing or unsafe",
    )
    require(
        git_output(source, "rev-parse", "HEAD") == EXPECTED_COMPONENT["immutable_ref"],
        "TDLib checkout is not at the accepted immutable ref",
    )
    require(
        not git_output(source, "status", "--porcelain", "--untracked-files=no"),
        "TDLib checkout has tracked changes",
    )
    for assertion_id, fragment in fragments.items():
        expected = EXPECTED_ASSERTIONS[assertion_id]
        relative = pathlib.PurePosixPath(expected["path"])
        require(
            not relative.is_absolute() and ".." not in relative.parts,
            f"{assertion_id} source path is unsafe",
        )
        source_file = source.joinpath(*relative.parts)
        require(
            source_file.is_file() and not source_file.is_symlink(),
            f"{assertion_id} source file is missing or unsafe",
        )
        content = source_file.read_bytes()
        require(
            sha256_bytes(content) == expected["file_sha256"],
            f"{assertion_id} pinned source file digest differs",
        )
        require(
            content.count(fragment) == 1,
            f"{assertion_id} fragment is not unique in pinned source",
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify the pinned TDLib session source contract"
    )
    parser.add_argument("--repo-root", type=pathlib.Path, required=True)
    parser.add_argument("--tdlib-source", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    root = arguments.repo_root.resolve()
    try:
        lock = load_json(root / "release/dependencies.lock.json", "dependency lock")
        contract = load_json(
            root / "release/tdlib-session-source-contract.json",
            "session source contract",
        )
        verify_component(lock, contract)
        fragments = verify_assertions(contract)
        if arguments.tdlib_source is not None:
            verify_checkout(arguments.tdlib_source.resolve(), fragments)
    except (OSError, VerificationError) as error:
        print(
            f"TDLib session source contract verification failed: {error}",
            file=sys.stderr,
        )
        return 1
    print("TDLib session source contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
