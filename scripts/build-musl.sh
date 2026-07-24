#!/bin/sh
# Builds //shim:static natively on musl, inside an Alpine container.
#
# aarch64 ONLY. x86_64-linux-musl is now cross-compiled from a glibc runner
# with the hermetic_cc_toolchain (zig cc) via --config=musl -- see the release
# workflow -- because that keeps the exec configuration on glibc, so no
# musl-native Bazel or musl CPython is needed. aarch64 cannot use that trick
# yet: it is not a compiler problem but a code-generator one. No rules_python
# release ships an aarch64-musl CPython, so GoogleSQL's Python code generators
# have no musl interpreter to run under; here they run the glibc interpreter
# under gcompat instead, which only works when the whole build is native musl.
#
# Invoked (by the release workflow, or locally) as:
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out <alpine image> \
#     sh /src/scripts/build-musl.sh
# where <alpine image> is the digest-pinned alpine:edge in the release workflow.
#
# Why this shape: upstream Bazel ships glibc-only binaries, so bazelisk is a
# dead end on musl. Alpine's own `bazel8` package is a musl-native Bazel 8,
# which builds this module; it is the one package that must come from
# edge/testing (no stable branch carries a bazel), pinned per-command via
# --repository so the rest of the install resolves against the base image's
# repositories.
#
# GoogleSQL's code generators run under rules_python's hermetic CPython: no
# rules_python version ships an aarch64-musl interpreter, so the glibc CPython
# must run -- which needs gcompat plus the newer musl from edge (stable's musl
# lacks the posix_fallocate64 the interpreter binds; runtime-only concern, the
# generators' output is text). Hence the digest-pinned edge base until upstream
# ships an aarch64-musl interpreter.
set -eux

# python3 stays: py_binary tools in the exec configuration resolve to
# Bazel's autodetecting toolchain (env python3) when no hermetic runtime
# matches the exec platform.
# tzdata is for //shim:smoke_test: AnalyzerOptions' constructor CHECKs a
# FindTimeZoneByName("America/Los_Angeles") lookup, which aborts on a
# zoneinfo-less Alpine base image.
apk add --no-cache clang bash linux-headers build-base python3 tzdata gcompat
apk add --no-cache bazel8 \
  --repository=https://dl-cdn.alpinelinux.org/alpine/edge/testing

arch="$(uname -m)"

# Work on a copy: /src is mounted read-only, and Bazel plants bazel-*
# convenience symlinks in the workspace.
cp -r /src /work
cd /work
rm -rf bazel-* dist

# //shim:smoke_test alongside //shim:static so the behavioral gate runs in
# the artifacts' exact configuration -- a build that computes wrong analyzer
# answers (the -c opt NDEBUG/QCHECK_OK bug) fails here rather than shipping.
bazel test -c opt --force_pic \
  --repo_env=CC=/usr/bin/clang \
  --repo_env=CXX=/usr/bin/clang++ \
  --test_output=errors \
  //shim:static //shim:smoke_test //shim:protos

cp -L bazel-bin/shim/libgooglesql_shim.a \
      bazel-bin/shim/libgooglesql_shim_alwayslink.a \
      bazel-bin/shim/libicu*.a \
      /out/
cp -rL bazel-bin/shim/protos /out/protos
# Bazel outputs are read-only; ship writable trees so a consumer's extracted
# cache can be removed with a plain rm -rf.
chmod -R u+w /out

{
  echo "triple: ${arch}-linux-musl"
  echo "bazel: $(bazel --version)"
  echo "cc: $(clang --version | head -1)"
  echo "libc: musl $(apk list --installed 2>/dev/null | grep '^musl-[0-9]' || true)"
  echo "os: Alpine $(cat /etc/alpine-release) (container on the runner)"
} > /out/toolchain.txt
