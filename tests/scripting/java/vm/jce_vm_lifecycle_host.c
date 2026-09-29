#include "jce_vm_lifecycle_host.h"

#include <jce/os/core/jce_alloc.h>

#include <stdint.h>
#include <string.h>

/* A float, as the bits of the double it promotes to.  Both languages hand the
 * host a float, so this is exact on both sides and a difference in the last
 * bit is a real difference rather than a formatting one. */
static void emit_float(FILE *out, float v)
{
    union { double d; uint64_t bits; } u;
    u.d = (double)v;
    fprintf(out, "|f:%llu", (unsigned long long)u.bits);
}

static void emit_double(FILE *out, double v)
{
    union { double d; uint64_t bits; } u;
    u.d = v;
    fprintf(out, "|f:%llu", (unsigned long long)u.bits);
}

/* An error line, split into the halves that CAN be compared.  See the header:
 * "on_update error: <language-specific detail>" becomes the method plus
 * "there was a detail". */
static bool split_error(const char *msg, char *method, size_t cap,
                        bool *has_detail)
{
    const char *sep = strstr(msg, " error: ");
    size_t      n;
    if (!sep) return false;
    n = (size_t)(sep - msg);
    if (n >= cap) n = cap - 1u;
    memcpy(method, msg, n);
    method[n] = '\0';
    *has_detail = sep[8] != '\0';
    return true;
}

static void host_log(void *user, const char *msg)
{
    LifecycleRecorder *rec = (LifecycleRecorder *)user;
    char               method[128];
    bool               detail = false;

    if (!rec || !rec->out || !msg) return;
    ++rec->host_calls;
    if (split_error(msg, method, sizeof(method), &detail)) {
        fprintf(rec->out, "T err|s:%s|b:%s\n", method, detail ? "true" : "false");
        return;
    }
    fprintf(rec->out, "T log|s:%s\n", msg);
}

static bool host_get_position(void *user, JceScriptEntity e, float out_xyz[3])
{
    LifecycleRecorder *rec = (LifecycleRecorder *)user;

    if (rec && rec->out) {
        ++rec->host_calls;
        fprintf(rec->out, "T get_position|u:%llu", (unsigned long long)e);
    }
    if (e >= LIFECYCLE_NO_TRANSFORM_ENTITY) {
        if (rec && rec->out) fprintf(rec->out, "|b:false\n");
        return false;
    }
    out_xyz[0] = (float)e * 2.0f;
    out_xyz[1] = (float)e * 3.0f;
    out_xyz[2] = (float)e * 5.0f;
    if (rec && rec->out) {
        fprintf(rec->out, "|b:true");
        emit_float(rec->out, out_xyz[0]);
        emit_float(rec->out, out_xyz[1]);
        emit_float(rec->out, out_xyz[2]);
        fprintf(rec->out, "\n");
    }
    return true;
}

static void host_set_position(void *user, JceScriptEntity e,
                              float x, float y, float z)
{
    LifecycleRecorder *rec = (LifecycleRecorder *)user;
    if (!rec || !rec->out) return;
    ++rec->host_calls;
    fprintf(rec->out, "T set_position|u:%llu", (unsigned long long)e);
    emit_float(rec->out, x);
    emit_float(rec->out, y);
    emit_float(rec->out, z);
    fprintf(rec->out, "\n");
}

/* The one host member the trace does NOT record, on purpose: it is how the
 * driver hands a script file to `instantiate`, and recording the bytes would
 * put a Lua chunk in the Lua stream and Java source in the Java one — a
 * guaranteed difference that says nothing about the VM.  The CALL is recorded;
 * the payload is not. */
static void *host_read_file(void *user, const char *path, uint64_t *out_size)
{
    LifecycleRecorder *rec = (LifecycleRecorder *)user;
    char               full[1024];
    FILE              *f;
    long               len;
    void              *buf;

    if (!rec || !path) return NULL;
    ++rec->host_calls;
    if (rec->out) {
        /* The EXTENSION is stripped, and that is a named limit rather than
         * laziness: `extra.lua` and `extra.java` are necessarily different
         * files, so recording the full name guarantees a difference that says
         * nothing about the VM.  What is compared is that instantiate reached
         * the host, once, for the script called `extra`.  The two files are
         * deliberately given the SAME stem so nothing else has to be
         * normalised away. */
        const char *dot = strrchr(path, '.');
        int         stem = dot ? (int)(dot - path) : (int)strlen(path);
        fprintf(rec->out, "T read_file|s:%.*s\n", stem, path);
    }

    snprintf(full, sizeof(full), "%s/%s", rec->scripts_dir ? rec->scripts_dir : ".",
             path);
    f = fopen(full, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    buf = jce_malloc((size_t)len + 1u);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1u, (size_t)len, f) != (size_t)len) {
        fclose(f);
        jce_free(buf);
        return NULL;
    }
    fclose(f);
    ((char *)buf)[len] = '\0';
    if (out_size) *out_size = (uint64_t)len;
    return buf;
}

void lifecycle_host_init(JceScriptHost *host, LifecycleRecorder *rec)
{
    memset(host, 0, sizeof(*host));
    host->user = rec;
    host->log = host_log;
    host->get_position = host_get_position;
    host->set_position = host_set_position;
    host->read_file = host_read_file;
}

void lifecycle_case(LifecycleRecorder *rec, int index, const char *name)
{
    ++rec->cases;
    fprintf(rec->out, "CASE %d %s\n", index, name);
}

void lifecycle_slot(LifecycleRecorder *rec, const char *slot)
{
    fprintf(rec->out, "SLOT %s\n", slot);
}

void lifecycle_bool(LifecycleRecorder *rec, const char *label, bool value)
{
    ++rec->observations;
    fprintf(rec->out, "R %s|b:%s\n", label, value ? "true" : "false");
}

void lifecycle_int(LifecycleRecorder *rec, const char *label, int value)
{
    ++rec->observations;
    fprintf(rec->out, "R %s|i:%d\n", label, value);
}

void lifecycle_done(LifecycleRecorder *rec)
{
    fprintf(rec->out, "DONE %d %d %d\n", rec->cases, rec->observations,
            rec->host_calls);
}

void lifecycle_double(LifecycleRecorder *rec, const char *label, double v)
{
    ++rec->observations;
    fprintf(rec->out, "R %s", label);
    emit_double(rec->out, v);
    fprintf(rec->out, "\n");
}
