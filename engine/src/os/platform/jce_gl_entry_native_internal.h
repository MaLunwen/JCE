/* Private GL entry ABI shared by the renderer capability probes.
 * Official SDL GL declarations own GLenum, GLubyte and GLAPIENTRY; in
 * particular Win32 GL entry points use stdcall rather than default C. */
#ifndef JCE_GL_ENTRY_NATIVE_INTERNAL_H
#define JCE_GL_ENTRY_NATIVE_INTERNAL_H

#include <SDL3/SDL_opengl.h>

typedef const GLubyte *(GLAPIENTRY *JceNativeGlGetStringFn)(GLenum name);

#endif
