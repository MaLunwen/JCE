/*
 * jce_llm.h  Ask a language model, from C, without blocking the frame.
 *
 * WHAT THIS IS FOR.  Authoring.  A tool -- the editor, or something a user
 * builds on the SDK -- describes what it wants in words and gets text back:
 * a scene to lay out, a material to suggest, a review of what is already
 * there.  It is not a runtime gameplay system; for deterministic, replayable
 * in-game content generation see jce_ai_dispatch.h, which is a different
 * thing wearing a similar word.
 *
 * ANY MODEL, AND WHY THAT IS NOT A BOAST.  A provider here is a PROGRAM: it
 * is handed a prompt file and the path to write its answer to, and JCE reads
 * that answer.  Every hosted API, every local runner and every script anyone
 * writes around an internal endpoint is reachable through the same three
 * fields, because "can a program on this machine reach the model" is the only
 * question this design asks.  The alternative -- an HTTP client with TLS in
 * the engine -- would have meant a new third-party dependency, a certificate
 * store, and a per-platform port on a codebase that targets machines from
 * 2008; and it would still not have reached a model behind somebody's shell
 * script.
 *
 * THE CREDENTIAL NEVER PASSES THROUGH HERE.  There is no api_key field and
 * there will not be one.  The child process inherits this process's
 * environment, which is where a key belongs; the engine never reads it, never
 * copies it, and so can never log it.  A field for it would be a field
 * somebody eventually prints.
 *
 * THREADING, and why this is a pump rather than a thread.  jce_process.h
 * states that all of its functions must be called from one thread, so the
 * child is driven from the caller's thread: jce_llm_tick() drains the pipe
 * and polls for exit, and must be called once per frame by whoever submitted.
 * Nothing here blocks -- an engine that stalls its main loop for a network
 * round trip has stopped being an engine for the length of the request.
 *
 * Layer: L4 middleware.  Depends on os/core only.
 */

#ifndef JCE_LLM_H
#define JCE_LLM_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* One request in flight at a time.  A second submit while one is running is
 * refused rather than queued: the answer is going somewhere a human is
 * looking, and two of them arriving in an order nobody chose is worse than
 * being told to wait.  Same reason the reflection probe bake is single-slot. */
typedef uint32_t JceLlmHandle;   /* 0 is never a valid handle */

typedef enum {
    JCE_LLM_IDLE      = 0,
    JCE_LLM_RUNNING   = 1,
    JCE_LLM_DONE      = 2,
    JCE_LLM_FAILED    = 3,
    JCE_LLM_CANCELLED = 4
} JceLlmStatus;

/* The program that talks to the model.
 *
 * `arguments` is a template.  Two substitutions are made, each as many times
 * as it appears:
 *
 *   {prompt}    absolute path to the file JCE wrote the prompt into
 *   {response}  absolute path the provider must write its answer to
 *
 * Tokens are split respecting quotes (jce_process_spawn's rule), so a path
 * containing a space survives being wrapped in double quotes in the template.
 *
 * Example -- this repository's own tool, which speaks five wire formats plus
 * a command escape hatch of its own:
 *
 *   executable = "python"
 *   arguments  = "scripts/jce.py design --prompt-file \"{prompt}\" "
 *                "--out \"{response}\" --send"
 *
 * Example -- a local model with no network and no key at all:
 *
 *   executable = "ollama"
 *   arguments  = "run llama3.2"        (see prompt_on_stdin below)
 */
typedef struct {
    const char *executable;      /* required */
    const char *arguments;       /* required; see the substitutions above */
    const char *working_dir;     /* NULL/"" inherits the caller's */
    /* Hard cap on how long the child may take.  0 means the built-in
     * default (120 s).  A model that has not answered in two minutes has
     * usually not been asked -- a wrong URL and a slow model look identical
     * from here, and only one of them gets better by waiting. */
    int         timeout_ms;
} JceLlmProvider;

typedef struct {
    JceLlmProvider provider;

    /* The prompt, as text.  JCE writes it to a file inside `work_dir` and
     * substitutes that path into the argument template.  A file rather than
     * a command-line argument because a scene brief is routinely longer than
     * the platform's argument limit, and because an argument is visible in
     * every process listing on the machine. */
    const char    *prompt;

    /* Where the prompt and response files live.  NULL/"" means the current
     * working directory.  The caller picks this -- the engine has no
     * temp-directory API and inventing one here would put a user's brief
     * somewhere they did not choose. */
    const char    *work_dir;
} JceLlmRequest;

typedef struct {
    JceLlmStatus status;
    /* Bytes of stdout captured so far.  A provider that prints progress
     * gives a caller something to show; one that prints nothing gives 0,
     * which is not an error. */
    size_t       stdout_bytes;
    /* Milliseconds since submit.  A caller with no other progress signal can
     * at least show that time is passing and offer a cancel. */
    uint32_t     elapsed_ms;
    /* Human-readable, stable for the life of the handle.  On FAILED this
     * says what went wrong in a sentence a user can act on. */
    const char  *message;
} JceLlmProgress;

/* Submit a request.  Returns 0 if the arguments are unusable, if a request
 * is already in flight, or if the child could not be spawned -- the three
 * cases are distinguished by jce_llm_last_error(). */
JCE_API JceLlmHandle JCE_CALL jce_llm_submit(const JceLlmRequest *req);

/* Drive the in-flight request.  Call once per frame from the thread that
 * submitted.  Cheap and safe when nothing is in flight. */
JCE_API void JCE_CALL jce_llm_tick(void);

/* Read the state of `h`.  Returns false once the handle has been released or
 * was never valid; `out` is zeroed first either way. */
JCE_API bool JCE_CALL jce_llm_poll(JceLlmHandle h, JceLlmProgress *out);

/* Ask the child to stop.  The status becomes CANCELLED once it has. */
JCE_API void JCE_CALL jce_llm_cancel(JceLlmHandle h);

/* The model's answer, valid only while the handle is and only once the
 * status is DONE.  Returns NULL otherwise.  The response is whatever the
 * provider wrote to {response}; JCE does not parse it, because what counts
 * as a valid answer belongs to the caller that asked the question. */
JCE_API const char *JCE_CALL jce_llm_response(JceLlmHandle h, size_t *out_len);

/* Everything the child printed to stdout and stderr, for a caller that wants
 * to show a log.  Valid while the handle is.  Never NULL (may be ""). */
JCE_API const char *JCE_CALL jce_llm_output_log(JceLlmHandle h);

/* Release the handle and free the response.  A terminal request that is
 * never released holds the single slot, so this is not optional. */
JCE_API void JCE_CALL jce_llm_release(JceLlmHandle h);

/* Why the last jce_llm_submit returned 0.  "" when none has failed. */
JCE_API const char *JCE_CALL jce_llm_last_error(void);

/* Free the module's state.  Kills any in-flight child.  Idempotent. */
JCE_API void JCE_CALL jce_llm_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_LLM_H */
