# Native Bazel build of ICU4C, applied to the icu4c source release via the
# http_archive in MODULE.bazel and swapped into GoogleSQL's dependency graph
# with override_repo -- replacing the rules_foreign_cc (autotools)
# configure_make build GoogleSQL uses. Why: autotools is what broke every
# non-Linux leg first (Apple ar/libtool quirks, MSYS path handling on
# Windows), it was the only configure_make target in the whole graph, and
# its sole irreplaceable job -- packaging the ICU *data* statically -- is
# reproduced here by rendering the release's prebuilt icudt76l.dat through
# @//third_party/icu:dat_to_c.py (a re-implementation of genccode's C
# output) and compiling it like any other source.
#
# Target shape mirrors what GoogleSQL references from its own icu.BUILD:
# `:icu` (plus the `:common`/`:headers`/`:unicode` aliases) is the
# compile-against surface -- headers only, deliberately contributing no
# objects, so the shim's cc_static_library bundle stays GoogleSQL-only and
# the ICU code ships as the same four separate archives
# (libicuuc/libicui18n/libicuio/libicudata) the autotools build produced,
# keeping smallquery's build.rs contract byte-compatible in layout.

load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# MSVC parses ICU's u'x' char16_t literals as multi-character constants
# unless the source charset is pinned to UTF-8 (error C2015 in
# static_unicode_sets.h); clang and gcc already assume UTF-8.
_PLATFORM_COPTS = select({
    "@platforms//os:windows": ["/utf-8"],
    "//conditions:default": [],
})

exports_files(["LICENSE"])

# What consumers compile against: the public headers of the three code
# libraries. U_STATIC_IMPLEMENTATION propagates so consumers link the
# static-library ABI (a no-op on ELF/Mach-O, dllimport-correct on Windows).
cc_library(
    name = "icu",
    hdrs = glob([
        "source/common/unicode/*.h",
        "source/i18n/unicode/*.h",
        "source/io/unicode/*.h",
    ]),
    defines = ["U_STATIC_IMPLEMENTATION"],
    includes = [
        "source/common",
        "source/i18n",
        "source/io",
    ],
)

alias(
    name = "common",
    actual = ":icu",
)

alias(
    name = "headers",
    actual = ":icu",
)

alias(
    name = "unicode",
    actual = ":icu",
)

# The components' private headers, shared because i18n and io include
# common's internals (cmemory.h and friends) exactly as the in-tree build
# does with its -I flags.
cc_library(
    name = "internal_headers",
    hdrs = glob([
        "source/common/*.h",
        "source/i18n/*.h",
        "source/io/*.h",
    ]),
    visibility = ["//visibility:private"],
    deps = [":icu"],
)

cc_library(
    name = "icuuc_lib",
    srcs = glob(["source/common/*.cpp"]),
    copts = ["-DU_COMMON_IMPLEMENTATION"] + _PLATFORM_COPTS,
    linkstatic = True,
    deps = [":internal_headers"],
)

cc_library(
    name = "icui18n_lib",
    srcs = glob(["source/i18n/*.cpp"]),
    copts = ["-DU_I18N_IMPLEMENTATION"] + _PLATFORM_COPTS,
    linkstatic = True,
    deps = [":internal_headers"],
)

cc_library(
    name = "icuio_lib",
    srcs = glob(["source/io/*.cpp"]),
    copts = ["-DU_IO_IMPLEMENTATION"] + _PLATFORM_COPTS,
    linkstatic = True,
    deps = [":internal_headers"],
)

# The ICU data, embedded the way `genccode` embeds it for static data
# packaging: the release's prebuilt little-endian icudt76l.dat rendered as
# a C array defining the icudt76_dat entry-point symbol that udata.cpp
# expects to find linked in.
genrule(
    name = "icudt_c",
    srcs = ["source/data/in/icudt76l.dat"],
    outs = ["icudt76_dat.c"],
    cmd = "$(location @//third_party/icu:dat_to_c) $(location source/data/in/icudt76l.dat) icudt76 $@",
    tools = ["@//third_party/icu:dat_to_c"],
)

cc_library(
    name = "icudata_lib",
    srcs = [":icudt_c"],
    # The data object exports one symbol that nothing references at archive
    # scan time in a single-pass link (udata.cpp's reference sits in icuuc,
    # which may already have been scanned), so an in-tree consumer like
    # //shim:smoke_test needs it force-loaded. The shipped libicudata.a is
    # unaffected: cc_static_library packs members regardless, and external
    # consumers link the archive explicitly.
    alwayslink = True,
    linkstatic = True,
    deps = [":icu"],
)

# One flattened archive per component, named so the staging genrules in the
# consuming workspace produce the exact libicu*.a set the autotools build
# published. symbol_check is off as it is for the shim bundle: Alpine's
# Bazel 8 toolchain flags duplicate weak/COMDAT definitions that the same
# archives pass under Bazel 9's validation, and the linker resolves such
# duplicates by ordinary member selection anyway.
cc_static_library(
    name = "icuuc",
    features = ["-symbol_check"],
    deps = [":icuuc_lib"],
)

cc_static_library(
    name = "icui18n",
    features = ["-symbol_check"],
    deps = [":icui18n_lib"],
)

cc_static_library(
    name = "icuio",
    features = ["-symbol_check"],
    deps = [":icuio_lib"],
)

cc_static_library(
    name = "icudata",
    features = ["-symbol_check"],
    deps = [":icudata_lib"],
)
