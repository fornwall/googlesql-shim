#!/bin/sh
# Builds //shim:static natively on musl, inside an Alpine container.
#
# Invoked (by the release workflow, or locally) as:
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out alpine:edge \
#     sh /src/scripts/build-musl.sh
#
# Why this shape: upstream Bazel ships glibc-only binaries, so bazelisk is a
# dead end on musl. Alpine's own `bazel8` package (edge/testing) is a
# musl-native Bazel 8, which builds this module. One wrinkle remains:
# GoogleSQL's code generators run under rules_python's *hermetic* CPython,
# which is glibc-linked -- `gcompat` (Alpine's glibc compatibility shim) is
# enough to run it. Only the generators run under that interpreter; every
# object in the published archives is compiled by Alpine's clang against
# musl.
set -eux

echo "https://dl-cdn.alpinelinux.org/alpine/edge/testing" >> /etc/apk/repositories
apk add --no-cache bazel8 clang make pkgconf python3 zstd bash linux-headers \
  build-base gcompat

# Work on a copy: /src is mounted read-only, and Bazel plants bazel-*
# convenience symlinks in the workspace.
cp -r /src /work
cd /work
rm -rf bazel-* dist

bazel build -c opt --force_pic \
  --repo_env=CC=/usr/bin/clang \
  --repo_env=CXX=/usr/bin/clang++ \
  //shim:static

cp -L bazel-bin/shim/libgooglesql_shim.a \
      bazel-bin/shim/libgooglesql_shim_alwayslink.a \
      bazel-bin/shim/libicu*.a \
      /out/

{
  echo "triple: $(uname -m)-linux-musl"
  echo "bazel: $(bazel --version)"
  echo "cc: $(clang --version | head -1)"
  echo "libc: musl $(apk list --installed 2>/dev/null | grep '^musl-[0-9]' || true)"
  echo "os: Alpine $(cat /etc/alpine-release) (container on the runner)"
} > /out/toolchain.txt
