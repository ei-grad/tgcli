# Third-party notices

This file records the third-party components compiled into tgcli and the
build-only inputs pinned for the v1.0 release. Exact source identities,
archive checksums, and Linux toolchain evidence are in
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

## Linux release-runtime closure

The Linux release recipe builds OpenSSL and zlib from the following verified
source archives. The pinned musl toolchain artifact supplies the selected musl
and GCC static archives and ELF startup objects. The dependency lock records
canonical paths and hashes for libc, libdl, libm, libatomic, libgcc,
libgcc_eh, libstdc++, crt1, crti, crtbeginT, crtend, and crtn, plus the
producer build log that ties them to the source releases below. The release
recipe rejects any different same-basename input selected by the compiler
driver or final linker map.

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

<!-- lock-id:musl -->
### musl 1.2.6

Source: <https://musl.libc.org/releases/musl-1.2.6.tar.gz>, release `v1.2.6`.

musl is available under the MIT License with the additional compatible terms
and attribution inventory reproduced in
[`release/licenses/musl.txt`](release/licenses/musl.txt).

<!-- lock-id:gcc-runtime -->
### GCC runtime libraries and startup objects 16.1.0

Source: <https://ftp.gnu.org/gnu/gcc/gcc-16.1.0/gcc-16.1.0.tar.xz>, release
`releases/gcc-16.1.0`.

The selected libatomic, libgcc, libgcc_eh, libstdc++, crtbeginT, and crtend
runtime portions are covered by GPL-3.0-or-later with the GCC Runtime Library
Exception 3.1. The terms are reproduced in
[`release/licenses/GCC-GPL-3.0.txt`](release/licenses/GCC-GPL-3.0.txt) and
[`release/licenses/GCC-Runtime-Library-Exception.txt`](release/licenses/GCC-Runtime-Library-Exception.txt).

## Build-only Linux inputs

The following components are used to produce the Linux binary but are not
distributed in the release package as runtime components.

<!-- build-lock-id:gperf -->
### GNU gperf 3.3

Source: <https://ftp.gnu.org/gnu/gperf/gperf-3.3.tar.gz>, release `gperf-3.3`.
The release recipe builds this host tool from source because the pinned build
image does not provide it. gperf is available under GPL-3.0-or-later; the terms
are reproduced in
[`release/licenses/GCC-GPL-3.0.txt`](release/licenses/GCC-GPL-3.0.txt).

<!-- build-lock-id:linux-musl-toolchain -->
### cross-tools musl toolchain 20260515

Source and release producer: <https://github.com/cross-tools/musl-cross> at
`aa4bf173d705256e7fc2db82604bdccca090e9c3`. The x86_64 release archive is
accepted only with the content digest in the dependency lock. The producer
wrapper is available under the MIT License, reproduced in
[`release/licenses/cross-tools-musl-cross.txt`](release/licenses/cross-tools-musl-cross.txt).

The toolchain archive also carries its complete upstream build-tool license
tree. Runtime-linked musl and GCC terms are independently checked in above.

<!-- build-lock-id:linux-build-image -->
### Dockcross base image 20260515-5fd14ac

Source: <https://github.com/dockcross/dockcross> at
`5fd14ac6dca6c8d89e6942ff35879906a5f3a932`. The workflow pulls the image only
by its OCI SHA-256 digest and runs the verified source build without network
access. Dockcross is available under the MIT License, reproduced in
[`release/licenses/dockcross.txt`](release/licenses/dockcross.txt).

## Build and test only

Catch2 3.8.1 (`2b60af89e23d28eefc081bc930831ee9d45ea58b`) and
jsoncons 1.7.0 (`cb54cdc3134a62634466bf7bcd24f1a906f4ef25`) are fetched
only when tests are enabled. They are recorded in the dependency lock but are
not runtime-distributed components, so their notices are not included in the
runtime notice bundle.
