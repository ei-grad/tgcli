from __future__ import annotations

import itertools
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]
CI_WORKFLOW = REPOSITORY / ".github/workflows/ci.yml"
CI_PROFILES = ("debug", "asan", "tsan", "macos", "lint")


def yaml_scope(text: str, keys: tuple[str, ...]) -> tuple[list[str], int, int, str]:
    lines = text.splitlines()
    start = 0
    end = len(lines)
    value = ""
    for depth, key in enumerate(keys):
        indentation = depth * 2
        prefix = " " * indentation + key + ":"
        header = next(
            (
                index
                for index in range(start, end)
                if lines[index].startswith(prefix)
                and lines[index][: len(prefix)] == prefix
            ),
            None,
        )
        if header is None:
            raise ValueError(f"missing YAML key path: {'.'.join(keys)}")
        value = lines[header][len(prefix) :].strip()
        block_end = end
        for index in range(header + 1, end):
            line = lines[index]
            if not line.strip():
                continue
            current_indentation = len(line) - len(line.lstrip(" "))
            if current_indentation <= indentation:
                block_end = index
                break
        start = header + 1
        end = block_end
    return lines, start, end, value


def yaml_value(text: str, keys: tuple[str, ...]) -> str:
    return yaml_scope(text, keys)[3]


def yaml_child_keys(text: str, keys: tuple[str, ...]) -> tuple[str, ...]:
    lines, start, end, _ = yaml_scope(text, keys)
    indentation = len(keys) * 2
    result: list[str] = []
    for line in lines[start:end]:
        if len(line) - len(line.lstrip(" ")) != indentation:
            continue
        match = re.fullmatch(r"\s*([A-Za-z0-9_-]+):(?:\s+.*)?", line)
        if match:
            result.append(match.group(1))
    return tuple(result)


def yaml_block_scalar(text: str, scope: tuple[str, ...], key: str, style: str) -> str:
    lines, start, end, _ = yaml_scope(text, scope)
    header = next(
        (
            index
            for index in range(start, end)
            if lines[index].strip() == f"{key}: {style}"
        ),
        None,
    )
    if header is None:
        raise ValueError(f"missing YAML block scalar: {key}")
    indentation = len(lines[header]) - len(lines[header].lstrip(" "))
    content: list[str] = []
    for line in lines[header + 1 : end]:
        if line.strip():
            current_indentation = len(line) - len(line.lstrip(" "))
            if current_indentation <= indentation:
                break
            content.append(line[indentation + 2 :])
        else:
            content.append("")
    if style == ">-":
        return " ".join(part.strip() for part in content if part.strip())
    return "\n".join(content).rstrip() + "\n"


def yaml_step_by_id(text: str, job: str, step_id: str) -> str:
    lines, start, end, _ = yaml_scope(text, ("jobs", job, "steps"))
    list_indentation = 6
    boundaries = [
        index
        for index in range(start, end)
        if len(lines[index]) - len(lines[index].lstrip(" ")) == list_indentation
        and lines[index].lstrip().startswith("- ")
    ]
    boundaries.append(end)
    for index, begin in enumerate(boundaries[:-1]):
        finish = boundaries[index + 1]
        step_lines = lines[begin:finish]
        if any(line.strip() == f"id: {step_id}" for line in step_lines):
            return "\n".join(step_lines) + "\n"
    raise ValueError(f"missing YAML step id: {job}.{step_id}")


class CiPortabilityContractTest(unittest.TestCase):
    def test_apple_cxx20_compatibility_has_no_unsupported_std_surface(self) -> None:
        forbidden = (
            "std::stop_token",
            "std::stop_source",
            "std::stop_callback",
            "std::jthread",
            "std::quick_exit",
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

    def test_posix_close_and_immediate_exit_users_include_their_own_declaration(
        self,
    ) -> None:
        candidates = [
            source
            for root in (REPOSITORY / "src", REPOSITORY / "tests")
            for source in root.rglob("*")
            if source.suffix in {".cpp", ".hpp"}
        ]
        for source in candidates:
            text = source.read_text(encoding="utf-8")
            for operation in ("::close(", "::_exit("):
                if operation not in text:
                    continue
                with self.subTest(
                    source=source.relative_to(REPOSITORY), operation=operation
                ):
                    self.assertIn("#include <unistd.h>\n", text)

    def test_signal_set_macros_are_not_namespace_qualified(self) -> None:
        qualified_signal_set_call = re.compile(
            r"::\s*(?:sigemptyset|sigfillset|sigaddset|sigdelset|sigismember)\s*\("
        )
        candidates = [
            source
            for root in (REPOSITORY / "src", REPOSITORY / "tests")
            for source in root.rglob("*")
            if source.suffix in {".cpp", ".hpp"}
        ]
        for source in candidates:
            with self.subTest(source=source.relative_to(REPOSITORY)):
                self.assertIsNone(
                    qualified_signal_set_call.search(source.read_text(encoding="utf-8"))
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
        self.assertIn("-v5", ci)
        self.assertIn("-v5", test_dc)
        self.assertNotIn("-v4", ci)
        self.assertNotIn("-v4", test_dc)

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
            self.assertIn("doxygen-normalized-v2", text)
        self.assertIn('"schema_version": 2', build)
        self.assertIn("NOT provenance_size EQUAL 4", cmake)


class CiWorkflowSelectionContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = CI_WORKFLOW.read_text(encoding="utf-8")
        cls.profile_step = yaml_step_by_id(cls.workflow, "selection", "profiles")
        cls.selection_template = yaml_block_scalar(
            cls.profile_step, (), "PROFILE_SELECTION", ">-"
        )
        selector_run = yaml_block_scalar(cls.profile_step, (), "run", "|").splitlines()
        if selector_run[0] != "python3 - <<'PY'" or selector_run[-1] != "PY":
            raise ValueError("CI profile selector must remain an inline Python heredoc")
        cls.selector_source = "\n".join(selector_run[1:-1]) + "\n"

    def assert_selector_wiring(self, workflow: str) -> None:
        self.assertEqual(
            yaml_value(workflow, ("jobs", "selection", "outputs", "linux_profiles")),
            "${{ steps.profiles.outputs.linux_profiles }}",
        )
        self.assertEqual(
            yaml_value(workflow, ("jobs", "selection", "outputs", "tdlib_os")),
            "${{ steps.profiles.outputs.tdlib_os }}",
        )
        profile_step = yaml_step_by_id(workflow, "selection", "profiles")
        yaml_block_scalar(profile_step, (), "PROFILE_SELECTION", ">-")
        yaml_block_scalar(profile_step, (), "run", "|")

    def render_selection(self, selection: dict[str, bool]) -> str:
        rendered = self.selection_template
        for profile in CI_PROFILES:
            marker = "${{ inputs." + profile + " }}"
            self.assertEqual(rendered.count(marker), 1)
            rendered = rendered.replace(marker, json.dumps(selection[profile]))
        self.assertNotIn("${{", rendered)
        self.assertEqual(json.loads(rendered), selection)
        return rendered

    def run_selector(
        self, selection: dict[str, bool]
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, str]]:
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / "github-output"
            completed = subprocess.run(
                [sys.executable, "-c", self.selector_source],
                check=False,
                capture_output=True,
                text=True,
                env={
                    "GITHUB_OUTPUT": str(output_path),
                    "PROFILE_SELECTION": self.render_selection(selection),
                },
            )
            outputs = {}
            if output_path.exists():
                for line in output_path.read_text(encoding="utf-8").splitlines():
                    key, value = line.split("=", maxsplit=1)
                    outputs[key] = value
            return completed, outputs

    def test_only_manual_dispatch_with_independent_full_defaults(self) -> None:
        self.assertEqual(
            yaml_child_keys(self.workflow, ("on",)), ("workflow_dispatch",)
        )
        self.assertEqual(
            yaml_child_keys(self.workflow, ("on", "workflow_dispatch", "inputs")),
            CI_PROFILES,
        )
        for profile in CI_PROFILES:
            root = ("on", "workflow_dispatch", "inputs", profile)
            with self.subTest(profile=profile):
                self.assertEqual(yaml_value(self.workflow, root + ("type",)), "boolean")
                self.assertEqual(
                    yaml_value(self.workflow, root + ("required",)), "true"
                )
                self.assertEqual(yaml_value(self.workflow, root + ("default",)), "true")
                self.assertIn(
                    "full run", yaml_value(self.workflow, root + ("description",))
                )

    def test_selector_accepts_every_nonempty_combination_and_rejects_empty(
        self,
    ) -> None:
        for values in itertools.product((False, True), repeat=len(CI_PROFILES)):
            selection = dict(zip(CI_PROFILES, values))
            with self.subTest(selection=selection):
                completed, outputs = self.run_selector(selection)
                if not any(values):
                    self.assertEqual(completed.returncode, 1)
                    self.assertIn(
                        "::error::Select at least one CI profile.", completed.stdout
                    )
                    self.assertEqual(outputs, {})
                    continue

                self.assertEqual(completed.returncode, 0, completed.stderr)
                expected_linux = [
                    profile
                    for profile in ("debug", "asan", "tsan")
                    if selection[profile]
                ]
                expected_os = []
                if expected_linux or selection["lint"]:
                    expected_os.append("ubuntu-24.04")
                if selection["macos"]:
                    expected_os.append("macos-14")
                self.assertEqual(json.loads(outputs["linux_profiles"]), expected_linux)
                self.assertEqual(json.loads(outputs["tdlib_os"]), expected_os)

    def test_selected_matrices_and_job_guards_consume_selector_outputs(self) -> None:
        self.assert_selector_wiring(self.workflow)
        self.assertEqual(
            yaml_child_keys(self.workflow, ("jobs",)),
            ("selection", "tdlib", "test", "test-macos", "lint"),
        )
        self.assertEqual(
            yaml_value(self.workflow, ("jobs", "tdlib", "needs")), "selection"
        )
        self.assertEqual(
            yaml_value(self.workflow, ("jobs", "tdlib", "strategy", "matrix", "os")),
            "${{ fromJSON(needs.selection.outputs.tdlib_os) }}",
        )
        self.assertEqual(
            yaml_value(self.workflow, ("jobs", "test", "needs")),
            "[selection, tdlib]",
        )
        self.assertEqual(
            yaml_value(self.workflow, ("jobs", "test", "if")),
            "${{ needs.selection.outputs.linux_profiles != '[]' }}",
        )
        self.assertEqual(
            yaml_value(self.workflow, ("jobs", "test", "strategy", "matrix", "preset")),
            "${{ fromJSON(needs.selection.outputs.linux_profiles) }}",
        )
        for job, profile in (("test-macos", "macos"), ("lint", "lint")):
            with self.subTest(job=job):
                self.assertEqual(
                    yaml_value(self.workflow, ("jobs", job, "needs")),
                    "[selection, tdlib]",
                )
                self.assertEqual(
                    yaml_value(self.workflow, ("jobs", job, "if")),
                    "${{ inputs." + profile + " }}",
                )

    def test_selector_wiring_mutations_fail_closed(self) -> None:
        without_id = self.workflow.replace("        id: profiles\n", "", 1)
        with self.assertRaisesRegex(ValueError, "missing YAML step id"):
            self.assert_selector_wiring(without_id)

        mistyped_output = self.workflow.replace(
            "steps.profiles.outputs.linux_profiles",
            "steps.profile.outputs.linux_profiles",
            1,
        )
        with self.assertRaises(AssertionError):
            self.assert_selector_wiring(mistyped_output)

    def test_default_selection_resolves_the_original_full_profile_set(self) -> None:
        defaults = {
            profile: yaml_value(
                self.workflow,
                ("on", "workflow_dispatch", "inputs", profile, "default"),
            )
            == "true"
            for profile in CI_PROFILES
        }
        completed, outputs = self.run_selector(defaults)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            json.loads(outputs["linux_profiles"]), ["debug", "asan", "tsan"]
        )
        self.assertEqual(json.loads(outputs["tdlib_os"]), ["ubuntu-24.04", "macos-14"])


if __name__ == "__main__":
    unittest.main()
