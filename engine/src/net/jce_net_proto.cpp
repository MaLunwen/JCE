/*
 * jce_net_proto.cpp  Protobuf C++ shim.
 *
 * Translates between the flat C JceNetEnvelope structs and the
 * protobuf-generated jce::net::NetEnvelope messages.
 */

#include "jce_net_proto.h"

#include "jce_net_messages.pb.h"

#include <cstring>
#include <string>

#include "core/jce_memory.h"

extern "C" {
#include <jce/core/jce_log.h>
}

#define LOG_TAG "net_proto"

/* ── Internal text buffer for decoded chat messages ─────────────── */

static thread_local std::string s_chat_text_buf;

/* ── Encode ─────────────────────────────────────────────────────── */

uint32_t jce_net_proto_encode(const JceNetEnvelope *env,
                              uint8_t *out_buf, uint32_t out_buf_size)
{
    if (!env || !out_buf || out_buf_size == 0) return 0;

    jce::net::NetEnvelope msg;
    msg.set_sequence(env->sequence);
    msg.set_timestamp_us(env->timestamp_us);

    switch (env->type) {
    case JCE_NET_MSG_PLAYER_ACTION: {
        auto *pa = msg.mutable_player_action();
        pa->set_entity_id(env->player_action.entity_id);
        pa->set_action_id(env->player_action.action_id);
        auto *pos = pa->mutable_position();
        pos->set_x(env->player_action.position.x);
        pos->set_y(env->player_action.position.y);
        pos->set_z(env->player_action.position.z);
        auto *dir = pa->mutable_direction();
        dir->set_x(env->player_action.direction.x);
        dir->set_y(env->player_action.direction.y);
        dir->set_z(env->player_action.direction.z);
        pa->set_value(env->player_action.value);
        break;
    }
    case JCE_NET_MSG_GAME_STATE: {
        auto *gs = msg.mutable_game_state();
        gs->set_tick(env->game_state.tick);
        for (uint32_t i = 0; i < env->game_state.entity_count; i++) {
            const JceNetEntitySnapshot *snap = &env->game_state.entities[i];
            auto *es = gs->add_entities();
            es->set_entity_id(snap->entity_id);
            auto *p = es->mutable_position();
            p->set_x(snap->position.x);
            p->set_y(snap->position.y);
            p->set_z(snap->position.z);
            auto *r = es->mutable_rotation();
            r->set_x(snap->rotation.x);
            r->set_y(snap->rotation.y);
            r->set_z(snap->rotation.z);
            r->set_w(snap->rotation.w);
            auto *v = es->mutable_velocity();
            v->set_x(snap->velocity.x);
            v->set_y(snap->velocity.y);
            v->set_z(snap->velocity.z);
            es->set_state(snap->state);
        }
        break;
    }
    case JCE_NET_MSG_CHAT: {
        auto *cm = msg.mutable_chat_message();
        cm->set_sender_id(env->chat.sender_id);
        if (env->chat.text)
            cm->set_text(env->chat.text);
        cm->set_channel(env->chat.channel);
        break;
    }
    case JCE_NET_MSG_PING: {
        auto *ping = msg.mutable_ping();
        ping->set_client_time_us(env->ping.client_time_us);
        ping->set_server_time_us(env->ping.server_time_us);
        break;
    }
    default:
        LOG_ERROR(LOG_TAG, "unknown message type %d", (int)env->type);
        return 0;
    }

    size_t needed = msg.ByteSizeLong();
    if (needed > out_buf_size) {
        LOG_ERROR(LOG_TAG, "encode buffer too small (%u < %zu)",
                  out_buf_size, needed);
        return 0;
    }

    if (!msg.SerializeToArray(out_buf, (int)out_buf_size)) {
        LOG_ERROR(LOG_TAG, "SerializeToArray failed");
        return 0;
    }

    return (uint32_t)needed;
}

/* ── Decode ─────────────────────────────────────────────────────── */

bool jce_net_proto_decode(const uint8_t *data, uint32_t size,
                          JceNetEnvelope *out_env)
{
    if (!data || size == 0 || !out_env) return false;

    jce::net::NetEnvelope msg;
    if (!msg.ParseFromArray(data, (int)size)) {
        LOG_ERROR(LOG_TAG, "ParseFromArray failed (%u bytes)", size);
        return false;
    }

    memset(out_env, 0, sizeof(*out_env));
    out_env->sequence     = msg.sequence();
    out_env->timestamp_us = msg.timestamp_us();

    switch (msg.payload_case()) {
    case jce::net::NetEnvelope::kPlayerAction: {
        out_env->type = JCE_NET_MSG_PLAYER_ACTION;
        const auto &pa = msg.player_action();
        out_env->player_action.entity_id  = pa.entity_id();
        out_env->player_action.action_id  = pa.action_id();
        if (pa.has_position()) {
            out_env->player_action.position.x = pa.position().x();
            out_env->player_action.position.y = pa.position().y();
            out_env->player_action.position.z = pa.position().z();
        }
        if (pa.has_direction()) {
            out_env->player_action.direction.x = pa.direction().x();
            out_env->player_action.direction.y = pa.direction().y();
            out_env->player_action.direction.z = pa.direction().z();
        }
        out_env->player_action.value = pa.value();
        break;
    }
    case jce::net::NetEnvelope::kGameState: {
        out_env->type = JCE_NET_MSG_GAME_STATE;
        const auto &gs = msg.game_state();
        out_env->game_state.tick = gs.tick();
        int count = gs.entities_size();
        out_env->game_state.entity_count = (uint32_t)count;
        if (count > 0) {
            out_env->game_state.entities = (JceNetEntitySnapshot *)JCE_CALLOC(
                (size_t)count, sizeof(JceNetEntitySnapshot));
            for (int i = 0; i < count; i++) {
                const auto &es = gs.entities(i);
                JceNetEntitySnapshot *snap = &out_env->game_state.entities[i];
                snap->entity_id = es.entity_id();
                if (es.has_position()) {
                    snap->position.x = es.position().x();
                    snap->position.y = es.position().y();
                    snap->position.z = es.position().z();
                }
                if (es.has_rotation()) {
                    snap->rotation.x = es.rotation().x();
                    snap->rotation.y = es.rotation().y();
                    snap->rotation.z = es.rotation().z();
                    snap->rotation.w = es.rotation().w();
                }
                if (es.has_velocity()) {
                    snap->velocity.x = es.velocity().x();
                    snap->velocity.y = es.velocity().y();
                    snap->velocity.z = es.velocity().z();
                }
                snap->state = es.state();
            }
        }
        break;
    }
    case jce::net::NetEnvelope::kChatMessage: {
        out_env->type = JCE_NET_MSG_CHAT;
        const auto &cm = msg.chat_message();
        out_env->chat.sender_id = cm.sender_id();
        s_chat_text_buf = cm.text();
        out_env->chat.text = s_chat_text_buf.c_str();
        out_env->chat.channel = cm.channel();
        break;
    }
    case jce::net::NetEnvelope::kPing: {
        out_env->type = JCE_NET_MSG_PING;
        const auto &p = msg.ping();
        out_env->ping.client_time_us = p.client_time_us();
        out_env->ping.server_time_us = p.server_time_us();
        break;
    }
    default:
        LOG_ERROR(LOG_TAG, "unknown payload case %d", (int)msg.payload_case());
        return false;
    }

    return true;
}

/* ── Cleanup ────────────────────────────────────────────────────── */

void jce_net_proto_free_state(JceNetGameState *gs)
{
    if (!gs) return;
    JCE_FREE(gs->entities);
    gs->entities = NULL;
    gs->entity_count = 0;
}
