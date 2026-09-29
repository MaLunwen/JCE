#include <panels/jce_terrain_history.h>

#include <jce/middleware/scene/jce_terrain.h>
#include <jce/renderer/jce_scene_renderer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

JceTerrainHistory::JceTerrainHistory(size_t budget_bytes)
    : budget_bytes_(std::max<size_t>(budget_bytes, 1u))
{
}

void JceTerrainHistory::attach(JceTerrain *terrain)
{
    if (terrain_ == terrain) return;
    terrain_ = terrain;
    clear();
}

void JceTerrainHistory::clear()
{
    undo_.clear();
    redo_.clear();
    reset_active();
}

bool JceTerrainHistory::can_undo() const
{
    return !undo_.empty();
}

bool JceTerrainHistory::can_redo() const
{
    return !redo_.empty();
}

uint64_t JceTerrainHistory::undo_sequence() const
{
    return undo_.empty() ? 0u : undo_.back().sequence;
}

uint64_t JceTerrainHistory::redo_sequence() const
{
    return redo_.empty() ? 0u : redo_.back().sequence;
}

bool JceTerrainHistory::set_latest_undo_sequence(uint64_t sequence)
{
    if (undo_.empty() || sequence == 0u) return false;
    undo_.back().sequence = sequence;
    return true;
}

void JceTerrainHistory::clear_redo()
{
    redo_.clear();
}

size_t JceTerrainHistory::bytes() const
{
    size_t total = 0;
    for (const Delta &delta : undo_) total += delta.bytes;
    for (const Delta &delta : redo_) total += delta.bytes;
    return total;
}

size_t JceTerrainHistory::undo_count() const
{
    return undo_.size();
}

size_t JceTerrainHistory::redo_count() const
{
    return redo_.size();
}

uint64_t JceTerrainHistory::tile_key(int tx, int tz)
{
    return ((uint64_t)(uint32_t)tz << 32u) | (uint32_t)tx;
}

void JceTerrainHistory::reset_active()
{
    active_ = Delta{};
    active_tiles_.clear();
    active_valid_ = false;
}

void JceTerrainHistory::make_room(size_t active_bytes)
{
    while (bytes() + active_bytes > budget_bytes_) {
        if (!redo_.empty())
            redo_.erase(redo_.begin());
        else if (!undo_.empty())
            undo_.erase(undo_.begin());
        else
            break;
    }
}

bool JceTerrainHistory::begin_edit()
{
    reset_active();
    if (!terrain_ || !jce_terrain_heights(terrain_)) return false;
    active_.terrain_width = jce_terrain_width(terrain_);
    active_.terrain_height = jce_terrain_height(terrain_);
    active_.bytes = sizeof(Delta);
    active_valid_ = active_.terrain_width > 0 &&
                    active_.terrain_height > 0 &&
                    active_.bytes <= budget_bytes_;
    return active_valid_;
}

void JceTerrainHistory::capture_tile_channels(TileState &tile,
                                               uint32_t edit_flags)
{
    const int terrain_width = jce_terrain_width(terrain_);
    const float *heights = jce_terrain_heights(terrain_);
    const uint32_t *splat = jce_terrain_splat(terrain_);

    if ((edit_flags & JCE_TERRAIN_EDIT_HEIGHTS) &&
        !(tile.flags & JCE_TERRAIN_EDIT_HEIGHTS) && heights) {
        tile.heights.resize((size_t)tile.width * (size_t)tile.height);
        for (int z = 0; z < tile.height; ++z)
            std::memcpy(tile.heights.data() + (size_t)z * tile.width,
                        heights + (size_t)(tile.z0 + z) * terrain_width +
                            tile.x0,
                        (size_t)tile.width * sizeof(float));
        tile.flags |= JCE_TERRAIN_EDIT_HEIGHTS;
    }
    if ((edit_flags & JCE_TERRAIN_EDIT_SPLAT) &&
        !(tile.flags & JCE_TERRAIN_EDIT_SPLAT) && splat) {
        tile.splat.resize((size_t)tile.width * (size_t)tile.height);
        for (int z = 0; z < tile.height; ++z)
            std::memcpy(tile.splat.data() + (size_t)z * tile.width,
                        splat + (size_t)(tile.z0 + z) * terrain_width +
                            tile.x0,
                        (size_t)tile.width * sizeof(uint32_t));
        tile.flags |= JCE_TERRAIN_EDIT_SPLAT;
    }
    if ((edit_flags & JCE_TERRAIN_EDIT_HOLES) &&
        !(tile.flags & JCE_TERRAIN_EDIT_HOLES)) {
        tile.holes.resize((size_t)tile.hole_width *
                          (size_t)tile.hole_height);
        for (int z = 0; z < tile.hole_height; ++z)
            for (int x = 0; x < tile.hole_width; ++x)
                tile.holes[(size_t)z * tile.hole_width + x] =
                    jce_terrain_cell_is_hole(terrain_, tile.x0 + x,
                                             tile.z0 + z) ? 1u : 0u;
        tile.flags |= JCE_TERRAIN_EDIT_HOLES;
    }
}

bool JceTerrainHistory::capture_region(float min_x, float min_z,
                                       float max_x, float max_z,
                                       uint32_t edit_flags)
{
    if (!active_valid_ || !terrain_ || edit_flags == 0u) return false;
    const int terrain_width = jce_terrain_width(terrain_);
    const int terrain_height = jce_terrain_height(terrain_);
    const float world_x = jce_terrain_world_size_x(terrain_);
    const float world_z = jce_terrain_world_size_z(terrain_);
    if (terrain_width != active_.terrain_width ||
        terrain_height != active_.terrain_height ||
        world_x <= 0.0f || world_z <= 0.0f)
        return false;

    if (min_x > max_x) std::swap(min_x, max_x);
    if (min_z > max_z) std::swap(min_z, max_z);
    min_x = std::clamp(min_x, 0.0f, world_x);
    min_z = std::clamp(min_z, 0.0f, world_z);
    max_x = std::clamp(max_x, 0.0f, world_x);
    max_z = std::clamp(max_z, 0.0f, world_z);
    const float sx = (float)(terrain_width - 1) / world_x;
    const float sz = (float)(terrain_height - 1) / world_z;
    const int gx0 = std::clamp((int)std::floor(min_x * sx), 0,
                               terrain_width - 1);
    const int gz0 = std::clamp((int)std::floor(min_z * sz), 0,
                               terrain_height - 1);
    const int gx1 = std::clamp((int)std::ceil(max_x * sx), 0,
                               terrain_width - 1);
    const int gz1 = std::clamp((int)std::ceil(max_z * sz), 0,
                               terrain_height - 1);
    const int tx0 = gx0 / kTileDimension;
    const int tz0 = gz0 / kTileDimension;
    const int tx1 = gx1 / kTileDimension;
    const int tz1 = gz1 / kTileDimension;

    for (int tz = tz0; tz <= tz1; ++tz) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const uint64_t key = tile_key(tx, tz);
            auto found = active_tiles_.find(key);
            size_t index = 0;
            if (found == active_tiles_.end()) {
                TileState tile;
                tile.x0 = tx * kTileDimension;
                tile.z0 = tz * kTileDimension;
                tile.width = std::min(kTileDimension,
                                      terrain_width - tile.x0);
                tile.height = std::min(kTileDimension,
                                       terrain_height - tile.z0);
                tile.hole_width = std::min(
                    kTileDimension,
                    std::max(0, terrain_width - 1 - tile.x0));
                tile.hole_height = std::min(
                    kTileDimension,
                    std::max(0, terrain_height - 1 - tile.z0));
                index = active_.tiles.size();
                active_.tiles.push_back(std::move(tile));
                active_tiles_.emplace(key, index);
                active_.bytes += sizeof(TileState);
            } else {
                index = found->second;
            }

            TileState &tile = active_.tiles[index];
            const size_t before = tile.heights.size() * sizeof(float) +
                                  tile.splat.size() * sizeof(uint32_t) +
                                  tile.holes.size();
            capture_tile_channels(tile, edit_flags);
            const size_t after = tile.heights.size() * sizeof(float) +
                                 tile.splat.size() * sizeof(uint32_t) +
                                 tile.holes.size();
            active_.bytes += after - before;
            if (active_.bytes > budget_bytes_) {
                reset_active();
                return false;
            }
            make_room(active_.bytes);
        }
    }
    return true;
}

bool JceTerrainHistory::commit_edit()
{
    if (!active_valid_ || active_.tiles.empty()) {
        reset_active();
        return false;
    }
    if (!active_changed()) {
        reset_active();
        return false;
    }
    redo_.clear();
    undo_.push_back(std::move(active_));
    active_tiles_.clear();
    active_valid_ = false;
    if (undo_.size() > kEntryLimit) undo_.erase(undo_.begin());
    make_room(0);
    return true;
}

bool JceTerrainHistory::active_changed() const
{
    if (!terrain_ ||
        active_.terrain_width != jce_terrain_width(terrain_) ||
        active_.terrain_height != jce_terrain_height(terrain_))
        return false;

    const int terrain_width = active_.terrain_width;
    const float *heights = jce_terrain_heights(terrain_);
    const uint32_t *splat = jce_terrain_splat(terrain_);
    for (const TileState &tile : active_.tiles) {
        if ((tile.flags & JCE_TERRAIN_EDIT_HEIGHTS) && heights &&
            tile.heights.size() ==
                (size_t)tile.width * (size_t)tile.height) {
            for (int z = 0; z < tile.height; ++z) {
                const float *current =
                    heights + (size_t)(tile.z0 + z) * terrain_width + tile.x0;
                const float *captured =
                    tile.heights.data() + (size_t)z * tile.width;
                if (std::memcmp(current, captured,
                                (size_t)tile.width * sizeof(float)) != 0)
                    return true;
            }
        }
        if ((tile.flags & JCE_TERRAIN_EDIT_SPLAT) && splat &&
            tile.splat.size() ==
                (size_t)tile.width * (size_t)tile.height) {
            for (int z = 0; z < tile.height; ++z) {
                const uint32_t *current =
                    splat + (size_t)(tile.z0 + z) * terrain_width + tile.x0;
                const uint32_t *captured =
                    tile.splat.data() + (size_t)z * tile.width;
                if (std::memcmp(current, captured,
                                (size_t)tile.width * sizeof(uint32_t)) != 0)
                    return true;
            }
        }
        if ((tile.flags & JCE_TERRAIN_EDIT_HOLES) &&
            tile.holes.size() ==
                (size_t)tile.hole_width * (size_t)tile.hole_height) {
            for (int z = 0; z < tile.hole_height; ++z) {
                for (int x = 0; x < tile.hole_width; ++x) {
                    const bool current = jce_terrain_cell_is_hole(
                        terrain_, tile.x0 + x, tile.z0 + z);
                    const bool captured =
                        tile.holes[(size_t)z * tile.hole_width + x] != 0u;
                    if (current != captured) return true;
                }
            }
        }
    }
    return false;
}

void JceTerrainHistory::cancel_edit()
{
    reset_active();
}

void JceTerrainHistory::discard_last_undo()
{
    if (!undo_.empty()) undo_.pop_back();
}

bool JceTerrainHistory::exchange(Delta &delta,
                                 JceTerrainHistoryChange *out_change)
{
    if (!terrain_ ||
        delta.terrain_width != jce_terrain_width(terrain_) ||
        delta.terrain_height != jce_terrain_height(terrain_))
        return false;

    float *heights =
        const_cast<float *>(jce_terrain_heights(terrain_));
    uint32_t *splat =
        const_cast<uint32_t *>(jce_terrain_splat(terrain_));
    uint32_t flags = 0u;
    int min_grid_x = delta.terrain_width - 1;
    int min_grid_z = delta.terrain_height - 1;
    int max_grid_x = 0;
    int max_grid_z = 0;

    for (TileState &tile : delta.tiles) {
        if ((tile.flags & JCE_TERRAIN_EDIT_HEIGHTS) && heights &&
            tile.heights.size() ==
                (size_t)tile.width * (size_t)tile.height) {
            std::vector<float> current(tile.heights.size());
            for (int z = 0; z < tile.height; ++z)
                std::memcpy(
                    current.data() + (size_t)z * tile.width,
                    heights + (size_t)(tile.z0 + z) *
                                   delta.terrain_width +
                        tile.x0,
                    (size_t)tile.width * sizeof(float));
            for (int z = 0; z < tile.height; ++z)
                std::memcpy(
                    heights + (size_t)(tile.z0 + z) *
                                  delta.terrain_width +
                        tile.x0,
                    tile.heights.data() + (size_t)z * tile.width,
                    (size_t)tile.width * sizeof(float));
            tile.heights.swap(current);
        }
        if ((tile.flags & JCE_TERRAIN_EDIT_SPLAT) && splat &&
            tile.splat.size() ==
                (size_t)tile.width * (size_t)tile.height) {
            std::vector<uint32_t> current(tile.splat.size());
            for (int z = 0; z < tile.height; ++z)
                std::memcpy(
                    current.data() + (size_t)z * tile.width,
                    splat + (size_t)(tile.z0 + z) *
                                 delta.terrain_width +
                        tile.x0,
                    (size_t)tile.width * sizeof(uint32_t));
            for (int z = 0; z < tile.height; ++z)
                std::memcpy(
                    splat + (size_t)(tile.z0 + z) *
                                delta.terrain_width +
                        tile.x0,
                    tile.splat.data() + (size_t)z * tile.width,
                    (size_t)tile.width * sizeof(uint32_t));
            tile.splat.swap(current);
        }
        if ((tile.flags & JCE_TERRAIN_EDIT_HOLES) &&
            tile.holes.size() ==
                (size_t)tile.hole_width * (size_t)tile.hole_height) {
            std::vector<uint8_t> current(tile.holes.size());
            for (int z = 0; z < tile.hole_height; ++z)
                for (int x = 0; x < tile.hole_width; ++x)
                    current[(size_t)z * tile.hole_width + x] =
                        jce_terrain_cell_is_hole(
                            terrain_, tile.x0 + x, tile.z0 + z) ? 1u : 0u;
            for (int z = 0; z < tile.hole_height; ++z)
                for (int x = 0; x < tile.hole_width; ++x)
                    jce_terrain_set_hole(
                        terrain_, tile.x0 + x, tile.z0 + z,
                        tile.holes[(size_t)z * tile.hole_width + x] != 0u);
            tile.holes.swap(current);
        }
        flags |= tile.flags;
        min_grid_x = std::min(min_grid_x, tile.x0);
        min_grid_z = std::min(min_grid_z, tile.z0);
        max_grid_x = std::max(max_grid_x, tile.x0 + tile.width - 1);
        max_grid_z = std::max(max_grid_z, tile.z0 + tile.height - 1);
        if (tile.hole_width > 0)
            max_grid_x = std::max(max_grid_x,
                                  tile.x0 + tile.hole_width);
        if (tile.hole_height > 0)
            max_grid_z = std::max(max_grid_z,
                                  tile.z0 + tile.hole_height);
    }

    if (out_change) {
        const float scale_x = jce_terrain_world_size_x(terrain_) /
                              (float)(delta.terrain_width - 1);
        const float scale_z = jce_terrain_world_size_z(terrain_) /
                              (float)(delta.terrain_height - 1);
        out_change->min_x = (float)min_grid_x * scale_x;
        out_change->min_z = (float)min_grid_z * scale_z;
        out_change->max_x = (float)max_grid_x * scale_x;
        out_change->max_z = (float)max_grid_z * scale_z;
        out_change->edit_flags = flags;
    }
    return true;
}

bool JceTerrainHistory::undo(JceTerrainHistoryChange *out_change)
{
    if (undo_.empty() || !terrain_) return false;
    Delta target = std::move(undo_.back());
    undo_.pop_back();
    if (!exchange(target, out_change)) {
        undo_.push_back(std::move(target));
        return false;
    }
    redo_.push_back(std::move(target));
    if (redo_.size() > kEntryLimit) redo_.erase(redo_.begin());
    make_room(0);
    return true;
}

bool JceTerrainHistory::redo(JceTerrainHistoryChange *out_change)
{
    if (redo_.empty() || !terrain_) return false;
    Delta target = std::move(redo_.back());
    redo_.pop_back();
    if (!exchange(target, out_change)) {
        redo_.push_back(std::move(target));
        return false;
    }
    undo_.push_back(std::move(target));
    if (undo_.size() > kEntryLimit) undo_.erase(undo_.begin());
    make_room(0);
    return true;
}
