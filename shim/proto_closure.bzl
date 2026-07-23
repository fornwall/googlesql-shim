"""Collects a proto_library's transitive `.proto` sources, laid out by import path.

smallquery's build.rs generates Rust types from
`googlesql/local_service/local_service.proto` with prost, which needs every
transitively imported `.proto` on disk under its import path -- including the
three upstream generates from `.template` files during the Bazel build
(`parse_tree.proto`, `resolved_ast.proto`, `resolved_node_kind.proto`), which
exist in no source tree anywhere. This rule materializes the whole closure as
one directory tree, shipped as `protos/` in every release tarball, so a
consumer can run protoc against it with no other include path.

Rather than hardcode file lists (which move when GoogleSQL updates), the rule
walks `ProtoInfo.transitive_sources` and strips each file's proto source root
(`ProtoInfo.transitive_proto_path`), so a proto added upstream is picked up by
construction -- the same shape as `cc_alwayslink_objects` next door.
"""

load("@com_google_protobuf//bazel/common:proto_info.bzl", "ProtoInfo")

def _import_path(f, roots):
    # Longest root first, so a generated file under
    # `bazel-out/.../external/googlesql+` is not claimed by the plain
    # `external/googlesql+` root, and `.` (which matches everything) loses to
    # every real root.
    for root in roots:
        if root == ".":
            continue
        prefix = root if root.endswith("/") else root + "/"
        if f.path.startswith(prefix):
            return f.path[len(prefix):]
    return f.path

def _proto_closure_impl(ctx):
    info = ctx.attr.proto[ProtoInfo]
    roots = sorted(info.transitive_proto_path.to_list(), key = len, reverse = True)

    # Import path -> File; first claimant wins. Two roots yielding the same
    # import path would be the same declaration reached two ways, and protoc
    # would have rejected a genuinely conflicting pair already.
    by_import = {}
    for f in info.transitive_sources.to_list():
        rel = _import_path(f, roots)
        if rel not in by_import:
            by_import[rel] = f

    if "googlesql/local_service/local_service.proto" not in by_import:
        fail("proto closure is missing local_service.proto itself; " +
             "the root-stripping in _import_path no longer matches " +
             "ProtoInfo's layout")

    out = ctx.actions.declare_directory(ctx.label.name)
    manifest = ctx.actions.declare_file(ctx.label.name + "_manifest.txt")
    ctx.actions.write(
        manifest,
        "".join([
            "%s\t%s\n" % (by_import[rel].path, rel)
            for rel in sorted(by_import.keys())
        ]),
    )
    ctx.actions.run_shell(
        inputs = depset([manifest], transitive = [info.transitive_sources]),
        outputs = [out],
        command = """
set -eu
while IFS="$(printf '\\t')" read -r src rel; do
  mkdir -p "$1/$(dirname "$rel")"
  cp "$src" "$1/$rel"
done < "$2"
""",
        arguments = [out.path, manifest.path],
        mnemonic = "ProtoClosure",
        progress_message = "Collecting proto closure into %{output}",
    )

    return [DefaultInfo(files = depset([out]))]

proto_closure = rule(
    implementation = _proto_closure_impl,
    doc = """Materializes the transitive `.proto` sources of a proto_library
as one directory tree keyed by import path, for a consumer outside Bazel to
run protoc against.""",
    attrs = {
        "proto": attr.label(providers = [ProtoInfo], mandatory = True),
    },
)
