import csv
import re
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SKETCH = PROJECT_ROOT / "firmware" / "SPOIL_ALERT_ESP32_ML_Ready" / "SPOIL_ALERT_ESP32_ML_Ready.ino"
REFERENCE_DATA = PROJECT_ROOT / "data" / "reference_sensor_profiles.csv"
STATUS = {"Fresh": 0, "Early": 1, "Medium": 2, "Spoiled": 3}
ROW_PATTERN = re.compile(
    r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*,"
    r"\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*(\d+)\s*\}"
)


class EmbeddedReferenceDataTests(unittest.TestCase):
    def test_firmware_item_list_matches_api_order_and_stability_gate(self):
        source = SKETCH.read_text(encoding="utf-8")
        start = source.index("const char* fruits[] = {")
        end = source.index("\n};", start)
        firmware_items = re.findall(r'"([^"]+)"', source[start:end])
        from ml_api.app import ITEMS
        self.assertEqual(firmware_items, ITEMS)
        self.assertIn("itemSelected && lastWindowStable", source)
        self.assertIn(r'\"measurementStable\":%s', source)

    def test_firmware_table_matches_csv_reference_profiles(self):
        source = SKETCH.read_text(encoding="utf-8")
        section_start = source.index("const RefRow referenceData[] = {")
        section_end = source.index("\n};", section_start)
        firmware_rows = [
            (int(fruit), int(minute), *(float(value) for value in values), int(status))
            for fruit, minute, *values, status in ROW_PATTERN.findall(source[section_start:section_end])
        ]
        with REFERENCE_DATA.open(newline="", encoding="utf-8") as f:
            csv_rows = list(csv.DictReader(f))
        expected_rows = [
            (
                int(row["fruitIndex"]), int(row["minute"]), float(row["temperature"]),
                float(row["humidity"]), float(row["mq135"]), float(row["mq3"]),
                float(row["mq4"]), STATUS[row["label"]],
            )
            for row in csv_rows
        ]

        self.assertEqual(len(firmware_rows), len(expected_rows), "reference row count drifted")
        for row_number, (actual, expected) in enumerate(zip(firmware_rows, expected_rows), start=1):
            self.assertEqual(actual[:2], expected[:2], f"fruit/minute mismatch at row {row_number}")
            self.assertEqual(actual[-1], expected[-1], f"class mismatch at row {row_number}")
            for column, actual_value, expected_value in zip(
                ("temperature", "humidity", "mq135", "mq3", "mq4"), actual[2:7], expected[2:7]
            ):
                self.assertAlmostEqual(actual_value, expected_value, places=2, msg=f"{column} mismatch at row {row_number}")


if __name__ == "__main__":
    unittest.main()
