/*
 * jce_physics_debug.c — P3-C.5 debug-draw process-wide state.
 *
 * Stores the active line sink + flag mask used by every physics world
 * during its debug-draw flush.  The state is intentionally process-wide
 * so the editor can wire the renderer to physics once, irrespective of
 * how many worlds exist.
 */

#include <jce/middleware/physics/jce_physics_debug.h>

static uint32_t           s_flags  = 0;
static jce_debug_line_fn  s_sink_fn = NULL;
static void              *s_sink_ud = NULL;

void jce_physics_debug_set_flags(uint32_t flags)
{
    s_flags = flags;
}

uint32_t jce_physics_debug_get_flags(void)
{
    return s_flags;
}

void jce_physics_debug_set_line_sink(jce_debug_line_fn fn, void *ud)
{
    s_sink_fn = fn;
    s_sink_ud = ud;
}

/* Internal accessors used by jce_physics.c. */
uint32_t          jce_physics_debug_state_flags_(void)   { return s_flags; }
jce_debug_line_fn jce_physics_debug_state_sink_fn_(void) { return s_sink_fn; }
void             *jce_physics_debug_state_sink_ud_(void) { return s_sink_ud; }
