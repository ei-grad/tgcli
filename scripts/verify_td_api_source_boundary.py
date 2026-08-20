#!/usr/bin/env python3

import argparse
from pathlib import Path


TD_API_HEADERS = ("td/telegram/td_api.h", "td/telegram/td_api.hpp")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify generated TD API include confinement")
    parser.add_argument("--repo-root", required=True, type=Path)
    root = parser.parse_args().repo_root.resolve()
    source = root / "src"
    failures: list[str] = []
    for filename in sorted(source.rglob("*")):
        if not filename.is_file() or filename.suffix not in {".cpp", ".hpp"}:
            continue
        text = filename.read_text(encoding="utf-8")
        if not any(header in text for header in TD_API_HEADERS):
            continue
        relative = filename.relative_to(root)
        allowed = filename.suffix == ".cpp" and filename.parent.name in {"core", "daemon"}
        if not allowed:
            failures.append(str(relative))
    if failures:
        raise SystemExit("generated TD API headers escaped the daemon boundary: " + ", ".join(failures))
    print("TD API source boundary verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
