"""Collects what `cc_static_library` cannot express: `alwayslink` metadata.

`cc_static_library` flattens *object files* into one `.a`, but a `.a` has
nowhere to record `alwayslink`: the bundle *contains* the objects of
`alwayslink = 1` targets, yet the linker's ordinary member selection drops
them (they export no strong global symbol by construction -- that is why
they need `alwayslink`). `cc_alwayslink_objects` packs exactly those objects
into a small side archive that the consumer links with `+whole-archive`, so
the hammer covers kilobytes instead of the whole bundle.

Rather than hardcode target names (which move when GoogleSQL updates), the
rule walks the same linking context `cc_static_library` walks and selects by
the library's own `alwayslink` property -- a second alwayslink target added
upstream is picked up automatically.
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

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
        # The toolchain's own environment for this action: some archivers are
        # wrapper scripts that need it (emsdk's emar.sh resolves the real
        # emar.py through EM_BIN_PATH/EM_CONFIG_PATH/BAZEL_PYTHON_RELPATH).
        # Empty for toolchains that declare none, so native links are
        # unchanged.
        env = cc_common.get_environment_variables(
            feature_configuration = feature_configuration,
            action_name = ACTION_NAMES.cpp_link_static_library,
            variables = cc_common.empty_variables(),
        ),
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
