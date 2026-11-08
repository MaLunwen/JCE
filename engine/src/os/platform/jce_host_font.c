/* jce_host_font.c -- one installed-font resolver for engine and editor. */
#include <jce/os/platform/jce_host_font.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <CoreText/CoreText.h>
#endif

static bool host_font_copy(char *dst, size_t cap, const char *path)
{
    size_t length = strlen(path);
    if (length + 1 > cap || !jce_fs_host_exists_file(path)) return false;
    memcpy(dst, path, length + 1);
    return true;
}

static bool host_font_windows_directory(char *out, size_t cap)
{
#if defined(_WIN32)
    wchar_t wide[1024];
    DWORD count = GetWindowsDirectoryW(wide, 1024);
    if (!count || count >= 1024 || cap > 0x7fffffffu) return false;
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap,
                              NULL, NULL) > 0;
#else
    const char *dir = getenv("WINDIR");
    if (!dir || !dir[0]) dir = getenv("SystemRoot");
    if (!dir || !dir[0] || strlen(dir) + 1 > cap) return false;
    memcpy(out, dir, strlen(dir) + 1);
    return true;
#endif
}

/* Comma-separated Fontconfig aliases must contain an exact requested name. */
static bool host_font_name_matches(const char *names, const char *expected)
{
    size_t wanted = strlen(expected);
    if (!names) return false;
    for (const char *p = names; *p;) {
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) --end;
        if ((size_t)(end - p) == wanted) {
            size_t i;
            for (i = 0; i < wanted; ++i) {
                unsigned a = (unsigned char)p[i], b = (unsigned char)expected[i];
                if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
                if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
                if (a != b) break;
            }
            if (i == wanted) return true;
        }
        p = strchr(p, ',');
        if (!p) break;
        ++p;
    }
    return false;
}

static void host_font_match_warning(const char *family, const char *style,
                                    const char *reason, const char *resolved,
                                    const char *resolved_style, bool quiet)
{
    if (family && !quiet)
        LOG_WARN("host-font", "installed font rejected: platform=%s requested=%s style=%s reason=%s resolved=%s resolved_style=%s",
                 jce_platform_name(), family, style ? style : "",
                 reason, resolved ? resolved : "", resolved_style ? resolved_style : "");
}

/* CJK keeps its language admission. Print additionally requires the chosen
 * face's actual family and style: fc-match success alone permits substitution. */
static bool host_font_fontconfig(const char *pattern, bool cjk,
                                 const char *family, const char *style, bool quiet,
                                 char *dst, size_t cap)
{
    char args[256], output[4096];
    JceProcessConfig config = {0};
    JceProcess *process;
    size_t length = 0;
    int code = -1;
    uint64_t start;
    if (strcmp(jce_platform_name(), "Linux")) return false;
    if (snprintf(args, sizeof args, "-f \"%%{file}\\n%%{lang}\\n%%{family}\\n%%{style}\" \"%s\"", pattern)
        >= (int)sizeof args) return false;
    config.executable_path = "fc-match";
    config.arguments = args;
    config.capture_stdout = true;
    process = jce_process_spawn(&config);
    if (!process) {
        host_font_match_warning(family, style, "fontconfig-spawn", NULL, NULL, quiet);
        return false;
    }
    start = jce_time_ticks_ms();
    for (;;) {
        if (length < sizeof output - 1)
            length += jce_process_read_stdout(process, output + length,
                                              sizeof output - 1 - length);
        if (jce_process_poll_exit(process, &code)) break;
        if (jce_time_ticks_ms() - start >= 1000) {
            jce_process_force_kill(process);
            jce_process_destroy(process);
            host_font_match_warning(family, style, "fontconfig-timeout", NULL, NULL, quiet);
            return false;
        }
        jce_thread_sleep_ms(2);
    }
    while (length < sizeof output - 1) {
        size_t n = jce_process_read_stdout(process, output + length,
                                          sizeof output - 1 - length);
        if (!n) break;
        length += n;
    }
    jce_process_destroy(process);
    output[length] = '\0';
    if (code != 0 || length >= sizeof output - 1 || memchr(output, '\0', length)) {
        host_font_match_warning(family, style, "fontconfig-output", NULL, NULL, quiet);
        return false;
    }
    char *languages = strchr(output, '\n');
    if (languages) *languages++ = '\0';
    char *resolved_family = languages ? strchr(languages, '\n') : NULL;
    if (resolved_family) *resolved_family++ = '\0';
    char *resolved_style = resolved_family ? strchr(resolved_family, '\n') : NULL;
    if (resolved_style) *resolved_style++ = '\0';
    if (family && (!host_font_name_matches(resolved_family, family) ||
                   !host_font_name_matches(resolved_style, style))) {
        host_font_match_warning(family, style, "fontconfig-substitution", resolved_family,
                                resolved_style, quiet);
        return false;
    }
    if (cjk) {
        bool supported = false;
        if (!languages) return false;
        for (const char *p = languages; *p; ++p) {
            if ((p == languages || p[-1] == '|') &&
                ((!strncmp(p, "zh", 2) && (p[2] == '-' || p[2] == '|' || !p[2])) ||
                 (!strncmp(p, "ja", 2) && (p[2] == '|' || !p[2])) ||
                 (!strncmp(p, "ko", 2) && (p[2] == '|' || !p[2])))) {
                supported = true;
                break;
            }
        }
        if (!supported) return false;
    }
    if (host_font_copy(dst, cap, output)) return true;
    host_font_match_warning(family, style, "file-or-capacity", NULL, NULL, quiet);
    return false;
}

static bool host_font_probe_stem(const char *stem, char *dst, size_t cap)
{
    static const char *extensions[] = { ".ttf", ".ttc", ".otf", NULL };
    static const char *apple_dirs[] = { "/System/Library/Fonts/Supplemental",
        "/System/Library/Fonts", "/Library/Fonts", NULL };
    static const char *linux_dirs[] = { "/usr/share/fonts/opentype/noto",
        "/usr/share/fonts/truetype/noto", "/usr/share/fonts/truetype/wqy",
        "/usr/share/fonts/truetype/droid", "/usr/share/fonts/truetype/arphic",
        "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/truetype/liberation",
        "/usr/share/fonts/TTF", "/usr/share/fonts/truetype", "/usr/share/fonts/opentype",
        "/usr/share/fonts", "/usr/local/share/fonts", NULL };
    const char *platform = jce_platform_name();
    const char *const *directories;
    char windows[1024], candidate[1536];
    if (!strcmp(platform, "Windows")) {
        if (!host_font_windows_directory(windows, sizeof windows)) return false;
        for (int e = 0; extensions[e]; ++e) {
            int n = snprintf(candidate, sizeof candidate, "%s/Fonts/%s%s",
                              windows, stem, extensions[e]);
            if (n > 0 && n < (int)sizeof candidate && host_font_copy(dst, cap, candidate))
                return true;
        }
        return false;
    }
    if (!strcmp(platform, "macOS")) directories = apple_dirs;
    else if (!strcmp(platform, "Linux")) directories = linux_dirs;
    else return false;
    for (int i = 0; directories[i]; ++i)
        for (int e = 0; extensions[e]; ++e) {
            int n = snprintf(candidate, sizeof candidate, "%s/%s%s",
                              directories[i], stem, extensions[e]);
            if (n > 0 && n < (int)sizeof candidate && host_font_copy(dst, cap, candidate))
                return true;
        }
    return false;
}

#if defined(__APPLE__)
/* Consult registered PostScript names before matching; never activate/download
 * a missing face. All copied CoreText/CoreFoundation objects stay local. */
static bool host_font_apple_face(CFStringRef expected, CFStringRef expected_family,
                                  bool bold, char *dst, size_t cap)
{
    CFArrayRef installed = CTFontManagerCopyAvailablePostScriptNames();
    bool available = false, success = false;
    if (!installed) return false;
    for (CFIndex i = 0, n = CFArrayGetCount(installed); i < n; ++i) {
        CFTypeRef name = CFArrayGetValueAtIndex(installed, i);
        if (name && CFGetTypeID(name) == CFStringGetTypeID() && CFEqual(name, expected)) {
            available = true;
            break;
        }
    }
    CFRelease(installed);
    if (!available) return false;
    CTFontRef font = CTFontCreateWithNameAndOptions(expected, 12.0, NULL,
        kCTFontOptionsPreventAutoActivation | kCTFontOptionsPreventAutoDownload);
    if (!font) return false;
    CFStringRef family = CTFontCopyFamilyName(font);
    CFStringRef name = CTFontCopyPostScriptName(font);
    CFTypeRef url = CTFontCopyAttribute(font, kCTFontURLAttribute);
    CFStringRef scheme = url && CFGetTypeID(url) == CFURLGetTypeID()
        ? CFURLCopyScheme((CFURLRef)url) : NULL;
    char path[4096];
    if (family && name && CFEqual(family, expected_family) && CFEqual(name, expected) &&
        ((CTFontGetSymbolicTraits(font) & kCTFontBoldTrait) != 0) == bold &&
        scheme && CFEqual(scheme, CFSTR("file")) &&
        CFURLGetFileSystemRepresentation((CFURLRef)url, true, (UInt8 *)path, sizeof path))
        success = host_font_copy(dst, cap, path);
    if (scheme) CFRelease(scheme);
    if (url) CFRelease(url);
    if (name) CFRelease(name);
    if (family) CFRelease(family);
    CFRelease(font);
    return success;
}
#endif

static bool host_font_ink_free(char *dst, size_t cap, bool quiet)
{
    const char *platform = jce_platform_name();
    if (!strcmp(platform, "Linux"))
        return host_font_fontconfig("Ink Free:style=Regular", false,
                                    "Ink Free", "Regular", quiet, dst, cap);
    if (!strcmp(platform, "Windows")) {
        char directory[1024], path[1536];
        if (host_font_windows_directory(directory, sizeof directory)) {
            int n = snprintf(path, sizeof path, "%s/Fonts/inkfree.ttf", directory);
            if (n > 0 && n < (int)sizeof path && host_font_copy(dst, cap, path)) return true;
        }
    }
#if defined(__APPLE__)
    else if (!strcmp(platform, "macOS") && host_font_apple_face(
        CFSTR("InkFree"), CFSTR("Ink Free"), false, dst, cap)) return true;
#endif
    host_font_match_warning("Ink Free", "Regular",
                            "installed-face-unavailable-or-capacity", NULL, NULL, quiet);
    return false;
}

static bool host_font_print(bool bold, char *dst, size_t cap, bool quiet)
{
    const char *platform = jce_platform_name();
    if (!strcmp(platform, "Linux"))
        return host_font_fontconfig(bold ? "Segoe Print:style=Bold" : "Segoe Print:style=Regular",
                                    false, "Segoe Print", bold ? "Bold" : "Regular", quiet,
                                    dst, cap);
    if (!strcmp(platform, "Windows")) {
        /* These Windows-provided filenames identify Regular/Bold respectively.
         * Do not accept a similarly named extension or another font family. */
        char directory[1024], path[1536];
        if (host_font_windows_directory(directory, sizeof directory)) {
            int n = snprintf(path, sizeof path, "%s/Fonts/%s.ttf", directory,
                             bold ? "segoeprb" : "segoepr");
            if (n > 0 && n < (int)sizeof path && host_font_copy(dst, cap, path)) return true;
        }
    }
#if defined(__APPLE__)
    else if (!strcmp(platform, "macOS") && host_font_apple_face(
        bold ? CFSTR("SegoePrint-Bold") : CFSTR("SegoePrint"),
        CFSTR("Segoe Print"), bold, dst, cap)) return true;
#endif
    host_font_match_warning("Segoe Print", bold ? "Bold" : "Regular",
                            "installed-face-unavailable-or-capacity", NULL, NULL, quiet);
    return false;
}

static bool host_font_segoe_symbols(char *dst, size_t cap, bool quiet)
{
    const char *platform = jce_platform_name();
    if (!strcmp(platform, "Linux"))
        return host_font_fontconfig("Segoe UI Symbol:style=Regular", false,
                                    "Segoe UI Symbol", "Regular", quiet, dst, cap);
    if (!strcmp(platform, "Windows")) {
        char directory[1024], path[1536];
        if (host_font_windows_directory(directory, sizeof directory)) {
            int n = snprintf(path, sizeof path, "%s/Fonts/seguisym.ttf", directory);
            if (n > 0 && n < (int)sizeof path && host_font_copy(dst, cap, path)) return true;
        }
    }
#if defined(__APPLE__)
    else if (!strcmp(platform, "macOS") && host_font_apple_face(
        CFSTR("SegoeUISymbol"), CFSTR("Segoe UI Symbol"), false, dst, cap)) return true;
#endif
    host_font_match_warning("Segoe UI Symbol", "Regular",
                            "installed-face-unavailable-or-capacity", NULL, NULL, quiet);
    return false;
}

static bool host_font_symbols(char *dst, size_t cap)
{
    const char *platform = jce_platform_name();
    if (!strcmp(platform, "Windows")) return host_font_segoe_symbols(dst, cap, true);
    if (!strcmp(platform, "Linux")) {
        static const char *families[] = { "Noto Sans Symbols 2", "Noto Sans Symbols", "Segoe UI Symbol", NULL };
        for (int i = 0; families[i]; ++i) {
            char pattern[128];
            snprintf(pattern, sizeof pattern, "%s:style=Regular", families[i]);
            if (host_font_fontconfig(pattern, false, families[i], "Regular", true, dst, cap)) return true;
        }
    }
#if defined(__APPLE__)
    else if (!strcmp(platform, "macOS")) {
        if (host_font_apple_face(CFSTR("AppleSymbols"), CFSTR("Apple Symbols"), false, dst, cap) ||
            host_font_apple_face(CFSTR("NotoSansSymbols2-Regular"), CFSTR("Noto Sans Symbols 2"), false, dst, cap) ||
            host_font_apple_face(CFSTR("SegoeUISymbol"), CFSTR("Segoe UI Symbol"), false, dst, cap)) return true;
    }
#endif
    return false;
}

bool jce_host_find_font(const char *family, char *dst, size_t cap)
{
    const char *stem;
    if (!dst || !cap) return false;
    dst[0] = '\0';
    if (!family || !family[0] || strlen(family) > 128 || strstr(family, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)family; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == '\"' || *p == '\'')
            return false;
    if (!jce_strcasecmp(family, "InkFree") || !jce_strcasecmp(family, "Ink Free") ||
        !jce_strcasecmp(family, "Ink Free:style=Regular"))
        return host_font_ink_free(dst, cap, false);
    if (!jce_strcasecmp(family, "Segoe Print") || !jce_strcasecmp(family, "SegoePrint") ||
        !jce_strcasecmp(family, "segoepr") || !jce_strcasecmp(family, "Segoe Print:style=Regular"))
        return host_font_print(false, dst, cap, false);
    if (!jce_strcasecmp(family, "Segoe Print Bold") || !jce_strcasecmp(family, "SegoePrint-Bold") ||
        !jce_strcasecmp(family, "segoeprb") || !jce_strcasecmp(family, "Segoe Print:style=Bold"))
        return host_font_print(true, dst, cap, false);
    if (!jce_strcasecmp(family, "Segoe UI Symbol") || !jce_strcasecmp(family, "SegoeUISymbol") ||
        !jce_strcasecmp(family, "seguisym")) return host_font_segoe_symbols(dst, cap, false);
    stem = family;
    if (!jce_strcasecmp(family, "KaiTi"))
        stem = !strcmp(jce_platform_name(), "Windows") ? "simkai" :
            !strcmp(jce_platform_name(), "macOS") ? "Kaiti" : "ukai";
    else if (!jce_strcasecmp(family, "Segoe UI")) stem = "segoeui";
    else if (!jce_strcasecmp(family, "Microsoft YaHei")) stem = "msyh";
    else if (!jce_strcasecmp(family, "DejaVu Sans")) stem = "DejaVuSans";
    if (host_font_probe_stem(stem, dst, cap)) return true;
    return host_font_fontconfig(family,
        strstr(family, ":lang=zh") || strstr(family, ":lang=ja") ||
        strstr(family, ":lang=ko"), NULL, NULL, true, dst, cap);
}

bool jce_host_default_font(JceHostFontRole role, char *dst, size_t cap)
{
    static const char *latin_windows[] = { "segoeui", "tahoma", "arial", NULL };
    static const char *cjk_windows[] = { "msyh", "msjh", "meiryo", "malgun", "simhei", "simsun", NULL };
    static const char *latin_apple[] = { "SFNS", "Helvetica", "Arial", NULL };
    static const char *cjk_apple[] = { "PingFang", "Hiragino Sans GB", "STHeiti Medium", "Arial Unicode", NULL };
    static const char *latin_linux[] = { "DejaVuSans", "LiberationSans-Regular", "NotoSans-Regular", NULL };
    static const char *cjk_linux[] = { "NotoSansCJK-Regular", "NotoSansSC-Regular", "wqy-microhei", "wqy-zenhei", "DroidSansFallbackFull", NULL };
    const char *const *names;
    const char *platform;
    if (!dst || !cap) return false;
    dst[0] = '\0';
    if (role == JCE_HOST_FONT_UI_SYMBOLS) return host_font_symbols(dst, cap);
    if (role != JCE_HOST_FONT_UI_LATIN && role != JCE_HOST_FONT_UI_CJK) return false;
    /* Latin prefers a genuinely installed Ink Free Regular face. Absence is a
     * normal silent fallback; CJK retains its own coverage-aware candidates. */
    if (role == JCE_HOST_FONT_UI_LATIN && host_font_ink_free(dst, cap, true)) return true;
    platform = jce_platform_name();
    if (!strcmp(platform, "Windows")) names = role == JCE_HOST_FONT_UI_LATIN ? latin_windows : cjk_windows;
    else if (!strcmp(platform, "macOS")) names = role == JCE_HOST_FONT_UI_LATIN ? latin_apple : cjk_apple;
    else if (!strcmp(platform, "Linux")) {
        if (host_font_fontconfig(role == JCE_HOST_FONT_UI_LATIN ? "sans-serif" : "sans-serif:lang=zh-cn",
                                role == JCE_HOST_FONT_UI_CJK, NULL, NULL, true, dst, cap)) return true;
        names = role == JCE_HOST_FONT_UI_LATIN ? latin_linux : cjk_linux;
    } else return false;
    for (int i = 0; names[i]; ++i)
        if (host_font_probe_stem(names[i], dst, cap)) return true;
    return false;
}
