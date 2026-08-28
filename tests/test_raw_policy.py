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
                "td_api_header_sha256": "sha256:5926a873f226667dfa69e6cda9f28c03407aefc31a90de73dd96be0e8fa6c536",
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
            'RawBodyValidatorDescriptor{"deny", &validate_raw_body_deny}', generated
        )
        self.assertNotIn("kGeneratedRawBodyValidatorSymbols", generated)

    def test_dormant_seed_denies_every_unreviewed_function(self) -> None:
        policy = raw_policy.load_json(
            REPOSITORY
            / "docs"
            / "raw"
            / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
        )
        self.assertFalse(policy["activation_ready"])
        self.assertEqual(policy["function_count"], 1001)
        for row in policy["functions"]:
            self.assertFalse(row["reviewed"])
            self.assertEqual(row["admission"], "denied")
            self.assertEqual(row["body_validator"], "deny")
            self.assertTrue(row["sensitive_input"])
            self.assertTrue(row["sensitive_output"])

    def test_activation_validator_rejects_the_dormant_seed(self) -> None:
        with self.assertRaisesRegex(raw_policy.PolicyError, "not activation-ready"):
            raw_policy.validate_assets(TDLIB_SOURCE, REPOSITORY, activation=True)

    def test_unreviewed_admission_and_asset_drift_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination = root / "docs" / "raw"
            shutil.copytree(REPOSITORY / "docs" / "raw", destination)
            policy_file = destination / f"raw-policy.{raw_policy.PINNED_TDLIB_SHA}.json"
            policy = json.loads(policy_file.read_text(encoding="utf-8"))
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
        policy["functions"][0]["reviewed"] = True
        policy["policy_sha256"] = raw_policy.rows_digest(policy["functions"])
        with self.assertRaisesRegex(raw_policy.PolicyError, "lacks concrete reasoning"):
            raw_policy.validate_policy(inventory, policy)

        policy["functions"][0].update(
            {
                "review_reason": "reviewed:fixture evidence",
                "admission": "read",
                "body_validator": "none",
            }
        )
        policy["policy_sha256"] = raw_policy.rows_digest(policy["functions"])
        with self.assertRaisesRegex(
            raw_policy.PolicyError, "response-sensitive function is admitted"
        ):
            raw_policy.validate_policy(inventory, policy)

        policy["functions"][0]["body_validator"] = "missing_callable"
        policy["policy_sha256"] = raw_policy.rows_digest(policy["functions"])
        with self.assertRaisesRegex(
            raw_policy.PolicyError, "compiled policy validator missing"
        ):
            raw_policy.validate_policy(inventory, policy)


if __name__ == "__main__":
    unittest.main()
