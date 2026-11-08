/* Private build-owner transcript; no general logger or second process owner. */
#ifndef JCE_BUILD_LOG_H
#define JCE_BUILD_LOG_H
#include "jce_build_manager.h"
#include <algorithm>
#include <string>
#include <vector>

struct JceBuildLog {
    std::string text;
    bool available = false, truncated = false, incomplete = false;
    void reset() { text.clear(); available = true; truncated = incomplete = false; }
    void append_line(const std::string &line)
    {
        const size_t room = JCE_BUILD_LOG_CAPACITY - text.size();
        const size_t count = std::min(room, line.size());
        text.append(line.data(), count);
        if (count != line.size()) truncated = true;
        if (text.size() < JCE_BUILD_LOG_CAPACITY) text.push_back('\n');
        else truncated = true;
    }
    JceBuildLogView view() const
    { return {text.c_str(), text.size(), available, truncated, incomplete}; }
};

/* The background phase retains bounded complete entries for main-thread replay.
 * Counts include separators and cap vector overhead independently of text bytes. */
template<typename Level> struct JceBuildWorkerLog {
    struct Entry { Level level; std::string text; };
    std::vector<Entry> entries;
    std::string last_error;
    size_t retained_bytes = 0;
    bool truncated = false;
    void append(Level level, const std::string &line)
    {
        if (entries.size() >= 4096 || line.size() >= JCE_BUILD_LOG_CAPACITY ||
            line.size() + 1 > JCE_BUILD_LOG_CAPACITY - retained_bytes) {
            truncated = true; return;
        }
        entries.push_back({level, line}); retained_bytes += line.size() + 1;
    }
    void reset() { entries.clear(); last_error.clear(); retained_bytes = 0; truncated = false; }
};
#endif
