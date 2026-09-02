from __future__ import annotations

import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]


class CiPortabilityContractTest(unittest.TestCase):
    def test_apple_cxx20_compatibility_has_no_unsupported_std_surface(self) -> None:
        forbidden = (
            "std::stop_token",
            "std::stop_source",
            "std::stop_callback",
            "std::jthread",
            "atomic<std::shared_ptr",
        )
        candidates = [
            source
            for root in (REPOSITORY / "src", REPOSITORY / "tests")
            for source in root.rglob("*")
            if source.suffix in {".cpp", ".hpp"}
        ]
        for source in candidates:
            text = source.read_text(encoding="utf-8")
            for token in forbidden:
                with self.subTest(source=source.relative_to(REPOSITORY), token=token):
                    self.assertNotIn(token, text)

        self.assertIn(
            "(void)error;\n    ::arc4random_buf",
            (REPOSITORY / "src/common/daemon_lock.cpp").read_text(encoding="utf-8"),
        )
        self.assertIn(
            "runs-on: macos-14",
            (REPOSITORY / ".github/workflows/ci.yml").read_text(encoding="utf-8"),
        )

    def test_whole_object_publications_use_the_portable_wrapper(self) -> None:
        expected = {
            "src/common/config.hpp": "SharedPublication<const PublishedSnapshot> current_;",
            "src/daemon/config_runtime.hpp": (
                "SharedPublication<const RuntimePublication> publication_;"
            ),
            "src/core/td_client.cpp": (
                "SharedPublication<const AuthStateSnapshot> auth_state_;"
            ),
        }
        for relative, declaration in expected.items():
            with self.subTest(relative=relative):
                self.assertIn(
                    declaration,
                    (REPOSITORY / relative).read_text(encoding="utf-8"),
                )

    def test_full_ctest_jobs_provision_real_zsh(self) -> None:
        ci = (REPOSITORY / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        test_dc = (REPOSITORY / ".github/workflows/test-dc.yml").read_text(
            encoding="utf-8"
        )
        release = (REPOSITORY / ".github/workflows/release.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "sudo apt-get install -y gperf libssl-dev zlib1g-dev ninja-build ccache zsh",
            ci,
        )
        self.assertIn(
            "sudo apt-get install -y gperf libssl-dev zlib1g-dev ninja-build ccache zsh",
            test_dc,
        )
        self.assertIn(
            "brew install gperf ninja ccache coreutils\n          command -v zsh", ci
        )
        self.assertIn(
            "brew install ccache coreutils gperf jq ninja\n          command -v zsh",
            release,
        )
        self.assertNotIn("-v3", ci)
        self.assertNotIn("-v3", test_dc)

    def test_offline_musl_excludes_only_host_proven_zsh_behavior(self) -> None:
        cmake = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        recipe = (REPOSITORY / "scripts/release/build-linux-musl.sh").read_text(
            encoding="utf-8"
        )
        workflow = (REPOSITORY / ".github/workflows/release.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("NAME command-registry-completion-zsh", cmake)
        self.assertIn("NAME command-registry-completion-runtime", cmake)
        exclusion = "--exclude-regex '^command-registry-completion-zsh$'"
        self.assertEqual(recipe.count(exclusion), 1)
        self.assertEqual(recipe.count("--exclude-regex"), 1)
        self.assertIn('"${RELEASE_TEST_COMMAND[@]}"', recipe)
        self.assertIn("sys.argv[2:]", recipe)
        identity = workflow.index("- name: Verify the release source identity")
        install = workflow.index(
            "- name: Install host completion test dependency", identity
        )
        behavior = workflow.index(
            "- name: Verify zsh completion behavior at the validated revision", install
        )
        offline = workflow.index(
            "- name: Build and test without dependency network access"
        )
        self.assertLess(identity, install)
        self.assertLess(install, behavior)
        self.assertLess(behavior, offline)
        behavior_block = workflow[behavior:offline]
        self.assertIn('test "$(git rev-parse HEAD)" = "$SOURCE_SHA"', behavior_block)
        self.assertIn("node tests/verify_command_assets.mjs zsh", behavior_block)

    def test_tdlib_prefix_provenance_names_the_normalization_contract(self) -> None:
        build = (REPOSITORY / "scripts/build-tdlib.sh").read_text(encoding="utf-8")
        cmake = (REPOSITORY / "cmake/DependencyLock.cmake").read_text(encoding="utf-8")
        for text in (build, cmake):
            self.assertIn("doxygen-normalized-v1", text)
        self.assertIn('"schema_version": 2', build)
        self.assertIn("NOT provenance_size EQUAL 4", cmake)


if __name__ == "__main__":
    unittest.main()
