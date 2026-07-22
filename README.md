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

## What a release contains

One tarball per platform, plus a single `SHA256SUMS` asset covering all of
them:

| Asset | Platform | Built on |
|---|---|---|
| `googlesql-shim-<tag>-x86_64-linux-gnu.tar.zst` | x86-64 Linux, glibc | GitHub `ubuntu-24.04` runner, its system clang |
| `googlesql-shim-<tag>-x86_64-linux-musl.tar.zst` | x86-64 Linux, musl | Alpine container (`alpine:edge`) on an ubuntu runner |
| `googlesql-shim-<tag>-aarch64-apple-darwin.tar.zst` | Apple Silicon macOS | GitHub `macos-15` runner, Xcode clang |
| `SHA256SUMS` | — | `sha256sum` lines for every tarball |

Each tarball is zstd-compressed (`tar --zstd -xf` to extract) and contains,
at its root:

| File | What it is |
|---|---|
| `libgooglesql_shim.a` | The shim and its whole transitive GoogleSQL closure (Abseil, protobuf, RE2, ...) flattened into one archive by `cc_static_library`. |
| `libgooglesql_shim_alwayslink.a` | The objects of every `alwayslink = 1` target in the closure (today exactly `row_type_with_catalog.pic.o`; `.o` on macOS). A `.a` cannot record `alwayslink`, and these objects export no strong global symbol, so ordinary archive member selection would silently drop them — the consumer must link this side archive with `--whole-archive` (`+whole-archive` in rustc terms), *before* the main archive. |
| `libicudata.a`, `libicuuc.a`, `libicui18n.a`, `libicuio.a` | ICU, built by `rules_foreign_cc` outside Bazel's C++ rules, so `cc_static_library` structurally cannot absorb it. Linked normally, after the main archive. |
| `BUILDINFO.txt` | Provenance: the googlesql-shim and googlesql commits, build flags, and the exact bazel/compiler/libc/OS versions used. |

To verify a download:

```sh
sha256sum -c --ignore-missing SHA256SUMS
```

## How this maps to smallquery's build

These are the same files that
`bazel build //crates/googlesql-sys/shim:static` produces inside smallquery
at `bazel-bin/crates/googlesql-sys/shim/`. smallquery's
`crates/googlesql-sys/build.rs` (static feature) expects exactly this set in
one directory: it links `googlesql_shim_alwayslink` with `+whole-archive`,
then `googlesql_shim`, then every other `lib*.a` beside them (ICU), then the
system `stdc++`/`pthread`/`m`/`dl`. Extract a tarball into a directory and
point `GOOGLESQL_SHIM_LIB` at the `libgooglesql_shim.a` there.

## Build configuration

`bazel build -c opt --force_pic //shim:static`, per platform.

`--force_pic` is load-bearing on Linux: without it `-c opt` packs both the
PIC and non-PIC variant of every object into the bundle (`shim.o` *and*
`shim.pic.o`), and a `--whole-archive` consumer link fails on the resulting
duplicate symbols. The release workflow verifies the bundle is PIC-only.
The configuration may change in a later release; the release notes and each
`BUILDINFO.txt` state what was used.

### The musl build

Upstream Bazel ships glibc-only binaries, so the musl build runs natively
inside an Alpine container using Alpine's own musl-native `bazel8` package
(edge/testing), with two portability notes (see `scripts/build-musl.sh`):

- `gcompat` is installed so rules_python's hermetic (glibc-linked) CPython
  can run GoogleSQL's code generators. Only the generators run under that
  interpreter; every object in the published archives is compiled by Alpine's
  clang against musl.
- `shim/link_extras.bzl` uses the public `objects`/`pic_objects` fields of
  `LibraryToLink` rather than Bazel 9's private `_contains_objects`, so the
  same rule analyzes under both Bazel 9 (glibc, macOS) and Bazel 8 (Alpine).

## Compatibility caveats

- **glibc / libstdc++**: the `x86_64-linux-gnu` archives are built on
  GitHub's `ubuntu-24.04` runner (glibc 2.39) and require a compatible (same
  or newer) glibc and libstdc++ when linked. The musl archives correspondingly
  track the Alpine edge musl and its libstdc++. `BUILDINFO.txt` in each
  tarball records the exact versions.
- The archives contain C++20 code; the final link needs the C++ runtime
  (`-lstdc++` on Linux, `-lc++` per Xcode's default on macOS).
- Linux archives are PIC-only objects; macOS archives are Mach-O.

## Building locally

```sh
# glibc/macOS (needs clang, bazelisk, make, pkg-config on PATH):
bazel build -c opt --force_pic \
  --repo_env=CC=/path/to/clang --repo_env=CXX=/path/to/clang++ \
  //shim:static

# musl (needs docker):
mkdir -p dist
docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out alpine:edge \
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
