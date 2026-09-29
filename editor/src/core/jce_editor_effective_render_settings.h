/*
 * jce_editor_effective_render_settings.h — one composition of the project's
 * render settings, for the viewport and the packager both.
 *
 * THEY USED TO COMPOSE IT DIFFERENTLY, and therefore disagreed:
 *
 *   - the packager started from jce_render_settings_default() and then set
 *     seven quality-level fields, so every OTHER field -- the whole authored
 *     Look Profile and grass_enabled, the project gate sr_draw_grass
 *     requires -- was reset to a default in every cooked tree and every
 *     shipped game.  A GrassField that drew in the viewport was silently
 *     absent from the build.
 *
 *   - the viewport read only the authored file, so the ACTIVE QUALITY LEVEL
 *     never reached the editor at all.  A designer's shadow tier, LOD bias,
 *     vsync and MSAA were visible only after a build.
 *
 * Both halves are the same question, so there is now one answer to it.
 *
 * PRECEDENCE: the authored render_settings.json (source first, cooked as a
 * fallback -- the same order the editor viewport resolves) supplies the Look
 * Profile and the grass gate; the active quality level then overrides the
 * seven fields it owns.
 *
 * This lives in editor/src/core rather than beside the path resolver because
 * jce_scene_content_context.cpp is deliberately dependency-free so it can be
 * unit-tested standalone -- its own test link caught the first attempt.
 */

#ifndef JCE_EDITOR_EFFECTIVE_RENDER_SETTINGS_H
#define JCE_EDITOR_EFFECTIVE_RENDER_SETTINGS_H

#include <string>

#include <jce/renderer/jce_render_settings.h>

/* Fills `out` (which must already hold a valid struct, typically
 * jce_render_settings_default()).  `out_source_path`, when non-null,
 * receives the file the authored half came from, or an empty string when no
 * file was found.  Returns false only when nothing at all could be
 * resolved -- no project and no file -- in which case `out` is untouched. */
/* Backbuffer MSAA for one quality level.  `level_anti_aliasing` is the
 * level's authored value: 0/2/4/8, or JCE_PS_AA_USE_PROJECT_DEFAULT (-1) for
 * "whatever Graphics says".  Split out because the composition below reaches
 * the shipped game through the packager, so the precedence rule is worth
 * stating once and testing directly rather than inferring from a build. */
int jce_editor_quality_msaa(int level_anti_aliasing, int project_default_msaa);

bool jce_editor_effective_render_settings(bool isolated,
                                          const char *project_root,
                                          JceRenderSettings *out,
                                          std::string *out_source_path);

#endif /* JCE_EDITOR_EFFECTIVE_RENDER_SETTINGS_H */
