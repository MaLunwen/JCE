# JCE release version: one manually edited source

The only manually edited engine/editor version is the root
`CMakeLists.txt` line `project(JCE VERSION X.Y.Z LANGUAGES C)`.

Two tracked version **mirrors** are required by their consumers:

- `engine/java/com/jce/JceRuntime.java` contains the compile-time Java JNI
  ABI handshake value, calculated as `0xMMmmpp00` (for `0.14.1`, `0x000E0100`).
  Do **not** remove the JNI handshake or derive its expected version from the
  loaded native library: that would defeat mismatch detection.
- The README's Version row is a human-facing copy of the CMake version.

`tools/hooks/pre-commit` invokes `tools/lint/sync_release_mirrors.py --staged`.
It reads the version from the **Git index** and stages only the two version
spans, without staging unrelated unstaged edits to either file. It mirrors
those two spans in the working files as well. It refuses an unstaged change to
CMake's version, unrecognizable file contents, symlinks, and merge-conflicted
index entries instead of overwriting data or guessing. The existing
`tools/hooks/commit-msg` retains its independent staged-version validator.

Enable the tracked hooks in each new clone (local Git config is not cloned):

```
git config --local core.hooksPath tools/hooks
```

In a checkout with the hook enabled:

```
# Edit ONLY the version in CMakeLists.txt.
git add CMakeLists.txt
git commit -m "v-0.14.1, release"
```

For a manual worktree synchronization before linting:

```
python tools/lint/sync_release_mirrors.py --working
```

For an explicit staged-only synchronization (without creating a commit):

```
python tools/lint/sync_release_mirrors.py --staged
python tools/lint/sync_release_mirrors.py --staged --check
```

The packed ABI format is an existing engine contract, not semantic-version
compatibility by itself. CMake also generates `jce_version.h` from the same
project version. CI/build tests do not certify a real native/JNI execution.
