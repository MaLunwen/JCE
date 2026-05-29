/*
 * jce_msvc_stl_shims.cpp  Backports of MSVC STL vectorised helpers
 *                         missing from older toolchain CRTs.
 *
 * Background
 * ----------
 * abseil (and other prebuilt libs) shipped via Conan are typically
 * compiled with MSVC toolset 19.40 / 14.40+, whose STL emits external
 * calls to vectorised search helpers declared in <__msvc_string_view.hpp>
 * (`__std_find_*_trivial_*`).  These symbols are satisfied at link
 * time by `msvcprt.lib` (the import lib for `msvcp140.dll`).
 *
 * However, MSVC 14.43.34808 — and possibly other intermediate
 * toolset releases — ship `__std_find_last_of_trivial_pos_1/2` but
 * forget to ship the symmetric `__std_find_first_of_trivial_pos_1`.
 * Code paths that template-instantiate the "find_first_of returns
 * position" form (e.g. absl::str_split internals) then fail to link
 * with:
 *
 *     LNK2019: unresolved external symbol
 *     __std_find_first_of_trivial_pos_1
 *
 * This TU provides a portable fallback compiled into the SDK's deps
 * fat lib.  Because deps is linked *normally* (not /WHOLEARCHIVE'd),
 * the linker only pulls this obj when the symbol is genuinely
 * unresolved — so once MSVC adds the export, the CRT wins and our
 * shim is silently skipped.  No duplicate-symbol risk.
 *
 * Layer: OS / Platform.  Windows + MSVC only.
 */

#if defined(_WIN32) && defined(_MSC_VER)

#include <cstddef>
#include <cstdint>

extern "C" {

/* Mirrors the MS STL declaration in <__msvc_string_view.hpp>.  Returns
 * the index of the first byte in [Haystack, Haystack+Haystack_length)
 * that matches any byte in [Needle, Needle+Needle_length).  Returns
 * Haystack_length if no match.
 *
 * O(N + 256) byte-bitmap implementation — vectorisation is not the
 * point; correctness + portability is.  Linker only pulls this when
 * the real intrinsic is missing from the toolset's msvcprt.lib.
 */
__declspec(noalias) std::size_t __stdcall __std_find_first_of_trivial_pos_1(
    const void *haystack,
    std::size_t haystack_length,
    const void *needle,
    std::size_t needle_length) noexcept
{
    if (haystack_length == 0 || needle_length == 0) {
        return haystack_length;
    }

    const std::uint8_t *h = static_cast<const std::uint8_t *>(haystack);
    const std::uint8_t *n = static_cast<const std::uint8_t *>(needle);

    bool table[256] = {};
    for (std::size_t i = 0; i < needle_length; ++i) {
        table[n[i]] = true;
    }

    for (std::size_t i = 0; i < haystack_length; ++i) {
        if (table[h[i]]) {
            return i;
        }
    }
    return haystack_length;
}

} /* extern "C" */

#endif /* _WIN32 && _MSC_VER */
