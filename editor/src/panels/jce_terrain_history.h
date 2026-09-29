#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct JceTerrain;

struct JceTerrainHistoryChange {
    float min_x = 0.0f;
    float min_z = 0.0f;
    float max_x = 0.0f;
    float max_z = 0.0f;
    uint32_t edit_flags = 0u;
};

class JceTerrainHistory {
public:
    static constexpr size_t kDefaultBudgetBytes = 72u * 1024u * 1024u;

    explicit JceTerrainHistory(
        size_t budget_bytes = kDefaultBudgetBytes);

    void attach(JceTerrain *terrain);
    void clear();

    bool begin_edit();
    bool capture_region(float min_x, float min_z,
                        float max_x, float max_z, uint32_t edit_flags);
    bool commit_edit();
    void cancel_edit();

    void discard_last_undo();

    bool undo(JceTerrainHistoryChange *out_change);
    bool redo(JceTerrainHistoryChange *out_change);

    bool can_undo() const;
    bool can_redo() const;
    uint64_t undo_sequence() const;
    uint64_t redo_sequence() const;
    bool set_latest_undo_sequence(uint64_t sequence);
    void clear_redo();
    size_t bytes() const;
    size_t undo_count() const;
    size_t redo_count() const;

private:
    struct TileState {
        int x0 = 0;
        int z0 = 0;
        int width = 0;
        int height = 0;
        int hole_width = 0;
        int hole_height = 0;
        uint32_t flags = 0u;
        std::vector<float> heights;
        std::vector<uint32_t> splat;
        std::vector<uint8_t> holes;
    };

    struct Delta {
        uint64_t sequence = 0;
        int terrain_width = 0;
        int terrain_height = 0;
        size_t bytes = 0;
        std::vector<TileState> tiles;
    };

    static constexpr size_t kEntryLimit = 64;
    static constexpr int kTileDimension = 32;

    static uint64_t tile_key(int tx, int tz);
    void reset_active();
    void make_room(size_t active_bytes);
    void capture_tile_channels(TileState &tile, uint32_t edit_flags);
    bool active_changed() const;
    bool exchange(Delta &delta, JceTerrainHistoryChange *out_change);

    JceTerrain *terrain_ = nullptr;
    size_t budget_bytes_ = kDefaultBudgetBytes;
    std::vector<Delta> undo_;
    std::vector<Delta> redo_;
    Delta active_;
    std::unordered_map<uint64_t, size_t> active_tiles_;
    bool active_valid_ = false;
};
