#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_history_entity.h"
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include "core/jce_editor_history_identity.h"

struct SceneOwner {
    JceScene *scene = jce_scene_create();
    ~SceneOwner() { jce_scene_destroy(scene); }
};
struct Snapshot {
    std::string json;
    std::vector<int> components;
    Snapshot(JceScene *scene, JceEntity entity) {
        JceJson *object = jce_json_object();
        jce_json_set_child(object, "components", jce_scene_serialize_entity_components(scene, entity));
        char *text = jce_json_print(object, false);
        json = text; jce_json_free_string(text); jce_json_free(object);
        for (int id = 0; id < jce_component_count(); ++id)
            if (jce_scene_has_comp(scene, entity, id)) components.push_back(id);
    }
};
static JceTransform transform(float x, float y, float z, float scale = 1) {
    JceTransform result{};
    result.position = jce_v3(x, y, z); result.rotation = jce_q_identity();
    result.scale = jce_v3(scale, scale, scale);
    return result;
}
static void matrix_equal(jce_mat4 left, jce_mat4 right) {
    for (int i = 0; i < 16; ++i) CHECK(left.raw[i / 4][i % 4] == doctest::Approx(right.raw[i / 4][i % 4]).epsilon(.0001));
}

TEST_CASE("scoped parent restores exact local TRS and descendant world through transformed parent") {
    SceneOwner owner; REQUIRE(owner.scene != nullptr);
    JceEntity parent = jce_scene_create_entity(owner.scene, "Parent");
    JceEntity child = jce_scene_create_entity(owner.scene, "Child");
    JceEntity descendant = jce_scene_create_entity(owner.scene, "Descendant");
    auto p = transform(10, 5, 2, 2), c = transform(2, 3, 4), d = transform(0, 1, 0);
    jce_scene_set_transform(owner.scene, parent, &p);
    jce_scene_set_transform(owner.scene, child, &c);
    jce_scene_set_transform(owner.scene, descendant, &d);
    REQUIRE(jce_scene_reparent(owner.scene, descendant, child, false));
    Snapshot before(owner.scene, child);
    auto child_world = jce_scene_get_world_matrix(owner.scene, child);
    auto descendant_world = jce_scene_get_world_matrix(owner.scene, descendant);
    REQUIRE(jce_scene_reparent(owner.scene, child, parent, true));
    Snapshot after(owner.scene, child);
    matrix_equal(child_world, jce_scene_get_world_matrix(owner.scene, child));
    matrix_equal(descendant_world, jce_scene_get_world_matrix(owner.scene, descendant));
    REQUIRE(jce_history_restore_entity(owner.scene, child, 0, true, 3, before.json, before.components));
    CHECK(jce_scene_get_parent(owner.scene, child) == 0);
    CHECK(jce_scene_get_transform(owner.scene, child)->position.x == c.position.x);
    matrix_equal(child_world, jce_scene_get_world_matrix(owner.scene, child));
    matrix_equal(descendant_world, jce_scene_get_world_matrix(owner.scene, descendant));
    REQUIRE(jce_history_restore_entity(owner.scene, child, parent, true, 3, after.json, after.components));
    CHECK(jce_scene_get_parent(owner.scene, child) == parent);
    matrix_equal(child_world, jce_scene_get_world_matrix(owner.scene, child));
}

TEST_CASE("scoped parent refuses cycle dead parent and malformed record before mutations") {
    SceneOwner owner; REQUIRE(owner.scene != nullptr);
    auto child = jce_scene_create_entity(owner.scene, "Child");
    auto descendant = jce_scene_create_entity(owner.scene, "Descendant");
    auto dead = jce_scene_create_entity(owner.scene, "Dead");
    REQUIRE(jce_scene_reparent(owner.scene, descendant, child, false));
    Snapshot before(owner.scene, child);
    auto world = jce_scene_get_world_matrix(owner.scene, child);
    jce_scene_destroy_entity(owner.scene, dead);
    CHECK_FALSE(jce_history_restore_entity(owner.scene, child, descendant, true, 3, before.json, before.components));
    CHECK_FALSE(jce_history_restore_entity(owner.scene, child, dead, true, 3, before.json, before.components));
    CHECK_FALSE(jce_history_restore_entity(owner.scene, child, 0, true, 3, "{", before.components));
    // A required logical parent resolving to zero must not become a root edit.
    CHECK_FALSE(jce_history_restore_entity(owner.scene, child, 0, true, 3, before.json, before.components, true));
    CHECK(jce_scene_get_parent(owner.scene, child) == 0);
    CHECK(jce_scene_get_parent(owner.scene, descendant) == child);
    matrix_equal(world, jce_scene_get_world_matrix(owner.scene, child));
}

TEST_CASE("duplicate display names do not select another scoped parent target") {
    SceneOwner owner; REQUIRE(owner.scene != nullptr);
    auto first = jce_scene_create_entity(owner.scene, "Duplicate");
    auto second = jce_scene_create_entity(owner.scene, "Duplicate");
    auto parent = jce_scene_create_entity(owner.scene, "Parent");
    JceEditorMeta meta{}; meta.enabled = true; strcpy(meta.name, "Duplicate");
    jce_scene_set_editor_meta(owner.scene, first, &meta);
    jce_scene_set_editor_meta(owner.scene, second, &meta);
    auto *first_meta = jce_scene_get_editor_meta(owner.scene, first);
    auto *second_meta = jce_scene_get_editor_meta(owner.scene, second);
    REQUIRE(first_meta != nullptr); REQUIRE(second_meta != nullptr);
    strcpy(first_meta->name, "Duplicate"); strcpy(second_meta->name, "Duplicate");
    auto a = transform(17, 0, 0), b = transform(29, 0, 0);
    jce_scene_set_transform(owner.scene, first, &a); jce_scene_set_transform(owner.scene, second, &b);
    Snapshot before(owner.scene, first);
    REQUIRE(jce_scene_reparent(owner.scene, first, parent, true));
    REQUIRE(jce_history_restore_entity(owner.scene, first, 0, true, 3, before.json, before.components));
    CHECK(jce_scene_get_parent(owner.scene, second) == 0);
    CHECK(jce_scene_get_transform(owner.scene, second)->position.x == 29);
    CHECK(jce_scene_get_transform(owner.scene, first)->position.x == 17);
    CHECK(std::string(jce_scene_get_editor_meta(owner.scene, first)->name) == "Duplicate");
    CHECK(std::string(jce_scene_get_editor_meta(owner.scene, second)->name) == "Duplicate");
}

TEST_CASE("scoped parent token follows actual full loader remap and rejects deleted parent") {
    SceneOwner owner; REQUIRE(owner.scene != nullptr);
    auto parent = jce_scene_create_entity(owner.scene, "Parent");
    auto child = jce_scene_create_entity(owner.scene, "Child");
    auto t = transform(2, 3, 4); jce_scene_set_transform(owner.scene, child, &t);
    REQUIRE(jce_scene_reparent(owner.scene, child, parent, false));
    Snapshot captured(owner.scene, child);
    JceEditorHistoryIdentity identity;
    auto parent_token = identity.token(parent), child_token = identity.token(child);
    JceJson *full = jce_scene_save_json(owner.scene);
    REQUIRE(full != nullptr);
    auto entities = jce_json_get(jce_json_get(full, "scene"), "entities");
    REQUIRE(entities != nullptr);
    JceEditorHistoryIdentity::Prepared prepared;
    REQUIRE(identity.prepare({{parent, parent_token}, {child, child_token}}, identity.epoch(), prepared));
    REQUIRE(jce_scene_clear(owner.scene) >= 0);
    auto *load = jce_scene_load_stream_begin(owner.scene, full, nullptr);
    REQUIRE(load != nullptr);
    jce_scene_load_stream_step(load, 0);
    uint32_t count = jce_scene_load_stream_new_entities(load, nullptr, 0);
    std::vector<JceEntity> created(count);
    jce_scene_load_stream_new_entities(load, created.data(), count);
    REQUIRE(jce_scene_load_stream_finalize(load) >= 0);
    std::unordered_map<uint64_t, uint64_t> remap;
    auto *item = jce_json_first_child(entities);
    for (uint32_t i = 0; i < count; ++i, item = jce_json_next_sibling(item)) {
        REQUIRE(item != nullptr);
        remap[(uint64_t)jce_json_get_number(item, "id", 0)] = created[i];
    }
    REQUIRE(identity.commit(prepared, {remap.at(parent), remap.at(child)}));
    auto new_parent = identity.handle(parent_token), new_child = identity.handle(child_token);
    REQUIRE(jce_scene_reparent(owner.scene, new_child, 0, false));
    REQUIRE(jce_history_restore_entity(owner.scene, new_child, new_parent, true, count,
                                      captured.json, captured.components));
    CHECK(jce_scene_get_parent(owner.scene, new_child) == new_parent);
    REQUIRE(jce_scene_reparent(owner.scene, new_child, 0, false));
    jce_scene_destroy_entity(owner.scene, new_parent);
    CHECK_FALSE(jce_history_restore_entity(owner.scene, new_child, identity.handle(parent_token),
                                          true, count, captured.json, captured.components));
    CHECK(jce_scene_get_parent(owner.scene, new_child) == 0);
    CHECK(jce_scene_entity_alive(owner.scene, new_child));
    jce_json_free(full);
}
