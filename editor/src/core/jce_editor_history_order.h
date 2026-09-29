#pragma once

#include <cstddef>
#include <cstdint>

struct JceEditorHistorySelection {
    size_t index = static_cast<size_t>(-1);
    uint64_t sequence = 0u;
};

inline JceEditorHistorySelection jce_editor_history_select_undo(
    const uint64_t *sequences, size_t count)
{
    JceEditorHistorySelection selected;
    if (!sequences) return selected;
    for (size_t i = 0; i < count; ++i) {
        if (sequences[i] > selected.sequence) {
            selected.index = i;
            selected.sequence = sequences[i];
        }
    }
    return selected;
}

inline JceEditorHistorySelection jce_editor_history_select_redo(
    const uint64_t *sequences, size_t count)
{
    JceEditorHistorySelection selected;
    if (!sequences) return selected;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t sequence = sequences[i];
        if (sequence != 0u &&
            (selected.sequence == 0u || sequence < selected.sequence)) {
            selected.index = i;
            selected.sequence = sequence;
        }
    }
    return selected;
}
