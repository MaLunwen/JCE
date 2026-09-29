/*
 * snake_body.cpp -- MODULE 5 of 7: THE BODY, in C++.
 *
 * Owns where every segment is, and the one rule that needs to know that:
 * the head may not enter a cell the body occupies.
 *
 * HOW IT FOLLOWS.  Not by shuffling each segment onto the one in front,
 * which needs the segments to be correct already and drifts the moment one
 * update is missed.  It keeps a HISTORY of the cells the head has left, and
 * places segment i at history[i].  Length can then change by any amount, in
 * either direction, and the body is right on the very next tick -- growth
 * is just a longer read of the same history.
 *
 * WHY IT READS PrevHead AND NOT THE HEAD.  Script order within a frame is
 * not guaranteed.  Reading the head directly, this module would push the
 * old cell when it happened to run before the snake module and the new cell
 * when it ran after -- a body that is correct or one cell short depending
 * on nothing visible in the code.  snake.lua writes the vacated cell to
 * PrevHead as part of the same move, so there is one value with one meaning
 * whenever it is read.
 *
 * NOTE ON THE ENTRY POINT.  Two native backends (this and the C module)
 * link into ONE executable for the single-file build, so both are compiled
 * with JCE_SCRIPT_MODULE_NO_ENTRY and the host publishes them itself.
 * Without that they both emit the per-shared-object entry and the link
 * fails with LNK2005.
 */
#include <jce/script_vm/jce_script_cpp.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <optional>

namespace {

constexpr int COLS = 30;          /* spec 11 */
constexpr int ROWS = 20;
constexpr int kBodyMax = 64;      /* must match tools/gen_scene.py */
constexpr float kParkY = -100.0f; /* where an unused segment waits */
constexpr float kBodyY = 0.0f;

constexpr float kHX = (COLS - 1) / 2.0f;
constexpr float kHZ = (ROWS - 1) / 2.0f;

int cell_x(float x) { return static_cast<int>(std::lround(x + kHX)); }
int cell_z(float z) { return static_cast<int>(std::lround(z + kHZ)); }
float world_x(int cx) { return static_cast<float>(cx) - kHX; }
float world_z(int cz) { return static_cast<float>(cz) - kHZ; }

struct Cell { int x = -1, z = -1; };

class SnakeBody : public jce::script::Script {
public:
    void on_start() override
    {
        head_ = first("SnakeHead");
        prev_ = first("PrevHead");
        state_ = first("GameState");
        meta_ = first("GameMeta");
        verdict_ = first("Verdict");
        diag_ = first("CppDiag");
        for (int i = 0; i < kBodyMax; ++i) {
            char nm[16];
            std::snprintf(nm, sizeof nm, "Body%02d", i);
            body_[i] = first(nm);
        }
        last_tick_ = -1.0f;
        last_run_ = -1.0f;
        count_ = 0;
        /* A THREE-WAY MARKER, because "resolved nothing" and "never ran" are
         * different defects and this surface has no log to tell them apart:
         *   (0,0,0)   on_start never ran
         *   (99,0,0)  on_start ran, on_update never did
         *   (5,t,n)   both ran; 5 entities resolved, tick t, n history cells
         * Without it the first two are the same observation. */
        if (diag_) api().set_position(*diag_, 99.0f, 0.0f, 0.0f);
    }

    void on_update(float) override
    {
        /* Report before anything can return early.  This module has no log
         * binding -- the C++ script surface has none -- so "it ran and found
         * nothing" and "it never ran" are otherwise the same observation,
         * which is exactly the ambiguity that made this defect take three
         * rounds to place. */
        if (diag_) {
            const int resolved = (head_ ? 1 : 0) + (prev_ ? 1 : 0) +
                                 (state_ ? 1 : 0) + (meta_ ? 1 : 0) +
                                 (verdict_ ? 1 : 0);
            api().set_position(*diag_, (float)resolved, last_tick_,
                               (float)count_);
        }
        if (!state_ || !meta_) return;
        const auto st = api().get_position(*state_);
        const auto meta = api().get_position(*meta_);
        if (!st || !meta) return;

        const float run = (*meta)[2];
        if (run != last_run_) {
            last_run_ = run;
            last_tick_ = (*st)[2];
            count_ = 0;
            park_from(0);
            return;
        }

        if (static_cast<int>(std::lround((*st)[0])) != 1) return;  /* PLAYING */

        const float tick = (*st)[2];
        if (tick == last_tick_) return;     /* the world has not moved */
        last_tick_ = tick;

        /* Push the cell the head just vacated. */
        if (const auto pv = api().get_position(*prev_)) {
            for (int i = kBodyMax - 1; i > 0; --i) hist_[i] = hist_[i - 1];
            hist_[0] = Cell{cell_x((*pv)[0]), cell_z((*pv)[2])};
            if (count_ < kBodyMax) ++count_;
        }

        int len = static_cast<int>(std::lround((*meta)[1]));
        if (len < 0) len = 0;
        if (len > kBodyMax) len = kBodyMax;

        for (int i = 0; i < len && i < count_; ++i) {
            if (!body_[i]) continue;
            api().set_position(*body_[i], world_x(hist_[i].x), kBodyY,
                               world_z(hist_[i].z));
        }
        park_from(len);

        /* Self-collision, against the segments that are actually placed.
         * Checked AFTER the shift, so the cell the tail just left is free
         * -- a snake moving straight into its own vacated tail cell is
         * legal, and checking before the shift would kill it there. */
        const auto hp = api().get_position(*head_);
        if (!hp) return;
        const int hx = cell_x((*hp)[0]);
        const int hz = cell_z((*hp)[2]);
        for (int i = 0; i < len && i < count_; ++i) {
            if (hist_[i].x == hx && hist_[i].z == hz) {
                if (const auto v = api().get_position(*verdict_))
                    api().set_position(*verdict_, (*v)[0], 1.0f, (*v)[2]);
                return;
            }
        }
    }

private:
    void park_from(int from)
    {
        for (int i = from; i < kBodyMax; ++i)
            if (body_[i]) api().set_position(*body_[i], 0.0f, kParkY, 0.0f);
    }

    std::optional<jce::script::Entity> first(const char *name)
    {
        return api().find_by_name(name).first;
    }

    std::optional<jce::script::Entity> head_, prev_, state_, meta_, verdict_,
                                       diag_;
    std::array<std::optional<jce::script::Entity>, kBodyMax> body_{};
    std::array<Cell, kBodyMax> hist_{};
    float last_tick_ = -1.0f;
    float last_run_ = -1.0f;
    int count_ = 0;
};

}  // namespace

JCE_CPP_SCRIPT_CLASS(SnakeBody, "SnakeBody")

JCE_CPP_MODULE_BEGIN()
    JCE_CPP_MODULE_CLASS(SnakeBody)
JCE_CPP_MODULE_GLOBALS()
JCE_CPP_MODULE_END("snakecpp", snake_cpp_module)
