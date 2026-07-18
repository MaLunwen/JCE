#ifndef JCE_BUNDLE_GRAPH_INTERNAL_H
#define JCE_BUNDLE_GRAPH_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct cJSON cJSON;

typedef struct JceBundleDependencyEdge {
    char *from;
    char *to;
    char *origin;
} JceBundleDependencyEdge;

typedef struct JceBundleDependencyEdges {
    JceBundleDependencyEdge *items;
    size_t n;
    size_t c;
} JceBundleDependencyEdges;

bool jce_bundle_edges_add(JceBundleDependencyEdges *edges, const char *from,
                          const char *to, const char *origin);
bool jce_bundle_edges_replace_path(JceBundleDependencyEdges *edges,
                                   const char *from, const char *to);
void jce_bundle_edges_free(JceBundleDependencyEdges *edges);
bool jce_bundle_edges_normalize_and_sort(JceBundleDependencyEdges *edges);

void jce_bundle_edges_add_json(cJSON *asset, const char *owner,
                               const JceBundleDependencyEdges *edges);
void jce_bundle_edges_format_chain(const JceBundleDependencyEdges *edges,
                                   const char *preferred_root,
                                   const char *target,
                                   char *out, size_t out_cap);

#endif
