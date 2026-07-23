#!/usr/bin/env bash
# Fetches the GoogleSQL patches this repo applies on top of the pinned
# google/googlesql commit and checks them into patches/googlesql/.
#
# GoogleSQL is pinned in MODULE.bazel to an exact google/googlesql commit
# (upstream, not a fork). The build then applies the patches below via the
# archive_override's `patches` attribute -- so the fixes smallquery needs live
# here as reviewable diffs against upstream instead of as a fork we have to
# rebase. Each patch is an open pull request against fornwall/googlesql; this
# script snapshots the current .patch for each so the checked-in copy is the
# exact bytes Bazel applies.
#
# Re-run this whenever a PR is updated, or edit PRS below to add/remove one.
# Keep the `patches = [...]` list in MODULE.bazel in sync with the files this
# writes (and mind the order: patches apply top to bottom).
set -euo pipefail

# Pull request numbers on fornwall/googlesql, applied in this order.
PRS=(1 3 4 5)

REPO=fornwall/googlesql
OUT_DIR="$(cd "$(dirname "$0")" && pwd)/patches/googlesql"

mkdir -p "$OUT_DIR"

for n in "${PRS[@]}"; do
  url="https://patch-diff.githubusercontent.com/raw/${REPO}/pull/${n}.patch"
  dest="$OUT_DIR/pull-${n}.patch"
  echo "Fetching $url -> ${dest#"$PWD"/}"
  curl --fail --silent --show-error --location "$url" -o "$dest"
done

echo "Done. ${#PRS[@]} patch(es) written to ${OUT_DIR#"$PWD"/}."
