"""Independent allocator invariants and source wiring; no GPU certification."""
from pathlib import Path
import random
import math
import re
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]


def allocate(live, requested, order):
    free = [slot for slot in order if not live[slot]]
    accepted = free[:min(requested, len(free))]
    updated = list(live)
    for slot in accepted:
        updated[slot] = True
    return updated, accepted


def old_probe_claims(live, requested):
    # Concurrent invocations can all observe the same pre-dispatch lifetime.
    claims = []
    for thread in range(requested):
        for step in range(32):
            slot = (thread + step) % len(live)
            if not live[slot]:
                claims.append(slot)
                break
    return claims


class ParticleAllocatorTests(unittest.TestCase):
    def test_original_probe_race_and_sparse_free_negative_control(self):
        live = [True, False, True, True]
        claims = old_probe_claims(live, 2)
        self.assertEqual([1, 1], claims)
        _, unique = allocate(live, 2, range(4))
        self.assertEqual([1], unique)
        sparse = [True] * 128
        sparse[100] = False
        self.assertEqual([], old_probe_claims(sparse, 1))
        self.assertEqual([100], allocate(sparse, 1, range(128))[1])

    def test_empty_saturated_mixed_and_recycle(self):
        for live in ([False] * 64, [True] * 64, [i % 3 == 0 for i in range(64)]):
            for requested in (0, 1, 32, 64, 1000):
                updated, accepted = allocate(live, requested, range(64))
                self.assertEqual(min(requested, live.count(False)), len(accepted))
                self.assertEqual(len(accepted), len(set(accepted)))
                self.assertEqual(sum(live) + len(accepted), sum(updated))
                self.assertTrue(all(updated[i] for i, was_live in enumerate(live) if was_live))
        live, _ = allocate([False] * 64, 64, range(64))
        for cycle in range(5):
            live = [state and i % 2 != cycle % 2 for i, state in enumerate(live)]
            live, accepted = allocate(live, 1000, range(64))
            self.assertEqual(64, sum(live))
            self.assertEqual(32, len(accepted))

    def test_random_append_order_preserves_membership_and_count(self):
        randomizer = random.Random(83)
        for _ in range(50):
            live = [randomizer.choice((True, False)) for _ in range(64)]
            order = list(range(64)); randomizer.shuffle(order)
            updated, accepted = allocate(live, 1000, order)
            self.assertEqual({i for i, value in enumerate(live) if not value}, set(accepted))
            self.assertTrue(all(updated))

    def test_actual_source_dispatch_and_unique_ownership(self):
        source = (ROOT / "engine/src/renderer/jce_gpu_particles.c").read_text(encoding="utf-8")
        update = source[source.index("void jce_gpu_particles_update("):source.index("void jce_gpu_particles_reset(")]
        markers = ["jce_gpu_particles_reset(sys, view)", "sys->update_program, groups",
                   "sys->counter_reset_program, 1", "sys->free_program,",
                   "sys->emit_program, groups", "particle_debug_snapshot(sys, view)"]
        self.assertEqual(sorted(update.index(marker) for marker in markers),
                         [update.index(marker) for marker in markers])
        free = (ROOT / "engine/shaders/particles/cs_particle_free.sc").read_text()
        emit = (ROOT / "engine/shaders/particles/cs_particle_emit.sc").read_text()
        self.assertIn("atomicFetchAndAdd(b_counts[0], 1u, index)", free)
        self.assertIn("b_free_slots[index] = slot", free)
        self.assertIn("uint slot = b_free_slots[thread]", emit)
        self.assertIn("thread >= free_count || thread >= pool_size", emit)
        self.assertNotIn("candidate", emit)
        self.assertIn("atomicFetchAndAdd(b_counts[1], 1u, ignored)", emit)
        self.assertIn("BGFX_BUFFER_COMPUTE_TYPE_UINT", source)
        self.assertIn("particle resources unavailable; CPU fallback required", source)
        self.assertIn("sys->debug_valid = false", update)

    def test_portable_dispatch_capacity_alignment_bound(self):
        limit = 64 * 65535
        for requested in (1, 63, 64, limit - 1, limit):
            rounded = (requested + 63) & ~63
            self.assertLessEqual(rounded // 64, 65535)
            self.assertLess(rounded, 16777216)
        self.assertGreater(((limit + 1 + 63) & ~63) // 64, 65535)
        source = (ROOT / "engine/src/renderer/jce_gpu_particles.c").read_text()
        self.assertIn("desc->max_particles > PARTICLE_CAPACITY_LIMIT", source)
        self.assertIn("(UPDATE_THREADS_X * 65535u)", source)
        reset = source[source.index("void jce_gpu_particles_reset("):source.index("void jce_gpu_particles_render(")]
        self.assertLess(reset.index("sys->debug_valid = false"), reset.index("!sys->needs_reset"))

    def test_actual_config_predicate_rejects_invalid_float_inputs(self):
        source = (ROOT / "engine/src/renderer/jce_gpu_particles.c").read_text()
        helper = source[source.index("static bool particle_config_valid("):
                        source.index("void jce_gpu_particles_update(")]
        fields = ["dt", "lifetime_min", "lifetime_max", "size_start", "size_end", "damping"]
        fields += [name + "." + axis for name in
                   ("origin", "velocity_min", "velocity_max", "gravity") for axis in "xyz"]
        fields += [name + "." + axis for name in ("color_start", "color_end") for axis in "xyzw"]
        array = re.search(r"values\[\] = \{(.*?)\};", helper, re.S).group(1)
        actual = [value.strip().replace("cfg->", "") for value in array.split(",")]
        self.assertEqual(fields, actual)
        self.assertIn("if (!isfinite(values[i])) return false;", helper)
        expression = re.search(r"return (dt >=.*?);", helper, re.S).group(1)
        expression = expression.replace("cfg->", "cfg.").replace("&&", "and")
        expression = re.sub(r"([0-9]+\.[0-9]+)f", r"\1", expression)
        expression = " ".join(expression.split())
        base = dict.fromkeys(fields, 0.0)
        base.update(lifetime_min=1.0, lifetime_max=2.0)

        def accepts(values):
            cfg = SimpleNamespace()
            for field, value in values.items():
                if field == "dt":
                    continue
                if "." in field:
                    name, axis = field.split(".")
                    if not hasattr(cfg, name):
                        setattr(cfg, name, SimpleNamespace())
                    setattr(getattr(cfg, name), axis, value)
                else:
                    setattr(cfg, field, value)
            return all(math.isfinite(values[field]) for field in actual) and eval(
                expression, {"__builtins__": {}}, {"dt": values["dt"], "cfg": cfg})

        self.assertTrue(accepts(base))
        for field in fields:
            for invalid in (math.nan, math.inf, -math.inf):
                with self.subTest(field=field, invalid=invalid):
                    self.assertFalse(accepts(dict(base, **{field: invalid})))
        for changes in ({"dt": -1}, {"lifetime_min": 0}, {"lifetime_min": -1},
                        {"lifetime_max": 0}, {"lifetime_max": 0.5},
                        {"size_start": -1}, {"size_end": -1}, {"damping": -1}):
            self.assertFalse(accepts(dict(base, **changes)))
        for axis in "xyz":
            self.assertFalse(accepts(dict(base, **{"velocity_min." + axis: 1})))
        valid = dict(base, **{"color_start.x": 8, "color_end.z": -2,
                             "velocity_min.x": -10, "velocity_max.x": -1})
        self.assertTrue(accepts(valid))
        update = source[source.index("void jce_gpu_particles_update("):
                        source.index("void jce_gpu_particles_reset(")]
        self.assertLess(update.index("sys->debug_valid = false"),
                        update.index("!particle_config_valid(dt, cfg)"))
        self.assertLess(update.index("!particle_config_valid(dt, cfg)"),
                        update.index("jce_gpu_particles_reset(sys, view)"))

    def test_count_encoding_contract_and_bounds(self):
        for count in (0, 1, 255, 256, 65535, 65536, 16777152):
            rgb = (count & 255, (count >> 8) & 255, (count >> 16) & 255)
            self.assertEqual(count, rgb[0] | rgb[1] << 8 | rgb[2] << 16)
        stats = (ROOT / "engine/shaders/particles/cs_particle_stats.sc").read_text()
        self.assertIn("capacity - free_count + emitted", stats)
        self.assertIn("emitted <= free_count", stats)
        self.assertIn("capacity < 16777216u", stats)
        self.assertIn("valid ? 255.0 : 0.0", stats)


if __name__ == "__main__":
    unittest.main()
