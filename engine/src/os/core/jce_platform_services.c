/*
 * jce_platform_services.c -- platform/online-service facade + LOCAL backend.
 *
 * The facade (JcePlatformServices) holds a backend vtable + opaque state and
 * forwards every public call to it.  The only backend shipped here is LOCAL:
 *   - achievements, stats, leaderboards live in one JSON file (services.json)
 *     under the data dir, loaded on create() and rewritten on flush().
 *   - cloud-save slots are raw byte blobs written one-file-each under
 *     <data_dir>/cloudsaves/<slot>.bin so a read returns the exact bytes.
 *
 * No third-party SDK is referenced; future Steam/EOS backends implement the
 * same JcePlatformBackend vtable in their own TUs.  C99, declare-at-top.
 */

#include <jce/os/core/jce_platform_services.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "platsvc"

/* ── Limits (LOCAL backend; fixed tables keep state pointer-stable) ───────── */

#define PSVC_MAX_ACH     256
#define PSVC_MAX_STATS   256
#define PSVC_MAX_BOARDS  64
#define PSVC_MAX_ENTRIES 256       /* per leaderboard */
#define PSVC_ID_LEN      96
#define PSVC_NAME_LEN    128
#define PSVC_USER_LEN    96
#define PSVC_PATH_LEN    1024

/* ── In-memory LOCAL backend state ───────────────────────────────────────── */

typedef struct {
    char id[PSVC_ID_LEN];
    char name[PSVC_NAME_LEN];
    int  target;        /* progress goal (>=1) */
    int  progress;      /* clamped [0,target]  */
    bool unlocked;
} PsvcAch;

typedef struct {
    char    id[PSVC_ID_LEN];
    int64_t value;
} PsvcStat;

typedef struct {
    char    user[PSVC_USER_LEN];
    int64_t score;
    uint32_t seq;       /* submission order for stable tie-break */
} PsvcEntry;

typedef struct {
    char      name[PSVC_ID_LEN];
    PsvcEntry entries[PSVC_MAX_ENTRIES];
    int       count;
} PsvcBoard;

typedef struct {
    char      data_dir[PSVC_PATH_LEN];
    char      state_path[PSVC_PATH_LEN];   /* services.json     */
    char      saves_dir[PSVC_PATH_LEN];    /* cloudsaves/       */
    char      user_id[PSVC_USER_LEN];

    PsvcAch   ach[PSVC_MAX_ACH];
    int       ach_count;
    PsvcStat  stats[PSVC_MAX_STATS];
    int       stat_count;
    PsvcBoard boards[PSVC_MAX_BOARDS];
    int       board_count;

    uint32_t  next_seq;
    bool      dirty;
} PsvcLocal;

/* ── Facade handle ───────────────────────────────────────────────────────── */

struct JcePlatformServices {
    JcePlatformBackend be;     /* copied vtable */
    void              *self;   /* backend state */
};

/* ── Small string helper ─────────────────────────────────────────────────── */

static void psvc_copy(char *dst, int cap, const char *src)
{
    int n = 0;
    if (cap <= 0) return;
    if (src) { while (src[n] && n < cap - 1) { dst[n] = src[n]; ++n; } }
    dst[n] = '\0';
}

/* Replace any character a filesystem might choke on with '_' so a slot/board
 * name maps to a safe single path component. */
static void psvc_sanitize(char *dst, int cap, const char *src)
{
    int n = 0;
    char c;
    if (cap <= 0) return;
    if (src) {
        while (src[n] && n < cap - 1) {
            c = src[n];
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|' || c < 32) {
                c = '_';
            }
            dst[n] = c;
            ++n;
        }
    }
    dst[n] = '\0';
}

/* ── LOCAL: table lookups ────────────────────────────────────────────────── */

static PsvcAch *local_find_ach(PsvcLocal *L, const char *id)
{
    int i;
    if (!id) return NULL;
    for (i = 0; i < L->ach_count; ++i)
        if (strcmp(L->ach[i].id, id) == 0) return &L->ach[i];
    return NULL;
}

static PsvcStat *local_find_stat(PsvcLocal *L, const char *id)
{
    int i;
    if (!id) return NULL;
    for (i = 0; i < L->stat_count; ++i)
        if (strcmp(L->stats[i].id, id) == 0) return &L->stats[i];
    return NULL;
}

static PsvcBoard *local_find_board(PsvcLocal *L, const char *name)
{
    int i;
    if (!name) return NULL;
    for (i = 0; i < L->board_count; ++i)
        if (strcmp(L->boards[i].name, name) == 0) return &L->boards[i];
    return NULL;
}

static PsvcBoard *local_board_get_or_add(PsvcLocal *L, const char *name)
{
    PsvcBoard *b = local_find_board(L, name);
    if (b) return b;
    if (L->board_count >= PSVC_MAX_BOARDS) return NULL;
    b = &L->boards[L->board_count++];
    memset(b, 0, sizeof *b);
    psvc_copy(b->name, sizeof b->name, name);
    return b;
}

/* ── LOCAL: JSON state load / save ───────────────────────────────────────── */

static void local_load_state(PsvcLocal *L)
{
    JceJson *root, *arr, *it, *entries, *e;
    int n, i, j, m;

    root = jce_json_parse_file(L->state_path);
    if (!root) return;   /* fresh / missing — defaults already zeroed */

    /* user id */
    {
        const char *uid = jce_json_get_string(root, "user_id", "");
        if (uid && uid[0]) psvc_copy(L->user_id, sizeof L->user_id, uid);
    }
    L->next_seq = (uint32_t)jce_json_get_int(root, "next_seq", 0);

    /* achievements */
    arr = jce_json_get(root, "achievements");
    if (arr && jce_json_is_array(arr)) {
        n = jce_json_array_size(arr);
        for (i = 0; i < n && L->ach_count < PSVC_MAX_ACH; ++i) {
            PsvcAch *a;
            it = jce_json_array_at(arr, i);
            if (!it) continue;
            a = &L->ach[L->ach_count++];
            memset(a, 0, sizeof *a);
            psvc_copy(a->id,   sizeof a->id,   jce_json_get_string(it, "id", ""));
            psvc_copy(a->name, sizeof a->name, jce_json_get_string(it, "name", ""));
            a->target   = jce_json_get_int(it, "target", 1);
            if (a->target < 1) a->target = 1;
            a->progress = jce_json_get_int(it, "progress", 0);
            a->unlocked = jce_json_get_bool(it, "unlocked", false);
        }
    }

    /* stats */
    arr = jce_json_get(root, "stats");
    if (arr && jce_json_is_array(arr)) {
        n = jce_json_array_size(arr);
        for (i = 0; i < n && L->stat_count < PSVC_MAX_STATS; ++i) {
            PsvcStat *s;
            it = jce_json_array_at(arr, i);
            if (!it) continue;
            s = &L->stats[L->stat_count++];
            memset(s, 0, sizeof *s);
            psvc_copy(s->id, sizeof s->id, jce_json_get_string(it, "id", ""));
            s->value = (int64_t)jce_json_get_number(it, "value", 0.0);
        }
    }

    /* leaderboards */
    arr = jce_json_get(root, "leaderboards");
    if (arr && jce_json_is_array(arr)) {
        n = jce_json_array_size(arr);
        for (i = 0; i < n && L->board_count < PSVC_MAX_BOARDS; ++i) {
            PsvcBoard *b;
            it = jce_json_array_at(arr, i);
            if (!it) continue;
            b = &L->boards[L->board_count++];
            memset(b, 0, sizeof *b);
            psvc_copy(b->name, sizeof b->name, jce_json_get_string(it, "name", ""));
            entries = jce_json_get(it, "entries");
            if (entries && jce_json_is_array(entries)) {
                m = jce_json_array_size(entries);
                for (j = 0; j < m && b->count < PSVC_MAX_ENTRIES; ++j) {
                    PsvcEntry *en;
                    e = jce_json_array_at(entries, j);
                    if (!e) continue;
                    en = &b->entries[b->count++];
                    psvc_copy(en->user, sizeof en->user, jce_json_get_string(e, "user", ""));
                    en->score = (int64_t)jce_json_get_number(e, "score", 0.0);
                    en->seq   = (uint32_t)jce_json_get_int(e, "seq", 0);
                }
            }
        }
    }

    jce_json_free(root);
}

static bool local_save_state(PsvcLocal *L)
{
    JceJson *root, *arr, *o, *entries, *eo;
    char *text;
    bool ok;
    int i, j;

    root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "user_id", L->user_id);
    jce_json_set_int(root, "next_seq", (int)L->next_seq);

    arr = jce_json_array();
    for (i = 0; i < L->ach_count; ++i) {
        o = jce_json_object();
        jce_json_set_string(o, "id",       L->ach[i].id);
        jce_json_set_string(o, "name",     L->ach[i].name);
        jce_json_set_int   (o, "target",   L->ach[i].target);
        jce_json_set_int   (o, "progress", L->ach[i].progress);
        jce_json_set_bool  (o, "unlocked", L->ach[i].unlocked);
        jce_json_array_push(arr, o);
    }
    jce_json_set_child(root, "achievements", arr);

    arr = jce_json_array();
    for (i = 0; i < L->stat_count; ++i) {
        o = jce_json_object();
        jce_json_set_string(o, "id",    L->stats[i].id);
        jce_json_set_number(o, "value", (double)L->stats[i].value);
        jce_json_array_push(arr, o);
    }
    jce_json_set_child(root, "stats", arr);

    arr = jce_json_array();
    for (i = 0; i < L->board_count; ++i) {
        o = jce_json_object();
        jce_json_set_string(o, "name", L->boards[i].name);
        entries = jce_json_array();
        for (j = 0; j < L->boards[i].count; ++j) {
            eo = jce_json_object();
            jce_json_set_string(eo, "user",  L->boards[i].entries[j].user);
            jce_json_set_number(eo, "score", (double)L->boards[i].entries[j].score);
            jce_json_set_int   (eo, "seq",   (int)L->boards[i].entries[j].seq);
            jce_json_array_push(entries, eo);
        }
        jce_json_set_child(o, "entries", entries);
        jce_json_array_push(arr, o);
    }
    jce_json_set_child(root, "leaderboards", arr);

    text = jce_json_print(root, true);
    jce_json_free(root);
    if (!text) return false;

    ok = jce_fs_host_write_all_atomic(L->state_path, text, (uint64_t)strlen(text));
    jce_json_free_string(text);
    if (ok) L->dirty = false;
    return ok;
}

/* ── LOCAL: cloud-save slot path ─────────────────────────────────────────── */

static bool local_slot_path(PsvcLocal *L, const char *slot, char *out, int cap)
{
    char safe[PSVC_USER_LEN];
    char file[PSVC_USER_LEN + 8];
    if (!slot || !slot[0]) return false;
    psvc_sanitize(safe, sizeof safe, slot);
    snprintf(file, sizeof file, "%s.bin", safe);
    return jce_path_join(out, (size_t)cap, L->saves_dir, file);
}

/* ── LOCAL backend vtable implementation ─────────────────────────────────── */

static void *local_create(const char *config)
{
    PsvcLocal *L;
    const char *dir = (config && config[0]) ? config : ".";

    L = (PsvcLocal *)jce_malloc(sizeof *L);
    if (!L) return NULL;
    memset(L, 0, sizeof *L);

    psvc_copy(L->data_dir, sizeof L->data_dir, dir);
    if (!jce_fs_host_create_directory(L->data_dir)) {
        LOG_ERROR(LOG_TAG, "cannot create data dir '%s'", L->data_dir);
        jce_free(L);
        return NULL;
    }
    jce_path_join(L->state_path, sizeof L->state_path, L->data_dir, "services.json");
    jce_path_join(L->saves_dir,  sizeof L->saves_dir,  L->data_dir, "cloudsaves");
    jce_fs_host_create_directory(L->saves_dir);

    /* Deterministic default user id derived from the data dir basename, so a
     * given install path reproduces the same id without any randomness. */
    {
        char base[PSVC_USER_LEN];
        if (!jce_path_basename(base, sizeof base, L->data_dir) || !base[0])
            psvc_copy(base, sizeof base, "player");
        snprintf(L->user_id, sizeof L->user_id, "local:%s", base);
    }

    local_load_state(L);   /* may overwrite user_id with the persisted one */
    return L;
}

static void local_destroy(void *self)
{
    if (self) jce_free(self);
}

static bool local_user_id(void *self, char *out, int cap)
{
    PsvcLocal *L = (PsvcLocal *)self;
    if (!L || !out || cap <= 0) return false;
    psvc_copy(out, cap, L->user_id);
    return true;
}

static bool local_ach_define(void *self, const char *id, const char *name, int target)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcAch *a;
    if (!L || !id || !id[0]) return false;
    a = local_find_ach(L, id);
    if (a) {                                  /* idempotent: refresh metadata */
        psvc_copy(a->name, sizeof a->name, name ? name : a->name);
        if (target >= 1) a->target = target;
        L->dirty = true;
        return true;
    }
    if (L->ach_count >= PSVC_MAX_ACH) return false;
    a = &L->ach[L->ach_count++];
    memset(a, 0, sizeof *a);
    psvc_copy(a->id,   sizeof a->id,   id);
    psvc_copy(a->name, sizeof a->name, name ? name : "");
    a->target   = (target >= 1) ? target : 1;
    a->progress = 0;
    a->unlocked = false;
    L->dirty = true;
    return true;
}

static bool local_ach_unlock(void *self, const char *id)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcAch *a;
    if (!L || !id || !id[0]) return false;
    a = local_find_ach(L, id);
    if (!a) {                                 /* auto-define a boolean ach */
        if (!local_ach_define(self, id, id, 1)) return false;
        a = local_find_ach(L, id);
        if (!a) return false;
    }
    if (a->unlocked) return true;             /* idempotent */
    a->unlocked = true;
    a->progress = a->target;
    L->dirty = true;
    return true;
}

static bool local_ach_is_unlocked(void *self, const char *id)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcAch *a = local_find_ach(L, id);
    (void)self;
    return a ? a->unlocked : false;
}

static bool local_ach_set_progress(void *self, const char *id, int current)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcAch *a;
    if (!L || !id || !id[0]) return false;
    a = local_find_ach(L, id);
    if (!a) return false;
    if (current < 0) current = 0;
    if (current > a->target) current = a->target;
    a->progress = current;
    if (a->progress >= a->target) a->unlocked = true;
    L->dirty = true;
    return true;
}

static int local_ach_progress(void *self, const char *id)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcAch *a = local_find_ach(L, id);
    (void)self;
    return a ? a->progress : 0;
}

static bool local_stat_set(void *self, const char *id, int64_t value)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcStat *s;
    if (!L || !id || !id[0]) return false;
    s = local_find_stat(L, id);
    if (!s) {
        if (L->stat_count >= PSVC_MAX_STATS) return false;
        s = &L->stats[L->stat_count++];
        memset(s, 0, sizeof *s);
        psvc_copy(s->id, sizeof s->id, id);
    }
    s->value = value;
    L->dirty = true;
    return true;
}

static int64_t local_stat_get(void *self, const char *id, int64_t def)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcStat *s = local_find_stat(L, id);
    (void)self;
    return s ? s->value : def;
}

static bool local_lb_submit(void *self, const char *board, const char *user, int64_t score)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcBoard *b;
    PsvcEntry *en;
    int i;
    if (!L || !board || !board[0] || !user || !user[0]) return false;
    b = local_board_get_or_add(L, board);
    if (!b) return false;
    for (i = 0; i < b->count; ++i) {          /* keep best score per user */
        if (strcmp(b->entries[i].user, user) == 0) {
            if (score > b->entries[i].score) {
                b->entries[i].score = score;
                b->entries[i].seq   = L->next_seq++;
                L->dirty = true;
            }
            return true;
        }
    }
    if (b->count >= PSVC_MAX_ENTRIES) return false;
    en = &b->entries[b->count++];
    psvc_copy(en->user, sizeof en->user, user);
    en->score = score;
    en->seq   = L->next_seq++;
    L->dirty = true;
    return true;
}

static int local_lb_query_top(void *self, const char *board, int cap,
                              int64_t *out_scores, char *out_users, int user_stride)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcBoard *b;
    PsvcEntry sorted[PSVC_MAX_ENTRIES];
    int i, j, n;
    if (!L || cap <= 0) return 0;
    b = local_find_board(L, board);
    if (!b || b->count == 0) return 0;

    /* Stable selection sort: score desc, then submission order (seq) asc. */
    n = b->count;
    for (i = 0; i < n; ++i) sorted[i] = b->entries[i];
    for (i = 0; i < n - 1; ++i) {
        int best = i;
        for (j = i + 1; j < n; ++j) {
            if (sorted[j].score > sorted[best].score ||
                (sorted[j].score == sorted[best].score && sorted[j].seq < sorted[best].seq)) {
                best = j;
            }
        }
        if (best != i) {
            PsvcEntry t = sorted[i];
            sorted[i] = sorted[best];
            sorted[best] = t;
        }
    }

    if (n > cap) n = cap;
    for (i = 0; i < n; ++i) {
        if (out_scores) out_scores[i] = sorted[i].score;
        if (out_users && user_stride > 0)
            psvc_copy(out_users + (size_t)i * (size_t)user_stride, user_stride, sorted[i].user);
    }
    return n;
}

static bool local_save_write(void *self, const char *slot, const void *data, uint32_t size)
{
    PsvcLocal *L = (PsvcLocal *)self;
    char path[PSVC_PATH_LEN];
    if (!L || !slot || !slot[0]) return false;
    if (!local_slot_path(L, slot, path, sizeof path)) return false;
    return jce_fs_host_write_all_atomic(path, data ? data : "", (uint64_t)size);
}

static bool local_save_read(void *self, const char *slot, void *out, uint32_t cap, uint32_t *out_size)
{
    PsvcLocal *L = (PsvcLocal *)self;
    char path[PSVC_PATH_LEN];
    void *buf;
    uint64_t sz = 0;
    if (out_size) *out_size = 0;
    if (!L || !slot || !slot[0]) return false;
    if (!local_slot_path(L, slot, path, sizeof path)) return false;
    buf = jce_fs_host_read_all(path, &sz);
    if (!buf) return false;
    if (out_size) *out_size = (uint32_t)sz;
    if (!out || cap < (uint32_t)sz) { jce_fs_buffer_free(buf); return false; }
    if (sz > 0) memcpy(out, buf, (size_t)sz);
    jce_fs_buffer_free(buf);
    return true;
}

static bool local_save_exists(void *self, const char *slot)
{
    PsvcLocal *L = (PsvcLocal *)self;
    char path[PSVC_PATH_LEN];
    if (!L || !slot || !slot[0]) return false;
    if (!local_slot_path(L, slot, path, sizeof path)) return false;
    return jce_fs_host_exists_file(path);
}

static bool local_save_delete(void *self, const char *slot)
{
    PsvcLocal *L = (PsvcLocal *)self;
    char path[PSVC_PATH_LEN];
    if (!L || !slot || !slot[0]) return false;
    if (!local_slot_path(L, slot, path, sizeof path)) return false;
    return jce_fs_host_remove_file(path);
}

/* save_list directory-walk context. */
typedef struct {
    char *out;
    int   cap;
    int   stride;
    int   count;
} PsvcListCtx;

static bool local_list_cb(const char *name, bool is_dir, void *user)
{
    PsvcListCtx *ctx = (PsvcListCtx *)user;
    size_t len, stem;     /* stem = bytes before ".bin" */
    int    cap;
    char   slot[PSVC_USER_LEN];
    if (is_dir || !name) return true;
    len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".bin") != 0) return true;   /* only *.bin */
    stem = len - 4;
    /* psvc_copy copies up to cap-1 chars; cap = stem+1 yields exactly `stem`,
     * clamped to the slot buffer. */
    cap = (int)stem + 1;
    if (cap > (int)sizeof slot) cap = (int)sizeof slot;
    psvc_copy(slot, cap, name);
    if (ctx->out && ctx->count < ctx->cap && ctx->stride > 0)
        psvc_copy(ctx->out + (size_t)ctx->count * (size_t)ctx->stride, ctx->stride, slot);
    ctx->count++;
    return true;
}

static int local_save_list(void *self, char *out_names, int cap, int name_stride)
{
    PsvcLocal *L = (PsvcLocal *)self;
    PsvcListCtx ctx;
    if (!L) return 0;
    ctx.out = out_names;
    ctx.cap = cap;
    ctx.stride = name_stride;
    ctx.count = 0;
    jce_fs_host_list_dir(L->saves_dir, local_list_cb, &ctx);
    return ctx.count;
}

static bool local_flush(void *self)
{
    PsvcLocal *L = (PsvcLocal *)self;
    if (!L) return false;
    return local_save_state(L);
}

static const JcePlatformBackend k_local_backend = {
    JCE_PLATFORM_BACKEND_LOCAL,
    "local",
    local_create,
    local_destroy,
    local_user_id,
    local_ach_define,
    local_ach_unlock,
    local_ach_is_unlocked,
    local_ach_set_progress,
    local_ach_progress,
    local_stat_set,
    local_stat_get,
    local_lb_submit,
    local_lb_query_top,
    local_save_write,
    local_save_read,
    local_save_exists,
    local_save_delete,
    local_save_list,
    local_flush
};

/* ── Facade ──────────────────────────────────────────────────────────────── */

JcePlatformServices *jce_platform_init_backend(const JcePlatformBackend *backend,
                                               const char *config)
{
    JcePlatformServices *svc;
    if (!backend || !backend->create) return NULL;
    svc = (JcePlatformServices *)jce_malloc(sizeof *svc);
    if (!svc) return NULL;
    svc->be   = *backend;
    svc->self = backend->create(config);
    if (!svc->self) {
        jce_free(svc);
        return NULL;
    }
    return svc;
}

JcePlatformServices *jce_platform_init_local(const char *data_dir)
{
    return jce_platform_init_backend(&k_local_backend, data_dir);
}

void jce_platform_shutdown(JcePlatformServices *svc)
{
    if (!svc) return;
    if (svc->be.flush)   svc->be.flush(svc->self);
    if (svc->be.destroy) svc->be.destroy(svc->self);
    jce_free(svc);
}

bool jce_platform_flush(JcePlatformServices *svc)
{
    if (!svc || !svc->be.flush) return false;
    return svc->be.flush(svc->self);
}

JcePlatformBackendKind jce_platform_backend_kind(const JcePlatformServices *svc)
{
    return svc ? svc->be.kind : JCE_PLATFORM_BACKEND_LOCAL;
}

const char *jce_platform_backend_name(const JcePlatformServices *svc)
{
    return (svc && svc->be.name) ? svc->be.name : "";
}

bool jce_platform_user_id(JcePlatformServices *svc, char *out, int cap)
{
    if (!svc || !svc->be.user_id) return false;
    return svc->be.user_id(svc->self, out, cap);
}

bool jce_platform_ach_define(JcePlatformServices *svc, const char *id, const char *name, int target)
{
    if (!svc || !svc->be.ach_define) return false;
    return svc->be.ach_define(svc->self, id, name, target);
}

bool jce_platform_ach_unlock(JcePlatformServices *svc, const char *id)
{
    if (!svc || !svc->be.ach_unlock) return false;
    return svc->be.ach_unlock(svc->self, id);
}

bool jce_platform_ach_is_unlocked(JcePlatformServices *svc, const char *id)
{
    if (!svc || !svc->be.ach_is_unlocked) return false;
    return svc->be.ach_is_unlocked(svc->self, id);
}

bool jce_platform_ach_set_progress(JcePlatformServices *svc, const char *id, int current)
{
    if (!svc || !svc->be.ach_set_progress) return false;
    return svc->be.ach_set_progress(svc->self, id, current);
}

int jce_platform_ach_progress(JcePlatformServices *svc, const char *id)
{
    if (!svc || !svc->be.ach_progress) return 0;
    return svc->be.ach_progress(svc->self, id);
}

bool jce_platform_stat_set(JcePlatformServices *svc, const char *id, int64_t value)
{
    if (!svc || !svc->be.stat_set) return false;
    return svc->be.stat_set(svc->self, id, value);
}

int64_t jce_platform_stat_get(JcePlatformServices *svc, const char *id, int64_t def)
{
    if (!svc || !svc->be.stat_get) return def;
    return svc->be.stat_get(svc->self, id, def);
}

bool jce_platform_lb_submit(JcePlatformServices *svc, const char *board, const char *user, int64_t score)
{
    if (!svc || !svc->be.lb_submit) return false;
    return svc->be.lb_submit(svc->self, board, user, score);
}

int jce_platform_lb_query_top(JcePlatformServices *svc, const char *board, int cap,
                              int64_t *out_scores, char *out_users, int user_stride)
{
    if (!svc || !svc->be.lb_query_top) return 0;
    return svc->be.lb_query_top(svc->self, board, cap, out_scores, out_users, user_stride);
}

bool jce_platform_save_write(JcePlatformServices *svc, const char *slot, const void *data, uint32_t size)
{
    if (!svc || !svc->be.save_write) return false;
    return svc->be.save_write(svc->self, slot, data, size);
}

bool jce_platform_save_read(JcePlatformServices *svc, const char *slot, void *out, uint32_t cap, uint32_t *out_size)
{
    if (!svc || !svc->be.save_read) return false;
    return svc->be.save_read(svc->self, slot, out, cap, out_size);
}

bool jce_platform_save_exists(JcePlatformServices *svc, const char *slot)
{
    if (!svc || !svc->be.save_exists) return false;
    return svc->be.save_exists(svc->self, slot);
}

bool jce_platform_save_delete(JcePlatformServices *svc, const char *slot)
{
    if (!svc || !svc->be.save_delete) return false;
    return svc->be.save_delete(svc->self, slot);
}

int jce_platform_save_list(JcePlatformServices *svc, char *out_names, int cap, int name_stride)
{
    if (!svc || !svc->be.save_list) return 0;
    return svc->be.save_list(svc->self, out_names, cap, name_stride);
}
