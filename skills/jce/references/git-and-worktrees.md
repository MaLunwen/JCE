# Git workflow

Preserve the user's branch, staged changes, ignored assets and backup refs. Read status and the requested baseline before changing history. A source export used for verification is generated test output, not a reason to replace the user's checkout.

When the user reserves commits and pushes, leave changes reviewable in the workspace. When explicitly authorized to rewrite a published commit, create a Git backup branch first and record the observed remote object ID.

Use force-with-lease with that expected remote ID for an authorized history update; if the lease fails, inspect the new remote history instead of overriding it. Publish only validated public content and exclude local/private delivery files.

Check staged whitespace, real index paths, effective attributes and ignore decisions. Keep unavailable verification separate from PASS and report the exact source/build identity of a delivered artifact.

Use managed worktree lifecycle tools for user-requested isolated checkouts and cleanup. Do not remove a worktree or ignored project content without accounting for ongoing work and retained assets.
