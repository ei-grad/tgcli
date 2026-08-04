#!/usr/bin/env python3

import argparse
import hashlib
import os
import pathlib
import posixpath
import shutil
import stat
import struct
import sys
import tarfile


class ArchiveError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ArchiveError(message)


def normalized_member_path(name: str) -> pathlib.PurePosixPath:
    require("\0" not in name and "\\" not in name, f"unsafe archive member: {name}")
    stripped = name.rstrip("/")
    require(stripped and not stripped.startswith("/"), f"unsafe archive member: {name}")
    parts = stripped.split("/")
    require(
        all(part not in {"", ".", ".."} for part in parts),
        f"unsafe archive member: {name}",
    )
    candidate = pathlib.PurePosixPath(*parts)
    require(not candidate.is_absolute(), f"unsafe archive member: {name}")
    return candidate


def normalized_link_target(member: tarfile.TarInfo) -> pathlib.PurePosixPath:
    target = member.linkname
    require(
        target and "\0" not in target and "\\" not in target,
        f"unsafe archive link: {member.name}",
    )
    require(not target.startswith("/"), f"unsafe archive link: {member.name}")
    if member.issym():
        combined = posixpath.normpath(
            posixpath.join(posixpath.dirname(member.name), target)
        )
    else:
        combined = posixpath.normpath(target)
    require(
        combined not in {"", ".", ".."} and not combined.startswith("../"),
        f"escaping archive link: {member.name}",
    )
    return normalized_member_path(combined)


def ensure_safe_parent(
    destination: pathlib.Path, relative: pathlib.PurePosixPath
) -> None:
    current = destination
    for part in relative.parts[:-1]:
        current = current / part
        if current.exists() or current.is_symlink():
            require(
                current.is_dir() and not current.is_symlink(),
                f"archive member parent is not a directory: {relative}",
            )
        else:
            current.mkdir(mode=0o755)


def open_archive(archive_file: pathlib.Path) -> tarfile.TarFile:
    try:
        return tarfile.open(archive_file, "r:*")
    except (OSError, tarfile.TarError) as error:
        raise ArchiveError(f"cannot open archive {archive_file}: {error}") from error


def extract_archive(
    archive_file: pathlib.Path,
    destination: pathlib.Path,
    *,
    max_members: int,
    max_member_size: int,
    max_expanded_size: int,
) -> None:
    require(archive_file.is_file(), f"archive is missing: {archive_file}")
    require(
        not archive_file.is_symlink(), f"archive cannot be a symlink: {archive_file}"
    )
    require(
        not destination.exists() and not destination.is_symlink(),
        f"archive extraction destination already exists: {destination}",
    )
    destination.mkdir(parents=True, mode=0o755)

    with open_archive(archive_file) as archive:
        members: list[tarfile.TarInfo] = []
        for member in archive:
            require(
                len(members) < max_members,
                "archive member count exceeds safety cap",
            )
            members.append(member)
        seen: set[pathlib.PurePosixPath] = set()
        expanded_size = 0
        prepared: list[tuple[tarfile.TarInfo, pathlib.PurePosixPath]] = []
        hardlinks: list[
            tuple[tarfile.TarInfo, pathlib.PurePosixPath, pathlib.PurePosixPath]
        ] = []

        for member in members:
            relative = normalized_member_path(member.name)
            require(relative not in seen, f"duplicate archive member: {member.name}")
            seen.add(relative)
            require(
                member.isdir() or member.isreg() or member.issym() or member.islnk(),
                f"unsupported archive member type: {member.name}",
            )
            if member.isreg():
                require(
                    member.size <= max_member_size,
                    f"archive member exceeds per-file safety cap: {member.name}",
                )
                expanded_size += member.size
                require(
                    expanded_size <= max_expanded_size,
                    "archive expanded size exceeds safety cap",
                )
            if member.issym() or member.islnk():
                target = normalized_link_target(member)
                if member.islnk():
                    hardlinks.append((member, relative, target))
            prepared.append((member, relative))

        directory_modes: list[tuple[pathlib.Path, int]] = []
        for member, relative in prepared:
            target = destination.joinpath(*relative.parts)
            ensure_safe_parent(destination, relative)
            if member.isdir():
                require(
                    not target.is_symlink(),
                    f"archive directory collides with a symlink: {member.name}",
                )
                target.mkdir(mode=0o755, exist_ok=True)
                directory_modes.append((target, member.mode & 0o777))
            elif member.isreg():
                require(
                    not target.exists() and not target.is_symlink(),
                    f"archive member already exists: {member.name}",
                )
                source = archive.extractfile(member)
                require(
                    source is not None, f"cannot read archive member: {member.name}"
                )
                with source, target.open("xb") as output:
                    copied = shutil.copyfileobj(source, output, length=1024 * 1024)
                require(copied is None, f"cannot extract archive member: {member.name}")
                require(
                    target.stat().st_size == member.size,
                    f"archive member size changed during extraction: {member.name}",
                )
                target.chmod(member.mode & 0o777)
            elif member.issym():
                require(
                    not target.exists() and not target.is_symlink(),
                    f"archive member already exists: {member.name}",
                )
                os.symlink(member.linkname, target)

        for member, relative, link_target in hardlinks:
            target = destination.joinpath(*relative.parts)
            source = destination.joinpath(*link_target.parts)
            ensure_safe_parent(destination, relative)
            require(
                source.is_file() and not source.is_symlink(),
                f"archive hardlink target is missing or unsafe: {member.name}",
            )
            require(
                not target.exists() and not target.is_symlink(),
                f"archive member already exists: {member.name}",
            )
            os.link(source, target)

        for directory, mode in reversed(directory_modes):
            directory.chmod(mode)


def update_length(digest, value: int) -> None:
    digest.update(struct.pack(">Q", value))


def source_tree_sha256(root: pathlib.Path) -> str:
    require(root.is_dir(), f"source tree is missing: {root}")
    require(not root.is_symlink(), f"source tree cannot be a symlink: {root}")
    entries: list[tuple[bytes, pathlib.Path, os.stat_result]] = []

    def visit(directory: pathlib.Path, prefix: pathlib.PurePosixPath) -> None:
        try:
            children = sorted(
                os.scandir(directory), key=lambda entry: os.fsencode(entry.name)
            )
        except OSError as error:
            raise ArchiveError(
                f"cannot read source tree {directory}: {error}"
            ) from error
        for child in children:
            relative = prefix / child.name
            encoded = os.fsencode(relative.as_posix())
            metadata = child.stat(follow_symlinks=False)
            child_path = pathlib.Path(child.path)
            entries.append((encoded, child_path, metadata))
            if stat.S_ISDIR(metadata.st_mode):
                visit(child_path, relative)

    visit(root, pathlib.PurePosixPath())
    digest = hashlib.sha256(b"tgcli-source-tree-v1\0")
    for encoded, entry, metadata in entries:
        update_length(digest, len(encoded))
        digest.update(encoded)
        digest.update(struct.pack(">I", stat.S_IMODE(metadata.st_mode)))
        if stat.S_ISREG(metadata.st_mode):
            digest.update(b"f")
            update_length(digest, metadata.st_size)
            with entry.open("rb") as stream:
                while chunk := stream.read(1024 * 1024):
                    digest.update(chunk)
        elif stat.S_ISDIR(metadata.st_mode):
            digest.update(b"d")
        elif stat.S_ISLNK(metadata.st_mode):
            digest.update(b"l")
            target = os.fsencode(os.readlink(entry))
            update_length(digest, len(target))
            digest.update(target)
        else:
            raise ArchiveError(f"unsupported source-tree entry: {entry}")
    return digest.hexdigest()


def positive_integer(value: str) -> int:
    converted = int(value)
    if converted <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return converted


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Safely handle locked source archives")
    subparsers = parser.add_subparsers(dest="command", required=True)

    extract = subparsers.add_parser("extract")
    extract.add_argument("--archive", type=pathlib.Path, required=True)
    extract.add_argument("--destination", type=pathlib.Path, required=True)
    extract.add_argument("--max-members", type=positive_integer, required=True)
    extract.add_argument("--max-member-size", type=positive_integer, required=True)
    extract.add_argument("--max-expanded-size", type=positive_integer, required=True)

    tree = subparsers.add_parser("tree-sha256")
    tree.add_argument("--root", type=pathlib.Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "extract":
            extract_archive(
                args.archive,
                args.destination,
                max_members=args.max_members,
                max_member_size=args.max_member_size,
                max_expanded_size=args.max_expanded_size,
            )
        else:
            print(source_tree_sha256(args.root))
    except (ArchiveError, OSError, tarfile.TarError) as error:
        print(f"archive verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
