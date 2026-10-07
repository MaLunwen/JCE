import copy
import math
import unittest
from physics_pressure_suite import (
    COUNT,
    KINDS,
    fixture,
    initial_position,
    validate_inventory,
    validate_records,
    environment,
    correctness_frames,
    LAST_OBSERVATION_FRAME,
    CAPTURE_TAIL_FRAMES,
)
from pathlib import Path


def records(kind, enabled=True):
    rows = []
    for frame in (1, 120, 600, 900):
        bodies = []
        for index in range(COUNT):
            position = initial_position(kind, index)
            velocity = [0, 0, 0]
            if enabled:
                if kind == "falling":
                    position[1] -= 0.5 * 9.81 * (frame / 60) ** 2
                    velocity[1] = -9.81 * frame / 60
                elif frame >= 600:
                    position[1] = (
                        0.45 + 0.9 * (index // 400) if kind == "boxes" else 0.45
                    )
            bodies.append(
                dict(
                    index=index,
                    entity=index + 10000,
                    handle=index if enabled else 4294967295,
                    position=position,
                    velocity=velocity,
                    angular_velocity=[0, 0, 0],
                    rotation=[0, 0, 0, 1],
                    contact_this_step=enabled and kind != "falling",
                )
            )
        rows.append(
            dict(
                frame=frame,
                kind=kind,
                enabled=enabled,
                actual_bodies=COUNT + (kind != "falling") if enabled else 0,
                finite=True,
                sleep_transition="UNAVAILABLE_PUBLIC_API",
                bodies=bodies,
                begin_events=COUNT if enabled and kind != "falling" else 0,
                persist_events=COUNT if enabled and kind != "falling" else 0,
                end_events=0,
                stationary_velocity_proxy=COUNT
                if kind != "falling" or not enabled
                else 0,
                contacted_this_step=COUNT if enabled and kind != "falling" else 0,
            )
        )
    return rows


class PhysicsPressureTests(unittest.TestCase):
    def test_real_components_shapes_and_initial_overlap_free_layout(self):
        for kind in KINDS:
            rows = fixture(kind)["entities"]
            bodies = [r for r in rows if r["name"].startswith("PBody")]
            self.assertEqual(len(bodies), COUNT)
            self.assertNotIn(
                "Rigid" + "Body",
                {component["type"] for row in rows for component in row["components"]},
            )
            self.assertEqual(len({r["id"] for r in rows}), len(rows))
            self.assertEqual(
                len({tuple(initial_position(kind, i)) for i in range(COUNT)}), COUNT
            )
            self.assertEqual(
                sum(
                    any(c["type"] == "Rigidbody" for c in row["components"])
                    for row in rows
                ),
                COUNT + (kind != "falling"),
            )
            spheres = sum(
                any(c["type"] == "SphereCollider" for c in row["components"])
                for row in bodies
            )
            self.assertEqual(
                spheres,
                COUNT if kind == "spheres" else COUNT // 2 if kind == "mixed" else 0,
            )
            self.assertTrue(
                all(
                    next(c for c in row["components"] if c["type"] == "Rigidbody")[
                        "bodyType"
                    ]
                    == 1
                    for row in bodies
                )
            )
        with self.assertRaises(ValueError):
            fixture("imaginary")

    def test_oracle_accepts_supported_pose_proxy_without_claiming_sleep(self):
        for kind in KINDS:
            self.assertEqual(
                validate_records(records(kind), kind, True)["sleep_transition"],
                "UNAVAILABLE_PUBLIC_API",
            )
            self.assertEqual(
                validate_records(records(kind, False), kind, False)["status"],
                "DISABLED_INVARIANCE_OBSERVED",
            )

    def test_wrong_contacts_overlap_drift_velocity_nonfinite_missing_body_fail(self):
        base = records("boxes")
        edits = (
            lambda r: r[2]["bodies"][0]["position"].__setitem__(1, -1),
            lambda r: r[3]["bodies"][0]["position"].__setitem__(0, 3),
            lambda r: r[2]["bodies"][0]["velocity"].__setitem__(0, 0.1),
            lambda r: r[2].__setitem__("persist_events", 0),
            lambda r: r[2]["bodies"][0]["rotation"].__setitem__(0, float("nan")),
            lambda r: r[2]["bodies"].pop(),
            lambda r: r[2]["bodies"][0].__setitem__("handle", 1),
            lambda r: r[2].__setitem__("sleep_transition", "SLEEPING_PASS"),
        )
        for edit in edits:
            value = copy.deepcopy(base)
            edit(value)
            with self.assertRaises(ValueError):
                validate_records(value, "boxes", True)
        value = records("boxes", False)
        value[2]["bodies"][0]["position"][1] -= 1
        with self.assertRaises(ValueError):
            validate_records(value, "boxes", False)
        value = records("falling")
        value[1]["bodies"][0]["position"][1] = 6
        with self.assertRaises(ValueError):
            validate_records(value, "falling", True)

    def test_same_pose_with_unique_recreated_body_generations_is_rejected(self):
        value = records("boxes")
        for body in value[2]["bodies"]:
            body["handle"] += 65536
        self.assertEqual(len({b["handle"] for b in value[2]["bodies"]}), COUNT)
        self.assertTrue(all(b["handle"] != 4294967295 for b in value[2]["bodies"]))
        with self.assertRaisesRegex(ValueError, "handle/generation changed"):
            validate_records(value, "boxes", True)

    def test_capture_budget_preserves_final_numeric_observation_and_gpu_tail(self):
        for shot in (120, 900):
            frames = correctness_frames(shot)
            self.assertEqual(frames, LAST_OBSERVATION_FRAME + CAPTURE_TAIL_FRAMES)
            self.assertGreater(frames - shot, 1)
            self.assertGreater(frames, LAST_OBSERVATION_FRAME)
        for bad in (-1, 0, 120.0, 901, 10000000):
            with self.assertRaises(ValueError):
                correctness_frames(bad)

    def test_inventory_and_timing_disable_observer_guards(self):
        text = "benchmark-player: entered Play\nphysics-pressure-inventory: frame=1 kind=boxes dynamic=2000 rigidbodies=2001 static=1 actual_bodies=2001 enabled=1 observer=0 fixed_hz=60 gravity=-9.81 solver_iterations=10 ccd=discrete sleep=unavailable"
        validate_inventory(text, "boxes", True, False)
        for bad in (
            text.replace("actual_bodies=2001", "actual_bodies=0"),
            text.replace("dynamic=2000", "dynamic=1999"),
            text.replace("observer=0", "observer=1"),
            text + "\n" + text.splitlines()[-1],
        ):
            with self.assertRaises(ValueError):
                validate_inventory(bad, "boxes", True, False)
        env = environment(
            "boxes", True, False, Path("scene.json"), Path("out"), "d3d11"
        )
        self.assertEqual(env["JCE_BENCH_PHYSICS_OBSERVE"], "0")
        self.assertEqual(env["JCE_BENCH_PHYSICS_DISABLED"], "0")
        self.assertNotIn("JCE_BENCH_AUTOSPAWN", env)


if __name__ == "__main__":
    unittest.main()
