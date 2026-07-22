#!/bin/sh
# Builds //shim:static natively on musl, inside an Alpine container.
#
# Invoked (by the release workflow, or locally) as:
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out <alpine image> \
#     sh /src/scripts/build-musl.sh
# where <alpine image> is alpine:3.23 on x86_64 and a digest-pinned
# alpine:edge on aarch64 (see the release workflow for why).
#
# Why this shape: upstream Bazel ships glibc-only binaries, so bazelisk is a
# dead end on musl. Alpine's own `bazel8` package is a musl-native Bazel 8,
# which builds this module; it is the one package that must come from
# edge/testing (no stable branch carries a bazel), pinned per-command via
# --repository so the rest of the install resolves against the base image's
# repositories.
#
# GoogleSQL's code generators run under rules_python's hermetic CPython, and
# the two architectures diverge there:
# - x86_64: rules_python has musl python-build-standalone interpreters. The
#   RULES_PYTHON_REPO_TOOLCHAIN__LINUX_* override selects one for the
#   version-agnostic *host* interpreter (the double underscore is real: the
#   host repo formats the variable with an empty version segment; the
#   selection code itself is broken upstream and fixed by
#   patches/rules_python-host-preference.patch), and py_linux_libc=musl
#   selects it for the py_binary toolchains. The base stays stable Alpine.
# - aarch64: NO rules_python version ships an aarch64-musl interpreter, so
#   the glibc CPython must run -- which needs gcompat plus the newer musl
#   from edge (stable's musl lacks the posix_fallocate64 the interpreter
#   binds; runtime-only concern, the generators' output is text). Hence the
#   digest-pinned edge base for this architecture until upstream ships an
#   aarch64-musl interpreter.
set -eux

apk add --no-cache clang bash linux-headers build-base
apk add --no-cache bazel8 \
  --repository=https://dl-cdn.alpinelinux.org/alpine/edge/testing

arch="$(uname -m)"
if [ "$arch" = "aarch64" ]; then
  apk add --no-cache gcompat
  PY_FLAGS=""
else
  PY_FLAGS="--repo_env=RULES_PYTHON_REPO_TOOLCHAIN__LINUX_X86_64=x86_64-unknown-linux-musl \
    --@rules_python//python/config_settings:py_linux_libc=musl"
fi

# Work on a copy: /src is mounted read-only, and Bazel plants bazel-*
# convenience symlinks in the workspace.
cp -r /src /work
cd /work
rm -rf bazel-* dist

# shellcheck disable=SC2086  # PY_FLAGS is deliberately word-split
bazel build -c opt --force_pic \
  --repo_env=CC=/usr/bin/clang \
  --repo_env=CXX=/usr/bin/clang++ \
  $PY_FLAGS \
  //shim:static

cp -L bazel-bin/shim/libgooglesql_shim.a \
      bazel-bin/shim/libgooglesql_shim_alwayslink.a \
      bazel-bin/shim/libicu*.a \
      /out/

{
  echo "triple: ${arch}-linux-musl"
  echo "bazel: $(bazel --version)"
  echo "cc: $(clang --version | head -1)"
  echo "libc: musl $(apk list --installed 2>/dev/null | grep '^musl-[0-9]' || true)"
  echo "os: Alpine $(cat /etc/alpine-release) (container on the runner)"
} > /out/toolchain.txt
