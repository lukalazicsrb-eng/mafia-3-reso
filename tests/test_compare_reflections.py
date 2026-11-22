import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "capture"))
from compare_reflections import compare


def report(width, digest="a" * 64, classification="reflection_reference"):
    return {"passes": [{"classification": classification, "shader_sha256": digest,
                        "reference_name": "SSR", "shader_crc32": "685D5D10", "stage": "Pixel",
                        "reads": [], "outputs": [{"kind": "rtv", "slot": 0, "view_width": width,
                                                   "view_height": 720, "samples": 1}]}]}


class ReflectionComparison(unittest.TestCase):
    def test_view_resolution_change_for_equal_shader(self):
        result = compare(report(1280), report(2560))
        self.assertIn("zabeležena razlika", result)
        self.assertIn("1280x720", result)
        self.assertIn("2560x720", result)

    def test_reference_crc_alone_does_not_match_shader(self):
        self.assertIn("Nema istih", compare(report(1280), report(2560, digest="b" * 64)))

    def test_candidates_not_compared(self):
        self.assertIn("Nema istih", compare(report(1280), report(2560, classification="candidate_only")))

    def test_ambiguous_occurrences_are_explicit(self):
        ultra = report(1280)
        ultra["passes"] += report(2560)["passes"]
        self.assertIn("više varijanti", compare(report(1280), ultra))


if __name__ == "__main__":
    unittest.main()
