# Third-party notices

This file records the third-party components that are compiled into tgcli or
are pinned for the v1.0 release-runtime closure. Exact source identities and
archive checksums are in
[`release/dependencies.lock.json`](release/dependencies.lock.json). tgcli's own
license is in [`LICENSE`](LICENSE).

## Runtime components

<!-- lock-id:tdlib -->
### TDLib 1.8.65

Source: <https://github.com/tdlib/td> at
`a17f87c4cff7b90b278d12b91ba0614383aaee82`.

TDLib is available under the Boost Software License 1.0. Its exact license
text is reproduced in [`release/licenses/TDLib.txt`](release/licenses/TDLib.txt).

The pinned TDLib tree embeds a modified SQLite 3.31.0/SQLCipher 4.4.0 community
amalgamation under `sqlite/sqlite`. SQLite's public-domain dedication and
blessing are reproduced in
[`release/licenses/SQLite-blessing.txt`](release/licenses/SQLite-blessing.txt).
The bundled Zetetic redistribution notice is reproduced in
[`release/licenses/TDLib-SQLCipher.txt`](release/licenses/TDLib-SQLCipher.txt).

<!-- lock-id:cli11 -->
### CLI11 2.5.0

Source: <https://github.com/CLIUtils/CLI11> at
`4160d259d961cd393fd8d67590a8c7d210207348`.

CLI11 is available under the BSD 3-Clause License. Its exact license text is
reproduced in [`release/licenses/CLI11.txt`](release/licenses/CLI11.txt).

<!-- lock-id:nlohmann_json -->
### JSON for Modern C++ 3.12.0

Source: <https://github.com/nlohmann/json> at
`55f93686c01528224f448c19128836e7df245f72`.

JSON for Modern C++ is available under the MIT License. Its exact license text
is reproduced in
[`release/licenses/nlohmann-json.txt`](release/licenses/nlohmann-json.txt).

<!-- lock-id:fmt -->
### fmt 11.2.0

Source: <https://github.com/fmtlib/fmt> at
`40626af88bd7df9a5fb80be7b25ac85b122d6c21`.

fmt is available under the MIT License with its stated object-code exception.
The exact terms are reproduced in
[`release/licenses/fmt.txt`](release/licenses/fmt.txt).

<!-- lock-id:tomlplusplus -->
### toml++ 3.4.0

Source: <https://github.com/marzer/tomlplusplus> at
`30172438cee64926dc41fdd9c11fb3ba5b2ba9de`.

toml++ is available under the MIT License. Its exact license text is reproduced
in [`release/licenses/tomlplusplus.txt`](release/licenses/tomlplusplus.txt).

## Locked release-runtime closure

The following source releases are locked for the future static-release
toolchain. The current build still resolves them from the host toolchain, so
this lock does not claim that an existing artifact contains these exact
versions.

<!-- lock-id:openssl -->
### OpenSSL 3.5.7

Source: <https://github.com/openssl/openssl>, release `openssl-3.5.7`.

OpenSSL is available under the Apache License 2.0. Its exact license text is
reproduced in
[`release/licenses/OpenSSL.txt`](release/licenses/OpenSSL.txt).

<!-- lock-id:zlib -->
### zlib 1.3.2

Source: <https://github.com/madler/zlib>, release `v1.3.2`.

zlib is available under the zlib License. Its exact license text is reproduced
in [`release/licenses/zlib.txt`](release/licenses/zlib.txt).

## Planned but unresolved release runtimes

<!-- planned-lock-id:musl -->
- musl libc is expected in the static Linux artifact and is generally licensed
  under MIT terms. No source version or archive is locked yet.

<!-- planned-lock-id:gcc-runtime -->
- libgcc and libstdc++ may be embedded by the selected compiler toolchain under
  `GPL-3.0-or-later WITH GCC-exception-3.1`. No GCC source version or archive is
  locked yet.

An artifact containing either planned component is not release-ready until the
toolchain selects an exact source version, the lock records its immutable source
and checksum, and the corresponding upstream notices are checked in. This file
does not infer those facts from the host compiler.

## Build and test only

Catch2 3.8.1 (`2b60af89e23d28eefc081bc930831ee9d45ea58b`) and
jsoncons 1.7.0 (`cb54cdc3134a62634466bf7bcd24f1a906f4ef25`) are fetched
only when tests are enabled. They are recorded in the dependency lock but are
not runtime-distributed components, so their notices are not included in the
runtime notice bundle.
