/*
 * jce_run_manager_resolve.cpp — pure half of the launcher's path resolution.
 * See jce_run_manager_internal.h for why this is a separate translation unit.
 * It must stay free of editor globals and of the filesystem: everything it
 * needs arrives in JceRunResolveInputs or through the injected predicate.
 */

#include "jce_run_manager_internal.h"

namespace {

/* Try `candidate` as-is and, on platforms with an exe suffix, with the suffix
 * appended -- but never doubled. */
bool try_with_suffix(const std::string &candidate,
                     const std::string &exe_suffix,
                     JceRunPathExists exists, void *user,
                     std::string *out)
{
    if (candidate.empty()) return false;
    if (exists(candidate, user)) { *out = candidate; return true; }
    if (exe_suffix.empty()) return false;

    const std::size_t slen = exe_suffix.size();
    const bool already = candidate.size() >= slen &&
        candidate.compare(candidate.size() - slen, slen, exe_suffix) == 0;
    if (already) return false;

    const std::string with = candidate + exe_suffix;
    if (exists(with, user)) { *out = with; return true; }
    return false;
}

bool looks_absolute(const std::string &p)
{
    const std::string seps = "/\\";
    if (!p.empty() && seps.find(p[0]) != std::string::npos) return true;
    if (p.size() > 1 && p[1] == ':') return true;   /* C:\... */
    return false;
}

} /* namespace */

std::string jce_run_manager_resolve_path(const JceRunResolveInputs &in,
                                         JceRunPathExists exists, void *user)
{
    std::string hit;

    if (!in.configured.empty() &&
        try_with_suffix(in.configured, in.exe_suffix, exists, user, &hit))
        return hit;

    if (!in.configured.empty() && !looks_absolute(in.configured)) {
        for (const std::string &par : in.parents) {
            if (try_with_suffix(par + "/" + in.configured, in.exe_suffix,
                                exists, user, &hit))
                return hit;
        }
    }

    if (!in.build_output_path.empty() && !in.target_name.empty() &&
        try_with_suffix(in.build_output_path + "/" + in.target_name,
                        in.exe_suffix, exists, user, &hit))
        return hit;

    if (!in.target_name.empty()) {
        for (const std::string &dir : in.output_dirs) {
            for (const std::string &par : in.parents) {
                if (try_with_suffix(par + "/" + dir + "/" + in.target_name,
                                    in.exe_suffix, exists, user, &hit))
                    return hit;
            }
        }
    }

    return in.configured;
}
