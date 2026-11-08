#include "jce_build_package.h"
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <cctype>
#include <cstring>
#include <vector>

namespace {
constexpr size_t kPathCapacity = 1024; // host enumeration/join capacity
bool fail(std::string *error, const std::string &message)
{
    if (error) *error = "package: " + message;
    return false;
}
bool filename_valid(const std::string &name, bool traversal_substring)
{
    if (name.empty() || name == "." || name == ".." ||
        (traversal_substring && name.find("..") != std::string::npos) ||
        name.back() == '.' || name.back() == ' ') return false;
    for (unsigned char c : name)
        if (c < 32 || c == 127 || std::strchr("/\\:*?\"<>|", c)) return false;
    std::string stem = name.substr(0, name.find('.'));
    for (char &c : stem) c = (char)std::toupper((unsigned char)c);
    return !(stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 && (stem.substr(0, 3) == "COM" ||
                             stem.substr(0, 3) == "LPT") &&
         stem[3] >= '1' && stem[3] <= '9'));
}
bool native_absolute(const std::string &path)
{
#if JCE_PLATFORM_WINDOWS
    return (path.size() >= 3 && std::isalpha((unsigned char)path[0]) &&
            path[1] == ':' && path[2] == '/') || path.rfind("//", 0) == 0;
#else
    return !path.empty() && path[0] == '/';
#endif
}
bool normalize(const std::string &path, bool allow_relative,
               std::string *out, std::string *error)
{
    if (path.empty() || path.size() >= kPathCapacity)
        return fail(error, "missing or overlong path: " + path);
    std::string canonical = path;
    for (char &c : canonical) {
        if ((unsigned char)c < 32 || (unsigned char)c == 127)
            return fail(error, "control character in path");
        if (c == '\\') c = '/';
    }
    if (!native_absolute(canonical)) {
        if (!allow_relative) return fail(error, "output must be absolute: " + path);
#if JCE_PLATFORM_WINDOWS
        if (canonical.find(':') != std::string::npos || canonical[0] == '/')
            return fail(error, "ambiguous drive-relative path: " + path);
#endif
        char cwd[kPathCapacity] = {};
        if (!jce_fs_host_get_current_dir(cwd, sizeof(cwd)))
            return fail(error, "cannot resolve current directory");
        canonical = std::string(cwd) + "/" + canonical;
        for (char &c : canonical) if (c == '\\') c = '/';
    }
    std::string prefix, tail = canonical;
#if JCE_PLATFORM_WINDOWS
    if (canonical.rfind("//", 0) == 0) {
        size_t server_end = canonical.find('/', 2);
        size_t share_end = server_end == std::string::npos ? std::string::npos :
                           canonical.find('/', server_end + 1);
        const auto server = canonical.substr(2, server_end - 2);
        const auto share = server_end == std::string::npos ? std::string() :
            canonical.substr(server_end + 1, share_end - server_end - 1);
        if (!filename_valid(server, false) || !filename_valid(share, false))
            return fail(error, "invalid network/device path: " + path);
        prefix = "//" + server + "/" + share;
        tail = share_end == std::string::npos ? "/" : canonical.substr(share_end);
    }
#endif
    char normalized[kPathCapacity] = {};
    if (!jce_path_normalize(normalized, sizeof(normalized), tail.c_str()))
        return fail(error, "cannot normalize path: " + path);
    std::string result = prefix + normalized;
    while (result.size() > 1 && result.back() == '/' &&
           !(result.size() == 3 && result[1] == ':')) result.pop_back();
    if (result.size() >= kPathCapacity) return fail(error, "overlong normalized path");
#if JCE_PLATFORM_WINDOWS
    size_t start = result.rfind("//", 0) == 0 ? 2 : 3;
    for (size_t end; start < result.size(); start = end + 1) {
        end = result.find('/', start);
        const auto part = result.substr(start, end - start);
        if (!filename_valid(part, false))
            return fail(error, "unsafe path component: " + part);
        if (end == std::string::npos) break;
    }
#endif
    *out = result;
    return true;
}
bool path_equal(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
#if JCE_PLATFORM_WINDOWS
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
#else
        if (a[i] != b[i]) return false;
#endif
    }
    return true;
}
bool ancestor(const std::string &a, const std::string &b)
{
    return path_equal(a, b) || (b.size() > a.size() &&
        path_equal(a, b.substr(0, a.size())) &&
        (a.back() == '/' || b[a.size()] == '/'));
}
bool is_root(const std::string &path)
{
    if (path == "/" || (path.size() == 3 && path[1] == ':')) return true;
    if (path.rfind("//", 0) == 0)
        return path.find('/', path.find('/', 2) + 1) == std::string::npos;
    return false;
}
struct Contents {
    std::string allowed;
    std::string unexpected;
    bool replace = false;
};
bool visit(const char *name, bool is_dir, void *user)
{
    auto &c = *static_cast<Contents *>(user);
    const std::string leaf = name ? name : "";
    if (is_dir || !c.replace || !path_equal(leaf, c.allowed)) {
        c.unexpected = leaf;
        return false;
    }
    return true;
}
struct Candidate {
    std::string dir, exe;
    ~Candidate()
    {
        // The reports directory is retained and reused, so empty candidate
        // directories do not accumulate. Only our exact candidate is removed.
        if (!exe.empty()) jce_fs_host_remove_file(exe.c_str());
    }
};
} // namespace

bool jce_build_package_component_valid(const std::string &name, std::string *error)
{
    return filename_valid(name, true) || fail(error, "unsafe filename component: " + name);
}
bool jce_build_package_default_path(const std::string &project,
    const std::string &app, const std::string &platform,
    const std::string &arch, const std::string &variant,
    std::string *out, std::string *error)
{
    if (!out) return fail(error, "output path destination is null");
    out->clear();
    for (const auto *name : {&app, &platform, &arch, &variant})
        if (!jce_build_package_component_valid(*name, error)) return false;
    std::string root;
    if (!normalize(project, true, &root, error)) return false;
    return normalize(root + "/dist/apps/" + app + "/" + platform + "-" + arch +
                     "-" + variant, false, out, error);
}
bool jce_build_package_check_directory(const std::string &out,
    const std::string &exe_name, bool replace_same_file, std::string *error)
{
    if (!jce_build_package_component_valid(exe_name, error)) return false;
    if (out.size() + exe_name.size() + 2 >= kPathCapacity)
        return fail(error, "output filename exceeds host path capacity");
    if (jce_fs_host_exists_file(out.c_str()))
        return fail(error, "output directory is a file: " + out);
    if (!jce_fs_host_exists_dir(out.c_str())) return true;
    Contents contents{exe_name, {}, replace_same_file};
    if (!jce_fs_host_list_dir(out.c_str(), visit, &contents))
        return fail(error, "cannot enumerate output directory: " + out);
    if (!contents.unexpected.empty())
        return fail(error, "output contains an unowned entry (kept): " + out + "/" +
                           contents.unexpected);
    return true;
}
bool jce_build_package_admit(const std::string &project,
    const std::string &sdk, const std::string &cooked,
    const std::string &requested, const std::string &exe_name, bool dist,
    std::string *out, std::string *error)
{
    if (!out) return fail(error, "output path destination is null");
    out->clear();
    std::string root, sdk_root, output;
    if (!jce_build_package_component_valid(exe_name, error) ||
        !normalize(project, true, &root, error) ||
        !normalize(sdk, true, &sdk_root, error) ||
        !normalize(requested, false, &output, error)) return false;
    if (is_root(output) || ancestor(output, root))
        return fail(error, "output overlaps project/root: " + output);
    std::vector<std::string> protected_roots{sdk_root};
    for (const char *leaf : {"build", "assets", "Assets", "resources", "src",
                            "source", "Source", ".jce", ".git"})
        protected_roots.push_back(root + "/" + leaf);
    if (!cooked.empty()) {
        std::string cooked_path;
        std::string raw = cooked;
        for (char &c : raw) if (c == '\\') c = '/';
        if (!normalize(native_absolute(raw) ? raw : root + "/" + raw,
                       false, &cooked_path, error)) return false;
        protected_roots.push_back(cooked_path);
    }
    for (const auto &protected_root : protected_roots)
        if (ancestor(output, protected_root) || ancestor(protected_root, output))
            return fail(error, "output overlaps protected directory: " + protected_root);
    if (!jce_build_package_check_directory(output, exe_name, dist, error)) return false;
    *out = output;
    return true;
}
bool jce_build_package_publish_dist(const std::string &built_exe,
    const std::string &reports_dir, const std::string &out,
    const std::string &exe_name, JceBuildPackageAuditFn audit, void *user,
    std::string *error)
{
    std::string reports, output;
    if (!audit) return fail(error, "artifact audit is required");
    if (!normalize(out, false, &output, error) || is_root(output) ||
        !normalize(reports_dir, true, &reports, error) || is_root(reports) ||
        ancestor(output, reports) || ancestor(reports, output))
        return fail(error, "candidate reports must be separate from final output");
    if (!jce_build_package_check_directory(output, exe_name, true, error)) return false;
    if (!jce_fs_host_exists_file(built_exe.c_str()))
        return fail(error, "built artifact does not exist: " + built_exe);
    if (!audit(built_exe, nullptr, user)) return fail(error, "built artifact audit failed");
    if (!jce_fs_host_create_directory(reports.c_str()))
        return fail(error, "cannot create owned reports directory: " + reports);
    Candidate candidate;
    candidate.dir = reports + "/package-candidate-" + exe_name;
    if (!jce_build_package_check_directory(candidate.dir, exe_name, true, error))
        return false;
    if (!jce_fs_host_create_directory(candidate.dir.c_str()))
        return fail(error, "cannot create candidate directory: " + candidate.dir);
    // This exact regenerable file belongs to the reports owner, including
    // stale output left by an interrupted previous invocation.
    candidate.exe = candidate.dir + "/" + exe_name;
    if (!jce_fs_host_copy_file(built_exe.c_str(), candidate.exe.c_str()))
        return fail(error, "cannot copy artifact into candidate: " + candidate.exe);
    if (!audit(candidate.exe, candidate.dir.c_str(), user))
        return fail(error, "candidate single-file audit failed");
    // Recheck after the asynchronous build/audit; never erase newly added files.
    if (!jce_build_package_check_directory(output, exe_name, true, error)) return false;
    if (!jce_fs_host_create_directory(output.c_str()))
        return fail(error, "cannot create final directory: " + output);
    const auto destination = output + "/" + exe_name;
    if (!jce_fs_host_rename(candidate.exe.c_str(), destination.c_str()))
        return fail(error, "cannot atomically publish artifact; old output kept: " + destination);
    candidate.exe.clear();
    return true;
}
