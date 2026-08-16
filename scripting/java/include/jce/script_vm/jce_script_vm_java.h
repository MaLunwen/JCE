/*
 * jce_script_vm_java.h — the Java JceScriptVM.
 *
 * JceScriptHost hands capabilities DOWN and scripting/java already carries
 * that half: com.jce.script.JceScript, 71 generated entries over the C ABI.
 * This is the MIRROR — the engine calling UP into a JVM, so a Java class
 * attached to an entity receives on_start / on_update / collisions / messages
 * through exactly the paths Lua uses today.
 *
 * Read engine/include/jce/middleware/script/jce_script_vm.h first: the vtable,
 * the clamped registration copy, the frozen handle header and the
 * owning-thread rule are stated there and are not repeated here.  What follows
 * is only what is TRUE OF THE JVM and of nothing else.
 *
 * ── WIRING IT UP, END TO END ─────────────────────────────────────────────
 *
 *     #include <jce/script_vm/jce_script_vm_java.h>
 *
 *     JceScriptVmJavaConfig cfg = {0};
 *     cfg.jvm_library = "D:/Java21/openjdk-21/bin/server/jvm.dll";
 *     cfg.class_path  = "C:/game/scripts.jar;C:/game/jce-script-1.jar";
 *     jce_script_vm_java_configure(&cfg, sizeof cfg);
 *     jce_script_vm_java_register();
 *
 *     JceScript *s = jce_script_vm_create("java", &host, sizeof host);
 *     JceScriptInstance i = jce_script_instantiate(s, "scripts/Spinner.java",
 *                                                  entity);
 *     jce_script_call_start(s, i);
 *     ... jce_script_call_update(s, i, dt) every frame ...
 *
 * The script itself:
 *
 *     import com.jce.script.vm.JceEntityScript;
 *     public class Spinner extends JceEntityScript {
 *         public void onStart()          { set("t", 0.0); log("hello"); }
 *         public void onUpdate(float dt) {
 *             double t = number("t", 0.0) + dt;
 *             set("t", t);
 *             setPosition(entity(), (float)t, 0.0f, 0.0f);
 *         }
 *     }
 *
 * `jce_script_instantiate` reads the path through JceScriptHost::read_file —
 * the same call Lua makes, so a Java script lives in the same PAK / mounted
 * directory as a .lua one.  The bytes may be SOURCE (compiled in memory; needs
 * a JDK) or a pre-compiled .class (loads on a plain JRE).  The dispatch is on
 * the class-file magic, mirroring luaL_loadbuffer, which accepts a .lua text
 * or a pre-compiled chunk through one entry point.
 *
 * ── THE JVM IS CREATED ONCE PER PROCESS.  THAT IS NOT A LIMITATION WE ────
 *    CHOSE; IT IS ONE WE HANDLE.
 *
 * JNI_CreateJavaVM may be called once per process and HotSpot cannot recreate
 * one after DestroyJavaVM.  So:
 *
 *   - the FIRST handle creates the JVM; every later handle REUSES it;
 *   - if a JVM already exists in the process (this code loaded into a JVM, or
 *     another library got there first) it is ADOPTED via JNI_GetCreatedJavaVMs
 *     rather than fought over;
 *   - destroy() NEVER calls DestroyJavaVM.  Destroying on the last handle
 *     would make the NEXT jce_script_vm_create("java", ...) fail forever — a
 *     failure appearing an arbitrary distance from the destroy that caused it.
 *     A JceScript handle is not the owner of the process's JVM.
 *
 * Two Java handles therefore share one JVM where two Lua handles are two
 * independent lua_States.  What keeps them apart is a per-handle
 * JceScriptRuntime and its own class loader, so a class defined through one
 * handle is a different Class — different static storage — from the same
 * source defined through another.
 * *Enforced by:* test_two_handles_do_not_share_script_statics and
 * test_the_jvm_is_created_exactly_once (tests/scripting/java/vm/).
 *
 * ── THREADS: ATTACH ONCE, ON THE OWNING THREAD, AND REFUSE THE REST ──────
 *
 * The engine's rule is "the thread that created the handle".  This backend
 * enforces the same rule INDEPENDENTLY, at the slot, and that is not
 * belt-and-braces: the engine's check lives in the public forwarders and is
 * disarmed while `owner_thread` is still 0 — which is every call made through
 * the vtable directly rather than through jce_script_*.  A JNIEnv is
 * per-thread and a JceScriptRuntime is not synchronised, so a slot reached
 * from another thread is refused (returning the same clean no-op / false / 0 a
 * NULL handle returns) and logged once per handle.  It is NEVER answered by
 * attaching that thread: that would hand two threads one unsynchronised
 * runtime, which is not a diagnosis anybody gets to make from the crash.
 * *Enforced by:* test_a_slot_called_from_another_attached_thread_is_refused
 * (a thread the JVM knows, so the refusal is the OWNERSHIP rule and not
 * merely a missing JNIEnv) and test_a_thread_the_jvm_never_saw_cannot_dispatch.
 *
 * The creating thread is attached at create and detached at destroy — but only
 * by the handle that attached it, and only when no other handle on that thread
 * is still live.  A thread the JVM already knew (the one that called
 * JNI_CreateJavaVM, or an application thread already attached) is never
 * detached by us.
 *
 * ── EXCEPTIONS ───────────────────────────────────────────────────────────
 *
 * Java script code does NOT catch its own exceptions, deliberately: the
 * Throwable travels out through JNI and every slot checks for it, CLEARS it,
 * and reports it through JceScriptHost::log — structurally what lua_pcall does
 * for Lua, and the reason jce_script.c's "on_update error: ..." line and this
 * backend's read the same.  Clearing is not hygiene: with a pending exception
 * every later JNI call is undefined, so the SECOND dispatch after a throw is
 * where a missing clear actually shows up.
 *
 * Because the throw has already LEFT the Java class by the time control is
 * back in Java, THE FAILING-CALLBACK RULE (jce_script.h) is decided on the
 * native side: `java_handler_threw` takes the Throwable, reports it, calls
 * JceScriptRuntime.disableHandler(handle, hook) and writes the published
 * disable notice.  The state lives in Java (Instance::disabled, the same H_*
 * bits as Instance::handlers) and `live()` is the one gate that reads it, so
 * a hook the rule disabled is not dispatched again on that instance until
 * rebindInstance clears it.  The four dispatchers that do NOT participate
 * — release/on_destroy, call_message and the three named-global slots — call
 * java_take_exception directly, which is what makes their non-participation
 * visible at the call site.
 * *Enforced by:* test_jce_script_vm_java.c ::
 * test_a_throwing_hook_is_reported_disabled_and_leaves_the_rest_alive, the
 * lifecycle differential's `update_throws` case, and
 * test_local_frames.py :: test_the_rule_wrapper_still_takes_the_exception.
 *
 * jce_script_call_named() and its two siblings return "a handler of that name
 * existed", not "the call succeeded".  A Java handler that throws returns the
 * JNI default (false) with an exception pending, so the slot answers TRUE
 * whenever an exception is pending — which makes the exception check
 * LOAD-BEARING rather than hygienic, and makes its absence a wrong RETURN
 * VALUE that a test can name instead of a crash it cannot.
 * *Enforced by:* the lifecycle differential's `named-throws` case.
 *
 * ── LOCAL REFERENCES ─────────────────────────────────────────────────────
 *
 * Every slot body runs inside PushLocalFrame / PopLocalFrame, so no local
 * reference can outlive one dispatch whatever the slot does inside it.  This
 * is a structural guarantee and not a counting exercise, because counting does
 * not work here: measured on JDK 21.0.9 by the batch-1 agent, -Xcheck:jni
 * reports NOTHING for 200,000 local references leaked inside one native frame.
 * *Enforced by:* tests/scripting/java/vm/test_local_frames.py, which reads
 * this backend's C and fails by name for a slot that creates a reference
 * outside a frame or forgets to pop one.
 *
 * ── WHAT THIS BACKEND DOES NOT HAVE ──────────────────────────────────────
 *
 *   - No watchdog.  Lua's dispatch is armed with a LUA_MASKCOUNT hook that
 *     aborts a runaway call; Java cannot interrupt a running method without
 *     Thread.stop, which is removed.  An infinite loop in a Java script hangs
 *     the frame.  Stated, not enforced — there is nothing here to enforce it
 *     with, and a comment claiming otherwise would be the defect.
 *   - No sandbox.  See JceScriptCompiler.java: a Java script is first-party
 *     code at the trust level of a DLL.  Never compile a downloaded mod.
 *   - GC pauses land inside the frame loop.  Nothing here hides that.
 */
#ifndef JCE_SCRIPT_VM_JAVA_H
#define JCE_SCRIPT_VM_JAVA_H

#include <jce/middleware/script/jce_script_vm.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* How to start the JVM.  CALLER-allocated and passed with its own sizeof, the
 * same contract JceScriptHost uses and for the same reason: this struct gains
 * members, the engine copies min(caller, ours) over a zeroed destination, and
 * a member the caller never had stays absent instead of being read off the end
 * of its allocation. */
typedef struct JceScriptVmJavaConfig {
    /* ABSOLUTE path to the JVM shared library — jvm.dll / libjvm.so /
     * libjvm.dylib.  A bare name is REFUSED, not resolved: jce_library_open()
     * documents that on Windows it attaches to an already-resident module of
     * the same name rather than loading the one you meant, so a bare name
     * silently binds to whichever JVM some other component loaded first.
     *
     * NULL derives <JAVA_HOME>/bin/server/jvm.dll (Windows) or
     * <JAVA_HOME>/lib/server/libjvm.{so,dylib}, and logs which one it used. */
    const char *jvm_library;

    /* -Djava.class.path.  NULL leaves the JVM's own default, which is almost
     * never what a game wants: the scripting classes (JceEntityScript and
     * friends) have to be on it. */
    const char *class_path;

    /* Extra raw JVM options, passed through verbatim ("-Xmx512m",
     * "-Xcheck:jni", "-XX:+UseSerialGC", ...).  `option_count` may be 0. */
    const char *const *options;
    int                option_count;
} JceScriptVmJavaConfig;

/* Record how the JVM should be started.  Call BEFORE the first
 * jce_script_vm_create("java", ...): once the JVM exists its options cannot be
 * changed, and a configure() after that point is refused rather than accepted
 * and ignored — an accepted-and-ignored setting is the failure mode this whole
 * campaign is about.
 *
 * The strings are COPIED, so the caller may free them.  Returns false and logs
 * the reason on a refusal. */
JCE_API bool JCE_CALL jce_script_vm_java_configure(
    const JceScriptVmJavaConfig *cfg, size_t cfg_size);

/* The table.  Every slot is filled; see jce_script_vm.h on why a NULL one is
 * refused at registration. */
JCE_API const JceScriptVM *JCE_CALL jce_script_vm_java(void);

/* Register the "java" language.  Idempotent in the honest sense: a second call
 * is refused by the registry and returns false, because live handles hold a
 * pointer into it. */
JCE_API bool JCE_CALL jce_script_vm_java_register(void);

/* Diagnostics, and the oracle for "the JVM is created once per process": the
 * number of times THIS backend called JNI_CreateJavaVM (0 or 1, forever), and
 * the number of times it adopted a JVM somebody else had already created. */
JCE_API int JCE_CALL jce_script_vm_java_jvm_creations(void);
JCE_API int JCE_CALL jce_script_vm_java_jvm_adoptions(void);

/* Live handles created through this backend.  0 does NOT mean the JVM went
 * away — see the header comment: it never does. */
JCE_API int JCE_CALL jce_script_vm_java_live_handles(void);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_VM_JAVA_H */
