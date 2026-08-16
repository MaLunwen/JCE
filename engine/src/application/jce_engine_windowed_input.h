/*
 * jce_engine_windowed_input.h — the ONE place a windowed engine builds its
 * input system, named so the thing that broke can be asserted WITHOUT a
 * window.
 *
 * NOT a public header: it lives under engine/src/, never under <jce/...>.
 *
 * WHY THIS IS A FUNCTION INSTEAD OF TWO LINES IN jce_engine_create.
 *
 * jce_input_create() installs NO backend by design (jce_input.c): the null
 * backend is what headless, a dedicated server and replay all want, and it is
 * what lets the device tests run with nothing plugged in.  A windowed engine
 * wants real hardware, so it has to say so — and for one release it did not,
 * while three comments (one in a public header) said it did.  That shipped as
 * a P0: the engine opened no gamepads with 25 lints, the ABI gate and 319
 * tests green (976d38fa).
 *
 * The regression test written for it, tests/application/
 * test_jce_engine_input_backend.c, can only observe the engine's JceInput
 * through JceServices.input — which requires a real windowed jce_engine_create
 * — so it self-ignores wherever that is unavailable, and Unity counts an
 * ignore as a pass.  A green tick that proves nothing about the thing it names
 * is the same shape as the defect.
 *
 * Putting the create+install pair behind one non-static name moves the
 * install line off the windowed-only boot and onto a path a test can call with
 * no display, no GPU and no SDL_Init: delete the jce_input_set_backend() call
 * inside it and test_windowed_input_construction_installs_the_sdl_backend
 * fails.
 *
 * WHERE THAT WAS MEASURED, AND WHERE IT IS ONLY INFERRED.  Both directions were
 * run on ONE Windows x64 developer desktop — plain, and with JCE_HEADLESS=1,
 * which skips the windowed half exactly as a display-less runner does; with the
 * install deleted the process still exits 1 (mutation table, 6bc1ba2e).
 * Nothing, mutated or unmutated, has been run on Linux or macOS.  That the same
 * thing happens on ubuntu-latest is an INFERENCE — from SDL_OpenGamepad(0)
 * being documented to fail for an invalid id on every backend — and the green
 * direction there, SDL_OpenGamepad(0) failing gracefully on the evdev/udev path
 * with SDL never initialised, has not been observed either.  ubuntu-latest is
 * merge-blocking (.github/workflows/ci.yml, its matrix entry is soft:false), so
 * if the inference is wrong this file is where the surprise comes from.
 *
 * WHAT THIS STILL DOES NOT GUARD, stated rather than left to be discovered:
 * that jce_engine_create() calls THIS function rather than jce_input_create()
 * directly.  That is a different defect from the one that shipped, and the
 * windowed end-to-end assertion in the same test file is what covers it — run
 * it with JCE_REQUIRE_WINDOWED_INPUT=1 on a machine with a GPU and its
 * host-limitation ignores become failures.
 */

#ifndef JCE_ENGINE_WINDOWED_INPUT_H
#define JCE_ENGINE_WINDOWED_INPUT_H

#include <jce/os/platform/jce_input.h>

/* Create the raw-input system a WINDOWED boot needs: jce_input_create() plus
 * the SDL device backend, which is what actually opens hardware and probes its
 * capabilities.  Returns NULL only if jce_input_create() failed; the caller
 * owns the handle and frees it with jce_input_destroy().
 *
 * Needs no window and no SDL_Init: with nothing initialised the backend's
 * open_device simply refuses every device, which is a correct null result and
 * the exact behaviour the headless assertion reads. */
JceInput *jce_engine_create_windowed_input(void);

#endif /* JCE_ENGINE_WINDOWED_INPUT_H */
