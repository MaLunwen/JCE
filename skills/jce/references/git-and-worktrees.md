# Git and workspace delivery

Read the actual branch, refs, index and ignore decisions before changing them.
Use git ls-files -- <exact path> and git check-ignore -v <exact path> to verify
tracking. docs/, .docs/ and private/ are local; public charters, skill, contracts,
tests and tools are tracked. Respect the owner's no-commit/no-push instruction.

Back up Git refs before history alignment. Do not copy ignored media or build
outputs unless the user requests a filesystem backup. Preserve unrelated edits
and original third-party bytes. Prefer managed worktrees when isolation is
needed; never delete or re-purpose another active checkout. Verify restored
paths and directory migrations, and avoid reset --hard or forced checkout.

Owned text uses LF, with CRLF checkout for BAT/CMD only. Write explicit newline
bytes, verify the index and include moves in the same pending change. Do not
normalize upstream source. ABI checks that inspect HEAD need a post-commit run
by the person who performs the eventual commit.
