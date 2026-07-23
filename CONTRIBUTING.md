# Contributing to googlesql-shim

This repository holds the build pipeline for prebuilt static GoogleSQL shim
archives consumed by [smallquery](https://github.com/fornwall/smallquery).
Changes to the shim's *behaviour* usually belong in smallquery (whose
`crates/googlesql-sys/shim` these sources originate from). Changes to GoogleSQL
itself go upstream as pull requests against
[fornwall/googlesql](https://github.com/fornwall/googlesql) and are applied to
the pinned `google/googlesql` commit as the patches in `patches/googlesql/`
(refresh them with `create-patches.sh`, and keep the `patches = [...]` list in
`MODULE.bazel` in sync). Changes to how the archives are built, packaged and
released belong here.

## Checks

CI runs actionlint, buildifier, a pinned clang-format
(`pip install clang-format==19.1.7`, config in `.clang-format`) and
shellcheck on every push to main and every pull request. To run them locally:

```sh
actionlint
buildifier -mode=check -lint=warn -r .
clang-format --dry-run --Werror shim/shim.cc shim/shim.h
git ls-files '*.sh' | xargs shellcheck
```

## Developer Certificate of Origin

This project uses the
[Developer Certificate of Origin 1.1](https://developercertificate.org) (DCO)
to certify that contributors have the right to submit their contributions
under the project's license, [Apache-2.0](LICENSE.txt).

Every commit must contain a sign-off line with your real name and email
address:

```
Signed-off-by: Jane Smith <jane.smith@example.com>
```

`git commit -s` adds it for you, using your `user.name` and `user.email` git
configuration. By adding this line you certify the DCO — that one of the
following holds:

- you created the contribution and have the right to submit it under the
  project's open source license; or
- it is based on previous work covered by an appropriate open source license
  and you have the right to submit it, with or without modifications, under
  the same license; or
- it was provided to you by someone else who certified one of the above, and
  you have not modified it;

and that you understand the contribution and sign-off are public and will be
kept permanently with the project's history.

As in the
[Linux kernel's coding-assistants guidance](https://docs.kernel.org/process/coding-assistants.html),
only humans certify the DCO: an AI tool must be neither a commit's author nor
a `Signed-off-by` signer. Credit assistance with a trailer instead
(`Co-Authored-By:` or the kernel's `Assisted-by:`) — CI rejects commits
authored or signed off by known AI-tool identities.

CI checks every pull-request commit for a matching sign-off; a PR with an
unsigned commit fails the check. If you forget, `git commit --amend -s` (or
`git rebase --signoff main`) and force-push the branch.
