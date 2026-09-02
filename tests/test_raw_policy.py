from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "scripts"))

import generate_raw_policy as raw_policy

TDLIB_SOURCE = Path(os.environ["TGCLI_TEST_TDLIB_SOURCE"])


class RawPolicyTest(unittest.TestCase):
    def test_committed_inventory_is_the_complete_pin_bijection(self) -> None:
        inventory, evidence = raw_policy.source_inventory(TDLIB_SOURCE)
        graph = raw_policy.source_type_graph(TDLIB_SOURCE)
        self.assertEqual(inventory["function_count"], 1001)
        self.assertEqual(len(inventory["functions"]), 1001)
        self.assertEqual(
            inventory["functions_sha256"],
            "sha256:30a9f4b449c80e0cae0b79a91060a0a5fddd91b46380c410464e2158107bbc99",
        )
        self.assertEqual(
            evidence,
            {
                "td_api_tl_sha256": "sha256:a8166ef37efb1a1440357b81e8e26c68ea45a35901c0bcc8d69964487c98476f",
                "td_api_header_contract": "doxygen-normalized-v1",
                "td_api_header_normalized_sha256": "sha256:5926a873f226667dfa69e6cda9f28c03407aefc31a90de73dd96be0e8fa6c536",
            },
        )
        self.assertEqual(graph["function_count"], 1001)
        self.assertEqual(graph["constructor_count"], 3118)
        self.assertEqual(len(graph["constructors"]), 3118)
        self.assertEqual(len(graph["result_types"]), 727)
        self.assertEqual(
            graph["graph_sha256"],
            "sha256:c8937fab296da09ca04874ed6b1eb23af40b5232008a77c52b4a8645b4ab5153",
        )
        raw_policy.validate_assets(TDLIB_SOURCE, REPOSITORY, activation=False)
        generated = (
            REPOSITORY / "src" / "daemon" / "raw_td_schema.generated.inc"
        ).read_text(encoding="utf-8")
        self.assertIn(
            'RawBodyValidatorDescriptor{"deny", &validate_raw_body_deny, &plan_raw_body_none}',
            generated,
        )
        self.assertNotIn("kGeneratedRawBodyValidatorSymbols", generated)

        policy = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        for index, validator in enumerate(("none", "raise_write", "raise_destructive")):
            policy["functions"][index]["body_validator"] = validator
        generated_fixture = raw_policy.generated_graph_include(graph, policy).decode()
        for symbol, (
            callable_name,
            planner_name,
        ) in raw_policy.COMPILED_VALIDATORS.items():
            self.assertEqual(
                generated_fixture.count(
                    f'RawBodyValidatorDescriptor{{"{symbol}", &{callable_name}, &{planner_name}}}'
                ),
                1,
            )
        self.assertEqual(
            len({value[0] for value in raw_policy.COMPILED_VALIDATORS.values()}),
            len(raw_policy.COMPILED_VALIDATORS),
        )

    def test_header_evidence_ignores_only_well_formed_doxygen_blocks(self) -> None:
        bare = "#pragma once\n\nclass value final : public Object {\n};\n"
        documented = (
            "#pragma once\n\n"
            "/**\n * Generated API documentation.\n */\n"
            "class value final : public Object {\n};\n"
        )
        self.assertEqual(raw_policy.normalize_doxygen_header(documented), bare)
        preserved = (
            "#define VALUE 1\n"
            "// /** line comment */\n"
            "/* ordinary\n"
            "/** nested-looking ordinary content\n"
            "*/\n"
            'constexpr auto value = "/** string */";\n'
        )
        self.assertEqual(raw_policy.normalize_doxygen_header(preserved), preserved)
        for malformed in (
            "/** outer /** nested */\n",
            "/** unterminated\n",
            "/** closed */ trailing\n",
            "*/\n",
        ):
            with (
                self.subTest(malformed=malformed),
                self.assertRaises(raw_policy.PolicyError),
            ):
                raw_policy.normalize_doxygen_header(malformed)

        changed = bare.replace("class value", "class changed")
        self.assertNotEqual(
            raw_policy.normalize_doxygen_header(changed),
            raw_policy.normalize_doxygen_header(bare),
        )
        constructor = (
            "class value final : public Object {\n"
            " public:\n"
            "  static const std::int32_t ID = 7;\n"
            "};\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bare_header = root / "bare.h"
            documented_header = root / "documented.h"
            changed_header = root / "changed.h"
            bare_header.write_text(constructor, encoding="utf-8")
            documented_header.write_text(
                "/**\n * Documentation only.\n */\n" + constructor,
                encoding="utf-8",
            )
            changed_header.write_text(
                constructor.replace("ID = 7", "ID = 8"), encoding="utf-8"
            )
            self.assertEqual(
                raw_policy.parse_header(bare_header),
                raw_policy.parse_header(documented_header),
            )
            self.assertNotEqual(
                raw_policy.parse_header(bare_header),
                raw_policy.parse_header(changed_header),
            )

    def test_accepted_candidate_is_exhaustive_reviewed_and_activation_ready(
        self,
    ) -> None:
        policy = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        self.assertTrue(policy["activation_ready"])
        self.assertEqual(policy["activation_blockers"], [])
        self.assertEqual(policy["unfinished_functions"], [])
        self.assertEqual(policy["function_count"], 1001)
        admission = {}
        principals = {}
        validators = {}
        for row in policy["functions"]:
            self.assertTrue(row["reviewed"])
            self.assertTrue(row["review_reason"].startswith("reviewed:td_api.tl:"))
            admission[row["admission"]] = admission.get(row["admission"], 0) + 1
            principals[row["principal"]] = principals.get(row["principal"], 0) + 1
            validators[row["body_validator"]] = (
                validators.get(row["body_validator"], 0) + 1
            )
            if row["principal"] == "both":
                self.assertTrue(
                    row["principal_evidence"].startswith("SynchronousRequests.cpp:")
                )
            if row["admission"] != "denied":
                self.assertFalse(row["sensitive_input"])
                self.assertFalse(row["sensitive_output"])
        self.assertEqual(
            admission, {"denied": 947, "destructive": 6, "read": 29, "write": 19}
        )
        self.assertEqual(principals, {"bot": 80, "both": 28, "user": 893})
        self.assertEqual(
            validators,
            {
                "chat_member_target": 3,
                "chat_optional_sender_target": 1,
                "chat_targets": 42,
                "deny": 947,
                "none": 8,
            },
        )

    def test_activation_validator_accepts_only_the_locked_reviewed_policy(self) -> None:
        raw_policy.validate_assets(TDLIB_SOURCE, REPOSITORY, activation=True)

    def test_unreviewed_admission_and_asset_drift_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination = root / "docs" / "raw"
            shutil.copytree(REPOSITORY / "docs" / "raw", destination)
            policy_file = destination / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
            policy = json.loads(policy_file.read_text(encoding="utf-8"))
            policy["functions"][0]["reviewed"] = False
            policy["functions"][0]["admission"] = "read"
            policy["policy_sha256"] = raw_policy.rows_digest(policy["functions"])
            policy_file.write_bytes(raw_policy.json_bytes(policy))
            lock_file = destination / "raw-policy.lock.json"
            lock = json.loads(lock_file.read_text(encoding="utf-8"))
            lock["policy_sha256"] = policy["policy_sha256"]
            lock_file.write_bytes(raw_policy.json_bytes(lock))
            with self.assertRaisesRegex(
                raw_policy.PolicyError, "unreviewed function is not denied"
            ):
                raw_policy.validate_assets(TDLIB_SOURCE, root, activation=False)

            policy_file.write_text(
                '{"schema_version":1,"schema_version":1}\n', encoding="utf-8"
            )
            with self.assertRaisesRegex(raw_policy.PolicyError, "duplicate JSON key"):
                raw_policy.validate_assets(TDLIB_SOURCE, root, activation=False)

    def test_review_claim_requires_concrete_reason_and_safe_response_policy(
        self,
    ) -> None:
        inventory = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"td-functions.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        policy = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        policy["functions"][0]["review_reason"] = "reviewed:"
        policy["policy_sha256"] = raw_policy.rows_digest(policy["functions"])
        with self.assertRaisesRegex(raw_policy.PolicyError, "lacks concrete reasoning"):
            raw_policy.validate_policy(inventory, policy)

        policy["functions"][0].update(
            {
                "review_reason": "reviewed:fixture evidence",
                "admission": "read",
                "body_validator": "none",
                "target_fields": [],
            }
        )
        policy["policy_sha256"] = raw_policy.rows_digest(policy["functions"])
        with self.assertRaisesRegex(
            raw_policy.PolicyError, "response-sensitive function is admitted"
        ):
            raw_policy.validate_policy(inventory, policy)

    def test_security_critical_rows_have_exact_frozen_policy(self) -> None:
        inventory = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"td-functions.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        policy = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        rows = {row["name"]: row for row in policy["functions"]}
        inventory_rows = {row["name"]: row for row in inventory["functions"]}
        for name in (
            "checkAuthenticationPassword",
            "createSecretChat",
            "downloadFile",
            "getChat",
            "getMe",
            "getMessageThread",
            "getMessageThreadHistory",
            "getPaymentForm",
            "getRepliedMessage",
            "setLogStream",
        ):
            self.assertEqual(rows[name]["admission"], "denied")
            self.assertEqual(rows[name]["body_validator"], "deny")
            self.assertEqual(
                rows[name]["evidence_category"], "denied_not_in_v1_allowlist"
            )
            self.assertTrue(rows[name]["sensitive_input"])
            self.assertTrue(rows[name]["sensitive_output"])
            self.assertIn(
                f"inventory constructor_id {inventory_rows[name]['constructor_id']}",
                rows[name]["review_reason"],
            )
            self.assertIn(
                f"fields_sha256 {inventory_rows[name]['fields_sha256']}",
                rows[name]["review_reason"],
            )
            self.assertIn(
                "outside the frozen reviewed v1 allowlist; denied whole",
                rows[name]["review_reason"],
            )

        for name, (admission, validator, target_fields) in {
            "deleteMessages": ("destructive", "chat_targets", ["chat_id"]),
            "setChatMemberStatus": (
                "destructive",
                "chat_member_target",
                ["chat_id", "member_id.chat_id|required"],
            ),
            "viewMessages": ("write", "chat_targets", ["chat_id"]),
            "getMessage": ("read", "chat_targets", ["chat_id"]),
        }.items():
            self.assertEqual(rows[name]["admission"], admission)
            self.assertEqual(rows[name]["body_validator"], validator)
            self.assertEqual(rows[name]["target_fields"], target_fields)
            self.assertFalse(rows[name]["sensitive_input"])
            self.assertFalse(rows[name]["sensitive_output"])

        policy["functions"][0]["body_validator"] = "missing_callable"
        policy["policy_sha256"] = raw_policy.rows_digest(policy["functions"])
        with self.assertRaisesRegex(
            raw_policy.PolicyError, "compiled policy validator missing"
        ):
            raw_policy.validate_policy(inventory, policy)

    def test_indirect_message_targets_are_pinned_and_denied_whole(self) -> None:
        requests = (TDLIB_SOURCE / "td" / "telegram" / "Requests.cpp").read_text(
            encoding="utf-8"
        )
        messages = (TDLIB_SOURCE / "td" / "telegram" / "MessagesManager.cpp").read_text(
            encoding="utf-8"
        )
        for source_anchor in (
            "replied_message_full_id_ =\n        td_->messages_manager_->get_replied_message",
            'get_message_object(replied_message_full_id_, "GetRepliedMessageRequest")',
            "messages_ = td_->messages_manager_->get_message_thread_history",
            "get_messages_object(-1, messages_.first, messages_.second, true",
        ):
            self.assertIn(source_anchor, requests)
        self.assertIn("Dialog *d = get_dialog(info.dialog_id);", messages)
        self.assertIn('get_chat_id_object(d->dialog_id, "messageThreadInfo")', messages)

        policy = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        rows = {row["name"]: row for row in policy["functions"]}
        for name in raw_policy.INDIRECT_TARGET_DENIAL_EVIDENCE:
            self.assertEqual(rows[name]["admission"], "denied")
            self.assertEqual(rows[name]["body_validator"], "deny")
            self.assertEqual(rows[name]["target_fields"], [])
            self.assertIn(
                raw_policy.INDIRECT_TARGET_DENIAL_EVIDENCE[name],
                rows[name]["review_reason"],
            )

    def test_admitted_message_senders_have_exhaustive_typed_preflight(self) -> None:
        graph = raw_policy.load_json(
            REPOSITORY / "docs" / "raw" / f"td-types.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        policy = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        graph_functions = {
            row["name"]: row
            for row in graph["constructors"]
            if row["kind"] == "function"
        }
        expected = {
            "banChatMember": (
                "chat_member_target",
                ["chat_id", "member_id.chat_id|required"],
            ),
            "getChatMember": (
                "chat_member_target",
                ["chat_id", "member_id.chat_id|required"],
            ),
            "searchChatMessages": (
                "chat_optional_sender_target",
                ["chat_id", "sender_id.chat_id|optional"],
            ),
            "setChatMemberStatus": (
                "chat_member_target",
                ["chat_id", "member_id.chat_id|required"],
            ),
        }
        found = {}
        for row in policy["functions"]:
            if row["admission"] == "denied":
                continue
            sender_fields = [
                field["name"]
                for field in graph_functions[row["name"]]["fields"]
                if field["type"] == "MessageSender"
            ]
            if sender_fields:
                found[row["name"]] = (
                    row["body_validator"],
                    row["target_fields"],
                )
        self.assertEqual(found, expected)


if __name__ == "__main__":
    unittest.main()
