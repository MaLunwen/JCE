/*
 * jce_build_stage_runtime.cpp — see jce_build_stage_runtime.h for WHY this
 * exists and why the selection is by shape rather than by language.
 */
#include "jce_build_stage_runtime.h"

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_filesystem.h>

#include <cctype>
#include <cstring>

namespace {

#if JCE_PLATFORM_WINDOWS
constexpr char PATH_SEP = '\\';
#else
constexpr char PATH_SEP = '/';
#endif

struct Payload {
	std::string src_dir;
	std::string dst_dir;
	int copied = 0;
	int failed = 0;
};

bool ends_with_ci(const std::string &s, const char *suffix)
{
	const size_t n = std::strlen(suffix);
	if (s.size() < n)
		return false;
	for (size_t i = 0; i < n; ++i) {
		const unsigned char a = (unsigned char)s[s.size() - n + i];
		const unsigned char b = (unsigned char)suffix[i];
		if (std::tolower(a) != std::tolower(b))
			return false;
	}
	return true;
}

bool is_payload(const std::string &name, bool is_dir)
{
	if (is_dir)
		return name.rfind("jce_script_", 0) == 0;
	return ends_with_ci(name, ".dll") || ends_with_ci(name, ".so") ||
	       ends_with_ci(name, ".dylib") ||
	       ends_with_ci(name, ".runtimeconfig.json");
}

bool visit(const char *name, bool is_dir, void *user)
{
	Payload *p = (Payload *)user;
	const std::string leaf(name ? name : "");
	if (leaf.empty() || !is_payload(leaf, is_dir))
		return true;
	const std::string src = p->src_dir + PATH_SEP + leaf;
	const std::string dst = p->dst_dir + PATH_SEP + leaf;
	const bool ok = is_dir
		? jce_fs_host_copy_recursive(src.c_str(), dst.c_str())
		: jce_fs_host_copy_file(src.c_str(), dst.c_str());
	if (ok)
		p->copied++;
	else
		p->failed++;
	return true;
}

/* Directory part of `path`, or "" when it has none. */
std::string parent_dir_of(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

} /* namespace */

int jce_build_stage_runtime_payload(const std::string &built_exe,
                                    const std::string &out_dir,
                                    std::string *error)
{
	Payload p;
	p.src_dir = parent_dir_of(built_exe);
	p.dst_dir = out_dir;
	if (p.src_dir.empty())
		return 0;

	jce_fs_host_list_dir(p.src_dir.c_str(), visit, &p);

	if (p.failed) {
		if (error)
			*error = "package: failed to stage " +
			         std::to_string(p.failed) +
			         " runtime payload entry/entries from " +
			         p.src_dir +
			         " - the packaged game would not start";
		return -1;
	}
	return p.copied;
}
