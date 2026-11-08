/* First-party typed CPU declarations for the complete host adapters only. */
#ifndef JCE_TEST_HOST_SDL_H
#define JCE_TEST_HOST_SDL_H
#include <stdbool.h>
#include <stdint.h>
typedef enum SDL_AppResult {
    SDL_APP_CONTINUE, SDL_APP_SUCCESS, SDL_APP_FAILURE
} SDL_AppResult;
typedef struct SDL_Event { unsigned type; } SDL_Event;
const char *SDL_getenv(const char *name);
void SDL_Delay(uint32_t milliseconds);
bool SDL_PollEvent(SDL_Event *event);
bool SDL_IsMainThread(void);
void SDL_Quit(void);
#endif
