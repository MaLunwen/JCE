#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "panels/jce_terrain_history.h"
#include "core/jce_editor_history_order.h"

#include <jce/middleware/scene/jce_terrain.h>
#include <jce/renderer/jce_scene_renderer.h>

#include <cstddef>
#include <cstdint>

namespace {

struct TerrainOwner {
    JceTerrain *value = nullptr;

    TerrainOwner()
        : value(jce_terrain_create(65, 65, 64.0f, 64.0f, 20.0f, 32))
    {
    }

    ~TerrainOwner()
    {
        jce_terrain_free(value);
    }
};

size_t index_of(int x, int z)
{
    return (size_t)z * 65u + (size_t)x;
}

float *mutable_heights(JceTerrain *terrain)
{
    return const_cast<float *>(jce_terrain_heights(terrain));
}

uint32_t *mutable_splat(JceTerrain *terrain)
{
    return const_cast<uint32_t *>(jce_terrain_splat(terrain));
}

}  // namespace

TEST_CASE("terrain history restores only captured height tiles")
{
    TerrainOwner terrain;
    REQUIRE(terrain.value != nullptr);
    JceTerrainHistory history;
    history.attach(terrain.value);

    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        10.0f, 10.0f, 12.0f, 12.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    mutable_heights(terrain.value)[index_of(11, 11)] = 7.0f;
    mutable_heights(terrain.value)[index_of(50, 50)] = 9.0f;
    REQUIRE(history.commit_edit());

    JceTerrainHistoryChange change;
    REQUIRE(history.undo(&change));
    CHECK(mutable_heights(terrain.value)[index_of(11, 11)] == doctest::Approx(0.0f));
    CHECK(mutable_heights(terrain.value)[index_of(50, 50)] == doctest::Approx(9.0f));
    CHECK(change.edit_flags == JCE_TERRAIN_EDIT_HEIGHTS);
    CHECK(change.min_x <= 10.0f);
    CHECK(change.max_x >= 12.0f);

    REQUIRE(history.redo(&change));
    CHECK(mutable_heights(terrain.value)[index_of(11, 11)] == doctest::Approx(7.0f));
    CHECK(mutable_heights(terrain.value)[index_of(50, 50)] == doctest::Approx(9.0f));
}

TEST_CASE("terrain history restores splat and hole channels together")
{
    TerrainOwner terrain;
    REQUIRE(terrain.value != nullptr);
    JceTerrainHistory history;
    history.attach(terrain.value);
    const size_t sample = index_of(33, 33);
    const uint32_t original_splat = mutable_splat(terrain.value)[sample];

    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        33.0f, 33.0f, 34.0f, 34.0f,
        JCE_TERRAIN_EDIT_SPLAT | JCE_TERRAIN_EDIT_HOLES));
    mutable_splat(terrain.value)[sample] = 0x44332211u;
    jce_terrain_set_hole(terrain.value, 33, 33, true);
    REQUIRE(history.commit_edit());

    JceTerrainHistoryChange change;
    REQUIRE(history.undo(&change));
    CHECK(mutable_splat(terrain.value)[sample] == original_splat);
    CHECK_FALSE(jce_terrain_cell_is_hole(terrain.value, 33, 33));
    CHECK((change.edit_flags & JCE_TERRAIN_EDIT_SPLAT) != 0u);
    CHECK((change.edit_flags & JCE_TERRAIN_EDIT_HOLES) != 0u);

    REQUIRE(history.redo(&change));
    CHECK(mutable_splat(terrain.value)[sample] == 0x44332211u);
    CHECK(jce_terrain_cell_is_hole(terrain.value, 33, 33));
}

TEST_CASE("overlapping captures store each tile channel once")
{
    TerrainOwner terrain;
    REQUIRE(terrain.value != nullptr);
    JceTerrainHistory history;
    history.attach(terrain.value);

    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        1.0f, 1.0f, 2.0f, 2.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    mutable_heights(terrain.value)[index_of(1, 1)] = 1.0f;
    REQUIRE(history.commit_edit());
    const size_t one_capture_bytes = history.bytes();

    history.clear();
    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        1.0f, 1.0f, 2.0f, 2.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    REQUIRE(history.capture_region(
        2.0f, 2.0f, 3.0f, 3.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    mutable_heights(terrain.value)[index_of(2, 2)] = 2.0f;
    REQUIRE(history.commit_edit());
    CHECK(history.bytes() == one_capture_bytes);
}

TEST_CASE("terrain history budget failure leaves no partial command")
{
    TerrainOwner terrain;
    REQUIRE(terrain.value != nullptr);
    JceTerrainHistory history(64u);
    history.attach(terrain.value);

    REQUIRE(history.begin_edit());
    CHECK_FALSE(history.capture_region(
        0.0f, 0.0f, 1.0f, 1.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    CHECK_FALSE(history.commit_edit());
    CHECK_FALSE(history.can_undo());
    CHECK(history.bytes() == 0u);
}

TEST_CASE("a new terrain edit invalidates the redo branch")
{
    TerrainOwner terrain;
    REQUIRE(terrain.value != nullptr);
    JceTerrainHistory history;
    history.attach(terrain.value);

    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        4.0f, 4.0f, 5.0f, 5.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    mutable_heights(terrain.value)[index_of(4, 4)] = 3.0f;
    REQUIRE(history.commit_edit());
    JceTerrainHistoryChange change;
    REQUIRE(history.undo(&change));
    REQUIRE(history.can_redo());

    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        8.0f, 8.0f, 9.0f, 9.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    mutable_heights(terrain.value)[index_of(8, 8)] = 5.0f;
    REQUIRE(history.commit_edit());
    CHECK_FALSE(history.can_redo());
}

TEST_CASE("a no-op terrain transaction preserves the redo branch")
{
    TerrainOwner terrain;
    REQUIRE(terrain.value != nullptr);
    JceTerrainHistory history;
    history.attach(terrain.value);

    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        4.0f, 4.0f, 5.0f, 5.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    mutable_heights(terrain.value)[index_of(4, 4)] = 3.0f;
    REQUIRE(history.commit_edit());
    JceTerrainHistoryChange change;
    REQUIRE(history.undo(&change));
    REQUIRE(history.can_redo());

    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        8.0f, 8.0f, 9.0f, 9.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    CHECK_FALSE(history.commit_edit());
    CHECK(history.undo_count() == 0u);
    CHECK(history.redo_count() == 1u);
}

TEST_CASE("terrain history preserves its global chronological sequence")
{
    TerrainOwner terrain;
    REQUIRE(terrain.value != nullptr);
    JceTerrainHistory history;
    history.attach(terrain.value);

    REQUIRE(history.begin_edit());
    REQUIRE(history.capture_region(
        1.0f, 1.0f, 2.0f, 2.0f, JCE_TERRAIN_EDIT_HEIGHTS));
    mutable_heights(terrain.value)[index_of(1, 1)] = 1.0f;
    REQUIRE(history.commit_edit());
    CHECK(history.undo_sequence() == 0u);
    REQUIRE(history.set_latest_undo_sequence(17u));
    CHECK(history.undo_sequence() == 17u);

    JceTerrainHistoryChange change;
    REQUIRE(history.undo(&change));
    CHECK(history.redo_sequence() == 17u);
    REQUIRE(history.redo(&change));
    CHECK(history.undo_sequence() == 17u);
    REQUIRE(history.undo(&change));
    history.clear_redo();
    CHECK(history.redo_sequence() == 0u);
}

TEST_CASE("global history ordering selects latest undo and earliest redo")
{
    const uint64_t interleaved[] = {4u, 9u, 6u};
    JceEditorHistorySelection selected =
        jce_editor_history_select_undo(interleaved, 3u);
    CHECK(selected.index == 1u);
    CHECK(selected.sequence == 9u);

    selected = jce_editor_history_select_redo(interleaved, 3u);
    CHECK(selected.index == 0u);
    CHECK(selected.sequence == 4u);

    const uint64_t ties[] = {5u, 5u};
    selected = jce_editor_history_select_undo(ties, 2u);
    CHECK(selected.index == 0u);
    const uint64_t empty[] = {0u, 0u};
    selected = jce_editor_history_select_redo(empty, 2u);
    CHECK(selected.index == static_cast<size_t>(-1));
    CHECK(selected.sequence == 0u);
}
