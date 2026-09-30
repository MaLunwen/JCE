# Git workflow

Preserve the user's branch, staged changes, ignored assets and backup refs. Read status and the requested baseline before changing history. A source export used for verification is generated test output, not a reason to replace the user's checkout.

When the user reserves commits and pushes, leave changes reviewable in the workspace. When explicitly authorized to rewrite a published commit, create a Git backup branch first and record the observed remote object ID.

Use force-with-lease with that expected remote ID for an authorized history update; if the lease fails, inspect the new remote history instead of overriding it. Publish only validated public content and exclude local/private delivery files.

Check staged whitespace, real index paths, effective attributes and ignore decisions. Keep unavailable verification separate from PASS and report the exact source/build identity of a delivered artifact.

For a `v-X.Y.Z` release commit, update `project(JCE VERSION ...)` in CMakeLists.txt, Java's `EXPECTED_API_VERSION` mirror and the README Version row together. Run `python scripts/jce.py lint` before committing. CI checks the committed subject against CMake; the tracked `tools/hooks/commit-msg` guard checks staged files locally when `git config core.hooksPath tools/hooks` has been set. A version change requires a matching release subject, and a mismatched release subject is rejected.

Use managed worktree lifecycle tools for user-requested isolated checkouts and cleanup. Do not remove a worktree or ignored project content without accounting for ongoing work and retained assets.
