#!/usr/bin/env python3
"""
migrate_models_to_glb.py — convert a project's source models to binary glTF
(.glb) and rewrite every scene/material/prefab reference from the source
extension to .glb.

Why: the runtime mesh loader is cgltf-only (a raw .obj/.fbx silently fails to
render — same class as the fixed collider path bug).  The canonical pipeline
normalises every model to .glb so the editor (authoring + Play) and the shipped
runtime mount ONE loader and behave identically.  See
.docs/PIPELINE_GLB_BUNDLE_SINGLEEXE_2026-06-17.md.

Conversion uses the engine's own converter via `jce_cook --convert-model`
(assimp import + meshopt + glTF2 'glb2' export), so editor/runtime/bundle all
agree on the bytes.

Default is a DRY RUN: it reports what it would convert/rewrite and changes
nothing.  Pass --apply to write.  Source files are kept by default (re-import
safety); pass --delete-source to remove them after a successful conversion AND
after every reference to them has been rewritten.

Usage:
  python tools/migrate_models_to_glb.py <assets_dir> --cook <jce_cook.exe> \
         [--apply] [--delete-source] [--exts obj,fbx,dae,3ds,ply,stl]

Example (dry run):
  python tools/migrate_models_to_glb.py examples/caged_kingdom/resources/assets \
         --cook build/desktop/windows-x64/tools/jce_cook.exe
"""

import argparse
import os
import re
import subprocess
import sys

DEFAULT_EXTS = ["obj", "fbx", "dae", "3ds", "ply", "stl"]
# Asset files that may carry model references.  Includes the non-".json"
# variants the editor also writes (".scene", ".jbundle", ".prefab") so a
# project that mixes both gets fully migrated.  ".bak" backups are skipped.
JSON_GLOBS = (".scene.json", ".prefab.json", ".mat.json", ".material",
              ".particle.json", ".scene", ".jbundle", ".prefab", ".json")


def norm(p):
    return p.replace("\\", "/").lstrip("./").lower()


def find_models(assets_dir, exts):
    """Return list of (abs_path, rel_path) for source model files."""
    out = []
    extset = {"." + e.lower() for e in exts}
    for root, _dirs, files in os.walk(assets_dir):
        for f in files:
            ext = os.path.splitext(f)[1].lower()
            if ext in extset:
                ap = os.path.join(root, f)
                rp = os.path.relpath(ap, assets_dir).replace("\\", "/")
                out.append((ap, rp))
    return out


def find_json_files(assets_dir):
    out = []
    for root, _dirs, files in os.walk(assets_dir):
        for f in files:
            low = f.lower()
            if low.endswith(JSON_GLOBS):
                out.append(os.path.join(root, f))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("assets_dir", help="project assets root (scenes resolve relative to this)")
    ap.add_argument("--cook", required=True, help="path to jce_cook executable")
    ap.add_argument("--apply", action="store_true", help="write changes (default: dry run)")
    ap.add_argument("--delete-source", action="store_true",
                    help="remove source model files after conversion + ref rewrite")
    ap.add_argument("--recook-colliders", action="store_true",
                    help="re-cook a model's <model>.jcol compound-collider blob for "
                         "the new .glb (and drop the stale source-named sidecar)")
    ap.add_argument("--force", action="store_true",
                    help="re-convert even when the .glb is newer than its source "
                         "(overwrites in place; never deletes — safe for authored "
                         ".glb that have no source)")
    ap.add_argument("--exts", default=",".join(DEFAULT_EXTS),
                    help="comma-separated source model extensions")
    args = ap.parse_args()

    assets_dir = os.path.abspath(args.assets_dir)
    if not os.path.isdir(assets_dir):
        print(f"error: not a directory: {assets_dir}", file=sys.stderr)
        return 2
    # Normalise to a native absolute path so Windows CreateProcess can find it
    # (a forward-slash relative path fails subprocess even when it exists).
    args.cook = os.path.normpath(os.path.abspath(args.cook))
    if args.apply and not os.path.isfile(args.cook):
        print(f"error: --cook not found: {args.cook}", file=sys.stderr)
        return 2

    exts = [e.strip().lstrip(".").lower() for e in args.exts.split(",") if e.strip()]
    ext_alt = "|".join(re.escape(e) for e in exts)
    # Match a quoted path token whose extension is a source model format.
    tok_re = re.compile(r'"([^"\n]*?\.(?:' + ext_alt + r'))"', re.IGNORECASE)

    models = find_models(assets_dir, exts)
    print(f"== scan == {len(models)} source model(s) under {assets_dir}")
    if not models:
        print("nothing to do.")
        return 0

    # --- Phase 1: convert each model to .glb -----------------------------
    converted_rel = set()     # normalized rel paths successfully (or would-be) converted
    failed = []
    n_ok = 0
    n_recook = 0
    for ap_path, rp in models:
        glb_rel = os.path.splitext(rp)[0] + ".glb"
        glb_abs = os.path.join(assets_dir, glb_rel.replace("/", os.sep))
        if args.apply:
            up_to_date = (not args.force and os.path.isfile(glb_abs) and
                          os.path.getmtime(glb_abs) >= os.path.getmtime(ap_path))
            if not up_to_date:
                r = subprocess.run([args.cook, "--convert-model", ap_path, "--out", glb_abs],
                                   capture_output=True, text=True)
                if r.returncode != 0 or not os.path.isfile(glb_abs):
                    failed.append((rp, (r.stderr or r.stdout).strip()[:200]))
                    continue
        n_ok += 1
        converted_rel.add(norm(rp))
        print(f"  convert {rp}  ->  {glb_rel}" + ("" if args.apply else "  (dry)"))

        # Re-cook the compound-collider sidecar for the .glb so the cached blob
        # matches the new model (else the runtime live-cooks from .glb at spawn).
        if args.recook_colliders:
            src_jcol = ap_path + ".jcol"
            if os.path.isfile(src_jcol):
                glb_jcol = glb_abs + ".jcol"
                if args.apply:
                    rc = subprocess.run([args.cook, "--collider", glb_abs, "--out", glb_jcol],
                                        capture_output=True, text=True)
                    if rc.returncode == 0 and os.path.isfile(glb_jcol):
                        n_recook += 1
                        try:
                            os.remove(src_jcol)
                        except OSError:
                            pass
                    else:
                        print(f"    ! collider re-cook failed for {glb_rel}: "
                              f"{(rc.stderr or rc.stdout).strip()[:160]}")
                else:
                    n_recook += 1
                    print(f"    recook collider {glb_rel}.jcol  (dry)")

    if failed:
        print(f"== convert FAILED for {len(failed)} model(s) ==")
        for rp, err in failed:
            print(f"  ! {rp}: {err}")

    # --- Phase 2: rewrite references in JSON assets ----------------------
    json_files = find_json_files(assets_dir)
    total_refs = 0
    changed_files = 0
    unresolved = set()

    def repl_factory(file_dir):
        def repl(m):
            nonlocal total_refs
            token = m.group(1)
            ntok = norm(token)
            base = os.path.splitext(ntok)[0]
            # Accept the rewrite when this token corresponds to a converted
            # model: either its asset-root-relative path was converted, OR the
            # .glb sibling actually exists on disk (resolved a few ways).
            is_conv = ntok in converted_rel
            if not is_conv:
                # try resolve relative to assets root and to the file's dir
                glb_rel = base + ".glb"
                cand_root = os.path.join(assets_dir, glb_rel.replace("/", os.sep))
                cand_local = os.path.join(file_dir, glb_rel.replace("/", os.sep))
                is_conv = os.path.isfile(cand_root) or os.path.isfile(cand_local)
                # in dry-run the .glb does not exist yet; match by basename
                if not is_conv and not args.apply:
                    bn = os.path.basename(base)
                    is_conv = any(os.path.basename(os.path.splitext(c)[0]) == bn
                                  for c in converted_rel)
            if not is_conv:
                unresolved.add(token)
                return m.group(0)
            total_refs += 1
            # preserve original directory/casing, swap extension to .glb
            stem = token[: token.rfind(".")]
            return '"' + stem + '.glb"'
        return repl

    for jf in json_files:
        try:
            with open(jf, "r", encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError):
            continue
        new_text = tok_re.sub(repl_factory(os.path.dirname(jf)), text)
        if new_text != text:
            changed_files += 1
            rel = os.path.relpath(jf, assets_dir).replace("\\", "/")
            n = len(tok_re.findall(text))
            print(f"  rewrite {rel}")
            if args.apply:
                with open(jf, "w", encoding="utf-8") as fh:
                    fh.write(new_text)

    # --- Phase 3: optional source deletion -------------------------------
    deleted = 0
    if args.delete_source and args.apply and not unresolved:
        for ap_path, rp in models:
            if norm(rp) in converted_rel:
                try:
                    os.remove(ap_path)
                    deleted += 1
                except OSError:
                    pass

    print("== summary ==")
    print(f"  models converted : {n_ok}/{len(models)}" + ("" if args.apply else " (dry)"))
    if args.recook_colliders:
        print(f"  colliders recook : {n_recook}" + ("" if args.apply else " (dry)"))
    print(f"  refs rewritten   : {total_refs} across {changed_files} file(s)"
          + ("" if args.apply else " (dry)"))
    if unresolved:
        print(f"  UNRESOLVED model refs (no matching .glb, left as-is): {len(unresolved)}")
        for u in sorted(unresolved)[:20]:
            print(f"    ? {u}")
    if args.delete_source:
        if unresolved and args.apply:
            print("  source NOT deleted (unresolved refs remain — rerun after fixing)")
        else:
            print(f"  sources deleted  : {deleted}" + ("" if args.apply else " (dry)"))
    if not args.apply:
        print("\nDRY RUN — no files changed. Re-run with --apply to write.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
