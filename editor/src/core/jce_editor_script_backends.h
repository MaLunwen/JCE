/*
 * jce_editor_script_backends.h — stand up the non-Lua script VMs in the
 * editor process, so Play runs what the game will run.
 *
 * TWO CALLS, AND THEY HAPPEN AT DIFFERENT TIMES BECAUSE THEY DEPEND ON
 * DIFFERENT THINGS:
 *
 *   jce_editor_register_script_backends()  once, during editor init, BEFORE
 *       any scene is loaded — a scene's Script components are instantiated
 *       inside jce_runtime_create(), and a language registered after that
 *       point is a language every entity has already been refused by.  It
 *       depends only on what this editor was BUILT with.
 *
 *   jce_editor_script_modules_reload()     every time the current project
 *       changes.  A C++ script is a class in a shared object the PROJECT
 *       builds, named in that project's jce_project.json, so it cannot be
 *       known at init: there is no project yet.
 *
 * See the .cpp for the full reasoning, including why the C++ VM belongs here
 * even though the classes it runs do not.
 */
#ifndef JCE_EDITOR_SCRIPT_BACKENDS_H
#define JCE_EDITOR_SCRIPT_BACKENDS_H

/* Registers whichever backends this editor was built with.  Never fails the
 * caller: every refusal is logged with the reason, and the editor keeps
 * running on Lua exactly as it did before this existed. */
void jce_editor_register_script_backends(void);

/* Unload the previous project's C++ script modules and load the new one's,
 * from `project_root`/jce_project.json's "script_modules" (overridden for one
 * run by the JCE_SCRIPT_CPP_MODULES environment variable).
 *
 * Pass NULL or "" when a project closes — that unloads and loads nothing.
 *
 * Never fails the caller.  Each module that cannot be loaded is reported with
 * WHICH of the four distinguishable states it is in (absent / the OS refused
 * to map it / it is not a JCE script module / it was refused after loading),
 * because they have four different fixes and none of them is "unknown
 * language".  Safe to call with no project, with a project that declares no
 * modules, and repeatedly with the same root. */
void jce_editor_script_modules_reload(const char *project_root);

#endif /* JCE_EDITOR_SCRIPT_BACKENDS_H */
