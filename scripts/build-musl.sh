#!/bin/sh
# Builds //shim:static natively on musl, inside an Alpine container.
#
# Invoked (by the release workflow, or locally) as:
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out alpine:3.23 \
#     sh /src/scripts/build-musl.sh
#
# Why this shape: upstream Bazel ships glibc-only binaries, so bazelisk is a
# dead end on musl. Alpine's own `bazel8` package is a musl-native Bazel 8,
# which builds this module. The base image is pinned to the current stable
# Alpine so the musl the archives are built against does not drift; bazel8
# is the one package that must come from edge/testing (no stable branch
# carries a bazel), pinned per-command via --repository so the rest of the
# install resolves against stable only.
#
# GoogleSQL's code generators run under rules_python's hermetic CPython.
# The RULES_PYTHON_REPO_TOOLCHAIN_* overrides below select the *musl*
# python-build-standalone interpreter for the host -- without them
# rules_python picks the glibc build, which does not run here (that
# selection env var is also broken upstream; patches/
# rules_python-host-preference.patch fixes it). The double underscore is
# real: the version-agnostic host repo formats the variable name with an
# empty version segment.
set -eux

apk add --no-cache clang make pkgconf python3 zstd bash linux-headers \
  build-base
apk add --no-cache bazel8 \
  --repository=https://dl-cdn.alpinelinux.org/alpine/edge/testing

# Work on a copy: /src is mounted read-only, and Bazel plants bazel-*
# convenience symlinks in the workspace.
cp -r /src /work
cd /work
rm -rf bazel-* dist

bazel build -c opt --force_pic \
  --repo_env=CC=/usr/bin/clang \
  --repo_env=CXX=/usr/bin/clang++ \
  --repo_env=RULES_PYTHON_REPO_TOOLCHAIN__LINUX_X86_64=x86_64-unknown-linux-musl \
  --repo_env=RULES_PYTHON_REPO_TOOLCHAIN__LINUX_AARCH64=aarch64-unknown-linux-musl \
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
