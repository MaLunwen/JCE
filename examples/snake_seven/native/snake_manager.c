/*
 * snake_manager.c -- MODULE 6 of 7: THE GAME MANAGER, and it is COMPILED AS C.
 *
 * Owns the state machine (spec 7): MENU -> PLAYING <-> PAUSED, and
 * PLAYING -> GAME OVER.  It owns the score reset, the best score, and what
 * a restart means.  It decides nothing about the rules -- walls are Java's
 * verdict, self-collision is C++'s, eating is Python's.  This module reads
 * their verdicts and is the only place that says what happens next.
 *
 * WHY THE BEST SCORE IS A FILE OF MY OWN AND NOT save_game().
 * jce_script_api_save_game() exists and is tempting, but it is
 * jce_runtime_save_to_file() -- a snapshot of the whole runtime.  Calling
 * it to persist one integer would write the entire live scene, and loading
 * it back would restore the snake mid-run rather than a number.  One
 * integer wants one integer's worth of storage.
 *
 * ON fopen IN THIS FILE.  The engine's lint refuses bare fopen in
 * first-party engine code and it is right to.  This is a user project: it
 * is the consumer, not the engine, and it has no JCE filesystem module of
 * its own to reach for here.
 *
 * ENTRY POINT: compiled with JCE_SCRIPT_MODULE_NO_ENTRY, like the C++
 * module, because both link into one executable for the single-file build.
 */
#include <jce/script_vm/jce_script_vm_c.h>
#include <jce/script_api/jce_script_api.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define ST_MENU      0
#define ST_PLAYING   1
#define ST_PAUSED    2
#define ST_GAMEOVER  3

#define INTENT_START 1
#define INTENT_PAUSE 2
#define INTENT_MENU  4

#define INITIAL_LENGTH 4        /* spec 14; body segments = this minus head */
#define BEST_FILE "snake_best.txt"

typedef struct GameManager {
    JceScriptApi   *api;
    JceScriptEntity self;       /* GameState */
    JceScriptEntity meta, pend, dir, verdict;
    int             best;
    int             started;
} GameManager;

static JceScriptEntity gm_find(GameManager *g, const char *name)
{
    JceScriptEntity found[2];
    if (jce_script_api_find_by_name(g->api, name, found, 2) > 0)
        return found[0];
    return (JceScriptEntity)0;
}

static int gm_read_best(void)
{
    FILE *f = fopen(BEST_FILE, "r");
    int v = 0;
    if (!f) return 0;               /* no file yet is not an error */
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    return v < 0 ? 0 : v;
}

static void gm_write_best(int v)
{
    FILE *f = fopen(BEST_FILE, "w");
    if (!f) return;                 /* a read-only install still plays */
    fprintf(f, "%d\n", v);
    fclose(f);
}

static void *gm_create(const JceCScriptContext *ctx)
{
    GameManager *g;
    if (!ctx || ctx->struct_size < sizeof(JceCScriptContext)) return NULL;
    if (!ctx->host) return NULL;

    g = (GameManager *)calloc(1, sizeof *g);
    if (!g) return NULL;

    g->self = (JceScriptEntity)ctx->entity;
    /* The clamped host size the VM handed us, NOT sizeof(JceScriptHost). */
    g->api = jce_script_api_open(ctx->host, ctx->host_size, 1);
    if (!g->api) { free(g); return NULL; }
    return g;
}

static void gm_destroy(void *self)
{
    GameManager *g = (GameManager *)self;
    if (!g) return;
    if (g->api) jce_script_api_close(g->api);
    free(g);
}

/* Start a new run.  Every module watches GameMeta.z and resets its own
 * private state when it changes; that is how a restart reaches seven
 * modules that share no code and cannot call each other. */
static void gm_new_run(GameManager *g)
{
    float meta[3];
    float run = 0.0f;

    if (jce_script_api_get_position(g->api, g->meta, meta))
        run = meta[2] + 1.0f;

    jce_script_api_set_position(g->api, g->self, (float)ST_PLAYING, 0.0f, 0.0f);
    jce_script_api_set_position(g->api, g->meta, (float)g->best,
                                (float)(INITIAL_LENGTH - 1), run);
    /* Heading right, so the snake is moving the moment the game starts and
     * a player who presses nothing still sees a game rather than a
     * stationary square. */
    jce_script_api_set_position(g->api, g->dir, 1.0f, 0.0f, 0.0f);
    jce_script_api_set_position(g->api, g->pend, 0.0f, 0.0f, 0.0f);
    jce_script_api_set_position(g->api, g->verdict, 0.0f, 0.0f, 0.0f);
}

static JceCStatus gm_start(void *self)
{
    GameManager *g = (GameManager *)self;
    if (!g || !g->api) return JCE_C_OK;

    g->meta    = gm_find(g, "GameMeta");
    g->pend    = gm_find(g, "Pending");
    g->dir     = gm_find(g, "Direction");
    g->verdict = gm_find(g, "Verdict");
    g->best    = gm_read_best();

    /* MENU, with the best score already on the board: spec 6 wants SCORE
     * and BEST visible in every state, including before the first game. */
    jce_script_api_set_position(g->api, g->self, (float)ST_MENU, 0.0f, 0.0f);
    jce_script_api_set_position(g->api, g->meta, (float)g->best,
                                (float)(INITIAL_LENGTH - 1), 0.0f);
    return JCE_C_OK;
}

static JceCStatus gm_update(void *self, float dt)
{
    GameManager *g = (GameManager *)self;
    float st[3], pend[3], verdict[3], meta[3];
    int state, intent, score;

    (void)dt;
    if (!g || !g->api || !g->meta || !g->pend) return JCE_C_OK;
    if (!jce_script_api_get_position(g->api, g->self, st)) return JCE_C_OK;
    if (!jce_script_api_get_position(g->api, g->pend, pend)) return JCE_C_OK;

    state  = (int)lroundf(st[0]);
    score  = (int)lroundf(st[1]);
    intent = (int)lroundf(pend[1]);

    if (intent & INTENT_START) {
        intent &= ~INTENT_START;
        jce_script_api_set_position(g->api, g->pend, pend[0], (float)intent,
                                    pend[2]);
        if (state == ST_MENU || state == ST_GAMEOVER) {
            gm_new_run(g);
            return JCE_C_OK;
        }
    }

    if (intent & INTENT_PAUSE) {
        intent &= ~INTENT_PAUSE;
        jce_script_api_set_position(g->api, g->pend, pend[0], (float)intent,
                                    pend[2]);
        if (state == ST_PLAYING) {
            jce_script_api_set_position(g->api, g->self, (float)ST_PAUSED,
                                        st[1], st[2]);
            return JCE_C_OK;
        }
        if (state == ST_PAUSED) {
            jce_script_api_set_position(g->api, g->self, (float)ST_PLAYING,
                                        st[1], st[2]);
            return JCE_C_OK;
        }
    }

    if (intent & INTENT_MENU) {
        intent &= ~INTENT_MENU;
        jce_script_api_set_position(g->api, g->pend, pend[0], (float)intent,
                                    pend[2]);
        if (state == ST_PAUSED || state == ST_GAMEOVER) {
            jce_script_api_set_position(g->api, g->self, (float)ST_MENU,
                                        0.0f, 0.0f);
            return JCE_C_OK;
        }
    }

    if (state != ST_PLAYING) return JCE_C_OK;

    if (!jce_script_api_get_position(g->api, g->verdict, verdict))
        return JCE_C_OK;

    /* The "ate" flag is a one-tick signal for the body module.  Clearing it
     * here rather than in Python keeps it to one writer per slot per
     * purpose: Python raises it, the manager lowers it. */
    if (verdict[2] >= 0.5f)
        jce_script_api_set_position(g->api, g->verdict, verdict[0],
                                    verdict[1], 0.0f);

    if (verdict[0] >= 0.5f || verdict[1] >= 0.5f) {
        jce_script_api_set_position(g->api, g->self, (float)ST_GAMEOVER,
                                    st[1], st[2]);
        if (score > g->best) {
            g->best = score;
            gm_write_best(g->best);
        }
        if (jce_script_api_get_position(g->api, g->meta, meta))
            jce_script_api_set_position(g->api, g->meta, (float)g->best,
                                        meta[1], meta[2]);
    }
    return JCE_C_OK;
}

JCE_C_SCRIPT_CLASS_BEGIN(GameManager, "GameManager", gm_create, gm_destroy)
    JCE_C_ON_START(gm_start)
    JCE_C_ON_UPDATE(gm_update)
JCE_C_SCRIPT_CLASS_END()

JCE_C_MODULE_BEGIN()
    JCE_C_MODULE_CLASS(GameManager)
JCE_C_MODULE_GLOBALS()
JCE_C_MODULE_END("snakec", snake_c_module)
