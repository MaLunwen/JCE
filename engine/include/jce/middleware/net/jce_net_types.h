/*
 * jce_net_types.h  Network handle and message types.
 *
 * Shared between the networking module and game code.
 *
 * Layer: Network (Layer 3).
 */

#ifndef JCE_NET_TYPES_H
#define JCE_NET_TYPES_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Handle types                                                        */
/* ================================================================== */

/* Peer handle — identifies a remote connection. */
typedef struct { uint32_t idx; } JcePeerHandle;
#define JCE_PEER_INVALID ((JcePeerHandle){ UINT32_MAX })

static inline bool jce_peer_valid(JcePeerHandle h) { return h.idx != UINT32_MAX; }

/* ================================================================== */
/* Delivery mode                                                       */
/* ================================================================== */

typedef enum {
    JCE_NET_UNRELIABLE        = 0, /* Fire-and-forget UDP */
    JCE_NET_RELIABLE          = 1, /* Reliable ordered */
    JCE_NET_RELIABLE_UNORDERED = 2  /* Reliable but arrival order not guaranteed */
} JceNetDelivery;

/* ================================================================== */
/* Network events                                                      */
/* ================================================================== */

typedef enum {
    JCE_NET_EVENT_CONNECT    = 0,
    JCE_NET_EVENT_DISCONNECT = 1,
    JCE_NET_EVENT_RECEIVE    = 2,
    JCE_NET_EVENT_TIMEOUT    = 3
} JceNetEventType;

typedef struct {
    JceNetEventType type;
    JcePeerHandle   peer;
    uint8_t         channel;
    const void     *data;        /* valid only during callback / poll */
    uint32_t        data_size;
} JceNetEvent;

/* Callback invoked for each received network event. */
typedef void (*jce_net_event_fn)(const JceNetEvent *event, void *userdata);

JCE_EXTERN_C_END

#endif /* JCE_NET_TYPES_H */
