/*
 * lifecycle_runner.c — one driver, two languages, the ENGINE's own entry
 * points.
 *
 * The cross-language differential for the CALL-DOWN direction
 * (differential.py) compares a Lua VM against the ctypes binding.  This is the
 * mirror: it compares the Lua VM against the PYTHON VM, driven UP through
 * jce_script_call_start / call_update / call_collision / ... — the same public
 * functions the engine calls in its frame loop, forwarded through JceScriptVM.
 *
 * WHY THE DRIVER IS ONE PROGRAM AND NOT TWO.  Both sides record into the SAME
 * mock host, in the same process, through the same steps in the same order.
 * There is no second driver to agree with the first, and the only thing that
 * differs between the two runs is the language argument — so a difference in
 * the recorded stream is a difference in the VM and cannot be a difference in
 * how it was driven.
 *
 * WHY THE STEP TABLE NAMES ITS SLOT.  The cases are derived from the vtable,
 * not invented here: `--slots` prints every step with the JceScriptVM slot it
 * exercises, and lifecycle_differential.py parses `struct JceScriptVM` out of
 * jce_script_vm.h and REFUSES a run in which any slot is exercised by no step.
 * A 19th slot appended to the vtable therefore fails this suite by name rather
 * than reaching only Lua, which is exactly the drift the vtable exists to stop.
 *
 * WHAT IS NOT PRINTED, AND WHY.  Never a raw instance or module id.  Lua's are
 * registry refs and Python's are a counter; both are opaque handles by
 * contract, and printing them would make the comparison fail on a difference
 * that is not one.  `nonzero` / `zero` is the whole of what the contract says.
 */

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>

#include "jce_script_py_mock.h"
#include "jce_script_vm_python.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every step, with the slot it exercises.  Read by --slots and checked
 * against jce_script_vm.h by the differential. */
typedef struct { const char *step; const char *slot; } Step;

static const Step k_steps[] = {
    { "create",                      "create_sized"       },
    { "instantiate_by_path",         "instantiate"        },
    { "instantiate_source",          "instantiate_source" },
    { "instance_count_after_one",    "instance_count"     },
    { "call_start",                  "call_start"         },
    { "call_update_1",               "call_update"        },
    { "call_fixed_update",           "call_fixed_update"  },
    { "update_coroutines_1",         "update_coroutines"  },
    { "call_update_2",               "call_update"        },
    { "update_coroutines_2",         "update_coroutines"  },
    { "call_update_3_raises",        "call_update"        },
    { "call_update_4_after_error",   "call_update"        },   /* silent: disabled */
    { "call_collision",              "call_collision"     },
    { "call_message",                "call_message"       },
    { "call_message_null_str",       "call_message"       },
    { "call_message_absent",         "call_message"       },
    { "call_anim_event",             "call_anim_event"    },
    { "call_anim_event_null_name",   "call_anim_event"    },
    { "call_named_hit",              "call_named"         },
    { "call_named_miss",             "call_named"         },
    { "call_named_num_hit",          "call_named_num"     },
    { "call_named_num_miss",         "call_named_num"     },
    { "call_named_str_hit",          "call_named_str"     },
    { "call_named_str_null",         "call_named_str"     },
    { "call_named_str_miss",         "call_named_str"     },
    { "compile_module",              "compile_module"     },
    { "rebind_instance",             "rebind_instance"    },
    { "call_update_after_rebind",    "call_update"        },
    { "release_module",              "release_module"     },
    { "call_update_after_release_module", "call_update"   },
    { "release",                     "release"            },
    { "instance_count_after_release", "instance_count"    },
    { "call_update_after_release",   "call_update"        },
    { "destroy",                     "destroy"            },
};
#define STEP_COUNT ((int)(sizeof k_steps / sizeof k_steps[0]))

static void step(const char *name)
{
    char line[256];
    snprintf(line, sizeof line, "CASE %s", name);
    jce_mock_note(line);
}

static void result(const char *name, const char *value)
{
    char line[256];
    snprintf(line, sizeof line, "RESULT %s=%s", name, value);
    jce_mock_note(line);
}

static char *read_all(const char *path)
{
    FILE  *f = fopen(path, "rb");
    long   n;
    size_t got;
    char  *buf;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const char       *lang;
    const char       *script_path;
    const char       *reload_path;
    const char       *pkg_dir;
    char             *source;
    char             *reload_source;
    JceScript        *s;
    JceScriptInstance inst;
    JceScriptModule   mod;
    int               i;

    if (argc == 2 && strcmp(argv[1], "--slots") == 0) {
        for (i = 0; i < STEP_COUNT; ++i)
            printf("%s\t%s\n", k_steps[i].step, k_steps[i].slot);
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr,
                "usage: lifecycle_runner <lua|python> <script> <reload>\n"
                "       lifecycle_runner --slots\n");
        return 2;
    }
    lang        = argv[1];
    script_path = argv[2];
    reload_path = argv[3];

    /* The Python backend registers itself; there is no central list.  Doing it
     * unconditionally (even for the Lua run) keeps the two runs identical in
     * everything but the language argument. */
    pkg_dir = getenv("JCE_PY_VM_PACKAGE_DIR");
    if (pkg_dir && pkg_dir[0]) jce_script_vm_python_add_path(pkg_dir);
    (void)jce_script_vm_python_register();

    source = read_all(script_path);
    reload_source = read_all(reload_path);
    if (!source || !reload_source) {
        fprintf(stderr, "lifecycle_runner: cannot read %s / %s\n",
                script_path, reload_path);
        return 2;
    }

    jce_mock_reset(JCE_MOCK_MODE_OK);

    step("create");
    s = jce_script_vm_create(lang, jce_mock_host(JCE_MOCK_MODE_OK),
                             jce_mock_host_size());
    if (!s) {
        result("create", "null");
        /* Printed anyway: the differential must be able to tell "the VM
         * refused to start" from "the VM produced nothing", and those look
         * identical if the trace is withheld. */
        fputs(jce_mock_trace_text(), stdout);
        return 3;
    }
    result("create", "ok");

    /* The mock host has no read_file member, so this is the REFUSAL branch on
     * both sides — which is the point: a host that cannot read a file must
     * fail identically in both languages.  The success branch is covered by
     * test_jce_script_vm_python.c, which supplies a host that has one. */
    step("instantiate_by_path");
    result("instantiate_by_path",
           jce_script_instantiate(s, "scripts/not_reachable", 41u) ? "nonzero"
                                                                  : "zero");

    step("instantiate_source");
    inst = jce_script_instantiate_source(s, "lifecycle", source, 42u);
    result("instantiate_source", inst ? "nonzero" : "zero");
    if (!inst) {
        fputs(jce_mock_trace_text(), stdout);
        return 4;
    }

    step("instance_count_after_one");
    result("instance_count",
           jce_script_instance_count(s) == 1 ? "1" : "not-1");

    step("call_start");
    jce_script_call_start(s, inst);

    step("call_update_1");
    jce_script_call_update(s, inst, 0.25f);

    /* 0.125 is exact in both float and double, so the two backends' logged
     * dt strings are comparable without a formatting rule. */
    step("call_fixed_update");
    jce_script_call_fixed_update(s, inst, 0.125f);

    step("update_coroutines_1");
    jce_script_update_coroutines(s, 0.25f);

    step("call_update_2");
    jce_script_call_update(s, inst, 0.25f);

    step("update_coroutines_2");
    jce_script_update_coroutines(s, 0.30f);

    /* The handler raises here.  Both VMs must log it, write the disable
     * notice THE FAILING-CALLBACK RULE publishes (jce_script.h), and RETURN
     * NORMALLY.  The step after this one is what proves the disable took: it
     * drives on_update again and both sides must record NOTHING for it.
     *
     * Two steps and not one, because each catches what the other cannot.  A
     * VM that let the exception escape dies at the raising step; a VM that
     * logged and kept dispatching (the behaviour the header promised against
     * for years) survives that and is caught only here, as an extra pair of
     * script lines the reference does not have.  And a VM that dropped the
     * whole instance is caught by the steps AFTER these, which drive
     * on_collision / on_ping / on_anim_event on the same instance. */
    step("call_update_3_raises");
    jce_script_call_update(s, inst, 0.25f);

    step("call_update_4_after_error");
    jce_script_call_update(s, inst, 0.25f);

    step("call_collision");
    jce_script_call_collision(s, inst, 77u);

    step("call_message");
    jce_script_call_message(s, inst, "on_ping", 2.5, "hello");

    step("call_message_null_str");
    jce_script_call_message(s, inst, "on_ping", -1.5, NULL);

    /* No such method: a clean no-op in both, recording nothing at all. */
    step("call_message_absent");
    jce_script_call_message(s, inst, "no_such_method", 1.0, "x");

    step("call_anim_event");
    jce_script_call_anim_event(s, inst, 9u, "footstep", 1.5f, -2.25f, -3);

    step("call_anim_event_null_name");
    jce_script_call_anim_event(s, inst, 10u, NULL, 0.5f, 0.0f, 7);

    step("call_named_hit");
    result("call_named",
           jce_script_call_named(s, "on_named_hit", 51u) ? "true" : "false");

    step("call_named_miss");
    result("call_named_miss",
           jce_script_call_named(s, "no_such_global", 51u) ? "true" : "false");

    step("call_named_num_hit");
    result("call_named_num",
           jce_script_call_named_num(s, "on_slider", 52u, 0.75) ? "true"
                                                                : "false");

    step("call_named_num_miss");
    result("call_named_num_miss",
           jce_script_call_named_num(s, "no_such_global", 52u, 0.75) ? "true"
                                                                    : "false");

    step("call_named_str_hit");
    result("call_named_str",
           jce_script_call_named_str(s, "on_field", 53u, "abc") ? "true"
                                                                : "false");

    step("call_named_str_null");
    result("call_named_str_null",
           jce_script_call_named_str(s, "on_field", 53u, NULL) ? "true"
                                                               : "false");

    step("call_named_str_miss");
    result("call_named_str_miss",
           jce_script_call_named_str(s, "no_such_global", 53u, "abc") ? "true"
                                                                     : "false");

    step("compile_module");
    mod = jce_script_compile_module(s, "reload", reload_source,
                                    strlen(reload_source));
    result("compile_module", mod ? "nonzero" : "zero");

    step("rebind_instance");
    jce_script_rebind_instance(s, inst, mod);

    step("call_update_after_rebind");
    jce_script_call_update(s, inst, 0.25f);

    step("release_module");
    jce_script_release_module(s, mod);

    /* The instance still holds the module, so the rebound on_update keeps
     * working: releasing the MODULE handle drops the VM's reference and not
     * the instance's.  Deliberately NOT followed by a rebind to the released
     * id — the two runtimes diverge there (Lua would install a nil __index)
     * and a differential must not be the place a known divergence is
     * discovered. */
    step("call_update_after_release_module");
    jce_script_call_update(s, inst, 0.25f);

    step("release");
    jce_script_release(s, inst);

    step("instance_count_after_release");
    result("instance_count_after_release",
           jce_script_instance_count(s) == 0 ? "0" : "not-0");

    step("call_update_after_release");
    jce_script_call_update(s, inst, 0.25f);

    step("destroy");
    jce_script_destroy(s);
    result("destroy", "ok");

    fputs(jce_mock_trace_text(), stdout);
    free(source);
    free(reload_source);
    return 0;
}
