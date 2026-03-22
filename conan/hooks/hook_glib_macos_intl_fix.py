"""
Conan 2 hook: fix glib/2.85.x build on macOS arm64 when cross-building.

glib's meson.build uses:
    dependency('intl', allow_fallback: true)
inside an else branch. Conan's Meson invocation may disable fallback
subprojects, which turns this into a hard error:
    ERROR: Dependency 'intl' is required but not found.

Fix: make the intl fallback optional in that branch, and continue when
not found.
"""

import os


def pre_build(conanfile):
    if conanfile.name != "glib":
        return

    if str(conanfile.settings.get_safe("os")) != "Macos":
        return

    if str(conanfile.settings.get_safe("arch")) not in ("armv8", "armv8.3"):
        return

    meson_file = os.path.join(conanfile.source_folder, "meson.build")
    if not os.path.exists(meson_file):
        conanfile.output.warning("[glib_macos_intl_fix hook] meson.build not found")
        return

    with open(meson_file, "r") as f:
        content = f.read()

    fallback_old_block = (
        "else\n"
        "  # using proxy-libintl fallback\n"
        "  libintl = dependency('intl', allow_fallback: true)\n"
        "  assert(libintl.type_name() == 'internal')\n"
        "  libintl_deps = [libintl]\n"
        "  have_bind_textdomain_codeset = true  # proxy-libintl supports it\n"
        "endif\n"
    )

    fallback_new_block = (
        "else\n"
        "  # [glib_macos_intl_fix] Conan often builds Meson with fallback disabled.\n"
        "  # Make intl optional here instead of failing hard on dependency('intl').\n"
        "  libintl_optional = dependency('intl', required : false, allow_fallback: true)\n"
        "  if libintl_optional.found()\n"
        "    libintl = libintl_optional\n"
        "    libintl_deps = [libintl]\n"
        "    have_bind_textdomain_codeset = true  # proxy-libintl supports it\n"
        "  else\n"
        "    libintl = disabler()\n"
        "    libintl_deps = []\n"
        "    have_bind_textdomain_codeset = false\n"
        "  endif\n"
        "endif\n"
    )

    keep_dep_old_block = (
        "      else\n"
        "        libintl = disabler()\n"
        "      endif\n"
    )

    keep_dep_new_block = (
        "      else\n"
        "        # [glib_macos_intl_fix] Keep include/link flags from libgettext\n"
        "        # in cross-builds where ngettext probe can give false negatives.\n"
        "        libintl_deps += [libintl]\n"
        "      endif\n"
    )

    patched = False
    if fallback_old_block in content:
        content = content.replace(fallback_old_block, fallback_new_block)
        patched = True

    if keep_dep_old_block in content:
        content = content.replace(keep_dep_old_block, keep_dep_new_block)
        patched = True

    if not patched:
        conanfile.output.info("[glib_macos_intl_fix hook] nothing to patch")
        return

    with open(meson_file, "w") as f:
        f.write(content)

    conanfile.output.info(
        "[glib_macos_intl_fix hook] patched meson intl fallback for macOS arm64"
    )
