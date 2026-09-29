/*
 * jce_vm_lifecycle_driver.c — ONE driver, TWO languages, two processes.
 *
 * Batch 1 spent the one free correctness oracle (hand-written Lua bindings the
 * manifest never saw) and deleted it.  LUA IS NOW THE REFERENCE LIFECYCLE, so
 * this driver expresses one script's semantics in Lua and in Java, drives both
 * through THE ENGINE'S OWN ENTRY POINTS — jce_script_vm_create,
 * jce_script_instantiate*, jce_script_call_* — and writes a stream of what each
 * one produced.  run_lifecycle_differential.py compares the two.
 *
 * ONE C FILE, NOT TWO.  The case list, the dt sequence, the entity ids and the
 * order of the calls are written ONCE and the language is a command-line
 * argument, so a difference between the streams cannot be a difference between
 * two drivers.  The only language-conditional code in this file is the three
 * script FILE NAMES and the JVM configuration, and both are named below.
 *
 * TWO PROCESSES, NOT TWO PASSES.  The suite runs this binary twice.  A JVM
 * that aborts (a JNI misuse under -Xcheck:jni does exactly that) must not be
 * able to take the reference stream with it, and the exit codes have to be
 * readable separately.  This is the shape the Java binding's own differential
 * already uses, for the same reason.
 *
 * THE DRIVER REFUSES ITS OWN DEAD RUN.  Before writing DONE it asserts that it
 * observed something, that the host was called, and that every slot in
 * JceScriptVM was exercised.  A run that produced nothing exits non-zero and
 * says so — because the comparator's job is to find a DIFFERENCE, and two
 * streams that both stopped producing answers compare equal.  That failure has
 * shipped in this repository once already, 150 cases of 151 dead.
 */

#include "jce_vm_lifecycle_host.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/os/core/jce_alloc.h>

#if defined(JCE_LIFECYCLE_WITH_JAVA)
#  include <jce/script_vm/jce_script_vm_java.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The entity ids THIS FILE passes in.  OWNER is below
 * LIFECYCLE_NO_TRANSFORM_ENTITY, so the scripts' get_position(self.entity)
 * succeeds; the id they query to make it FAIL is theirs and is written in
 * both scripts, not here — a constant defined in this file and used only in
 * two other files would be a contract with nothing to check it. */
#define OWNER_ENTITY   7u
#define OTHER_ENTITY   77u
#define HANDLER_ENTITY 5u

typedef struct Driver {
    const char       *language;
    LifecycleRecorder rec;
    JceScriptHost     host;
    JceScript        *vm;
    const char       *main_script;
    const char       *v2_script;
    const char       *extra_script;
    int               case_index;
} Driver;

/* Read a script from the scripts directory.  The driver reads the SOURCE it
 * hands to instantiate_source itself rather than going through the host,
 * because instantiate_source's contract is "here is the text" — routing it
 * through read_file would be testing the host, not the slot. */
static char *slurp(const char *dir, const char *name)
{
    char  path[1024];
    FILE *f;
    long  len;
    char *buf;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "driver: cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)len + 1u);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1u, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static void begin(Driver *d, const char *name)
{
    lifecycle_case(&d->rec, ++d->case_index, name);
}

int main(int argc, char **argv)
{
    Driver d;
    const char *out_path = NULL;
    const char *scripts = ".";
    const char *classpath = NULL;
    const char *jvm_library = NULL;
    const char *jvm_options[8];
    int         jvm_option_count = 0;
    char       *main_src = NULL;
    char       *v2_src = NULL;
    JceScriptInstance inst = 0;
    JceScriptInstance extra = 0;
    JceScriptModule   mod = 0;
    int i;
    int rc = 0;

    memset(&d, 0, sizeof(d));
    d.language = "lua";

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--language") && i + 1 < argc)      d.language = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)      out_path = argv[++i];
        else if (!strcmp(argv[i], "--scripts") && i + 1 < argc)  scripts = argv[++i];
        else if (!strcmp(argv[i], "--classpath") && i + 1 < argc) classpath = argv[++i];
        else if (!strcmp(argv[i], "--jvm") && i + 1 < argc)      jvm_library = argv[++i];
        else if (!strcmp(argv[i], "--jvm-option") && i + 1 < argc) {
            if (jvm_option_count >= (int)(sizeof(jvm_options) / sizeof(jvm_options[0]))) {
                fprintf(stderr, "driver: too many --jvm-option\n");
                return 2;
            }
            jvm_options[jvm_option_count++] = argv[++i];
        }
        else {
            fprintf(stderr, "driver: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!out_path) {
        fprintf(stderr, "driver: --out is required\n");
        return 2;
    }

    /* THE ONLY LANGUAGE-CONDITIONAL DATA IN THIS FILE. */
    if (!strcmp(d.language, "lua")) {
        d.main_script  = "lifecycle.lua";
        d.v2_script    = "lifecycle_v2.lua";
        d.extra_script = "extra.lua";
    } else if (!strcmp(d.language, "java")) {
        d.main_script  = "lifecycle.java";
        d.v2_script    = "lifecycle_v2.java";
        d.extra_script = "extra.java";
    } else {
        fprintf(stderr, "driver: unknown language '%s'\n", d.language);
        return 2;
    }

    d.rec.out = fopen(out_path, "wb");
    if (!d.rec.out) {
        fprintf(stderr, "driver: cannot write %s\n", out_path);
        return 2;
    }
    d.rec.scripts_dir = scripts;
    lifecycle_host_init(&d.host, &d.rec);

#if defined(JCE_LIFECYCLE_WITH_JAVA)
    if (!strcmp(d.language, "java")) {
        JceScriptVmJavaConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.jvm_library = jvm_library;
        cfg.class_path = classpath;
        /* -Xcheck:jni comes through here.  It is NOT the local-reference
         * oracle — measured on JDK 21.0.9, it says nothing about 200,000
         * leaked references in one frame — but it DOES abort on reference
         * misuse (a double delete, a stale ref), and a checker whose output
         * nobody reads is a checker that cannot fail. */
        cfg.options = jvm_options;
        cfg.option_count = jvm_option_count;
        if (!jce_script_vm_java_configure(&cfg, sizeof(cfg))) {
            fprintf(d.rec.out, "R VMFAIL|b:true\n");
            fclose(d.rec.out);
            return 1;
        }
        if (!jce_script_vm_java_register()) {
            fprintf(d.rec.out, "R VMFAIL|b:true\n");
            fclose(d.rec.out);
            return 1;
        }
    }
#else
    (void)classpath;
    (void)jvm_library;
    (void)jvm_options;
    (void)jvm_option_count;
    if (!strcmp(d.language, "java")) {
        fprintf(stderr, "driver: built without the Java backend\n");
        fprintf(d.rec.out, "R VMFAIL|b:true\n");
        fclose(d.rec.out);
        return 1;
    }
#endif

    main_src = slurp(scripts, d.main_script);
    v2_src = slurp(scripts, d.v2_script);
    if (!main_src || !v2_src) {
        fprintf(d.rec.out, "R VMFAIL|b:true\n");
        fclose(d.rec.out);
        free(main_src);
        free(v2_src);
        return 1;
    }

    /* ── 1. create_sized ───────────────────────────────────────────────── */
    begin(&d, "create");
    lifecycle_slot(&d.rec, "create_sized");
    d.vm = jce_script_vm_create(d.language, &d.host, sizeof(d.host));
    lifecycle_bool(&d.rec, "vm", d.vm != NULL);
    if (!d.vm) {
        fprintf(stderr, "driver: jce_script_vm_create(\"%s\") failed\n",
                d.language);
        fprintf(d.rec.out, "R VMFAIL|b:true\n");
        fclose(d.rec.out);
        free(main_src);
        free(v2_src);
        return 1;
    }
    lifecycle_bool(&d.rec, "language_is_ours",
                   jce_script_vm_language_of(d.vm) != NULL &&
                   !strcmp(jce_script_vm_language_of(d.vm), d.language));

    /* ── 2. instantiate_source ─────────────────────────────────────────── */
    begin(&d, "instantiate_source");
    lifecycle_slot(&d.rec, "instantiate_source");
    inst = jce_script_instantiate_source(d.vm, "lifecycle", main_src,
                                         (JceScriptEntity)OWNER_ENTITY);
    lifecycle_bool(&d.rec, "instance", inst != 0);
    lifecycle_slot(&d.rec, "instance_count");
    lifecycle_int(&d.rec, "count_after_instantiate",
                  jce_script_instance_count(d.vm));

    /* ── 3. call_start ─────────────────────────────────────────────────── */
    begin(&d, "call_start");
    lifecycle_slot(&d.rec, "call_start");
    jce_script_call_start(d.vm, inst);

    /* ── 4. call_update ────────────────────────────────────────────────── */
    begin(&d, "call_update");
    lifecycle_slot(&d.rec, "call_update");
    {
        static const float k_dt[3] = { 0.5f, 0.25f, 0.125f };
        for (i = 0; i < 3; ++i) {
            lifecycle_double(&d.rec, "dt_fed", (double)k_dt[i]);
            jce_script_call_update(d.vm, inst, k_dt[i]);
        }
    }

    /* ── 4b. call_fixed_update ─────────────────────────────────────────
     * The fixed-clock half of the lifecycle.  Driven here so the differential
     * exercises the slot in BOTH languages: a slot no step drives reaches
     * only the backend that happened to implement it, which is the drift the
     * vtable exists to stop.  0.125 is exact in float and double, so the two
     * sides' recorded dt is comparable without a formatting rule. */
    begin(&d, "call_fixed_update");
    lifecycle_slot(&d.rec, "call_fixed_update");
    lifecycle_double(&d.rec, "dt_fed", 0.125);
    jce_script_call_fixed_update(d.vm, inst, 0.125f);

    /* ── 5. call_collision ─────────────────────────────────────────────── */
    begin(&d, "call_collision");
    lifecycle_slot(&d.rec, "call_collision");
    jce_script_call_collision(d.vm, inst, (JceScriptEntity)OTHER_ENTITY);
    /* An untagged body is entity 0, and jce_script.h says so.  Both languages
     * must treat it as a number, not as absence. */
    jce_script_call_collision(d.vm, inst, (JceScriptEntity)0);

    /* ── 6. call_message — the three states of a string argument ───────── */
    begin(&d, "call_message");
    lifecycle_slot(&d.rec, "call_message");
    jce_script_call_message(d.vm, inst, "ping", 1.5, "hi");
    jce_script_call_message(d.vm, inst, "ping", 2.5, NULL);  /* nil / null   */
    jce_script_call_message(d.vm, inst, "ping", 3.5, "");    /* empty, NOT   */
                                                             /* the same     */
    /* No such method: a clean no-op on both sides, and the proof is that the
     * trace gains nothing between this call and the next case. */
    jce_script_call_message(d.vm, inst, "no_such_handler", 9.0, "x");

    /* ── 7. call_anim_event ────────────────────────────────────────────── */
    begin(&d, "call_anim_event");
    lifecycle_slot(&d.rec, "call_anim_event");
    jce_script_call_anim_event(d.vm, inst, 7u, "hit", 0.5f, 0.25f, -3);
    jce_script_call_anim_event(d.vm, inst, 8u, NULL, 1.5f, 0.75f, 4);
    /* Empty is absence here, and jce_script.c's `name && name[0]` is why. */
    jce_script_call_anim_event(d.vm, inst, 9u, "", 2.5f, 1.25f, 5);

    /* ── 8. the three named dispatchers ────────────────────────────────── */
    begin(&d, "call_named");
    lifecycle_slot(&d.rec, "call_named");
    lifecycle_bool(&d.rec, "named_present",
                   jce_script_call_named(d.vm, "on_ping",
                                         (JceScriptEntity)HANDLER_ENTITY));
    lifecycle_bool(&d.rec, "named_absent",
                   jce_script_call_named(d.vm, "no_such_global",
                                         (JceScriptEntity)HANDLER_ENTITY));
    begin(&d, "call_named_num");
    lifecycle_slot(&d.rec, "call_named_num");
    lifecycle_bool(&d.rec, "named_num_present",
                   jce_script_call_named_num(d.vm, "on_value",
                                             (JceScriptEntity)HANDLER_ENTITY,
                                             2.5));
    lifecycle_bool(&d.rec, "named_num_absent",
                   jce_script_call_named_num(d.vm, "no_such_global",
                                             (JceScriptEntity)HANDLER_ENTITY,
                                             2.5));
    begin(&d, "call_named_str");
    lifecycle_slot(&d.rec, "call_named_str");
    lifecycle_bool(&d.rec, "named_str_present",
                   jce_script_call_named_str(d.vm, "on_text",
                                             (JceScriptEntity)HANDLER_ENTITY,
                                             "abcd"));
    lifecycle_bool(&d.rec, "named_str_null",
                   jce_script_call_named_str(d.vm, "on_text",
                                             (JceScriptEntity)HANDLER_ENTITY,
                                             NULL));

    /* ── 9. a named handler that THROWS ────────────────────────────────────
     *
     * jce_script.h: these return "a handler of that name existed", NOT "the
     * call succeeded".  Lua returns true after catching; the Java slot returns
     * true because an exception is pending.  A Java backend that forgot to
     * CHECK the exception would answer false here — a wrong return value with
     * a name, instead of a crash with none. */
    begin(&d, "named_throws");
    lifecycle_bool(&d.rec, "named_throws_still_invoked",
                   jce_script_call_named(d.vm, "on_boom",
                                         (JceScriptEntity)HANDLER_ENTITY));
    /* And the dispatch AFTER it must be unaffected.  With a pending exception
     * every later JNI call is undefined, so this is the line that sees a
     * missing ExceptionClear. */
    lifecycle_bool(&d.rec, "named_after_throw",
                   jce_script_call_named(d.vm, "on_ping",
                                         (JceScriptEntity)HANDLER_ENTITY));

    /* ── 10. an on_update that throws, then one that does not ──────────── */
    begin(&d, "update_throws");
    jce_script_call_message(d.vm, inst, "arm_boom", 0.0, NULL);
    jce_script_call_update(d.vm, inst, 0.5f);   /* reports, then raises */
    jce_script_call_update(d.vm, inst, 0.5f);   /* must report normally */
    lifecycle_int(&d.rec, "count_after_throw",
                  jce_script_instance_count(d.vm));

    /* ── 11. update_coroutines ─────────────────────────────────────────── */
    begin(&d, "update_coroutines");
    lifecycle_slot(&d.rec, "update_coroutines");
    /* on_start scheduled one step 0.5s out.  0.25 + 0.25 lands it exactly on
     * zero, which is the boundary `remaining > 0` decides — the same float
     * subtraction on both sides. */
    jce_script_update_coroutines(d.vm, 0.25f);
    jce_script_update_coroutines(d.vm, 0.25f);
    jce_script_update_coroutines(d.vm, 0.25f);

    /* ── 12. instantiate, through the host's read_file ─────────────────── */
    begin(&d, "instantiate");
    lifecycle_slot(&d.rec, "instantiate");
    extra = jce_script_instantiate(d.vm, d.extra_script,
                                   (JceScriptEntity)OWNER_ENTITY);
    lifecycle_bool(&d.rec, "extra_instance", extra != 0);
    jce_script_call_start(d.vm, extra);
    lifecycle_int(&d.rec, "count_with_extra",
                  jce_script_instance_count(d.vm));

    /* ── 13. hot reload: compile_module + rebind_instance + release ────── */
    begin(&d, "hot_reload");
    lifecycle_slot(&d.rec, "compile_module");
    mod = jce_script_compile_module(d.vm, "lifecycle_v2", v2_src,
                                    strlen(v2_src));
    lifecycle_bool(&d.rec, "module", mod != 0);
    lifecycle_slot(&d.rec, "rebind_instance");
    jce_script_rebind_instance(d.vm, inst, mod);
    /* The rebound instance must run the NEW on_update and keep the OLD count:
     * that pair is the whole contract of rebind, and one without the other
     * passes half of it. */
    jce_script_call_update(d.vm, inst, 0.5f);
    lifecycle_slot(&d.rec, "release_module");
    jce_script_release_module(d.vm, mod);
    jce_script_call_update(d.vm, inst, 0.5f);   /* still alive after release */

    /* ── 14. release ───────────────────────────────────────────────────── */
    begin(&d, "release");
    lifecycle_slot(&d.rec, "release");
    jce_script_release(d.vm, inst);
    lifecycle_int(&d.rec, "count_after_release",
                  jce_script_instance_count(d.vm));
    jce_script_release(d.vm, extra);
    lifecycle_int(&d.rec, "count_after_both", jce_script_instance_count(d.vm));
    /* Releasing twice, and releasing 0, are clean no-ops on both sides. */
    jce_script_release(d.vm, inst);
    jce_script_release(d.vm, 0);
    lifecycle_int(&d.rec, "count_after_double_release",
                  jce_script_instance_count(d.vm));

    /* ── 15. destroy ───────────────────────────────────────────────────── */
    begin(&d, "destroy");
    lifecycle_slot(&d.rec, "destroy");
    jce_script_destroy(d.vm);
    d.vm = NULL;

    /* ── The driver's own liveness gate ────────────────────────────────── */
    if (d.rec.observations == 0 || d.rec.host_calls == 0) {
        fprintf(stderr,
                "driver: %s produced %d observation(s) and %d host call(s) — "
                "a run that produced nothing is not a run that agrees\n",
                d.language, d.rec.observations, d.rec.host_calls);
        rc = 1;
    }
    lifecycle_done(&d.rec);
    fclose(d.rec.out);
    free(main_src);
    free(v2_src);
    return rc;
}
