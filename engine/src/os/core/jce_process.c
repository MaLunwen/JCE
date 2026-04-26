/*
 * jce_process.c  Cross-platform external process supervision via SDL3.
 */

#include <jce/os/core/jce_process.h>

#include "jce_memory.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <stdlib.h>
#include <string.h>

struct JceProcess {
	SDL_Process  *handle;
	SDL_IOStream *stdout_stream; /* owned by SDL_Process */
	SDL_IOStream *stderr_stream; /* owned by SDL_Process */
};

/* ── Argument splitter (single & double quotes, no shell expansion) ── */
static char **split_args(const char *exe, const char *args, int *out_count)
{
	*out_count = 0;
	int cap = 8;
	char **argv = (char **)JCE_CALLOC((size_t)cap, sizeof(char *));
	if (!argv) return NULL;

	argv[0] = SDL_strdup(exe ? exe : "");
	if (!argv[0]) { JCE_FREE(argv); return NULL; }
	int n = 1;

	if (args && args[0]) {
		const char *p = args;
		char buf[1024];
		size_t bl = 0;
		bool in_quote = false;
		char quote = '\0';
		while (*p) {
			char c = *p++;
			if ((c == '"' || c == '\'') && (!in_quote || quote == c)) {
				in_quote = !in_quote;
				quote = in_quote ? c : '\0';
				continue;
			}
			if ((c == ' ' || c == '\t') && !in_quote) {
				if (bl) {
					buf[bl] = '\0';
					if (n >= cap) {
						cap *= 2;
						char **g = (char **)SDL_realloc(argv,
							(size_t)cap * sizeof(char *));
						if (!g) goto fail;
						argv = g;
					}
					argv[n] = SDL_strdup(buf);
					if (!argv[n]) goto fail;
					++n;
					bl = 0;
				}
				continue;
			}
			if (bl + 1 < sizeof(buf)) buf[bl++] = c;
		}
		if (bl) {
			buf[bl] = '\0';
			if (n >= cap) {
				cap *= 2;
				char **g = (char **)SDL_realloc(argv,
					(size_t)cap * sizeof(char *));
				if (!g) goto fail;
				argv = g;
			}
			argv[n] = SDL_strdup(buf);
			if (!argv[n]) goto fail;
			++n;
		}
	}

	if (n >= cap) {
		++cap;
		char **g = (char **)SDL_realloc(argv,
			(size_t)cap * sizeof(char *));
		if (!g) goto fail;
		argv = g;
	}
	argv[n] = NULL;
	*out_count = n;
	return argv;

fail:
	for (int i = 0; i < n; ++i) SDL_free(argv[i]);
	JCE_FREE(argv);
	return NULL;
}

static void free_argv(char **argv, int count)
{
	if (!argv) return;
	for (int i = 0; i < count; ++i) SDL_free(argv[i]);
	JCE_FREE(argv);
}

JceProcess *jce_process_spawn(const JceProcessConfig *cfg)
{
	if (!cfg || !cfg->executable_path || !cfg->executable_path[0])
		return NULL;

	int    argc = 0;
	char **argv = split_args(cfg->executable_path, cfg->arguments, &argc);
	if (!argv) return NULL;

	SDL_PropertiesID props = SDL_CreateProperties();
	if (!props) { free_argv(argv, argc); return NULL; }

	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
	                       (void *)argv);
	if (cfg->working_directory && cfg->working_directory[0]) {
		SDL_SetStringProperty(props,
		                      SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING,
		                      cfg->working_directory);
	}
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
	                      SDL_PROCESS_STDIO_NULL);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
	                      cfg->capture_stdout ? SDL_PROCESS_STDIO_APP
	                                          : SDL_PROCESS_STDIO_INHERITED);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
	                      cfg->capture_stderr ? SDL_PROCESS_STDIO_APP
	                                          : SDL_PROCESS_STDIO_INHERITED);

	SDL_Process *raw = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	free_argv(argv, argc);
	if (!raw) return NULL;

	JceProcess *p = (JceProcess *)JCE_CALLOC(1, sizeof(*p));
	if (!p) {
		SDL_KillProcess(raw, true);
		SDL_DestroyProcess(raw);
		return NULL;
	}
	p->handle = raw;

	SDL_PropertiesID pp = SDL_GetProcessProperties(raw);
	if (cfg->capture_stdout) {
		p->stdout_stream = (SDL_IOStream *)SDL_GetPointerProperty(
			pp, SDL_PROP_PROCESS_STDOUT_POINTER, NULL);
	}
	if (cfg->capture_stderr) {
		p->stderr_stream = (SDL_IOStream *)SDL_GetPointerProperty(
			pp, SDL_PROP_PROCESS_STDERR_POINTER, NULL);
	}
	return p;
}

void jce_process_destroy(JceProcess *p)
{
	if (!p) return;
	if (p->handle) {
		int code = 0;
		/* Try non-blocking poll first; if still running, force kill. */
		if (!SDL_WaitProcess(p->handle, false, &code)) {
			SDL_KillProcess(p->handle, true);
			SDL_WaitProcess(p->handle, true, &code);
		}
		SDL_DestroyProcess(p->handle);
	}
	JCE_FREE(p);
}

static size_t s_read(SDL_IOStream *s, char *buf, size_t cap)
{
	if (!s || !buf || cap == 0) return 0;
	return SDL_ReadIO(s, buf, cap);
}

size_t jce_process_read_stdout(JceProcess *p, char *buf, size_t cap)
{
	return p ? s_read(p->stdout_stream, buf, cap) : 0;
}

size_t jce_process_read_stderr(JceProcess *p, char *buf, size_t cap)
{
	return p ? s_read(p->stderr_stream, buf, cap) : 0;
}

bool jce_process_request_stop(JceProcess *p)
{
	if (!p || !p->handle) return false;
	return SDL_KillProcess(p->handle, false);
}

bool jce_process_force_kill(JceProcess *p)
{
	if (!p || !p->handle) return false;
	return SDL_KillProcess(p->handle, true);
}

bool jce_process_poll_exit(JceProcess *p, int *out_exit_code)
{
	if (!p || !p->handle) return false;
	int code = 0;
	if (SDL_WaitProcess(p->handle, false, &code)) {
		if (out_exit_code) *out_exit_code = code;
		return true;
	}
	return false;
}
