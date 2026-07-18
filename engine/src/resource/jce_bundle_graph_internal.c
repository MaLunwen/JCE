#include "jce_bundle_graph_internal.h"

#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_bundle_format.h>

#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define JCE_BUNDLE_GRAPH_PATH_CAP 2048u

static char *graph_strdup(const char *value)
{
    size_t size = strlen(value) + 1;
    char *copy = (char *)JCE_MALLOC(size);
    if (copy)
        memcpy(copy, value, size);
    return copy;
}

bool jce_bundle_edges_add(JceBundleDependencyEdges *edges, const char *from,
                          const char *to, const char *origin)
{
    if (!edges || !from || !from[0] || !to || !to[0])
        return true;
    if (!origin || !origin[0])
        origin = "unknown";
    for (size_t i = 0; i < edges->n; ++i) {
        const JceBundleDependencyEdge *edge = &edges->items[i];
        if (strcmp(edge->from, from) == 0 && strcmp(edge->to, to) == 0 &&
            strcmp(edge->origin, origin) == 0)
            return true;
    }

    if (edges->n == edges->c) {
        size_t cap = edges->c ? edges->c * 2 : 64;
        JceBundleDependencyEdge *items =
            (JceBundleDependencyEdge *)JCE_REALLOC(
                edges->items, cap * sizeof(*items));
        if (!items)
            return false;
        edges->items = items;
        edges->c = cap;
    }

    JceBundleDependencyEdge *edge = &edges->items[edges->n];
    edge->from = graph_strdup(from);
    edge->to = graph_strdup(to);
    edge->origin = graph_strdup(origin);
    if (!edge->from || !edge->to || !edge->origin) {
        JCE_FREE(edge->from);
        JCE_FREE(edge->to);
        JCE_FREE(edge->origin);
        return false;
    }
    ++edges->n;
    return true;
}

bool jce_bundle_edges_replace_path(JceBundleDependencyEdges *edges,
                                   const char *from, const char *to)
{
    if (!edges || !from || !to || strcmp(from, to) == 0)
        return true;
    for (size_t i = 0; i < edges->n; ++i) {
        JceBundleDependencyEdge *edge = &edges->items[i];
        char **fields[2] = { &edge->from, &edge->to };
        for (size_t j = 0; j < 2; ++j) {
            if (strcmp(*fields[j], from) != 0)
                continue;
            char *replacement = graph_strdup(to);
            if (!replacement)
                return false;
            JCE_FREE(*fields[j]);
            *fields[j] = replacement;
        }
    }
    return true;
}

void jce_bundle_edges_free(JceBundleDependencyEdges *edges)
{
    if (!edges)
        return;
    for (size_t i = 0; i < edges->n; ++i) {
        JCE_FREE(edges->items[i].from);
        JCE_FREE(edges->items[i].to);
        JCE_FREE(edges->items[i].origin);
    }
    JCE_FREE(edges->items);
    edges->items = NULL;
    edges->n = edges->c = 0;
}

static int edge_compare(const JceBundleDependencyEdge *a,
                        const JceBundleDependencyEdge *b)
{
    int cmp = strcmp(a->from, b->from);
    if (cmp != 0)
        return cmp;
    cmp = strcmp(a->to, b->to);
    if (cmp != 0)
        return cmp;
    return strcmp(a->origin, b->origin);
}

bool jce_bundle_edges_normalize_and_sort(JceBundleDependencyEdges *edges)
{
    JceBundleDependencyEdges normalized = {0};
    char from[JCE_BUNDLE_GRAPH_PATH_CAP];
    char to[JCE_BUNDLE_GRAPH_PATH_CAP];

    for (size_t i = 0; i < edges->n; ++i) {
        const JceBundleDependencyEdge *edge = &edges->items[i];
        if (jce_archive_normalize_path(edge->from, from, sizeof(from)) == 0 ||
            jce_archive_normalize_path(edge->to, to, sizeof(to)) == 0) {
            jce_bundle_edges_free(&normalized);
            return false;
        }
        if (strcmp(from, to) != 0 &&
            !jce_bundle_edges_add(&normalized, from, to, edge->origin)) {
            jce_bundle_edges_free(&normalized);
            return false;
        }
    }

    for (size_t i = 1; i < normalized.n; ++i) {
        for (size_t j = i; j > 0 &&
             edge_compare(&normalized.items[j - 1],
                          &normalized.items[j]) > 0; --j) {
            JceBundleDependencyEdge swap = normalized.items[j];
            normalized.items[j] = normalized.items[j - 1];
            normalized.items[j - 1] = swap;
        }
    }

    jce_bundle_edges_free(edges);
    *edges = normalized;
    return true;
}

void jce_bundle_edges_add_json(cJSON *asset, const char *owner,
                               const JceBundleDependencyEdges *edges)
{
    cJSON *dependencies = cJSON_AddArrayToObject(
        asset, JCE_BUNDLE_KEY_DEPENDENCIES);
    if (!edges)
        return;

    for (size_t i = 0; i < edges->n; ++i) {
        const JceBundleDependencyEdge *edge = &edges->items[i];
        if (strcmp(edge->from, owner) != 0)
            continue;
        cJSON *dependency = cJSON_CreateObject();
        cJSON_AddStringToObject(dependency,
                                JCE_BUNDLE_KEY_ASSET_ADDRESS, edge->to);
        char asset_id[17];
        snprintf(asset_id, sizeof(asset_id), "%016llx",
                 (unsigned long long)jce_archive_hash_normalized(
                     edge->to, strlen(edge->to)));
        cJSON_AddStringToObject(dependency, JCE_BUNDLE_KEY_ASSET_ID,
                                asset_id);
        cJSON_AddStringToObject(dependency, JCE_BUNDLE_KEY_ORIGIN,
                                edge->origin);
        cJSON_AddItemToArray(dependencies, dependency);
    }
}

typedef struct DependencyChainNode {
    const char *address;
    size_t parent;
    const char *origin;
} DependencyChainNode;

static bool chain_contains(const DependencyChainNode *nodes, size_t count,
                           const char *address)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(nodes[i].address, address) == 0)
            return true;
    }
    return false;
}

static bool edge_has_incoming(const JceBundleDependencyEdges *edges,
                              const char *address)
{
    for (size_t i = 0; i < edges->n; ++i) {
        if (strcmp(edges->items[i].to, address) == 0)
            return true;
    }
    return false;
}

static void chain_append(char *out, size_t cap, size_t *length,
                         const char *fmt, ...)
{
    if (*length >= cap)
        return;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *length, cap - *length, fmt, args);
    va_end(args);
    if (written < 0)
        return;
    size_t available = cap - *length;
    *length += (size_t)written < available ? (size_t)written : available - 1;
}

void jce_bundle_edges_format_chain(const JceBundleDependencyEdges *edges,
                                   const char *preferred_root,
                                   const char *target,
                                   char *out, size_t out_cap)
{
    if (!out || out_cap == 0)
        return;
    out[0] = '\0';
    if (!edges || !target || !target[0])
        return;

    size_t max_nodes = edges->n * 2 + 1;
    DependencyChainNode *nodes = (DependencyChainNode *)JCE_MALLOC(
        max_nodes * sizeof(*nodes));
    size_t *path = (size_t *)JCE_MALLOC(max_nodes * sizeof(*path));
    if (!nodes || !path) {
        JCE_FREE(nodes);
        JCE_FREE(path);
        snprintf(out, out_cap, "%s", target);
        return;
    }

    size_t count = 0;
    if (preferred_root && preferred_root[0]) {
        nodes[count++] = (DependencyChainNode){
            preferred_root, (size_t)-1, NULL
        };
    }
    for (size_t i = 0; i < edges->n; ++i) {
        const char *candidate = edges->items[i].from;
        if (!edge_has_incoming(edges, candidate) &&
            !chain_contains(nodes, count, candidate)) {
            nodes[count++] = (DependencyChainNode){
                candidate, (size_t)-1, NULL
            };
        }
    }
    if (count == 0) {
        for (size_t i = 0; i < edges->n; ++i) {
            const char *candidate = edges->items[i].from;
            if (!chain_contains(nodes, count, candidate)) {
                nodes[count++] = (DependencyChainNode){
                    candidate, (size_t)-1, NULL
                };
            }
        }
    }

    size_t found = (size_t)-1;
    for (size_t cursor = 0; cursor < count && found == (size_t)-1;
         ++cursor) {
        if (strcmp(nodes[cursor].address, target) == 0) {
            found = cursor;
            break;
        }
        for (size_t i = 0; i < edges->n; ++i) {
            const JceBundleDependencyEdge *edge = &edges->items[i];
            if (strcmp(edge->from, nodes[cursor].address) != 0)
                continue;
            if (strcmp(edge->to, target) == 0) {
                nodes[count] = (DependencyChainNode){
                    edge->to, cursor, edge->origin
                };
                found = count++;
                break;
            }
            if (count < max_nodes &&
                !chain_contains(nodes, count, edge->to)) {
                nodes[count++] = (DependencyChainNode){
                    edge->to, cursor, edge->origin
                };
            }
        }
    }

    if (found == (size_t)-1) {
        snprintf(out, out_cap, "%s", target);
    } else {
        size_t path_count = 0;
        for (size_t at = found; at != (size_t)-1 && path_count < max_nodes;
             at = nodes[at].parent) {
            path[path_count++] = at;
        }
        size_t length = 0;
        for (size_t i = path_count; i > 0; --i) {
            const DependencyChainNode *node = &nodes[path[i - 1]];
            if (i == path_count)
                chain_append(out, out_cap, &length, "%s", node->address);
            else
                chain_append(out, out_cap, &length, " --%s--> %s",
                             node->origin ? node->origin : "unknown",
                             node->address);
        }
    }

    JCE_FREE(path);
    JCE_FREE(nodes);
}
