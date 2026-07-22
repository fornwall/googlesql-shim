# WebAssembly build (experimental)

The whole shim — GoogleSQL front end, absl, protobuf, RE2, zlib, ICU with
real data — builds to wasm32-unknown-emscripten and runs: a smoke test
drives the parser and analyzer through the C ABI inside a wasm module under
node.

```sh
bazel build --config=wasm -c opt //shim:static     # the archives
bazel build --config=wasm -c opt //wasm:smoke_wasm # the runtime proof
node bazel-bin/wasm/smoke_wasm/smoke.js
```

`--config=wasm` selects `@emsdk//:platform_wasm`, resolved by the Emscripten
SDK's official Bazel toolchain (a plain `bazel_dep` from the Bazel Central
Registry — Bazel downloads the SDK itself, nothing to install). Generator
tools — protoc, bison/flex, textmapper — still build for and run on the host;
only target code compiles with emcc.

## What is different from the native build

- **ICU: nothing.** This is the payoff of the repo's native ICU build
  (`third_party/icu/icu.BUILD`, swapped over GoogleSQL's `icu` repo by
  `override_repo` in MODULE.bazel). That build already replaced ICU's
  autotools `configure_make` with plain `cc_library` targets over the icu4c
  source release, and embeds the prebuilt little-endian common data
  (`source/data/in/icudt76l.dat`, filtered down and rendered to C by
  `third_party/icu:dat_to_c`) as an ordinary compiled source. Being plain
  Bazel C++ with no host-tool cross-build dance, it cross-compiles to
  emscripten unchanged — so the wasm leg reuses it verbatim and needs **no
  ICU override of its own**. (Autotools ICU could not: its cross build runs
  code-generation tools out of a completed *native* ICU build tree via
  `--with-cross-build`, which rules_foreign_cc has no story for. The native
  Bazel build sidesteps that on every platform, wasm included.)

  Consequence for the artifact set: exactly like the native build, the shim
  bundle stays ICU-free (GoogleSQL compiles against `@icu`'s headers-only
  targets) and ICU ships as the same four side archives
  (`libicuuc/libicui18n/libicuio/libicudata.a`) beside `libgooglesql_shim.a`.
  A wasm consumer links the same set a native consumer does — the artifact
  contract is identical across platforms.

- **Exceptions.** Emscripten compiles with `-fno-exceptions` by default; the
  shim's exception firewalls (`GSQL_TRY` in shim.cc) compile away under
  `__cpp_exceptions` absence. Nothing can unwind across the C ABI either
  way — without exception support a `throw` aborts, which is the same
  containment minus the error message. GoogleSQL itself is exception-free;
  the whole closure compiles under `-fno-exceptions` unmodified.

- **Threads.** The archive is built without atomics (`threads = "off"` on
  the smoke target; the analyzer is single-threaded per request, and absl's
  mutexes compile to emscripten's single-threaded stubs). absl and protobuf
  still say `-pthread`/`-lpthread` in their linkopts via their default POSIX
  select branches; `--config=wasm` appends `-sUSE_PTHREADS=0`, which lands
  after them and wins, and `-lpthread` resolves to emscripten's stub.

- **`build:linux` suppression.** The host-keyed `build:linux` GNU-ld flags
  (`--no-as-needed`, `-l:libstdc++.so.6`) would otherwise land on wasm-ld's
  command line; `--config=wasm` turns `--enable_platform_specific_config`
  off. Those flags exist for the native link and mean nothing to wasm-ld.

- **No `--force_pic`.** PIC on wasm means dynamic-linking support the
  consumer would have to opt into; the release recipe's `--force_pic` (which
  exists to stop `-c opt` packing both PIC and non-PIC objects into the
  bundle) stays off, and `-c opt` alone does not double the objects under
  the emscripten toolchain.

## The smoke test

`wasm/smoke.c` hand-encodes request protos (so it needs no protobuf of its
own) and calls the shim like the Rust consumer does:

- `FORMAT_SQL` of a messy statement → pretty-printed by the real parser;
- `ANALYZE` of `SELECT 1 + 2` against an inline catalog with the builtin
  function library → a resolved AST comes back;
- `FORMAT_SQL` of malformed SQL → the parse error surfaces as a status code
  plus GoogleSQL's caret-diagnostic message.

`wasm/smoke.c` is a plain `cc_binary` on `//shim:shim_lib` plus the four
`@icu//:icu*_lib` targets (GoogleSQL compiles against headers-only `@icu`, so
any actual binary must link the ICU code and data itself, exactly as
`//shim:smoke_test` does). `wasm_cc_binary` applies its own platform
transition, so its `cc_target` rebuilds in a second output directory; that is
expected. Build it *with* `--config=wasm` anyway — the transition keeps
command-line flags, and the `.bazelrc` wasm additions (pthread override,
`build:linux` suppression) are what make the link work.

**tzdata**: `AnalyzerOptions`' constructor CHECK-fails unless
`FindTimeZoneByName("America/Los_Angeles")` succeeds, and absl/cctz resolves
zone names by reading `/usr/share/zoneinfo` — a filesystem a wasm module
does not have. The smoke target embeds the host's tzdata (~0.6M) into the
module's in-memory filesystem via `--embed-file`. Any real consumer faces
the same decision: embed tzdata, or mount a filesystem (emscripten
`NODERAWFS`, a WASI preopen, ...). Without it the analyzer aborts at
options-construction time; the parser path needs no tzdata.

## Toolchain versions

- emsdk Bazel module 6.0.3 (BCR), emscripten 6.0.3-git, clang 23
- Bazel 9.2.0
- Run under node v26 (any recent node works; emscripten's bundled node too)

## The wasi-sdk alternative

Emscripten is clang; the wasm32 backend is the same LLVM. What bare clang
lacks is a sysroot — libc/libc++ for wasm — and the packaged alternatives
are emsdk (musl-based libc, POSIX emulation, official Bazel toolchain) and
wasi-sdk (wasi-libc + libc++, target wasm32-wasip1, no Bazel toolchain
module). wasi-sdk 33 was probed by hand and compiles representative absl
files cleanly, `absl/synchronization/mutex.cc` included, so a wasip1 leg is
plausible; it would cost a hand-rolled `cc_toolchain` (or `toolchains_llvm`
wired to a wasi sysroot) and a thinner POSIX story at runtime. The native ICU
build is toolchain-agnostic (plain `cc_library` + a compiled data source), so
a wasi leg would reuse it exactly as the emscripten leg does.

## Known limits

- The wasm leg is compile-and-smoke verified; no Rust wasm32 consumer has
  linked these archives yet. The archives are ordinary `llvm-ar` archives of
  wasm objects, so `wasm32-unknown-emscripten` Rust linking is the expected
  path (matching libc), not `wasm32-wasip1`.
- `wasm_cc_binary`'s transition config compiles the closure a second time —
  archives and smoke test do not share a Bazel cache config.
- CI builds none of this yet; the release workflow is untouched.

Note: the ILP32 `RoundUpToNextPowerOfTwo` overflow that this experiment
originally surfaced (`1L << 62` is UB where `long` is 32-bit, logging
`Out of range: 2` on every analyzer run) is now fixed upstream-of-us by
`patches/googlesql/pull-4.patch`, so a wasm analyzer run is quiet.
