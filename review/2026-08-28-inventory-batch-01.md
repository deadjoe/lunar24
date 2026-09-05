# Codex inventory audit — batch 01

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b` (`feat/p0-full-registry`)

Scope: repository state, fresh local build/test gates, hosted CI, and whether the declared macOS/Windows standalone is actually compiled. No product code was changed.

Verification baseline: clean worktree; fresh Debug, Release, and ASan/UBSan builds; CTest 45/45 in all three configurations; generator double-run produced zero diff. The exact audit head is two commits ahead of the remote feature branch and has no hosted CI run.

| ID | Severity | Finding | Local evidence | GitHub |
|---|---|---|---|---|
| A08 | High | Feature CI has failed for 15 consecutive pushes and the current review head has no hosted run | `review/issues/08-feature-ci-red-no-exact-head-run.md` | [#9](https://github.com/deadjoe/lunar24/issues/9) |
| A09 | High | CI silently omits the product standalone host and the declared pinned iPlug2 submodule does not exist | `review/issues/09-product-host-not-built-in-ci.md` | [#10](https://github.com/deadjoe/lunar24/issues/10) |

Existing issues #2–#8 remain open and require exact-head revalidation before closure. The 12 classified keyboard gaps are declared structural dispositions, not ordinary missing P5 controls.
