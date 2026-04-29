/*
 * jce_road_network.h -- runtime road graph for vehicle and ped systems.
 */
#ifndef JCE_ROAD_NETWORK_H
#define JCE_ROAD_NETWORK_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JCE_ROAD_INVALID_ID UINT32_MAX

typedef enum {
    JCE_ROAD_CLASS_HIGHWAY  = 0,
    JCE_ROAD_CLASS_ARTERIAL = 1,
    JCE_ROAD_CLASS_LOCAL    = 2,
    JCE_ROAD_CLASS_ALLEY    = 3
} JceRoadClass;

typedef struct {
    uint32_t        node_a;
    uint32_t        node_b;
    uint8_t         lanes_ab;
    uint8_t         lanes_ba;
    JceRoadClass    road_class;
    float           speed_limit;
    float           length;
    uint32_t        point_offset;
    uint32_t        point_count;
} JceRoadSegment;

typedef struct {
    jce_vec3        position;
    uint32_t        first_segment;
    uint32_t        segment_count;
} JceRoadNode;

typedef struct JceRoadNetwork JceRoadNetwork;

typedef struct {
    uint32_t initial_node_capacity;
    uint32_t initial_segment_capacity;
    uint32_t initial_point_capacity;
} JceRoadNetworkDesc;

JCE_API JceRoadNetwork *jce_road_network_create(const JceRoadNetworkDesc *desc);
JCE_API void            jce_road_network_destroy(JceRoadNetwork *net);

JCE_API uint32_t        jce_road_network_add_node(JceRoadNetwork *net, jce_vec3 pos);
JCE_API uint32_t        jce_road_network_add_segment(JceRoadNetwork *net,
                                                      uint32_t node_a,
                                                      uint32_t node_b,
                                                      const jce_vec3 *points,
                                                      uint32_t point_count,
                                                      uint8_t lanes_ab,
                                                      uint8_t lanes_ba,
                                                      JceRoadClass cls,
                                                      float speed_limit);
JCE_API void            jce_road_network_clear(JceRoadNetwork *net);
JCE_API void            jce_road_network_finalize(JceRoadNetwork *net);

JCE_API uint32_t        jce_road_network_node_count(const JceRoadNetwork *net);
JCE_API uint32_t        jce_road_network_segment_count(const JceRoadNetwork *net);
JCE_API const JceRoadSegment *jce_road_network_get_segment(const JceRoadNetwork *net, uint32_t id);
JCE_API const JceRoadNode    *jce_road_network_get_node(const JceRoadNetwork *net, uint32_t id);
JCE_API const jce_vec3       *jce_road_network_get_points(const JceRoadNetwork *net, uint32_t segment_id, uint32_t *out_count);

JCE_API bool            jce_road_network_sample_segment(const JceRoadNetwork *net,
                                                         uint32_t segment_id,
                                                         float t,
                                                         jce_vec3 *out_pos,
                                                         jce_vec3 *out_forward);

JCE_API uint32_t        jce_road_network_closest_segment(const JceRoadNetwork *net,
                                                          jce_vec3 pos,
                                                          float *out_t,
                                                          float *out_dist);

JCE_API uint32_t        jce_road_network_pick_random_near(const JceRoadNetwork *net,
                                                           jce_vec3 pos,
                                                           float radius,
                                                           uint64_t *rng_state);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ROAD_NETWORK_H */
