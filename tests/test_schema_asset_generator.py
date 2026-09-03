from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True
REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "scripts"))

import generate_schema_assets as generator


def json_bytes(document: object) -> bytes:
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode()


def compile_cpp(source: str, directory: Path) -> None:
    source_file = directory / "literal_test.cpp"
    source_file.write_text(source, encoding="utf-8")
    subprocess.run(
        [
            *shlex.split(os.environ.get("CXX", "c++")),
            "-std=c++20",
            "-fsyntax-only",
            "-Werror",
            "-Woverlength-strings",
            "-I",
            str(REPOSITORY / "src"),
            str(source_file),
        ],
        check=True,
        capture_output=True,
        text=True,
    )


class SchemaFixture:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "source"
        self.schemas = self.root / "docs" / "schemas"
        self.schemas.mkdir(parents=True)
        for filename in (
            "alpha.result.schema.json",
            "alpha.item.schema.json",
            "alpha.error.schema.json",
            "other.result.schema.json",
        ):
            self.write_schema(filename)
        self.write_catalog(
            "manifest.json",
            {"alpha": {"result": "alpha.result.schema.json"}},
        )
        self.write_catalog(
            "stream-manifest.json",
            {
                "alpha": {
                    "result": "alpha.result.schema.json",
                    "item": "alpha.item.schema.json",
                }
            },
        )
        self.write_catalog(
            "error-manifest.json",
            {"alpha": {"error": "alpha.error.schema.json"}},
        )

    def close(self) -> None:
        self.temporary.cleanup()

    def write_schema(self, filename: str, document: object | None = None) -> None:
        if document is None:
            document = {
                "$schema": generator.DIALECT,
                "type": "object",
                "additionalProperties": False,
            }
        (self.schemas / filename).write_bytes(json_bytes(document))

    def write_catalog(self, filename: str, commands: object) -> None:
        (self.schemas / filename).write_bytes(
            json_bytes(
                {
                    "schemaDialect": generator.DIALECT,
                    "commands": commands,
                }
            )
        )


class GeneratorTest(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = SchemaFixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def assert_generation_fails(self, pattern: str) -> None:
        with self.assertRaisesRegex(generator.GenerationError, pattern):
            generator.build_inventory(self.fixture.root)

    def test_deterministic_cpp_and_package_inventory(self) -> None:
        first = self.fixture.root.parent / "first.cpp"
        second = self.fixture.root.parent / "second.cpp"
        generator.emit_cpp(self.fixture.root, first)
        generator.emit_cpp(self.fixture.root, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertNotIn(str(self.fixture.root).encode(), first.read_bytes())
        self.assertEqual(
            generator.package_files(self.fixture.root),
            (
                "docs/schemas/alpha.error.schema.json",
                "docs/schemas/alpha.item.schema.json",
                "docs/schemas/alpha.result.schema.json",
                "docs/schemas/error-manifest.json",
                "docs/schemas/manifest.json",
                "docs/schemas/stream-manifest.json",
            ),
        )
        inventory = generator.build_inventory(self.fixture.root)
        self.assertEqual(
            [(item.command, item.kind, item.filename) for item in inventory.mappings],
            [
                ("alpha", "result", "alpha.result.schema.json"),
                ("alpha", "item", "alpha.item.schema.json"),
                ("alpha", "error", "alpha.error.schema.json"),
            ],
        )

    def test_raw_literal_uses_a_bounded_collision_safe_delimiter(self) -> None:
        data = b'before)TG000000000000"after\n'
        digest = mock.Mock()
        digest.hexdigest.return_value = "0" * 64
        with mock.patch.object(generator.hashlib, "sha256", return_value=digest):
            literal = generator.raw_literal(data, "fixture.schema.json")
            repeated = generator.raw_literal(data, "fixture.schema.json")

        delimiter = literal[2 : literal.index("(")]
        self.assertEqual(delimiter, "TG000000000001")
        self.assertEqual(literal, repeated)
        self.assertLessEqual(len(delimiter), 16)
        self.assertNotIn(f'){delimiter}"', data.decode())
        self.assertTrue(literal.endswith(f'){delimiter}"'))
        escaped = generator.cpp_string_literal('unsafe"\\\n\x01')
        compile_cpp(
            "#include <string_view>\n"
            f"constexpr std::string_view raw = {literal};\n"
            f"constexpr std::string_view escaped = {escaped};\n"
            "static_assert(raw.size() > 0 && escaped.size() > 0);\n",
            self.fixture.root.parent,
        )

    def test_literal_chunks_preserve_exact_bytes_at_portable_boundaries(self) -> None:
        boundary = b"a" * generator.CPP_LITERAL_CHUNK_BYTES
        over_boundary = boundary + b"b"
        utf8_boundary = b"a" * (generator.CPP_LITERAL_CHUNK_BYTES - 1) + "雪".encode()
        escaped_nul_utf8 = b'{"title":"\\u0000 snow \xe9\x9b\xaa"}\n'
        cases = (
            (boundary, (generator.CPP_LITERAL_CHUNK_BYTES,)),
            (over_boundary, (generator.CPP_LITERAL_CHUNK_BYTES, 1)),
            (utf8_boundary, (generator.CPP_LITERAL_CHUNK_BYTES - 1, 3)),
            (escaped_nul_utf8, (len(escaped_nul_utf8),)),
        )
        for data, expected_sizes in cases:
            with self.subTest(size=len(data)):
                chunks = generator.literal_chunks(data, "fixture")
                reconstructed = b"".join(chunks)
                self.assertEqual(tuple(map(len, chunks)), expected_sizes)
                self.assertEqual(reconstructed, data)
                self.assertEqual(
                    hashlib.sha256(reconstructed).digest(),
                    hashlib.sha256(data).digest(),
                )
                self.assertTrue(
                    all(
                        len(chunk) <= generator.CPP_LITERAL_CHUNK_BYTES
                        for chunk in chunks
                    )
                )

    def test_large_generated_assets_use_only_bounded_literals_and_compile(self) -> None:
        padding = '雪/raw)TGdelimiter"' * generator.CPP_LITERAL_CHUNK_BYTES
        self.fixture.write_schema(
            "alpha.result.schema.json",
            {
                "$schema": generator.DIALECT,
                "type": "object",
                "description": padding,
            },
        )
        self.fixture.write_schema(
            "other.result.schema.json",
            {
                "$schema": generator.DIALECT,
                "type": "object",
                "description": padding + "second\0",
            },
        )
        self.fixture.write_catalog(
            "manifest.json",
            {
                "alpha": {"result": "alpha.result.schema.json"},
                "beta": {"result": "other.result.schema.json"},
            },
        )
        inventory = generator.build_inventory(self.fixture.root)
        rendered = generator.render_cpp(inventory).decode()
        literals = tuple(
            match.group("body").encode()
            for match in re.finditer(
                r'R"(?P<delimiter>[A-Za-z0-9]+)\((?P<body>.*?)\)(?P=delimiter)"',
                rendered,
                re.DOTALL,
            )
        )
        self.assertTrue(literals)
        self.assertLess(generator.CPP_LITERAL_CHUNK_BYTES, 65_536)
        self.assertLessEqual(max(map(len, literals)), generator.CPP_LITERAL_CHUNK_BYTES)
        expected_assets = tuple(data for _, data in inventory.assets) + tuple(
            catalog.bytes for catalog in inventory.catalogs
        )
        literal_index = 0
        for expected in expected_assets:
            chunk_count = len(generator.literal_chunks(expected, "expected asset"))
            reconstructed = b"".join(
                literals[literal_index : literal_index + chunk_count]
            )
            self.assertEqual(reconstructed, expected)
            self.assertEqual(
                hashlib.sha256(reconstructed).digest(),
                hashlib.sha256(expected).digest(),
            )
            literal_index += chunk_count
        self.assertEqual(literal_index, len(literals))
        self.assertIn("constexpr auto kSchema", rendered)
        self.assertNotIn("constexpr std::string_view kSchema0 =", rendered)
        compile_cpp(rendered, self.fixture.root.parent)

    def test_duplicate_json_keys_are_rejected(self) -> None:
        (self.fixture.schemas / "manifest.json").write_text(
            '{"schemaDialect":"x","schemaDialect":"y","commands":{}}\n',
            encoding="utf-8",
        )
        self.assert_generation_fails("duplicate JSON key")

    def test_different_raw_spellings_that_canonicalize_together_are_rejected(
        self,
    ) -> None:
        cases = (
            ("alpha beta", "alpha  beta"),
            ("read", "history"),
        )
        for canonical, colliding in cases:
            with self.subTest(colliding=colliding):
                fixture = SchemaFixture()
                try:
                    fixture.write_catalog(
                        "manifest.json",
                        {canonical: {"result": "alpha.result.schema.json"}},
                    )
                    fixture.write_catalog(
                        "error-manifest.json",
                        {colliding: {"error": "alpha.error.schema.json"}},
                    )
                    with self.assertRaisesRegex(
                        generator.GenerationError, "catalog command collision"
                    ):
                        generator.build_inventory(fixture.root)
                finally:
                    fixture.close()

    def test_noncanonical_command_keys_are_rejected(self) -> None:
        cases = (
            "",
            " alpha",
            "alpha ",
            "alpha  beta",
            "alpha\tbeta",
            "alpha\nbeta",
            "alpha\rbeta",
            "alpha\fbeta",
            "alpha\vbeta",
            "Alpha",
            "álpha",
            '"alpha"',
            "history",
        )
        for command in cases:
            with self.subTest(command=command):
                fixture = SchemaFixture()
                try:
                    fixture.write_catalog(
                        "error-manifest.json",
                        {command: {"error": "alpha.error.schema.json"}},
                    )
                    with self.assertRaises(generator.GenerationError):
                        generator.build_inventory(fixture.root)
                finally:
                    fixture.close()

    def test_identical_keys_deduplicate_or_coexist_but_conflicts_fail(self) -> None:
        inventory = generator.build_inventory(self.fixture.root)
        self.assertEqual(len(inventory.mappings), 3)

        self.fixture.write_catalog(
            "stream-manifest.json",
            {"alpha": {"result": "other.result.schema.json"}},
        )
        self.assert_generation_fails("conflicting schema mapping")

    def test_catalog_roots_dialect_commands_and_kinds_are_strict(self) -> None:
        cases = (
            {"commands": {}},
            {
                "schemaDialect": "https://example.invalid/dialect",
                "commands": {},
            },
            {
                "schemaDialect": generator.DIALECT,
                "commands": [],
            },
            {
                "schemaDialect": generator.DIALECT,
                "commands": {},
                "extra": True,
            },
        )
        for index, document in enumerate(cases):
            with self.subTest(index=index):
                fixture = SchemaFixture()
                try:
                    (fixture.schemas / "manifest.json").write_bytes(
                        json_bytes(document)
                    )
                    with self.assertRaises(generator.GenerationError):
                        generator.build_inventory(fixture.root)
                finally:
                    fixture.close()

        self.fixture.write_catalog(
            "manifest.json",
            {"alpha": {"item": "alpha.item.schema.json"}},
        )
        self.assert_generation_fails("invalid kinds")

    def test_schema_references_are_safe_leaf_names(self) -> None:
        controls = tuple(
            f"alpha{chr(byte)}.result.schema.json" for byte in (*range(32), 127)
        )
        cases = (
            "",
            ".schema.json",
            "..schema.json",
            "./alpha.result.schema.json",
            "../alpha.result.schema.json",
            "/alpha.result.schema.json",
            "alpha.result.schema.json/",
            "nested/alpha.result.schema.json",
            "nested\\alpha.result.schema.json",
            ".\\alpha.result.schema.json",
            "C:alpha.result.schema.json",
            'alpha"result.schema.json',
            "alpha'result.schema.json",
            "alpha result.schema.json",
            "alpha:result.schema.json",
            "alpha*result.schema.json",
            "alpha?result.schema.json",
            "alpha<result.schema.json",
            "alpha>result.schema.json",
            "alpha|result.schema.json",
            "Alpha.result.schema.json",
            "álpha.result.schema.json",
            "-alpha.result.schema.json",
            "con.result.schema.json",
            "lpt9.result.schema.json",
            ".",
            "..",
            "alpha.json",
            *controls,
        )
        for reference in cases:
            with self.subTest(reference=reference):
                fixture = SchemaFixture()
                try:
                    fixture.write_catalog(
                        "manifest.json", {"alpha": {"result": reference}}
                    )
                    with self.assertRaises(generator.GenerationError):
                        generator.build_inventory(fixture.root)
                    with self.assertRaises(generator.GenerationError):
                        generator.package_files(fixture.root)
                finally:
                    fixture.close()

    def test_opened_catalog_and_schema_leaves_ignore_path_substitution(self) -> None:
        outside_catalog = json_bytes(
            {
                "schemaDialect": generator.DIALECT,
                "commands": {"outside": {"result": "other.result.schema.json"}},
            }
        )
        outside_schema = json_bytes(
            {
                "$schema": generator.DIALECT,
                "type": "object",
                "title": "outside replacement",
            }
        )
        cases = (
            ("source catalog manifest.json", "manifest.json", outside_catalog),
            (
                "source schema alpha.result.schema.json",
                "alpha.result.schema.json",
                outside_schema,
            ),
        )
        for owner, filename, outside_bytes in cases:
            with self.subTest(filename=filename):
                fixture = SchemaFixture()
                try:
                    candidate = fixture.schemas / filename
                    expected = candidate.read_bytes()
                    retained = fixture.root.parent / f"retained-{filename}"
                    original_reader = generator.read_descriptor_bytes
                    substituted = False

                    def substitute_then_read(
                        descriptor: int,
                        observed_owner: str,
                        expected_owner: str = owner,
                        target: Path = candidate,
                        retained_target: Path = retained,
                        replacement: bytes = outside_bytes,
                        reader=original_reader,
                    ) -> bytes:
                        nonlocal substituted
                        if observed_owner == expected_owner:
                            self.assertFalse(substituted)
                            target.rename(retained_target)
                            target.write_bytes(replacement)
                            substituted = True
                        return reader(descriptor, observed_owner)

                    with mock.patch.object(
                        generator,
                        "read_descriptor_bytes",
                        side_effect=substitute_then_read,
                    ):
                        inventory = generator.build_inventory(fixture.root)

                    self.assertTrue(substituted)
                    embedded = {
                        catalog.name: catalog.bytes for catalog in inventory.catalogs
                    }
                    embedded.update(dict(inventory.assets))
                    self.assertEqual(embedded[filename], expected)
                    self.assertNotIn(outside_bytes, embedded.values())
                finally:
                    fixture.close()

    def test_every_source_open_uses_retained_no_follow_descriptors(self) -> None:
        calls: list[tuple[str, int, int | None]] = []
        original_open = generator.os.open

        def record_open(
            candidate: str | os.PathLike[str], flags: int, *, dir_fd: int | None = None
        ) -> int:
            calls.append((os.fspath(candidate), flags, dir_fd))
            return original_open(candidate, flags, dir_fd=dir_fd)

        with mock.patch.object(generator.os, "open", side_effect=record_open):
            generator.build_inventory(self.fixture.root)

        self.assertTrue(calls)
        for _, flags, _ in calls:
            self.assertEqual(flags & os.O_CLOEXEC, os.O_CLOEXEC)
            self.assertEqual(flags & os.O_NOFOLLOW, os.O_NOFOLLOW)
        self.assertEqual(calls[0][0], os.fspath(self.fixture.root.absolute()))
        self.assertIsNone(calls[0][2])
        self.assertEqual(calls[0][1] & os.O_DIRECTORY, os.O_DIRECTORY)
        self.assertTrue(all(dir_fd is not None for _, _, dir_fd in calls[1:]))
        leaf_calls = [
            (name, flags, dir_fd)
            for name, flags, dir_fd in calls
            if name.endswith(".json")
        ]
        self.assertTrue(leaf_calls)
        self.assertTrue(all(flags & os.O_DIRECTORY == 0 for _, flags, _ in leaf_calls))

    def test_retained_parent_edges_fail_if_moved_outside_the_root(self) -> None:
        for edge in ("docs", "schemas"):
            with self.subTest(edge=edge):
                fixture = SchemaFixture()
                try:
                    candidate = (
                        fixture.root / "docs" if edge == "docs" else fixture.schemas
                    )
                    detached = fixture.root.parent / f"detached-{edge}"
                    replacement_schemas = (
                        fixture.root / "docs" / "schemas"
                        if edge == "docs"
                        else fixture.schemas
                    )
                    original_reader = generator.read_descriptor_bytes
                    substituted = False

                    def detach_then_read(
                        descriptor: int,
                        owner: str,
                        target: Path = candidate,
                        detached_target: Path = detached,
                        replacement: Path = replacement_schemas,
                        reader=original_reader,
                    ) -> bytes:
                        nonlocal substituted
                        if owner == "source catalog manifest.json":
                            self.assertFalse(substituted)
                            target.rename(detached_target)
                            replacement.mkdir(parents=True)
                            (replacement / "manifest.json").write_bytes(
                                b"outside bytes must not be accepted\n"
                            )
                            substituted = True
                        return reader(descriptor, owner)

                    with (
                        mock.patch.object(
                            generator,
                            "read_descriptor_bytes",
                            side_effect=detach_then_read,
                        ),
                        self.assertRaisesRegex(
                            generator.GenerationError, "outside its trusted parent"
                        ),
                    ):
                        generator.build_inventory(fixture.root)
                    self.assertTrue(substituted)
                finally:
                    fixture.close()

    def test_schema_utf8_object_dialect_and_final_lf_are_strict(self) -> None:
        cases = (
            b"\xff\n",
            b"{\n",
            b"[]\n",
            (
                b'{"$schema":"https://json-schema.org/draft/2020-12/schema",'
                b'"value":NaN}\n'
            ),
            b'{"$schema":"wrong"}\n',
            b'{"$schema":"https://json-schema.org/draft/2020-12/schema"}',
            b'{"$schema":"https://json-schema.org/draft/2020-12/schema"}\n\n',
            b'{"$schema":"https://json-schema.org/draft/2020-12/schema","title":"raw\0nul"}\n',
        )
        for index, data in enumerate(cases):
            with self.subTest(index=index):
                fixture = SchemaFixture()
                try:
                    (fixture.schemas / "alpha.result.schema.json").write_bytes(data)
                    with self.assertRaises(generator.GenerationError):
                        generator.build_inventory(fixture.root)
                finally:
                    fixture.close()

    def test_trusted_source_components_reject_symlinks_and_wrong_types(self) -> None:
        def root_symlink(fixture: SchemaFixture) -> Path:
            link = fixture.root.parent / "source-link"
            link.symlink_to(fixture.root, target_is_directory=True)
            return link

        def directory_symlink(fixture: SchemaFixture, name: str) -> Path:
            target = fixture.root.parent / f"outside-{name}"
            candidate = fixture.root / name
            candidate.rename(target)
            candidate.symlink_to(target, target_is_directory=True)
            return fixture.root

        def schema_directory_symlink(fixture: SchemaFixture) -> Path:
            target = fixture.root.parent / "outside-schemas"
            fixture.schemas.rename(target)
            fixture.schemas.symlink_to(target, target_is_directory=True)
            return fixture.root

        mutators = (
            root_symlink,
            lambda fixture: directory_symlink(fixture, "docs"),
            schema_directory_symlink,
        )
        for mutator in mutators:
            with self.subTest(mutator=mutator.__name__):
                fixture = SchemaFixture()
                try:
                    source_root = mutator(fixture)
                    with self.assertRaisesRegex(
                        generator.GenerationError, "cannot be a symlink"
                    ):
                        generator.build_inventory(source_root)
                finally:
                    fixture.close()

        components = (
            "manifest.json",
            "stream-manifest.json",
            "error-manifest.json",
            "alpha.result.schema.json",
            "alpha.item.schema.json",
            "alpha.error.schema.json",
        )
        for component in components:
            with self.subTest(component=component):
                fixture = SchemaFixture()
                try:
                    candidate = fixture.schemas / component
                    target = fixture.root.parent / f"outside-{component}"
                    candidate.rename(target)
                    candidate.symlink_to(target)
                    with self.assertRaisesRegex(
                        generator.GenerationError, "cannot be a symlink"
                    ):
                        generator.build_inventory(fixture.root)
                finally:
                    fixture.close()

        def root_file(fixture: SchemaFixture) -> Path:
            fixture.root.rename(fixture.root.parent / "real-source")
            fixture.root.write_text("not a directory", encoding="utf-8")
            return fixture.root

        def directory_file(fixture: SchemaFixture, name: str) -> Path:
            candidate = fixture.root / name
            candidate.rename(fixture.root / f"real-{name}")
            candidate.write_text("not a directory", encoding="utf-8")
            return fixture.root

        def schema_directory_file(fixture: SchemaFixture) -> Path:
            fixture.schemas.rename(fixture.root / "real-schemas")
            fixture.schemas.write_text("not a directory", encoding="utf-8")
            return fixture.root

        wrong_directories = (
            root_file,
            lambda fixture: directory_file(fixture, "docs"),
            schema_directory_file,
        )
        for mutator in wrong_directories:
            with self.subTest(mutator=mutator.__name__):
                fixture = SchemaFixture()
                try:
                    source_root = mutator(fixture)
                    with self.assertRaisesRegex(
                        generator.GenerationError, "must be a directory"
                    ):
                        generator.build_inventory(source_root)
                finally:
                    fixture.close()

        for component in components:
            with self.subTest(component=f"wrong type: {component}"):
                fixture = SchemaFixture()
                try:
                    candidate = fixture.schemas / component
                    candidate.unlink()
                    candidate.mkdir()
                    with self.assertRaisesRegex(
                        generator.GenerationError, "must be a regular file"
                    ):
                        generator.build_inventory(fixture.root)
                finally:
                    fixture.close()

    def test_missing_catalog_and_schema_are_rejected(self) -> None:
        def missing_root(fixture: SchemaFixture) -> Path:
            fixture.root.rename(fixture.root.parent / "real-source")
            return fixture.root

        def missing_directory(fixture: SchemaFixture, name: str) -> Path:
            candidate = fixture.root / name
            candidate.rename(fixture.root / f"real-{name}")
            return fixture.root

        def missing_schema_directory(fixture: SchemaFixture) -> Path:
            fixture.schemas.rename(fixture.root / "real-schemas")
            return fixture.root

        missing_directories = (
            missing_root,
            lambda fixture: missing_directory(fixture, "docs"),
            missing_schema_directory,
        )
        for mutator in missing_directories:
            with self.subTest(mutator=mutator.__name__):
                fixture = SchemaFixture()
                try:
                    source_root = mutator(fixture)
                    with self.assertRaisesRegex(
                        generator.GenerationError, "missing or inaccessible"
                    ):
                        generator.build_inventory(source_root)
                finally:
                    fixture.close()

        components = (
            "manifest.json",
            "stream-manifest.json",
            "error-manifest.json",
            "alpha.result.schema.json",
            "alpha.item.schema.json",
            "alpha.error.schema.json",
        )
        for component in components:
            with self.subTest(component=component):
                fixture = SchemaFixture()
                try:
                    (fixture.schemas / component).unlink()
                    with self.assertRaisesRegex(
                        generator.GenerationError, "missing or inaccessible"
                    ):
                        generator.build_inventory(fixture.root)
                finally:
                    fixture.close()


if __name__ == "__main__":
    unittest.main()
