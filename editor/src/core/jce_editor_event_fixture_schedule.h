#ifndef JCE_EDITOR_EVENT_FIXTURE_SCHEDULE_H
#define JCE_EDITOR_EVENT_FIXTURE_SCHEDULE_H

#include "jce_editor_event_fixture.h"
#include "jce_editor_game_input_bridge.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_timer.h>
#include <cstdio>
#include <cstring>

/* Explicit synthetic QA source, owned by one Editor host session. The ordinary
 * frame mode performs no clock work or marker polling. Elapsed mode never
 * changes native focus, hover, capture, or runtime/viewport ownership. */
struct JceEditorEventFixture {
    char *data = nullptr;
    size_t offset = 0;
    char start_path[4096] = {};
    bool elapsed = false, pending = false, end_pending = false;
    bool active = false, waiting_admitted = false, clock_sampled = false, poll_started = false;
    unsigned frame = 0, due = 0;
    uint64_t frequency = 0, origin = 0, previous = 0, last_poll = 0;
    const void *runtime_owner = nullptr;
    JceEditorGameInputViewport geometry = {};
    char kind = 0;
    double x = 0, y = 0, a = 0, b = 0;

    JceEditorEventFixture() = default;
    JceEditorEventFixture(const JceEditorEventFixture &) = delete;
    JceEditorEventFixture &operator=(const JceEditorEventFixture &) = delete;
    ~JceEditorEventFixture() { release_file(); }

    void release_file()
    {
        jce_fs_buffer_free(data); data = nullptr; offset = 0;
    }

    void end(JceEditorGameInputBridge *bridge)
    {
        release_file();
        jce_editor_game_input_bridge_clear_virtual(bridge);
        start_path[0] = 0;
        elapsed = pending = end_pending = active = waiting_admitted = false;
        clock_sampled = poll_started = false;
        frame = due = 0;
        frequency = origin = previous = last_poll = 0;
        runtime_owner = nullptr;
        geometry = {};
    }

    void reject(const char *reason, JceEditorGameInputBridge *bridge = nullptr)
    {
        LOG_ERROR("editor-main", "Game event fixture refused: %s", reason);
        release_file(); active = pending = false;
        if (bridge) {
            jce_editor_game_input_bridge_clear_virtual(bridge);
            end_pending = false; runtime_owner = nullptr; geometry = {};
        }
    }

    bool marker_exists() const
    {
        return jce_fs_host_exists_file(start_path) || jce_fs_host_exists_dir(start_path);
    }

    /* Environment values are borrowed only for this call, never per draw. */
    void begin(const char *path, const char *clock, const char *start)
    {
        end(nullptr);
        if (!path || !path[0]) return;
        elapsed = clock && std::strcmp(clock, "elapsed_ms") == 0;
        if (clock && clock[0] && std::strcmp(clock, "frame") != 0 && !elapsed) {
            reject("JCE_KPI_GAME_EVENTS_CLOCK must be frame or elapsed_ms"); return;
        }
        if (start && start[0]) {
            if (!elapsed || !jce_path_is_absolute(start) ||
                std::strlen(start) >= sizeof(start_path)) {
                reject("JCE_KPI_GAME_EVENTS_START requires elapsed_ms and an absolute host path"); return;
            }
#if JCE_PLATFORM_WINDOWS
            if ((start[0] == '/' || start[0] == '\\') &&
                start[1] != '/' && start[1] != '\\') {
                reject("start marker must name a fully qualified host path"); return;
            }
#endif
            std::memcpy(start_path, start, std::strlen(start) + 1);
            if (marker_exists()) { reject("start marker already exists"); return; }
        }
        uint64_t read = 0, total = 0;
        data = static_cast<char *>(jce_fs_host_read_capped(path, 1048576, &read, &total));
        if (!data || read != total) { reject("cannot read complete <=1 MiB event file"); return; }
        if (elapsed) {
            frequency = jce_time_perf_freq();
            if (!frequency) reject("performance clock frequency is zero");
        }
    }

    static bool admitted(const JceEditorGameInputViewport &v, bool playing,
                         const void *runtime)
    {
        JceEvent unused{};
        return playing && runtime && v.visible &&
            v.owner == JCE_EDITOR_GAME_INPUT_RUNTIME &&
            jce_editor_fixture_event('M', 0, 0, 0, 0,
                v.x, v.y, v.width, v.height, &unused) &&
            std::isfinite(v.x + v.width) && std::isfinite(v.y + v.height) &&
            std::isfinite(v.render_width) && std::isfinite(v.render_height) &&
            v.render_width >= 0 && v.render_height >= 0 &&
            std::isfinite(v.render_width / v.width) &&
            std::isfinite(v.render_height / v.height);
    }

    bool same_admission(const JceEditorGameInputViewport &v, const void *runtime) const
    {
        return runtime_owner == runtime && geometry.x == v.x && geometry.y == v.y &&
            geometry.width == v.width && geometry.height == v.height &&
            geometry.render_width == v.render_width && geometry.render_height == v.render_height;
    }

    void validate_active(JceEditorGameInputBridge *bridge, bool playing, const void *runtime)
    {
        if (elapsed && (active || end_pending) && bridge &&
            (!admitted(bridge->viewport, playing, runtime) ||
             !same_admission(bridge->viewport, runtime)))
            reject("elapsed admission changed; remainder cancelled", bridge);
    }

    void update(JceEditorGameInputBridge *bridge, bool playing, const void *runtime,
                bool (*process_key)(const JceEvent *))
    {
        /* Preserve the last row through its normal following runtime commit. */
        if (end_pending) {
            jce_editor_game_input_bridge_clear_virtual(bridge);
            end_pending = active = pending = false;
            runtime_owner = nullptr; geometry = {};
        }
        if (!data || !bridge) return;
        const auto &vp = bridge->viewport;
        long double position = 0;
        if (elapsed) {
            const bool valid = admitted(vp, playing, runtime);
            validate_active(bridge, playing, runtime);
            if (!data) return;
            if (!valid) { waiting_admitted = false; return; }
            const uint64_t now = jce_time_perf_counter();
            if (clock_sampled && now < previous) {
                reject("performance clock moved backwards", bridge); return;
            }
            previous = now; clock_sampled = true;
            if (!active) {
                if (start_path[0]) {
                    const uint64_t interval = frequency / 10 + (frequency % 10 != 0);
                    if (poll_started && now - last_poll < interval) return;
                    poll_started = true; last_poll = now;
                    if (!waiting_admitted) {
                        waiting_admitted = true;
                        if (marker_exists()) reject("start marker predates admitted Play", bridge);
                        return;
                    }
                    if (jce_fs_host_exists_dir(start_path)) {
                        reject("start marker is a directory", bridge); return;
                    }
                    if (!jce_fs_host_exists_file(start_path)) return;
                }
                active = true; origin = now;
                runtime_owner = runtime; geometry = vp;
                jce_log_write(JCE_LOG_LEVEL_INFO, "editor-main", __FILE__, __LINE__,
                    "Game event fixture source=synthetic elapsed_ms started marker=%s",
                    start_path[0] ? start_path : "none");
            }
            position = static_cast<long double>(now - origin) * 1000 / frequency;
        } else {
            if (!playing || !vp.visible || vp.width <= 0 || vp.height <= 0) return;
            position = ++frame;
        }
        for (;;) {
            if (!pending) {
                int consumed = 0; double row_position = 0;
                const int fields = std::sscanf(data + offset, " %lf %c %lf %lf %lf %lf %n",
                    &row_position, &kind, &x, &y, &a, &b, &consumed);
                if (fields == EOF) {
                    release_file(); active = false; end_pending = true; return;
                }
                if (fields != 6 || consumed <= 0 || !jce_editor_fixture_frame(row_position, &due)) {
                    reject("invalid row"); end_pending = true; return;
                }
                offset += static_cast<size_t>(consumed); pending = true;
            }
            if (due > position) return;
            JceEvent event{};
            if (!jce_editor_fixture_event(kind, x, y, a, b,
                    vp.x, vp.y, vp.width, vp.height, &event)) {
                reject("invalid event kind/data"); end_pending = true; return;
            }
            int accepted = -1; /* Key dispatch is not a runtime acceptance witness. */
            if (kind == 'K') { if (process_key) process_key(&event); }
            else accepted = jce_editor_game_input_bridge_handle_virtual_event(bridge, &event) ? 1 : 0;
            jce_log_write(JCE_LOG_LEVEL_INFO, "editor-main", __FILE__, __LINE__,
                "Game event fixture source=%s frame=%u type=%c accepted=%d owner=%d focused=%d hovered=%d captured=%d clock=%s due=%u",
                kind == 'K' ? "synthetic-editor-key" : "synthetic-virtual-game",
                frame, kind, accepted, static_cast<int>(vp.owner), vp.focused ? 1 : 0,
                vp.hovered ? 1 : 0, vp.captured ? 1 : 0, elapsed ? "elapsed_ms" : "frame", due);
            pending = false;
        }
    }
};

#endif
