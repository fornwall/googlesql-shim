# googlesql-shim

Prebuilt static [GoogleSQL](https://github.com/google/googlesql) shim archives
for [smallquery](https://github.com/fornwall/smallquery).

smallquery analyzes queries with real GoogleSQL through a small C shim
(`shim/shim.cc`), built by Bazel together with all of GoogleSQL. That C++
build takes tens of minutes cold. This repository runs it in GitHub Actions
for each supported platform and publishes the results as GitHub Release
assets, so smallquery builds can download a prebuilt archive set instead.

The shim sources in `shim/` originate from smallquery's
`crates/googlesql-sys/shim` (the PR #91 / `agent/alwayslink-split` state) and
are now versioned by this repository. GoogleSQL itself is not vendored here:
`MODULE.bazel` pins it with an `archive_override` to an exact commit of
[fornwall/googlesql](https://github.com/fornwall/googlesql) — the same commit
smallquery's `vendor/googlesql` submodule points at.

## Platform support

| Target | Status | Built on |
|---|---|---|
| `x86_64-linux-gnu` | released | `ubuntu-24.04` runner, apt.llvm.org clang (pinned major), glibc 2.39 |
| `aarch64-linux-gnu` | released | `ubuntu-24.04-arm` runner, same toolchain recipe |
| `x86_64-linux-musl` | released | Alpine 3.23 container (musl-native Bazel 8, Alpine clang) |
| `aarch64-linux-musl` | released | Digest-pinned Alpine edge container on the arm64 runner (stable is not possible yet: no rules_python release ships an aarch64-musl CPython, so the generators' glibc interpreter needs gcompat + edge musl) |
| `aarch64-apple-darwin` | released | `macos-15` runner, Homebrew llvm (same pinned major) |
| `x86_64-apple-darwin` | released | `macos-15-intel` runner, same recipe (a `--cpu=darwin_x86_64` cross-compile from arm64 silently produced arm64 objects — the generic Unix toolchain ignores the legacy flag — so the leg builds natively) |
| `x86_64-pc-windows` | not built | The historical blocker (ICU's autotools build under MSYS) is gone with the native ICU build below; GoogleSQL's own MSVC portability is the open question, iterated in `.github/workflows/windows-probe.yml` |
| `aarch64-pc-windows` | not built | Blocked before the C++ even starts: rules_python has no hermetic CPython for windows-arm64, so GoogleSQL's pip extension fails analysis; after that it shares the x86_64 unknowns |

## What a release contains

One tarball per platform plus a single `SHA256SUMS` asset covering all of
them: `googlesql-shim-<tag>-<triple>.tar.zst`, zstd-compressed
(`tar --zstd -xf` to extract), containing at its root:

| File | What it is |
|---|---|
| `libgooglesql_shim.a` | The shim and its whole transitive GoogleSQL closure (Abseil, protobuf, RE2, ...) flattened into one archive by `cc_static_library`. Contains no ICU objects. |
| `libgooglesql_shim_alwayslink.a` | The objects of every `alwayslink = 1` target in the closure (today exactly `row_type_with_catalog.pic.o`). A `.a` cannot record `alwayslink`, and these objects export no strong global symbol, so ordinary archive member selection would silently drop them — the consumer must link this side archive with `--whole-archive` (`+whole-archive` in rustc terms), *before* the main archive. |
| `libicudata.a`, `libicuuc.a`, `libicui18n.a`, `libicuio.a` | ICU, built natively from the same icu4c release GoogleSQL pins (see below). Linked normally, after the main archive. |
| `BUILDINFO.txt` | Provenance: the googlesql-shim and googlesql commits, build flags, and the exact bazel/compiler/libc/OS versions used. |

To verify a download:

```sh
sha256sum -c --ignore-missing SHA256SUMS
```

## How this maps to smallquery's build

These are the same artifact set that
`bazel build //crates/googlesql-sys/shim:static` produces inside smallquery
at `bazel-bin/crates/googlesql-sys/shim/`. smallquery's
`crates/googlesql-sys/build.rs` (static feature) expects exactly this set in
one directory: it links `googlesql_shim_alwayslink` with `+whole-archive`,
then `googlesql_shim`, then every other `lib*.a` beside them (ICU), then the
system `stdc++`/`pthread`/`m`/`dl`. Extract a tarball into a directory and
point `GOOGLESQL_SHIM_LIB` at the `libgooglesql_shim.a` there.

## Build configuration

`bazel build -c opt --force_pic //shim:static`, per platform.

- **`--force_pic` is load-bearing on Linux**: without it `-c opt` packs both
  the PIC and non-PIC variant of every object into the bundle (`shim.o` *and*
  `shim.pic.o`), and a `--whole-archive` consumer link fails on the resulting
  duplicate symbols. The release workflow verifies the bundle is PIC-only.
- **Toolchains are pinned**, not taken from whatever the runner image ships:
  apt.llvm.org's stable branch (one `LLVM_MAJOR` shared with Homebrew's
  `llvm@` formula on macOS), and a pinned stable Alpine base for musl. Each
  tarball's `BUILDINFO.txt` records the exact versions used.
- **ICU is built natively by Bazel** (`third_party/icu/icu.BUILD`), replacing
  GoogleSQL's rules_foreign_cc/autotools build — autotools was the only
  configure_make target in the graph and the first thing to break on every
  non-Linux platform. Its one irreplaceable job, static *data* packaging, is
  reproduced by rendering the icu4c release's prebuilt `icudt76l.dat` through
  `third_party/icu/dat_to_c.py` (a re-implementation of `genccode`'s C
  output) and compiling it like any other source. GoogleSQL compiles against
  headers-only ICU targets, so the shim bundle stays GoogleSQL-only and ICU
  ships as the same four archives the autotools build produced.
  Equivalence was verified against the autotools artifacts: same bundle
  member set, matching ICU archive sizes, and smallquery's full query test
  suite (including the timestamp/timezone tests that read ICU data at
  runtime) passes against the result.

### The musl legs

Upstream Bazel ships glibc-only binaries, so the musl build runs natively
inside an Alpine container (`scripts/build-musl.sh`):

- `bazel8` is the one package pulled from edge/testing (no stable branch
  carries a Bazel), pinned per-command via `--repository`.
- x86_64 builds on stable Alpine 3.23: GoogleSQL's code generators run
  under rules_python's hermetic CPython, and the build selects the **musl**
  python-build-standalone interpreter via
  `RULES_PYTHON_REPO_TOOLCHAIN__LINUX_*` (host) and
  `--@rules_python//python/config_settings:py_linux_libc=musl` (toolchains).
  The host-selection env var is broken upstream (it compares the preference
  against `(platform, meta)` tuples), so rules_python is taken via
  `single_version_override` with the one-hunk
  `patches/rules_python-host-preference.patch`.
- aarch64 builds on a digest-pinned Alpine edge image instead: no
  rules_python release ships an aarch64-musl interpreter, so the glibc
  CPython must run — which needs `gcompat` plus edge's musl (stable's lacks
  `posix_fallocate64`). Only the generators run under that interpreter;
  every published object is compiled by Alpine clang against musl.
- `shim/link_extras.bzl` uses public `LibraryToLink` fields and emits Apple
  `libtool` or ar-style archiver arguments as the toolchain requires, so the
  same rules analyze under Bazel 9 (glibc, macOS) and Alpine's Bazel 8.

## Compatibility caveats

- **glibc / libstdc++**: the `*-linux-gnu` archives are built against the
  `ubuntu-24.04` image's glibc 2.39 and require a compatible (same or newer)
  glibc and libstdc++ when linked. The musl archives track the pinned
  Alpine's musl. `BUILDINFO.txt` in each tarball records the exact versions.
- The archives contain C++20 code; the final link needs the C++ runtime
  (`-lstdc++` on Linux, `-lc++` on macOS).
- Linux archives are PIC-only objects; macOS archives are Mach-O
  (arm64 or x86_64 per the tarball's triple, verified with `lipo` at build
  time).

## Building locally

```sh
# glibc/macOS (needs clang, bazelisk on PATH):
bazel build -c opt --force_pic \
  --repo_env=CC=/path/to/clang --repo_env=CXX=/path/to/clang++ \
  //shim:static

# musl (needs docker):
mkdir -p dist
docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out alpine:3.23 \
  sh /src/scripts/build-musl.sh
```

Build with Clang, not GCC (GCC 15 rejects GoogleSQL's
`virtual const Type* Type() const` under `-Wchanges-meaning`). Network access
is required for Bazel to fetch GoogleSQL and its dependencies.

## Licenses of the bundled components

This repository is licensed Apache-2.0 (`LICENSE.txt`). The published
archives embed object code from these upstream projects:

| Component | License |
|---|---|
| GoogleSQL | Apache-2.0 |
| Abseil (abseil-cpp) | Apache-2.0 |
| Protocol Buffers | BSD-3-Clause |
| RE2 | BSD-3-Clause |
| ICU | Unicode-3.0 (ICU License) |
| zlib | zlib License |

(Among others pulled in transitively by GoogleSQL's Bazel module; consult the
upstream module for the full dependency set.)
