#ifndef JCE_BUNDLE_CONTRACT_INTERNAL_H
#define JCE_BUNDLE_CONTRACT_INTERNAL_H

#include "jce_bundle_graph_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

typedef struct StrVec {
    char **items;
    size_t n;
    size_t c;
} StrVec;

typedef enum CookClass {
    COOK_CLASS_NONE = 0,
    COOK_CLASS_TEXTURE,
    COOK_CLASS_MODEL,
    COOK_CLASS_AUDIO
} CookClass;

typedef struct Bundle {
    char *id;
    char *kind;
    char *scene_path;
    char *scene_src_path;
    StrVec assets;
    StrVec deps;
} Bundle;

typedef struct PakEntry {
    char *vpath;
    uint8_t *raw;
    size_t raw_size;
    uint64_t content_hash;
} PakEntry;

typedef struct ReportEntry {
    char *path;
    char *hash;
    uint64_t size;
} ReportEntry;

typedef struct ReportEntryVec {
    ReportEntry *items;
    size_t n;
    size_t c;
} ReportEntryVec;

typedef struct CatalogEntry {
    char *id;
    char *file;
    char *kind;
    char *scene_path;
    char *content_hash;
    char *build_hash;
    uint64_t size;
    bool encrypted;
    StrVec deps;
    ReportEntryVec entries;
} CatalogEntry;

typedef struct CatalogVec {
    CatalogEntry *items;
    size_t n;
    size_t c;
} CatalogVec;

#define JCE_BUILD_REPORT_NAME "build_report.json"

CookClass jce_bundle_classify_cook(const char *path);

ReportEntry *jce_bundle_report_entry_create(ReportEntryVec *entries);
CatalogEntry *jce_bundle_catalog_entry_create(CatalogVec *catalog);
void jce_bundle_catalog_entries_free(CatalogVec *catalog);

char *jce_bundle_build_manifest(const Bundle *bundle,
                                const PakEntry *entries,
                                size_t entry_count,
                                uint64_t build_hash,
                                uint32_t version,
                                bool encrypted,
                                bool cooked,
                                int target_platform,
                                const JceBundleDependencyEdges *edges,
                                size_t *out_len);
char *jce_bundle_build_sidecar(const char *manifest, size_t manifest_size,
                               uint64_t content_hash,
                               uint64_t archive_size,
                               size_t *out_len);
char *jce_bundle_build_catalog_json(const CatalogVec *catalog,
                                    uint32_t version,
                                    bool cooked,
                                    int target_platform,
                                    size_t *out_len);
char *jce_bundle_build_graph_json(const CatalogVec *catalog,
                                  bool cooked,
                                  int target_platform,
                                  const JceBundleDependencyEdges *edges,
                                  size_t *out_len);
char *jce_bundle_build_report_json(const CatalogVec *catalog,
                                   size_t *out_len);

const char *jce_bundle_previous_content_hash(const cJSON *catalog,
                                             const char *id);
const char *jce_bundle_previous_build_hash(const cJSON *catalog,
                                           const char *id);
const char *jce_bundle_previous_file(const cJSON *catalog, const char *id);

#endif
