/* Private terminal handoff for the existing impostor bake owner. */
#ifndef JCE_IMPOSTOR_INTERNAL_H
#define JCE_IMPOSTOR_INTERNAL_H
/* CPU-only; call only after bgfx_shutdown has joined the backend. */
void jce_impostor_after_backend_shutdown(void);
#endif
