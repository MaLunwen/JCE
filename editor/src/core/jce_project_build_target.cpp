#include "jce_project_build_target.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <memory>
#include <set>
#include <utility>

static bool has_exe_suffix(const std::string &name)
{
    if (name.size() < 4)
        return false;

    const size_t pos = name.size() - 4;
    const char suffix[5] = ".exe";
    for (size_t i = 0; i < 4; ++i) {
        char c = name[pos + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != suffix[i])
            return false;
    }
    return true;
}

std::string jce_project_build_exe_name(const std::string &target,
                             const char *configured)
{
    std::string name = (configured && configured[0])
                           ? std::string(configured)
                           : target;
#if JCE_PLATFORM_WINDOWS
    if (!has_exe_suffix(name))
        name += ".exe";
#endif
    return name;
}

std::string jce_project_build_root(const std::string &project,
    JceProjectBuildPlatform platform, const std::string &arch)
{
    std::string root = project;
    if (!root.empty() && root.back() != '/' && root.back() != '\\') root += '/';
    return root + "build/" + jce_project_build_platform_tag(platform) + "-" + arch;
}

std::string jce_project_build_output_dir(const std::string &build_root,
    const std::string &variant)
{
    return build_root + "/" + variant;
}

const char *jce_project_build_delivery(const std::string &variant)
{
    return variant == "debug" ? "authoring" : "embedded";
}

std::string jce_project_build_sdk_target_error(const std::string &sdk,
    const char *arch, JceProjectBuildPlatform platform, const std::string &variant)
{
    const char *system = platform == JceProjectBuildPlatform::Windows ? "Windows" :
                         platform == JceProjectBuildPlatform::MacOS ? "Darwin" : "Linux";
    const char *config = variant == "debug" ? "Debug" : "Release";
    const char *delivery = jce_project_build_delivery(variant);
    const std::string file = sdk + "/lib/" + config + "/jce-sdk-target.json";
    const std::string refresh = "; refresh/select the matching SDK (GUI debug uses authoring; release/dist use embedded)";
    const char *canonical = jce_project_build_arch(arch, nullptr);
    if (!canonical || std::strcmp(canonical, arch) ||
        (variant != "debug" && variant != "release" && variant != "dist"))
        return "Unsupported SDK target selection" + refresh;
    uint64_t size = 0, total = 0;
    // Bound the allocation itself; a pre-stat cannot guard a replaced file.
    auto *raw = static_cast<char *>(jce_fs_host_read_capped(file.c_str(), 16385, &size, &total));
    if (!raw) return "Missing/unreadable SDK target metadata at " + file + refresh;
    const bool invalid_size = size > 16384 || total > 16384 || size != total;
    // Generated schema strings are fixed ASCII; rejecting escapes also closes
    // decoded duplicate aliases and embedded NUL before C-string comparisons.
    const bool escaped = !invalid_size && std::memchr(raw, '\\', (size_t)size) != nullptr;
    std::unique_ptr<JceJson, decltype(&jce_json_free)> metadata(
        !invalid_size && !escaped ? jce_json_parse_strict(raw, (size_t)size) : nullptr,
        jce_json_free);
    jce_fs_buffer_free(raw);
    if (invalid_size) return "SDK target metadata exceeds 16 KiB or read is incomplete" + refresh;
    if (escaped) return "SDK target metadata requires unescaped ASCII scalars" + refresh;
    if (!metadata || !jce_json_is_object(metadata.get()))
        return "Malformed SDK target metadata at " + file + refresh;
    const char *fields[] = { "schema", "system", "arch", "pointer_bits", "config",
                             "variant", "delivery", "crt", "archive" };
    std::set<std::string> keys;
    for (JceJson *it = jce_json_first_child(metadata.get()); it;
         it = jce_json_next_sibling(it)) {
        const char *key = jce_json_member_key(it);
        bool known = false;
        for (const char *field : fields) if (key && !std::strcmp(key, field)) known = true;
        if (!known || !keys.insert(key).second)
            return "Duplicate/unknown SDK target metadata fields at " + file + refresh;
    }
    if (keys.size() != sizeof(fields) / sizeof(fields[0]))
        return "Incomplete SDK target metadata at " + file + refresh;
    for (const char *key : fields) {
        const JceJson *value = jce_json_get(metadata.get(), key);
        const bool numeric = !std::strcmp(key, "schema") || !std::strcmp(key, "pointer_bits");
        if (numeric ? !jce_json_is_number(value) : !jce_json_is_string(value))
            return std::string("Invalid SDK target metadata field ") + key + refresh;
    }
    const auto text = [&](const char *key) {
        return jce_json_get_string(metadata.get(), key, "");
    };
    const bool is32 = !std::strcmp(arch, "i686") || !std::strcmp(arch, "armv7");
    if (jce_json_get_number(metadata.get(), "schema", 0) != 1 ||
        jce_json_get_number(metadata.get(), "pointer_bits", 0) != (is32 ? 32 : 64))
        return "Unsupported SDK target schema or pointer size" + refresh;
    const std::pair<const char *, const char *> expected_fields[] = {
        {"system", system}, {"arch", arch}, {"config", config},
        {"variant", variant.c_str()}, {"delivery", delivery}
    };
    for (const auto &expected : expected_fields) {
        if (std::strcmp(text(expected.first), expected.second))
            return std::string("SDK target ") + expected.first + " mismatch: selected " +
                   expected.second + ", SDK " + text(expected.first) + refresh;
    }
    const char *crt = text("crt");
    const char *archive = text("archive");
    if (platform == JceProjectBuildPlatform::Windows) {
        const bool debug = variant == "debug";
        const bool valid = debug ? (!std::strcmp(crt, "MultiThreadedDebug") ||
                                   !std::strcmp(crt, "MultiThreadedDebugDLL")) :
                                  (!std::strcmp(crt, "MultiThreaded") ||
                                   !std::strcmp(crt, "MultiThreadedDLL"));
        if (!valid || (!std::strcmp(delivery, "embedded") && std::strstr(crt, "DLL")))
            return "SDK CRT incompatible with selected configuration/delivery" + refresh;
        if (std::strcmp(archive, "jce_engine_core.lib"))
            return "SDK target archive name invalid" + refresh;
    } else if (std::strcmp(crt, "none") || std::strcmp(archive, "jce_engine_core.a")) {
        return "SDK target archive/CRT invalid for selected system" + refresh;
    }
    if (!jce_fs_host_exists_file((sdk + "/lib/" + config + "/" + archive).c_str()))
        return "SDK archive missing next to selected configuration metadata" + refresh;
    return {};
}
