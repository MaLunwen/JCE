/*
 * jce_host_shell.c  Host OS shell helpers.
 *
 * Cross-platform wrapper around SDL3 (SDL_OpenURL, SDL_CreateProcess).
 * Engine-internal #ifdef branches are allowed here: their entire purpose
 * is to give consumers (editor, tools, games) a single uniform API and
 * absorb every host-OS quirk in one place.
 */

#include <jce/os/platform/jce_host_shell.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static bool s_path_is_empty(const char *p)
{
	return !p || p[0] == '\0';
}

/* Detached spawn: launches `argv` with stdio routed to /dev/null and
   immediately destroys the SDL_Process handle so the host process is
   not joined to the editor lifetime.  Optional `cwd` sets the working
   directory; pass NULL to inherit. */
static bool s_spawn_detached_cwd(const char *const *argv, const char *cwd)
{
	if (!argv || !argv[0]) return false;

	SDL_PropertiesID props = SDL_CreateProperties();
	if (!props) return false;

	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
	                       (void *)argv);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
	                      SDL_PROCESS_STDIO_NULL);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
	                      SDL_PROCESS_STDIO_NULL);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
	                      SDL_PROCESS_STDIO_NULL);
	SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN,
	                       true);
	if (cwd && cwd[0]) {
		SDL_SetStringProperty(props,
		                      SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING,
		                      cwd);
	}

	SDL_Process *p = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	if (!p) return false;

	SDL_DestroyProcess(p);
	return true;
}

static bool s_spawn_detached(const char *const *argv)
{
	return s_spawn_detached_cwd(argv, NULL);
}

/* Build a "file://" URL from an OS path.  Output buffer must be large
   enough; on overflow returns false. */
static bool s_path_to_file_url(const char *path, char *out, size_t out_size)
{
	if (s_path_is_empty(path) || !out || out_size == 0) return false;

	const char *prefix = "file:///";
	size_t plen = strlen(path);
	size_t need = strlen(prefix) + plen + 1;
	if (need > out_size) return false;

	size_t offset = 0;
	if (path[0] == '/' || path[0] == '\\') {
		(void)snprintf(out, out_size, "file://%s", path);
		offset = 7;
	} else {
		(void)snprintf(out, out_size, "%s%s", prefix, path);
		offset = strlen(prefix);
	}
	for (size_t i = offset; out[i]; ++i) {
		if (out[i] == '\\') out[i] = '/';
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool jce_host_open_url(const char *url)
{
	if (s_path_is_empty(url)) return false;
	return SDL_OpenURL(url);
}

bool jce_host_reveal_path(const char *path)
{
	if (s_path_is_empty(path)) return false;

	SDL_PathInfo info;
	if (!SDL_GetPathInfo(path, &info)) return false;

#if defined(_WIN32)
	/* Windows: explorer.exe with /select reveals the file inside its
	   parent folder; on a directory we just open it.  The /select
	   syntax is `/select,<path>` with the comma directly preceding
	   the path; SDL will quote the joined arg correctly. */
	if (info.type == SDL_PATHTYPE_FILE) {
		char arg[1100];
		(void)snprintf(arg, sizeof(arg), "/select,%s", path);
		const char *argv[] = { "explorer.exe", arg, NULL };
		if (s_spawn_detached(argv)) return true;
	} else {
		const char *argv[] = { "explorer.exe", path, NULL };
		if (s_spawn_detached(argv)) return true;
	}
#elif defined(__APPLE__)
	if (info.type == SDL_PATHTYPE_FILE) {
		const char *argv[] = { "open", "-R", path, NULL };
		if (s_spawn_detached(argv)) return true;
	} else {
		const char *argv[] = { "open", path, NULL };
		if (s_spawn_detached(argv)) return true;
	}
#else
	/* Generic POSIX: xdg-open opens the directory; selection is not
	   available through a portable command. */
	const char *target = path;
	char parent[1100];
	if (info.type == SDL_PATHTYPE_FILE) {
		size_t len = strlen(path);
		if (len < sizeof(parent)) {
			memcpy(parent, path, len + 1);
			char *sep = NULL;
			for (size_t i = len; i > 0; --i) {
				if (parent[i - 1] == '/' || parent[i - 1] == '\\') {
					sep = &parent[i - 1];
					break;
				}
			}
			if (sep && sep != parent) {
				*sep = '\0';
				target = parent;
			}
		}
	}
	const char *argv[] = { "xdg-open", target, NULL };
	if (s_spawn_detached(argv)) return true;
#endif

	/* Final fallback: hand the path (or its parent) to SDL_OpenURL. */
	const char *fallback_target = path;
	char parent[1100];
	if (info.type == SDL_PATHTYPE_FILE) {
		size_t len = strlen(path);
		if (len < sizeof(parent)) {
			memcpy(parent, path, len + 1);
			char *sep = NULL;
			for (size_t i = len; i > 0; --i) {
				if (parent[i - 1] == '/' || parent[i - 1] == '\\') {
					sep = &parent[i - 1];
					break;
				}
			}
			if (sep && sep != parent) {
				*sep = '\0';
				fallback_target = parent;
			}
		}
	}
	char url[1200];
	if (!s_path_to_file_url(fallback_target, url, sizeof(url))) return false;
	return SDL_OpenURL(url);
}

bool jce_host_open_in_text_editor(const char *path)
{
	if (s_path_is_empty(path)) return false;

#if defined(_WIN32)
	/* On Windows VS Code installs a `code.cmd` shim on PATH but
	   CreateProcess will not resolve .cmd files directly.  Run it
	   through cmd.exe /C with code+path as separate argv elements,
	   so SDL's per-arg quoting handles the path correctly. */
	{
		const char *argv[] = { "cmd.exe", "/C", "code", path, NULL };
		if (s_spawn_detached(argv)) return true;
	}
#else
	/* POSIX/macOS: `code` is a real executable on PATH. */
	{
		const char *argv[] = { "code", path, NULL };
		if (s_spawn_detached(argv)) return true;
	}
#endif

	/* Fall back to OS default file handler. */
	char url[1100];
	if (!s_path_to_file_url(path, url, sizeof(url))) return false;
	return SDL_OpenURL(url);
}

bool jce_host_open_terminal(const char *cwd)
{
	const char *target = (cwd && cwd[0]) ? cwd : ".";

#if defined(_WIN32)
	/* Build `cmd.exe /K cd /D <target>` so the new console explicitly
	   chdirs after launch.  This bypasses PowerShell/wt profiles that
	   reset the working directory on startup.  We rely on SDL to
	   quote each argv element when assembling the Windows command
	   line, so DO NOT add quotes inside cd_cmd. */
	char cd_cmd[1100];
	SDL_snprintf(cd_cmd, sizeof(cd_cmd), "cd /D %s", target);
	{
		const char *argv[] = {
			"cmd.exe", "/C", "start", "",
			"cmd.exe", "/K", cd_cmd, NULL
		};
		if (s_spawn_detached_cwd(argv, target)) return true;
	}
	return false;
#elif defined(__APPLE__)
	const char *argv[] = { "open", "-a", "Terminal", target, NULL };
	return s_spawn_detached(argv);
#else
	/* Try a short list of common Linux terminal emulators.  Each is
	   given the working directory via the SDL cwd property; for the
	   ones that need an explicit flag, supply it as well. */
	{
		const char *argv[] = {
			"gnome-terminal", "--working-directory", target, NULL
		};
		if (s_spawn_detached_cwd(argv, target)) return true;
	}
	{
		const char *argv[] = { "konsole", "--workdir", target, NULL };
		if (s_spawn_detached_cwd(argv, target)) return true;
	}
	{
		const char *argv[] = {
			"xfce4-terminal", "--working-directory", target, NULL
		};
		if (s_spawn_detached_cwd(argv, target)) return true;
	}
	{
		const char *argv[] = { "x-terminal-emulator", NULL };
		if (s_spawn_detached_cwd(argv, target)) return true;
	}
	{
		const char *argv[] = { "xterm", NULL };
		if (s_spawn_detached_cwd(argv, target)) return true;
	}
	return false;
#endif
}
