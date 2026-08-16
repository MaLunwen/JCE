/*
 * jce_editor_headless_build.h
 *
 * Same-executable, UI-free project-build automation for the generic editor.
 * The environment contract is parsed before any project state is changed.
 */

#ifndef JCE_EDITOR_HEADLESS_BUILD_H
#define JCE_EDITOR_HEADLESS_BUILD_H

bool jce_editor_headless_build_environment_present();
bool jce_editor_headless_build_initialize();
void jce_editor_headless_build_poll();
void jce_editor_headless_build_shutdown();
bool jce_editor_headless_build_should_quit();

#endif /* JCE_EDITOR_HEADLESS_BUILD_H */
