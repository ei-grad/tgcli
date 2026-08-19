from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "scripts"))

import generate_schema_assets as generator


def json_bytes(document: object) -> bytes:
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode()


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
        data = b'{"value": ")TG012345678900\\""}\n'
        literal = generator.raw_literal(data, "fixture.schema.json")
        delimiter = literal[2 : literal.index("(")]
        self.assertLessEqual(len(delimiter), 16)
        self.assertNotIn(f'){delimiter}"', data.decode())
        self.assertTrue(literal.endswith(f'){delimiter}"'))

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
        cases = (
            "../alpha.result.schema.json",
            "/alpha.result.schema.json",
            "nested/alpha.result.schema.json",
            "nested\\alpha.result.schema.json",
            "alpha\0result.schema.json",
            ".",
            "alpha.json",
        )
        for reference in cases:
            with self.subTest(reference=reference):
                fixture = SchemaFixture()
                try:
                    fixture.write_catalog(
                        "manifest.json", {"alpha": {"result": reference}}
                    )
                    with self.assertRaisesRegex(
                        generator.GenerationError, "unsafe schema reference"
                    ):
                        generator.build_inventory(fixture.root)
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
