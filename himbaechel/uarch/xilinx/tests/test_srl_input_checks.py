import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gen"))
from parse_sdf import parse_sdf_file
from xilinx_gen import srl_input_checks


def read_checks(records):
    """Import the test SDF through the same parser as the Xilinx chip database."""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "slicem.sdf"
        path.write_text(f'(DELAYFILE (CELL (CELLTYPE "LUT_OR_MEM6SHFREG") '
                        f'(INSTANCE SLICEM/A6LUT) (TIMINGCHECK {records})))')
        return srl_input_checks(parse_sdf_file(path))


class SrlInputChecksTest(unittest.TestCase):
    def test_split_and_combined(self):
        """Both SDF spellings preserve signed ranges and select the rising clock."""
        split = """
            (SETUP DI1 (posedge CLK) (-0.040::0.000))
            (HOLD DI1 (posedge CLK) (0.011::0.014))
            (SETUP DI1 (negedge CLK) (0.700::0.800))
            (HOLD DI1 (negedge CLK) (0.900::1.000))
        """
        combined = """
            (SETUPHOLD DI1 (posedge CLK) (-0.040::0.000) (0.011::0.014))
            (SETUPHOLD DI1 (negedge CLK) (0.700::0.800) (0.900::1.000))
            (SETUPHOLD DI1 (posedge OTHER) (0.700::0.800) (0.900::1.000))
        """
        for records in (split, combined):
            with self.subTest(records=records):
                checks = read_checks(records)
                self.assertEqual(set(checks), {"DI1"})
                setup, hold = checks["DI1"]
                for value, lo, hi in ((setup, -40, 0), (hold, 11, 14)):
                    self.assertEqual((value.fast_min, value.fast_max, value.slow_min, value.slow_max),
                                     (lo, hi, lo, hi))

    def test_unpaired_clock_edges(self):
        """Setup and hold on opposite clock edges cannot form an SRL check."""
        self.assertEqual(read_checks("""
            (SETUP WE (posedge CLK) (0.100::0.200))
            (HOLD WE (negedge CLK) (0.100::0.200))
        """), {})

    def test_data_edge_envelope(self):
        """Both data transitions contribute to the single check for a physical input."""
        checks = read_checks("""
            (SETUP (posedge DI2) CLK (0.100::0.120))
            (HOLD (posedge DI2) CLK (0.050::0.060))
            (SETUPHOLD (negedge DI2) CLK (0.110::0.140) (0.040::0.055))
        """)
        setup, hold = checks["DI2"]
        self.assertEqual((setup.fast_min, setup.fast_max, hold.fast_min, hold.fast_max), (100, 140, 40, 60))


if __name__ == "__main__":
    unittest.main()
