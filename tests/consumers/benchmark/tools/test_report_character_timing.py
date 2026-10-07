import unittest
from report_character_timing import validate_identity


class CharacterIdentityTests(unittest.TestCase):
    def test_completed_scale(self):
        good = {"status": "PASS", "count": 100, "runs": [{}]*7}
        validate_identity(good, 100)
        for bad in ({**good, "count": 500}, {**good, "status": "FAIL"}, {**good, "runs": [{}]*6}):
            with self.assertRaises(ValueError):
                validate_identity(bad, 100)


if __name__ == "__main__":
    unittest.main()
