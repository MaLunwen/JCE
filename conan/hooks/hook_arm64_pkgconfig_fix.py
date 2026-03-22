"""
Conan 2 hook: fix various ARM64 cross-compilation issues.

1. pkg-config path fix: When cross-compiling for aarch64 on an x86_64 host,
   Conan's system package recipes (xorg/system, etc.) detect the host's x86_64
   library paths and write them into the generated .pc files. This causes linker
   errors like:
       /usr/lib/x86_64-linux-gnu/libX11-xcb.so: error adding symbols: file in wrong format
   Fix: replace x86_64-linux-gnu with aarch64-linux-gnu in all .pc files.

2. libsndio configure fix: libsndio uses a custom (non-autoconf) configure script
   that does not accept standard --host/--build/--target flags. Conan's
   AutotoolsToolchain adds these when cross-compiling, causing the configure to
   print usage and exit 1.
   Fix: patch the configure script to ignore unknown arguments.
"""

import os
import glob


def pre_build(conanfile):
    if str(conanfile.settings.get_safe("arch")) not in ("armv8", "armv8.3"):
        return

    _fix_pkgconfig_paths(conanfile)

    if conanfile.name == "libsndio":
        _fix_sndio_configure(conanfile)


def _fix_pkgconfig_paths(conanfile):
    """Replace x86_64-linux-gnu with aarch64-linux-gnu in all .pc files."""
    pc_dirs = []
    if hasattr(conanfile, "generators_folder") and conanfile.generators_folder:
        pc_dirs.append(conanfile.generators_folder)
    if hasattr(conanfile, "build_folder") and conanfile.build_folder:
        for root, dirs, files in os.walk(conanfile.build_folder):
            if any(f.endswith(".pc") for f in files):
                pc_dirs.append(root)
            if root.count(os.sep) - conanfile.build_folder.count(os.sep) > 3:
                dirs.clear()

    patched_count = 0
    for pc_dir in set(pc_dirs):
        for pc_file in glob.glob(os.path.join(pc_dir, "*.pc")):
            try:
                with open(pc_file, "r") as f:
                    content = f.read()
                if "x86_64-linux-gnu" in content:
                    new_content = content.replace(
                        "x86_64-linux-gnu", "aarch64-linux-gnu"
                    )
                    with open(pc_file, "w") as f:
                        f.write(new_content)
                    patched_count += 1
            except (IOError, OSError):
                pass

    if patched_count > 0:
        conanfile.output.info(
            "[arm64_pkgconfig_fix hook] patched %d .pc files: "
            "x86_64-linux-gnu -> aarch64-linux-gnu" % patched_count
        )


def _fix_sndio_configure(conanfile):
    """Patch libsndio's custom configure script to ignore unknown arguments.

    libsndio uses a hand-written configure script (not autoconf) that only
    accepts a limited set of flags.  When cross-compiling, Conan's
    AutotoolsToolchain adds --host=... and --build=... which cause the
    script to print its usage text and exit 1.

    We change the catch-all  *)  case from 'help; exit 1' to a warning
    that simply skips the unrecognised flag.
    """
    configure_file = os.path.join(conanfile.source_folder, "configure")
    if not os.path.exists(configure_file):
        conanfile.output.warning("[arm64_sndio_fix hook] configure not found")
        return

    with open(configure_file, "r") as f:
        content = f.read()

    # The original catch-all in the case statement is:
    #   *)
    #       help
    #       exit 1
    #       ;;
    # Replace it so unknown flags are silently skipped.
    old_block = '\t*)\n\t\thelp\n\t\texit 1\n\t\t;;'
    new_block = (
        '\t*)\n'
        '\t\techo "WARNING: ignoring unknown option: $i"\n'
        '\t\tshift;;\n'
    )

    if old_block not in content:
        # Try alternate indentation (spaces instead of tabs)
        old_block_spaces = old_block.replace('\t', '    ')
        if old_block_spaces in content:
            old_block = old_block_spaces
            new_block = new_block.replace('\t', '    ')
        else:
            conanfile.output.warning(
                "[arm64_sndio_fix hook] could not find catch-all block in configure"
            )
            return

    content = content.replace(old_block, new_block)

    with open(configure_file, "w") as f:
        f.write(content)

    conanfile.output.info(
        "[arm64_sndio_fix hook] patched configure to ignore unknown flags "
        "(--host, --build, etc.)"
    )
