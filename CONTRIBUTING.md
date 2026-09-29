# Contributing to JCE

Read [AGENTS.md](AGENTS.md) for the maintained engineering contract and
[skills/jce/SKILL.md](skills/jce/SKILL.md) for task procedures.

## Before anything else

**Everything here is runnable by you.** The unit suite and the CI configuration
are in the repository; a clean clone builds, tests and gates itself. That was
not true before 2026-09-17 and this file said so; it is true now.

**CI is tiered, and the tiers mean different things.** The `lint` job runs on
every push to every branch and needs no compiler — it is the one to satisfy
before opening a PR, and you can run it locally in seconds. The build-and-test
matrix is on `schedule` / `workflow_dispatch` because that suite had never run
in CI until now: **expect its early runs to be red for reasons unrelated to
your change**, and do not read a red matrix as a verdict on your PR yet.
`.github/workflows/ci.yml` says which tier each job is in.

## Licensing of contributions

JCE is [MPL-2.0](LICENSE). By opening a pull request you are contributing your
changes under the same licence (inbound = outbound); no separate CLA is
required and copyright stays yours.

Practically, MPL-2.0 is file-level: someone shipping a closed-source game on
JCE owes nothing, but a change to a JCE source file stays open. That is the
whole intent — **improvements to the engine come back, games do not have to.**

## The gates

Both must exit `0` with **no `FAIL` line**. This is not negotiable and it is
not graded on a curve: "no worse than before", "≥ N checkers pass" and
"matches the existing baseline" are all explicitly rejected phrasings in this
tree. A `SKIP` is not a `PASS`.

```bash
python tools/lint/run_all.py                    # the lint suite
python tools/audit/run_architecture_audit.py      # ABI snapshot, layering, licences, bindings
```

The second one is **not optional and not implied by the first.** They cover
different things, and "`run_all.py` is green" and "a gate is red" have been
true at the same time in this repository's history.

Two traps worth knowing, both of which have cost real time here:

* **`check_abi_snapshot.py --committed` compares HEAD to HEAD.** If you touch a
  public header, run the architecture audit again *after* committing —
  appending to a struct is compatible, so the working-tree run passes and the
  committed run then fails. Fix with
  `python tools/audit/check_abi_snapshot.py --update` and put the snapshot in
  **the same commit**.
* **The duplication detector enumerates sources with `git ls-files`.** A brand
  new `.c` file is invisible to it until it is tracked, so it also has to be
  re-run after committing.

If you change a public header under `engine/include/`, also run:

```bash
python scripts/jce.py sdk      # reinstall the SDK, or consumers keep the old ABI
python scripts/jce.py smoke    # all seven scripting languages still load
```

## Evidence

This is the part that is different from most repositories, and it is the part
worth keeping.

**A claim about pixels needs pixels. A claim about time needs a noise floor.**
`tools/envshot.py` exists for the first: it rebuilds the shader pak, pins the
camera as data, records the region compared, and refuses a capture whose
subject is not in frame. Use it rather than eyeballing two screenshots.

Three rules that this tree learned the expensive way:

1. **State the noise floor.** Two captures of the *same* build differ. A change
   smaller than that difference is not a result. Every measurement in
   `contracts/engine-parity.json` carries one.
2. **Include a control that would fail.** "It changed 10,930 pixels" means
   nothing on its own; "…and the half of the frame that should not have
   changed moved 0 pixels" is what makes it a measurement.
3. **A gate that only passes has not been tested.** When you add a checker,
   reproduce the defect it is written for and show that it goes red. Several
   checkers here were green for weeks against a tree that already contained
   the thing they were meant to catch.

## House rules for code

| Topic | Rule |
| --- | --- |
| Engine | **C99.** No C++ in `engine/src` outside sanctioned bridge files. Private C++ deps only behind C wrappers — no C++ types, exceptions, RTTI or STL across the public ABI. |
| Editor | **C++20**, ImGui only. No SDL or bgfx calls in panel files. |
| Platform symbols | `_WIN32`, `<windows.h>`, `<unistd.h>` and friends are **forbidden** in first-party source. Go through `engine/src/os/platform`. A lint enforces this. |
| Raw allocation / IO | `malloc`, `free`, `fopen`, `pthread_*` are rejected in first-party code — use the `jce_*` wrappers. "It's only for debugging" is not an exemption. |
| Naming | `jce_<module>_<verb>()` · `JceXxx` types · `JCE_UPPER` macros |
| File size | Soft cap ~2000 lines, hard cap 3000 — enforced by `check_file_size.py`. Over the cap, split at a real seam rather than raising the cap. |
| Line endings | **LF**, everywhere except `.bat`/`.cmd`. Pinned by `.gitattributes`. Note that Python's `write_text()` emits CRLF on Windows unless you pass `newline="\n"`. |
| Reuse first | Search before you write. This tree has had component defaults in four places and world streaming implemented twice; both cost more to merge than they would have to find. |

## Comments

Write down **why**, especially why an obvious-looking alternative is wrong.
Most of the load-bearing comments in this codebase record a measurement or a
defect, not a description of the code — if a comment could be deleted without
losing anything a reader could not re-derive, it probably should be.

Do not write a comment that asserts a number the code does not produce. A
header that names a parameter which does not exist, or a table that counts
something nobody recounts, is the specific failure mode this project spends the
most effort on.

## Pull requests

* One concern per PR. A refactor and a behaviour change in the same diff cannot
  be reviewed.
* The commit message is where the evidence goes: what you measured, against
  what floor, and what control would have failed.
* Say what you did **not** do. "Still behind on X" in a commit message is worth
  more than a claim of completeness that a reader later has to disprove.

## Reporting a bug

Open a GitHub issue. What makes a JCE bug report actionable is the same thing
that makes a commit here actionable:

* **What you ran**, verbatim — the preset, the variant, the command.
* **Which backend**, from the engine's own `renderer:` log line, not from what
  you asked for. `JCE_BACKEND` is a *request*; bgfx falls back, and a capture
  taken on a backend nobody asked for looks exactly like one taken on the right
  one.
* **Your platform and toolchain**, including compiler version. The baseline is
  deliberately low (single-core, 512 MB, integrated GPU) so "it works here"
  covers less ground than usual.
* For anything visual: two captures and **what stayed the same between them**.
  `tools/envshot.py` does this properly and refuses a capture whose subject is
  not in frame.

"I think X is broken" with no reproduction is still welcome — just say that is
what it is, so nobody reads it as a measurement.

## Asking a question

GitHub Discussions if they are enabled on the repository, otherwise an issue
labelled `question`. There is no support obligation attached to any of this;
see the status note in [README.md](README.md).

## Reporting a security issue

Please do not open a public issue for anything exploitable. Use GitHub's
private vulnerability reporting on this repository, or email the maintainer
directly at **xy2017335842@gmail.com** with `[JCE security]` in the subject.

Include what you would put in a bug report plus the impact you believe it has.
Expect a slow reply — this is a single-author project — but you will get one.

## Conduct

Be straightforward and be kind; assume the other person is trying to get
something right. Technical disagreement is welcome and is expected to be about
evidence. Harassment, personal attacks and deliberate bad faith are not, and
the maintainer will remove comments and block accounts over them without
further discussion.

## Maintainer

MaLunwen — xy2017335842@gmail.com

JCE is a single-author project. Pull requests are read, but not quickly, and a
large unsolicited change is more likely to be declined for reasons of
maintenance burden than of quality. **Open an issue before writing anything
substantial**, so the design conversation happens before the work does.
