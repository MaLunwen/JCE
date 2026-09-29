/*
 * api_net.h  Networking.
 *
 * Client/server networking via ENet with protobuf serialisation.
 */

#ifndef JCE_API_NET_H
#define JCE_API_NET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/net/jce_net.h>
#include <jce/middleware/net/jce_net_quant.h>
#include <jce/middleware/net/jce_net_prediction.h>
#include <jce/middleware/net/jce_net_animator.h>
#include <jce/middleware/net/jce_net_transform.h>
#include <jce/middleware/net/jce_net_types.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/net/jce_network_variable.h>
#include <jce/middleware/net/jce_rpc.h>
#include <jce/middleware/net/jce_net_input_command.h>
#include <jce/middleware/net/jce_session.h>
#include <jce/middleware/net/jce_lan_discovery.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/middleware/net/jce_predict_locomotion.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_NET_H */
