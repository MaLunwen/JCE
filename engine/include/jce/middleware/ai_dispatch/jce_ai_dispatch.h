/*
 * jce_ai_dispatch.h -- AI dispatch middleware (L4): provider-agnostic
 * constraint schemas, self-contained constraint records, and the
 * in-engine deterministic solver.
 *
 * DETERMINISM CONTRACT (keep this paragraph verbatim -- spec F.6):
 *   Records are self-contained (the payload carries every value the
 *   solver consumes).  Replay and network sync only re-play records and
 *   re-solve them; providers are never re-queried.  Therefore any drift
 *   in a calibration table never affects the replay result of an
 *   existing record -- tables only shape future generations.
 *
 * WARNING: `context` key/values passed to acquisition APIs are defined
 * by the game.  Never put player personal data into context (spec K).
 *
 * Layer: L4 middleware.  Depends only on os/core (L1).
 * Threading: main thread only (v1), matching engine conventions.
 * Build: compiled only when JCE_ENABLE_AI_DISPATCH is ON (default OFF).
 */
#ifndef JCE_AI_DISPATCH_H
#define JCE_AI_DISPATCH_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_rand.h>

#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ---- limits / protocol versions ------------------------------------ */
#define JCE_AID_MAX_FIELDS      32u  /* fields per schema */
#define JCE_AID_MAX_PAYLOAD     256u /* bytes; 32 fields x 8B worst case */
#define JCE_AID_MAX_RECORD_SIZE 512u /* encoded record hard cap (spec E.2) */
#define JCE_AID_FMT_VERSION     1u   /* record wire-format version */
#define JCE_AID_SOLVER_VERSION  1u   /* deterministic solver behaviour version */

/* ---- results -------------------------------------------------------- */
typedef enum JceAidResult {
    JCE_AID_OK = 0,
    JCE_AID_ERR_INVALID_ARG,
    JCE_AID_ERR_NOT_INIT,
    JCE_AID_ERR_NOT_FOUND,     /* unknown schema id */
    JCE_AID_ERR_DUPLICATE,     /* schema name:version already registered */
    JCE_AID_ERR_LIMIT,         /* fixed capacity exceeded */
    JCE_AID_ERR_BUFFER_SMALL,  /* encode: caller buffer too small */
    JCE_AID_ERR_BAD_FORMAT,    /* decode: magic/fmt/length mismatch */
    JCE_AID_ERR_CHECKSUM,      /* decode: tamper/corruption -- refuse */
    JCE_AID_ERR_VERSION        /* solve: unsupported solver_ver -- never
                                  silently approximate (spec E.3) */
} JceAidResult;

/* ---- schema (spec E.1) ---------------------------------------------- */
typedef enum JceAidFieldType {
    JCE_AID_F_I32 = 0, /* signed integer, bounds [min_q, max_q] */
    JCE_AID_F_F32Q,    /* float semantics carried as Q16.16; bounds are Q16.16 */
    JCE_AID_F_ENUM,    /* enum_names[enum_count] */
    JCE_AID_F_COLOR8,  /* RGBA8 packed u32 */
    JCE_AID_F_TAGSET   /* bitset of <=64 tags; enum_names reused as tag names */
} JceAidFieldType;

typedef struct JceAidField {
    const char*        name;
    JceAidFieldType    type;
    int32_t            min_q, max_q;  /* I32/F32Q only */
    uint16_t           enum_count;    /* ENUM/TAGSET only */
    const char* const* enum_names;
} JceAidField;

typedef struct JceAidSchema {
    const char*        name;     /* e.g. "grass_field" */
    uint16_t           version;  /* schema evolution version */
    uint16_t           field_count;
    const JceAidField* fields;
} JceAidSchema;

typedef uint64_t JceAidSchemaId; /* = 64-bit xxHash of "<name>:<version>" */

/* ---- record (spec E.2; in-memory form -- wire form via encode) ------ */
typedef struct JceAidRecord {
    JceAidSchemaId schema_id;
    uint16_t       schema_ver;
    uint64_t       record_id;   /* hi 48 = first-seen tick, lo 16 = counter */
    uint64_t       tick;        /* fixed-clock tick at generation time */
    uint8_t        tier;        /* 1/2/3 */
    uint16_t       solver_ver;  /* expected solver version */
    uint16_t       calib_ver;   /* provenance only -- NEVER read by solve */
    uint64_t       seed;        /* solve seed */
    uint16_t       payload_len;
    uint8_t        payload[JCE_AID_MAX_PAYLOAD]; /* schema field order,
        tightly packed: I32/F32Q -> i32, ENUM -> u16, COLOR8 -> u32,
        TAGSET -> u64, little-endian */
} JceAidRecord;

/* ---- solved output (spec G) ----------------------------------------- */
typedef struct JceAidSolvedField {
    JceAidFieldType type;
    union {
        float    f;          /* F32Q: exact Q16.16 -> float mapping */
        int32_t  i;          /* I32 */
        uint16_t enum_index; /* ENUM */
        uint32_t color;      /* COLOR8 */
        uint64_t tags;       /* TAGSET */
    } v;
} JceAidSolvedField;

typedef struct JceAidSolved {
    uint16_t          field_count;
    JceAidSolvedField fields[JCE_AID_MAX_FIELDS];
} JceAidSolved;

/* ---- config (spec F.5; network fields consumed from P2 on) ---------- */
typedef enum JceAidMode {
    JCE_AID_MODE_AUTO = 0,
    JCE_AID_MODE_FORCE_T1,
    JCE_AID_MODE_FORCE_T2,
    JCE_AID_MODE_FORCE_T3
} JceAidMode;

typedef struct JceAidConfig {
    JceAidMode  mode;
    const char* gateway_url;
    const char* gateway_token;  /* injected by host; never persisted */
    const char* local_url;      /* NULL = default 127.0.0.1:11434 */
    const char* local_model;    /* NULL = default "llama3" (T2 chat model) */
    uint32_t    t1_timeout_ms;  /* 0 = default 1500 */
    uint32_t    t2_timeout_ms;  /* 0 = default 4000 */
    uint32_t    mem_floor_mb;   /* below this -> straight to T3 */
} JceAidConfig;

/* Game-defined context passed with acquisition requests.  String values
 * only; semantics belong to the game.  WARNING: never put player
 * personal data into context (spec K). */
typedef struct JceAidContextKV {
    const char* key;
    const char* value;
} JceAidContextKV;

/* ---- lifecycle ------------------------------------------------------- */
/* NULL cfg = all defaults (offline FORCE_T3-like behaviour with no
 * transport installed).  Calling init twice is a no-op returning
 * JCE_AID_OK; shutdown when not initialised is a safe no-op.
 * DEBUG TOGGLES: JCE_AID_RECORD=<file.jarc> / JCE_AID_REPLAY=<file.jarc>
 * env vars auto-open the record/replay stream, mirroring the engine's
 * JCE_INPUT_RECORD / JCE_INPUT_REPLAY pair. */
JCE_API JceAidResult JCE_CALL jce_aid_init(const JceAidConfig* cfg);
JCE_API void         JCE_CALL jce_aid_shutdown(void);
JCE_API JceBool      JCE_CALL jce_aid_initialised(void);

/* ---- P2: async acquisition (spec I) ----------------------------------- */
typedef uint32_t JceAidHandle;   /* 0 = invalid */

typedef enum JceAidStatus {
    JCE_AID_PENDING = 0,  /* queued or running on the worker */
    JCE_AID_READY,        /* pumped; take_record may consume it */
    JCE_AID_FAILED        /* invalid handle / discarded (stale generation) */
} JceAidStatus;

/* Fire-and-poll: returns immediately.  record_id/tick are stamped HERE
 * (main thread, request time); the worker only acquires the payload.
 * Queue-full requests fall straight to a synchronous T3 result (spec J:
 * never wait).  Returns 0 when replay is active (acquisition ban) or no
 * result slot is available. */
JCE_API JceAidHandle JCE_CALL
jce_aid_request(JceAidSchemaId id, const JceAidContextKV* kv, uint32_t kv_count);

JCE_API JceAidStatus JCE_CALL jce_aid_status(JceAidHandle h);

/* READY only; the handle dies with the take. */
JCE_API JceAidResult JCE_CALL
jce_aid_take_record(JceAidHandle h, JceAidRecord* out);

/* Main-thread, once per frame: moves completed acquisitions into READY,
 * publishing each through the record log / stream / bound event bus and
 * discarding results whose generation token went stale.  max_results==0
 * uses the default budget (2).  A 0.5 ms time budget applies either way.
 * Returns the number of results consumed. */
JCE_API uint32_t JCE_CALL jce_aid_pump(uint32_t max_results);

/* Bus used by jce_aid_pump/replay to publish JCE_EVT_AID_RECORD. */
JCE_API void JCE_CALL jce_aid_bind_bus(struct jce_event_bus* bus);

/* world/scene switch: results requested under an older token are
 * discarded at pump time (spec H). */
JCE_API void JCE_CALL jce_aid_bump_generation(void);

/* Engine main-loop hook (one call per frame, main thread): pumps the
 * replay stream at input parity when active, otherwise pumps completed
 * acquisitions.  Returns records delivered. */
JCE_API uint32_t JCE_CALL
jce_aid_engine_tick(uint64_t up_to_tick, uint32_t max_records);

/* ---- schema registry ------------------------------------------------- */
JCE_API JceAidResult JCE_CALL
jce_aid_register_schema(const JceAidSchema* s, JceAidSchemaId* out_id);

/* Pure function: 64-bit xxHash of "<name>:<version>" (no registry access). */
JCE_API JceAidSchemaId JCE_CALL jce_aid_schema_id(const char* name, uint16_t version);

/* Registry-owned deep copy; NULL if unknown.  Pointer valid until shutdown. */
JCE_API const JceAidSchema* JCE_CALL jce_aid_schema_get(JceAidSchemaId id);

/* ---- T3 synchronous local generation (spec I) ------------------------ */
/* Offline / placeholder / editor preview.  Never fails once the schema is
 * registered: depends only on schema + PCG32. */
JCE_API JceAidResult JCE_CALL
jce_aid_generate_local(JceAidSchemaId id, uint64_t seed, JceAidRecord* out);

/* ---- deterministic solve (spec G) ------------------------------------ */
JCE_API JceAidResult JCE_CALL
jce_aid_solve(const JceAidRecord* rec, JceAidSolved* out);

/* Per-instance expansion sub-stream: seeded from
 *   record->seed XOR hash64(field_name)
 * so per-instance fan-out (e.g. per-blade hue jitter) is inside the
 * determinism contract (spec G). */
JCE_API JceAidResult JCE_CALL
jce_aid_field_rng(const JceAidRecord* rec, const char* field_name, JceRng* out_rng);

/* ---- wire codec (spec E.2: little-endian, trailing 64-bit checksum) -- */
/* encode: *io_len in = buffer capacity, out = bytes written. */
JCE_API JceAidResult JCE_CALL
jce_aid_record_encode(const JceAidRecord* rec, void* buf, size_t* io_len);
JCE_API JceAidResult JCE_CALL
jce_aid_record_decode(const void* buf, size_t len, JceAidRecord* out);

/* ---- P1: event stream / record log / replay -------------------------- */
struct jce_event_bus;            /* os/core jce_event.h */
struct JceSnapshotRegistry;      /* middleware/save jce_snapshot.h */

/* Event id published for every record: 64-bit hash of "aid.record".
 * Payload = encoded record bytes (decode + checksum-verify on receipt). */
JCE_API uint64_t JCE_CALL jce_aid_event_id(void);

/* Append to the session log (+ live .jarc stream when recording) and
 * publish JCE_EVT_AID_RECORD on `bus` (bus may be NULL: log/stream only). */
JCE_API JceAidResult JCE_CALL
jce_aid_publish_record(struct jce_event_bus* bus, const JceAidRecord* rec);

/* Disk stream: record mode appends every published record to `path`.
 * .jarc v1 (frozen): "JARC" u32 + version u32 + pad u64 + entries of
 * [u16 len][len bytes = one encoded record]. */
JCE_API JceAidResult JCE_CALL jce_aid_stream_record_open(const char* path);
JCE_API void         JCE_CALL jce_aid_stream_close(void); /* record & replay */

/* Replay: open a .jarc, then pump per tick.  While replay is active the
 * module refuses acquisition (generate_local asserts + errors) -- replay
 * NEVER re-queries any provider (spec H).  Delivered records go to `bus`
 * (when non-NULL) and are copied to `out_opt` (use max_records==1 with
 * out_opt).  Returns the number of records delivered; stops at the first
 * record with tick > up_to_tick.  EOF closes the stream and clears the
 * replay flag. */
JCE_API JceAidResult JCE_CALL jce_aid_replay_open(const char* path);
JCE_API JceBool      JCE_CALL jce_aid_replay_active(void);
JCE_API uint32_t     JCE_CALL
jce_aid_replay_pump(struct jce_event_bus* bus, uint64_t up_to_tick,
                    uint32_t max_records, JceAidRecord* out_opt);

/* Save-middleware integration: registers section "aid_records" (v1).
 * write = dump the session log; read = replace the session log. */
JCE_API JceBool JCE_CALL
jce_aid_register_save_provider(struct JceSnapshotRegistry* reg);

/* ---- P3: calibration (spec F.6) --------------------------------------- */
/* Feed one game-side ACCEPTED tier-1 record into the per-schema
 * reservoir (payload only, never context; cap 256).  Non-T1 records
 * are rejected. */
JCE_API JceAidResult JCE_CALL
jce_aid_calib_note_accepted(const JceAidRecord* rec);

/* Fit the reservoir into a versioned parameter table (numeric fields ->
 * integer mean/stddev/seen-range; ENUM/TAGSET -> frequency tables) and
 * persist it to <calib_dir>/aid_calib_<schema_id>.bin.  calib_ver
 * auto-increments per fit.  Tables shape FUTURE T3 generations only --
 * records are self-contained, so no fit ever changes the replay result
 * of an existing record. */
JCE_API JceAidResult JCE_CALL jce_aid_calib_fit(JceAidSchemaId id);

/* Load a persisted table (strictly validated against the registered
 * schema; corrupt/mismatched files are rejected and T3 falls back to
 * schema-bounds uniform sampling). */
JCE_API JceAidResult JCE_CALL jce_aid_calib_load(JceAidSchemaId id);

/* Current table version for the schema; 0 = no table (uniform T3). */
JCE_API uint16_t JCE_CALL jce_aid_calib_version(JceAidSchemaId id);

/* Directory for calibration files (default "."). */
JCE_API void JCE_CALL jce_aid_calib_set_dir(const char* dir);

/* ---- verification helpers (DoD tests) -------------------------------- */
/* Net count of live module allocations (alloc minus free). */
JCE_API uint64_t JCE_CALL jce_aid_debug_live_allocs(void);
/* Total acquisitions (generate_local + provider chain).  MUST stay flat
 * across any replay window (P1 DoD 2). */
JCE_API uint64_t JCE_CALL jce_aid_debug_acquire_count(void);
/* Session-log occupancy / drop-oldest overflow count. */
JCE_API uint32_t JCE_CALL jce_aid_debug_log_count(void);
JCE_API uint64_t JCE_CALL jce_aid_debug_log_dropped(void);
/* Out-of-range values clamped at the T1/T2 boundary (spec K). */
JCE_API uint64_t JCE_CALL jce_aid_debug_clamp_count(void);

JCE_EXTERN_C_END

#endif /* JCE_AI_DISPATCH_H */
