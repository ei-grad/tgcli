from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
REPOSITORY = Path(__file__).resolve().parents[1]
SCHEMAS = REPOSITORY / "docs" / "schemas"
TDLIB_SOURCE = Path(os.environ["TGCLI_TEST_TDLIB_SOURCE"])
GENERATOR = "generate-m7-foundation-schemas.mjs"
FUTURE_FILES = {
    "chat-info.result.schema.json",
    "chat-members.result.schema.json",
    "chat-read.error.schema.json",
    "download.error.schema.json",
    "download.result.schema.json",
    "raw.error.schema.json",
    "raw.result.schema.json",
    "raw-audit-checkpoint.v3.schema.json",
    "raw-audit-intent.v3.schema.json",
    "raw-audit-outcome.v3.schema.json",
    "search.error.schema.json",
    "search.result.schema.json",
}
CURRENT_M2_ERRORS = {
    "chats": "chats.error.schema.json",
    "fetch": "fetch.error.schema.json",
    "msg get": "msg-get.error.schema.json",
    "msg link": "msg-link.error.schema.json",
    "read": "read.error.schema.json",
    "unread": "unread.error.schema.json",
    "search": "search.error.schema.json",
    "chat info": "chat-read.error.schema.json",
    "chat members": "chat-read.error.schema.json",
    "download": "download.error.schema.json",
}


class M7FoundationAssetTest(unittest.TestCase):
    @staticmethod
    def _filtered_basic_members(
        members: list[dict[str, object]], mode: str, query: str = ""
    ) -> list[int]:
        def selected(member: dict[str, object]) -> bool:
            if mode == "admins":
                return member["status"] in {"creator", "administrator"}
            if mode == "bots":
                return member["sender_type"] == "user" and member["is_bot"] is True
            if mode == "query":
                return query in str(member["display_name"]) or any(
                    query in username for username in member["usernames"]
                )
            return mode == "recent"

        return [int(member["id"]) for member in members if selected(member)]

    @staticmethod
    def _basic_member_cursor_error(
        cursor_source_count: int, current_filtered: list[int]
    ) -> dict[str, object] | None:
        if len(current_filtered) == cursor_source_count:
            return None
        return {
            "code": "PAGINATION_INVALID",
            "details": {"operation": "chat_members", "reason": "source_changed"},
        }

    def test_generator_is_byte_deterministic(self) -> None:
        selected = {
            SCHEMAS / "error-manifest.json",
            *(SCHEMAS / filename for filename in CURRENT_M2_ERRORS.values()),
            *(
                SCHEMAS / filename
                for filename in (
                    "search.result.schema.json",
                    "chat-info.result.schema.json",
                    "chat-members.result.schema.json",
                    "raw.result.schema.json",
                    "raw.error.schema.json",
                )
            ),
            *(SCHEMAS / "future" / filename for filename in FUTURE_FILES),
        }
        expected = {
            source.relative_to(SCHEMAS): source.read_bytes() for source in selected
        }
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "schemas"
            shutil.copytree(SCHEMAS, copied, symlinks=True)
            subprocess.run(
                ["node", str(copied / GENERATOR)],
                check=True,
                capture_output=True,
                text=True,
            )
            actual = {
                relative: (copied / relative).read_bytes() for relative in expected
            }
        self.assertEqual(actual, expected)

    def test_future_assets_are_complete_strict_and_materialized_atomically(
        self,
    ) -> None:
        self.assertEqual(
            {candidate.name for candidate in (SCHEMAS / "future").iterdir()},
            FUTURE_FILES,
        )
        cataloged_files: set[str] = set()
        cataloged_commands: set[str] = set()
        for name in ("manifest.json", "stream-manifest.json", "error-manifest.json"):
            document = json.loads((SCHEMAS / name).read_text(encoding="utf-8"))
            cataloged_commands.update(document["commands"])
            for contract in document["commands"].values():
                cataloged_files.update(contract.values())
        materialized = {
            "search.result.schema.json",
            "search.error.schema.json",
            "chat-info.result.schema.json",
            "chat-members.result.schema.json",
            "chat-read.error.schema.json",
            "download.result.schema.json",
            "download.error.schema.json",
            "raw.result.schema.json",
            "raw.error.schema.json",
        }
        self.assertTrue((FUTURE_FILES - materialized).isdisjoint(cataloged_files))
        self.assertTrue(
            {"search", "chat info", "chat members", "download", "raw"}
            <= cataloged_commands
        )
        for filename in materialized:
            self.assertEqual(
                (SCHEMAS / filename).read_bytes(),
                (SCHEMAS / "future" / filename).read_bytes(),
            )
        for filename in FUTURE_FILES:
            schema = json.loads(
                (SCHEMAS / "future" / filename).read_text(encoding="utf-8")
            )
            self.assertEqual(
                schema.get("$schema"),
                "https://json-schema.org/draft/2020-12/schema",
            )

    def test_current_m2_error_catalog_is_exact(self) -> None:
        manifest = json.loads(
            (SCHEMAS / "error-manifest.json").read_text(encoding="utf-8")
        )
        for command, filename in CURRENT_M2_ERRORS.items():
            self.assertEqual(manifest["commands"][command], {"error": filename})
        self.assertNotIn("history", manifest["commands"])

    def test_pinned_search_member_and_download_source_contract(self) -> None:
        scheme = (TDLIB_SOURCE / "td" / "generate" / "scheme" / "td_api.tl").read_text(
            encoding="utf-8"
        )
        requests = (TDLIB_SOURCE / "td" / "telegram" / "Requests.cpp").read_text(
            encoding="utf-8"
        )
        for signature in (
            (
                "chatMember member_id:MessageSender tag:string inviter_user_id:int53 "
                "joined_chat_date:int32 status:ChatMemberStatus = ChatMember;"
            ),
            (
                "searchChatMessages chat_id:int53 topic_id:MessageTopic query:string "
                "sender_id:MessageSender from_message_id:int53 offset:int32 limit:int32 "
                "filter:SearchMessagesFilter = FoundChatMessages;"
            ),
            (
                "searchMessages chat_list:ChatList query:string offset:string limit:int32 "
                "filter:SearchMessagesFilter chat_type_filter:SearchMessagesChatTypeFilter "
                "min_date:int32 max_date:int32 = FoundMessages;"
            ),
            (
                "downloadFile file_id:int32 priority:int32 offset:int53 limit:int53 "
                "synchronous:Bool = File;"
            ),
            "getSuggestedFileName file_id:int32 directory:string = Text;",
            (
                "getSupergroupMembers supergroup_id:int53 "
                "filter:SupergroupMembersFilter offset:int32 limit:int32 = ChatMembers;"
            ),
            "supergroupMembersFilterAdministrators = SupergroupMembersFilter;",
            "supergroupMembersFilterBots = SupergroupMembersFilter;",
            "supergroupMembersFilterRecent = SupergroupMembersFilter;",
            "supergroupMembersFilterSearch query:string = SupergroupMembersFilter;",
            "getUserFullInfo user_id:int53 = UserFullInfo;",
            "getBasicGroup basic_group_id:int53 = BasicGroup;",
            "getBasicGroupFullInfo basic_group_id:int53 = BasicGroupFullInfo;",
            "getSupergroupFullInfo supergroup_id:int53 = SupergroupFullInfo;",
            (
                "file id:int32 size:int53 expected_size:int53 local:localFile "
                "remote:remoteFile = File;"
            ),
        ):
            self.assertEqual(scheme.count(signature), 1)
        self.assertIn(
            "downloaded_size Total downloaded file size, in bytes. Can be used only "
            "for calculating download progress.",
            scheme,
        )
        filesystem = (
            REPOSITORY / "src" / "daemon" / "download_filesystem.cpp"
        ).read_text(encoding="utf-8")
        for primitive in (
            "SYS_renameat2",
            "RENAME_NOREPLACE",
            "renameatx_np",
            "RENAME_EXCL",
            "O_NOFOLLOW",
        ):
            self.assertIn(primitive, filesystem)
        chat_handler = requests[
            requests.index(
                "void Requests::on_request(uint64 id, td_api::searchChatMessages"
            ) : requests.index(
                "void Requests::on_request(uint64 id, td_api::searchSecretMessages"
            )
        ]
        self.assertEqual(chat_handler.count("CLEAN_INPUT_STRING(request.query_)"), 1)
        self.assertNotIn("CLEAN_INPUT_STRING(request.offset_)", chat_handler)
        global_handler = requests[
            requests.index(
                "void Requests::on_request(uint64 id, td_api::searchMessages"
            ) : requests.index(
                "void Requests::on_request(uint64 id, td_api::searchSavedMessages"
            )
        ]
        self.assertEqual(global_handler.count("CLEAN_INPUT_STRING(request.query_)"), 1)
        self.assertEqual(global_handler.count("CLEAN_INPUT_STRING(request.offset_)"), 1)

        design = (REPOSITORY / "DESIGN.md").read_text(encoding="utf-8")
        for contract in (
            "`source_id` is the positive\nobserved basic-group or supergroup id",
            "byte-exact case-sensitive substring over the derived `display_name` or\nany active username",
            "relative captured\n`TGCLI_MEDIA_DIR` are each resolved against that same frozen cwd",
            "The first structurally valid completed state is\nthe candidate",
            "`local.can_be_downloaded` is advisory and may still be true",
            "Publication uses one acknowledged receive-sequence lease",
            "writes exactly that captured size",
            "sole permitted duplicate or regression relative to advisory progress",
        ):
            self.assertIn(contract, design)

    def test_basic_member_cursor_counts_the_filtered_live_vector(self) -> None:
        source = [
            {
                "id": 1,
                "status": "administrator",
                "sender_type": "user",
                "is_bot": True,
                "display_name": "Build Bot",
                "usernames": ["build_bot"],
            },
            {
                "id": 2,
                "status": "creator",
                "sender_type": "user",
                "is_bot": False,
                "display_name": "Ada Project",
                "usernames": ["ada_project"],
            },
        ]
        mutations = {
            "admins": {"status": "member"},
            "bots": {"is_bot": False},
            "query-name": {"display_name": "Ada"},
            "query-username": {"usernames": ["ada"]},
        }
        selectors = {
            "admins": ("admins", ""),
            "bots": ("bots", ""),
            "query-name": ("query", "Project"),
            "query-username": ("query", "project"),
        }
        for label, mutation in mutations.items():
            current = [dict(member) for member in source]
            current[1 if label.startswith("query") else 0].update(mutation)
            mode, query = selectors[label]
            before = self._filtered_basic_members(source, mode, query)
            after = self._filtered_basic_members(current, mode, query)
            self.assertEqual(len(current), len(source))
            self.assertNotEqual(len(after), len(before))
            self.assertEqual(
                self._basic_member_cursor_error(len(before), after),
                {
                    "code": "PAGINATION_INVALID",
                    "details": {
                        "operation": "chat_members",
                        "reason": "source_changed",
                    },
                },
            )

        reordered = list(reversed(source))
        self.assertEqual(
            len(self._filtered_basic_members(reordered, "recent")),
            len(self._filtered_basic_members(source, "recent")),
        )
        self.assertNotEqual(
            self._filtered_basic_members(reordered, "recent"),
            self._filtered_basic_members(source, "recent"),
        )
        self.assertIsNone(
            self._basic_member_cursor_error(
                len(self._filtered_basic_members(source, "recent")),
                self._filtered_basic_members(reordered, "recent"),
            )
        )

        design = " ".join(
            (REPOSITORY / "DESIGN.md").read_text(encoding="utf-8").split()
        )
        for contract in (
            "`source_count` is the exact filtered-vector length",
            "compare its current filtered length to cursor `source_count` before slicing",
            "An unchanged filtered count does not freeze order or member identity",
            "For `basic_group`, `offset` is a nonnegative int32 index into the fully validated, identity-enriched and filtered vector",
            "For `supergroup` and `channel`, `offset` is the raw TD `getSupergroupMembers` offset advanced by the raw returned page length",
            "Exactly one empty request at the next raw offset proves exhaustion",
        ):
            self.assertIn(contract, design)
        self.assertNotIn(
            "Offset is a nonnegative int32 and is the raw source offset", design
        )

    def test_raw_option_b_uses_the_explicit_tdlib_ownership_boundary(self) -> None:
        design = " ".join(
            (REPOSITORY / "DESIGN.md").read_text(encoding="utf-8").split()
        )
        for contract in (
            "wiped on every exit while tgcli owns them",
            "moves the sole native `object_ptr<td_api::Function>` exactly once into pinned TDLib",
            "tgcli retains no alias and makes no post-transfer zeroization claim",
            "Credential, authentication, payment, proxy-secret, logging, lifecycle",
            "Returned native responses cross back into tgcli ownership",
        ):
            self.assertIn(contract, design)
        self.assertNotIn(
            "wiped by the pin-generated visitor after ownership transfer", design
        )

        claude = " ".join(
            (REPOSITORY / "CLAUDE.md").read_text(encoding="utf-8").split()
        )
        self.assertIn(
            "Raw request ownership ends at the pinned TDLib rvalue boundary", claude
        )
        self.assertIn("makes no zeroization claim for TDLib allocator", claude)

        review = " ".join(
            (REPOSITORY / "REVIEW.md").read_text(encoding="utf-8").split()
        )
        self.assertIn("one-move/no-retained-alias TDLib boundary", review)
        self.assertIn("request memory already owned by uninstrumented TDLib", review)

        runtime = (REPOSITORY / "src" / "core" / "td_runtime.cpp").read_text(
            encoding="utf-8"
        )
        for source_contract in (
            "class ProductionFunctionTransfer final",
            "manager_->send(client_id, query_id, transfer.argument())",
            "if (!transfer.consumed())",
            "function.raw_request_wiper()",
        ):
            self.assertIn(source_contract, runtime)

        client_header = (TDLIB_SOURCE / "td" / "telegram" / "Client.h").read_text(
            encoding="utf-8"
        )
        self.assertEqual(
            client_header.count(
                "void send(ClientId client_id, RequestId request_id, "
                "td_api::object_ptr<td_api::Function> &&request);"
            ),
            1,
        )

    def test_asan_suppresses_only_the_exact_pinned_tdlib_leak_frames(self) -> None:
        suppressions = (REPOSITORY / "tests" / "tdlib.lsan.supp").read_text(
            encoding="utf-8"
        )
        self.assertEqual(
            suppressions.splitlines(),
            [
                "leak:tdsqlite3MemMalloc",
                "leak:td::SqliteDb::set_cipher_version",
                "leak:td::SqliteDb::open_with_key",
                "leak:td::Global::Global",
                "leak:td::Status::Status",
            ],
        )
        self.assertNotIn("leak:td::", suppressions.splitlines())

        cmake = (REPOSITORY / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*address"', cmake)
        self.assertIn(
            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tdlib.lsan.supp:print_suppressions=0",
            cmake,
        )
        self.assertIn("add_executable(tgcli_lsan_control lsan_control.cpp)", cmake)
        self.assertIn("NAME lsan-tgcli-owned-control", cmake)
        self.assertNotIn("detect_leaks=0", cmake)

        control = (REPOSITORY / "tests" / "verify_lsan_control.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn("tgcli-owned leak was incorrectly suppressed", control)
        self.assertIn("tgcli_owned_leak_control", control)

        account_cli = (REPOSITORY / "tests" / "account_cli_test.cpp").read_text(
            encoding="utf-8"
        )
        self.assertNotIn('::setenv("LSAN_OPTIONS"', account_cli)


if __name__ == "__main__":
    unittest.main()
