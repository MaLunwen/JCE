/*
 * jce_vm_lifecycle_host.h — the recording mock host both languages run over.
 *
 * ONE host, ONE stream writer, compiled ONCE and linked into the driver that
 * runs BOTH sides.  Two hosts written to agree would be a second two-sided
 * contract; the Python line's differential says the same thing about its mock
 * and it is the reason a trace difference here is a difference in what the VM
 * did, never a difference in what the host did.
 *
 * WHAT GETS RECORDED, AND WHY EACH TOKEN CARRIES ITS TYPE.  A stream token is
 * `i:`, `u:`, `b:`, `f:`, `s:` or `nil`, so nil / 0 / false cannot pass for one
 * another in the comparison.  Floats are recorded as the IEEE bits of the
 * double the float promotes to: C's %g and the value's decimal spelling are
 * about printf, and a differential that compares printf output is comparing the
 * wrong thing.
 *
 * ERROR LINES ARE NORMALISED, AND THAT IS A DELIBERATE, NAMED LIMIT.  Lua
 * reports `on_update error: chunk:12: boom`; Java reports
 * `on_update error: java.lang.IllegalStateException: boom`.  The message text
 * is language-specific and cannot be compared; the FACT, the METHOD and the
 * PRESENCE OF A DETAIL are not, so an error line is recorded as
 * `T err s:<method>|b:<detail was non-empty>`.  A VM that reports nothing where
 * the other reports an error still fails, and a VM that reports an empty detail
 * fails the b: half — which is the part a bare "an error happened" token would
 * have thrown away.
 */
#ifndef JCE_VM_LIFECYCLE_HOST_H
#define JCE_VM_LIFECYCLE_HOST_H

#include <jce/middleware/script/jce_script.h>

#include <stdbool.h>
#include <stdio.h>

typedef struct LifecycleRecorder {
    FILE *out;
    int   host_calls;      /* every T line, whatever kind */
    int   observations;    /* every R line */
    int   cases;
    const char *scripts_dir;
} LifecycleRecorder;

/* Fill `host` with the recording implementation.  Only the four members the
 * lifecycle scripts use are non-NULL — log, get_position, set_position,
 * read_file — because a partial host is the normal case (jce_script.h: any
 * callback may be NULL) and a mock that filled all 78 would be testing a
 * configuration no game has. */
void lifecycle_host_init(JceScriptHost *host, LifecycleRecorder *rec);

/* Stream writers.  Every one of them counts what it wrote, so the driver can
 * assert its own liveness before the comparator ever sees the file. */
void lifecycle_case(LifecycleRecorder *rec, int index, const char *name);
void lifecycle_slot(LifecycleRecorder *rec, const char *slot);
void lifecycle_bool(LifecycleRecorder *rec, const char *label, bool value);
void lifecycle_int(LifecycleRecorder *rec, const char *label, int value);
/* The driver records the dt it FEEDS as well as what came back, so a driver
 * that took a language-conditional branch could not produce two equal
 * streams — the input half would already differ. */
void lifecycle_double(LifecycleRecorder *rec, const char *label, double v);
void lifecycle_done(LifecycleRecorder *rec);

/* The deterministic answer the mock gives get_position: (e*2, e*3, e*5) for
 * e < 1000, and "no transform" at or above it.  Exposed so a test can assert
 * the same numbers the scripts assert. */
#define LIFECYCLE_NO_TRANSFORM_ENTITY 1000u

#endif /* JCE_VM_LIFECYCLE_HOST_H */
