"""Collects what `cc_static_library` cannot express: foreign archives and
`alwayslink` metadata.

`cc_static_library` flattens *object files* into one `.a`. Two things fall
through that model, each with its own rule here:

- `cc_link_extras`: a dependency built outside Bazel's C++ rules -- here ICU,
  via `rules_foreign_cc`'s `configure_make` -- contributes a finished `.a` and
  no objects, so it is left out of the bundle and only named in the rule's
  `linkdeps` output group. This rule stages those archives beside the bundle.

- `cc_alwayslink_objects`: a `.a` has nowhere to record `alwayslink`, so the
  bundle *contains* the objects of `alwayslink = 1` targets but the linker's
  ordinary member selection drops them (they export no strong global symbol
  by construction -- that is why they need `alwayslink`). This rule packs
  exactly those objects into a small side archive that `build.rs` links with
  `+whole-archive`, so the hammer covers kilobytes instead of the whole
  gigabyte bundle. See crates/googlesql-sys/CLAUDE.md, "The alwayslink
  hazard".

Rather than hardcode target names or repo layouts (both of which move when
the submodule updates), both rules walk the same linking context
`cc_static_library` walks and select by the library's own properties -- a
foreign dependency or a second `alwayslink` target added upstream is picked
up automatically.
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _cc_link_extras_impl(ctx):
    outs = []
    seen = {}
    for dep in ctx.attr.deps:
        for linker_input in dep[CcInfo].linking_context.linker_inputs.to_list():
            for lib in linker_input.libraries:
                # Contributed objects, so it is already inside the bundle.
                # Checked via the public `objects`/`pic_objects` fields rather
                # than the private `_contains_objects`, which only exists in
                # Bazel 9's Starlark LibraryToLink -- the musl release builds
                # under Alpine's musl-native Bazel 8, whose native
                # LibraryToLink has no such field.
                if getattr(lib, "pic_objects", None) or getattr(lib, "objects", None):
                    continue
                archive = lib.pic_static_library or lib.static_library
                if archive == None or archive.basename in seen:
                    continue
                seen[archive.basename] = True
                out = ctx.actions.declare_file(archive.basename)
                ctx.actions.symlink(output = out, target_file = archive)
                outs.append(out)

    return [DefaultInfo(files = depset(outs))]

cc_link_extras = rule(
    implementation = _cc_link_extras_impl,
    doc = "Stages the static libraries of `deps` that provide no object files.",
    attrs = {
        "deps": attr.label_list(providers = [CcInfo], mandatory = True),
    },
)

def _cc_alwayslink_objects_impl(ctx):
    objects = []
    seen = {}
    for dep in ctx.attr.deps:
        for linker_input in dep[CcInfo].linking_context.linker_inputs.to_list():
            for lib in linker_input.libraries:
                # `alwayslink` is a public, documented field of LibraryToLink.
                # Selecting on it (rather than naming the one target known to
                # set it) means a second alwayslink target added upstream is
                # collected by construction.
                if not lib.alwayslink:
                    continue

                # Prefer pic objects, matching what `cc_static_library`
                # merges into the bundle -- the side archive must carry the
                # same flavour the rest of the link uses.
                for obj in lib.pic_objects or lib.objects:
                    if obj.path in seen:
                        continue
                    seen[obj.path] = True
                    objects.append(obj)

    if not objects:
        # Only reachable if upstream drops its last alwayslink target -- an
        # empty archive would then be correct, but the far likelier cause is
        # a Bazel API change that stopped this rule seeing the libraries at
        # all. Fail loudly; the human can delete the rule if alwayslink is
        # truly gone (and the canary test with it).
        fail("no alwayslink libraries found in the linking context; " +
             "expected at least @googlesql//googlesql/public/types:type_with_catalog_impl")

    out = ctx.actions.declare_file("lib" + ctx.label.name + ".a")

    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    archiver = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_static_library,
    )

    args = ctx.actions.args()
    if archiver.endswith("libtool"):
        # Apple's libtool (what the Xcode toolchain registers for the
        # static-library action) takes -static -o <out>, not ar-style
        # member commands.
        args.add("-static")
        args.add("-D")  # deterministic
        args.add("-o", out)
    else:
        args.add("rcsD")  # replace, create, index, deterministic
        args.add(out)
    args.add_all(objects)
    ctx.actions.run(
        executable = archiver,
        arguments = [args],
        inputs = depset(objects, transitive = [cc_toolchain.all_files]),
        outputs = [out],
        mnemonic = "AlwayslinkArchive",
        progress_message = "Archiving alwayslink objects into %{output}",
    )

    return [DefaultInfo(files = depset([out]))]

cc_alwayslink_objects = rule(
    implementation = _cc_alwayslink_objects_impl,
    doc = """Packs the objects of every `alwayslink = 1` library in the deps'
linking context into one `lib<name>.a`, for a consumer outside Bazel to link
with `--whole-archive`.""",
    attrs = {
        "deps": attr.label_list(providers = [CcInfo], mandatory = True),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
