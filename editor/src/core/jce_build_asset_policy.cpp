#include "jce_build_asset_policy.h"

#include <jce/resource/jce_asset_format.h>

#include <string>

namespace {

bool ascii_ends_with(const std::string &value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(),
                         suffix) == 0;
}

bool segment_is_authoring(std::string_view segment)
{
    return segment == ".git" || segment == ".github" ||
           segment == ".idea" || segment == ".vscode" ||
           segment == "__pycache__" || segment == "raw_assets";
}

bool leaf_is_authoring(std::string_view leaf)
{
    return leaf == "agents.md" || leaf == "cmakelists.txt" ||
           leaf == "readme" || leaf == "readme.md" ||
           leaf == "thumbs.db" || leaf == "desktop.ini";
}

} /* namespace */

bool jce_build_asset_path_is_publishable(std::string_view virtual_path)
{
    if (virtual_path.empty())
        return false;

    std::string path(virtual_path);
    for (char &ch : path) {
        if (ch == '\\')
            ch = '/';
        else if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
    }

    std::string_view leaf;
    size_t begin = 0;
    while (begin <= path.size()) {
        const size_t end = path.find('/', begin);
        const size_t count = end == std::string::npos
                                 ? path.size() - begin
                                 : end - begin;
        const std::string_view segment(path.data() + begin, count);
        if (segment.empty() || segment.front() == '.' ||
            segment == ".." || segment_is_authoring(segment)) {
            return false;
        }
        leaf = segment;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }

    if (leaf_is_authoring(leaf))
        return false;

    /* A file in a language the engine SHIPS is content, and this check runs
     * BEFORE the tooling deny-list so no future entry can shadow it.
     *
     * `.py` used to sit in that deny-list, from when Python could only be
     * build tooling here.  It is a gameplay scripting language now, and an
     * extension can no longer tell turret.py from build.py — so the rule
     * that dropped one silently dropped the other.  This is the failure the
     * whole extension-authority change exists for: the editor keeps running
     * a Python script from loose files while the PACKAGED build ships
     * without it, and nothing says so.
     * *Enforced by:* tests/editor/test_jce_build_asset_policy.cpp ::
     * "every shippable script language survives publication". */
    if (jce_asset_script_language_from_ext(path.c_str()))
        return true;

    /* Tooling. `.pyc` stays: it is a CPython cache artefact, not authored
     * content, and no script language claims it. */
    return !ascii_ends_with(path, ".bat") &&
           !ascii_ends_with(path, ".cmd") &&
           !ascii_ends_with(path, ".ps1") &&
           !ascii_ends_with(path, ".pyc") &&
           !ascii_ends_with(path, ".sh");
}
