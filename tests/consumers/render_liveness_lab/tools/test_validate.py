"""CPU evidence-validator controls; these synthetic rows are not GPU results."""
import unittest
from validate import PHASES, COLOUR_PHASES, SHADOW_PHASES, validate_records


def records():
    result = []
    for index, name in enumerate(PHASES):
        result.append({
            "index": index, "name": name, "owner": 471, "entity": 9, "lost": 0,
            "accepted": 72 if index in COLOUR_PHASES else 0,
            "authored": 72 if index in COLOUR_PHASES else 0,
            "generation": 0 if index < 2 else 1 if index < 5 else 2,
            "colour_program": 65535 if index < 2 else 0,
            "depth_program": 65535 if index < 5 else 1,
            "shadow_admissions": 1 if index in SHADOW_PHASES else 0,
            "shadow_scopes": 1 if index in SHADOW_PHASES else 0,
            "colour_failures": 1 if index in (0, 1) else 0,
            "depth_failures": 1 if index in (3, 4) else 0,
        })
    return result


class EvidenceControls(unittest.TestCase):
    def test_complete_control_records(self):
        self.assertEqual(validate_records(records())["accepted_max"], 72)

    def test_zero_actual_colour_is_rejected(self):
        rows = records()
        rows[5]["accepted"] = 0
        with self.assertRaisesRegex(ValueError, "submission mismatch"):
            validate_records(rows)

    def test_empty_shadow_scopes_are_rejected(self):
        rows = records()
        rows[5]["shadow_admissions"] = 0
        with self.assertRaisesRegex(ValueError, "shadow source admission"):
            validate_records(rows)

    def test_reset_generation_wire_required(self):
        rows = records()
        rows[2]["generation"] = 0
        with self.assertRaisesRegex(ValueError, "reset generation"):
            validate_records(rows)

    def test_successful_colour_program_must_survive_depth_recovery(self):
        rows = records()
        rows[5]["colour_program"] = 7
        with self.assertRaisesRegex(ValueError, "unnecessarily replaced"):
            validate_records(rows)

    def test_different_owner_is_rejected(self):
        rows = records()
        rows[5]["owner"] = 472
        with self.assertRaisesRegex(ValueError, "owner changed"):
            validate_records(rows)

    def test_lost_trace_is_rejected(self):
        rows = records()
        rows[5]["lost"] = 1
        with self.assertRaisesRegex(ValueError, "lost production trace"):
            validate_records(rows)


if __name__ == "__main__":
    unittest.main()
