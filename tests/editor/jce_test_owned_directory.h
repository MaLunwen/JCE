#ifndef JCE_TEST_OWNED_DIRECTORY_H
#define JCE_TEST_OWNED_DIRECTORY_H
#include <doctest.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <cstdlib>
#include <string>
#if JCE_PLATFORM_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#endif
struct JceTestOwnedDirectory {
    /* Test-only host PID adapter: path selection is not an atomic reservation.
       Give parallel normal/mutant processes disjoint candidate namespaces. */
    static unsigned long current_process_id()
    {
#if JCE_PLATFORM_WINDOWS
        return static_cast<unsigned long>(_getpid());
#else
        return static_cast<unsigned long>(getpid());
#endif
    }
    std::string base, root, prefix;
    explicit JceTestOwnedDirectory(const char *name) : prefix(name)
    {
        char cwd[1024] = {}, normalized[1024] = {}, unique[1024] = {};
        REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
        REQUIRE(jce_path_normalize(normalized, sizeof(normalized), cwd));
        base = normalized;
        const auto pid = current_process_id();
        REQUIRE(pid > 0);
        prefix += "_pid" + std::to_string(pid);
        REQUIRE(jce_fs_host_make_unique_path((base + "/" + prefix).c_str(), unique, sizeof(unique)));
        root = unique;
        REQUIRE_FALSE(jce_fs_host_exists_dir(root.c_str()));
        REQUIRE(jce_fs_host_create_directory(root.c_str()));
    }
    std::string path(const char *leaf) const { return root + "/" + leaf; }
    ~JceTestOwnedDirectory()
    {
        const bool owned = root.rfind(base + "/" + prefix, 0) == 0 &&
            root.find('/', base.size() + 1) == std::string::npos;
        CHECK(owned);
        if (owned) CHECK(jce_fs_host_remove_recursive(root.c_str()));
    }
};
struct JceTestEnvironment {
    std::string key, previous;
    bool present;
    JceTestEnvironment(const char *name, const char *value) : key(name)
    {
        const char *old = std::getenv(name);
        present = old != nullptr; previous = old ? old : "";
        set(value);
    }
    void set(const char *value) const
    {
#if JCE_PLATFORM_WINDOWS
        REQUIRE(_putenv_s(key.c_str(), value ? value : "") == 0);
#else
        REQUIRE((value ? setenv(key.c_str(), value, 1) : unsetenv(key.c_str())) == 0);
#endif
    }
    ~JceTestEnvironment() { set(present ? previous.c_str() : nullptr); }
};
#endif
