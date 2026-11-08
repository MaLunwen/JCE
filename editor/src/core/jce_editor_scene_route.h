/*
 * jce_editor_scene_route.h — editor-only project/scene route validation.
 * Pure C++ helpers: no global state, no I/O and no editor or engine ownership.
 * Paths must be absolute, normalised lexically and remain project-owned.
 * Never use this lexical check as a filesystem security / symlink boundary.
 */
#ifndef JCE_EDITOR_SCENE_ROUTE_H
#define JCE_EDITOR_SCENE_ROUTE_H

#include <string>
#include <vector>

namespace jce_editor_scene_route {

inline bool ascii_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
inline char fold_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}
inline std::string slash(std::string p) {
    for (char &c : p) if (c == '\\') c = '/';
    return p;
}

/* Empty indicates an invalid/non-absolute path or a path escaping its root. */
inline std::string absolute_normalized(const std::string &input) {
    const std::string path = slash(input);
    if (path.empty()) return {};
    std::string prefix;
    size_t offset = 0;
    if (path.size() >= 3 && ascii_alpha(path[0]) && path[1] == ':' && path[2] == '/') {
        prefix.push_back(path[0]);
        prefix += ":/";
        offset = 3;
    } else if (path.size() >= 3 && path[0] == '/' && path[1] == '/') {
        prefix = "//";
        offset = 2;
    } else if (path[0] == '/') {
        prefix = "/";
        offset = 1;
    } else {
        return {};
    }
    std::vector<std::string> sections;
    while (offset < path.size()) {
        const size_t next = path.find('/', offset);
        const std::string part = path.substr(offset, next == std::string::npos ? next : next - offset);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (sections.empty()) return {};
                sections.pop_back();
            } else {
                sections.push_back(part);
            }
        }
        if (next == std::string::npos) break;
        offset = next + 1;
    }
    std::string out = prefix;
    for (const std::string &part : sections) {
        if (out.back() != '/') out += '/';
        out += part;
    }
    return out;
}

inline std::string comparison_form(const std::string &path) {
    std::string p = absolute_normalized(path);
    if (p.size() >= 2 &&
        ((ascii_alpha(p[0]) && p[1] == ':') ||
         (p[0] == '/' && p[1] == '/'))) {
        for (char &c : p) c = fold_ascii(c);
    }
    return p;
}
inline bool same_absolute(const std::string &a, const std::string &b) {
    const std::string aa = comparison_form(a);
    return !aa.empty() && aa == comparison_form(b);
}
inline bool owned_scene(const std::string &project, const std::string &scene) {
    const std::string base = comparison_form(project);
    const std::string target = comparison_form(scene);
    if (base.empty() || target.size() <= base.size() ||
        target.compare(0, base.size(), base) != 0) return false;
    /* Base ending in / is the drive or filesystem root. */
    return base.back() == '/' || target[base.size()] == '/';
}

/* Resolve two manifest-relative segments under project root, rejecting
 * drive/UNC/root escapes and ../ traversal before any filesystem access. */
inline std::string join_below(const std::string &absolute_root,
                              const std::string &first,
                              const std::string &second = {}) {
    const std::string root = absolute_normalized(absolute_root);
    if (root.empty()) return {};
    auto valid_relative = [](const std::string &raw) -> bool {
        const std::string p = slash(raw);
        if (p.empty() || p[0] == '/' || p.find(':') != std::string::npos) return false;
        size_t offset = 0;
        while (offset < p.size()) {
            const size_t next = p.find('/', offset);
            const std::string seg = p.substr(offset, next == std::string::npos ? next : next - offset);
            if (seg == "..") return false;
            if (next == std::string::npos) break;
            offset = next + 1;
        }
        return true;
    };
    if (!valid_relative(first) || (!second.empty() && !valid_relative(second))) return {};
    const std::string joined = root + (root.back() == '/' ? "" : "/") + slash(first)
        + (second.empty() ? "" : "/" + slash(second));
    const std::string out = absolute_normalized(joined);
    return owned_scene(root, out) ? out : std::string();
}

/* Manifest source_assets can be an absolute external asset root (supported
 * by the editor's existing scene dialog), or relative to the project root. */
inline std::string asset_root(const std::string &project,
                              const std::string &source_assets) {
    const std::string raw = slash(source_assets.empty() ? "assets" : source_assets);
    const std::string absolute = absolute_normalized(raw);
    if (!absolute.empty()) return absolute;
    return join_below(project, raw);
}
inline bool project_scene(const std::string &project,
                          const std::string &source_assets,
                          const std::string &scene) {
    const std::string assets = asset_root(project, source_assets);
    return owned_scene(project, scene) ||
           (!assets.empty() && owned_scene(assets, scene));
}
inline std::string startup_scene_path(const std::string &project,
                                       const std::string &source_assets,
                                       const std::string &startup_scene) {
    const std::string assets = asset_root(project, source_assets);
    return assets.empty() ? std::string() : join_below(assets, startup_scene);
}

}  // namespace jce_editor_scene_route
#endif
