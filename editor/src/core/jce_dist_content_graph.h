/* jce_dist_content_graph.h  Bundle-derived project content for Dist. */
#ifndef JCE_DIST_CONTENT_GRAPH_H
#define JCE_DIST_CONTENT_GRAPH_H

#include <cstdint>
#include <string>
#include <vector>

struct JceDistContentGraphAsset {
    std::string source_label;
    std::string address;
    std::vector<uint8_t> bytes;
    uint64_t content_id = 0;
};

struct JceDistContentGraph {
    std::vector<JceDistContentGraphAsset> assets;
    std::string snapshot_path;
};

using JceDistContentGraphLogFn = void (*)(int level, const char *message,
                                          void *user);

int jce_dist_content_graph_host_platform(void);

bool jce_dist_content_graph_build(const std::string &project_root,
                                  const std::string &generated_dir,
                                  const std::string &reports_dir,
                                  int target_platform,
                                  JceDistContentGraphLogFn log_fn,
                                  void *log_user,
                                  JceDistContentGraph *out_graph,
                                  std::string *out_error);

#endif /* JCE_DIST_CONTENT_GRAPH_H */
