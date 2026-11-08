/* Shared admission for GPU scene picks and retained CPU pick geometry. */
#ifndef JCE_PICK_CAPABILITY_INTERNAL_H
#define JCE_PICK_CAPABILITY_INTERNAL_H

#include <stdbool.h>

bool jce_pick_gpu_readback_available(void);

#endif
