import unittest
from unittest.mock import patch

from ml_api import collect_samples


class SelectItemTests(unittest.TestCase):
    def test_selects_and_confirms_requested_item(self):
        response = {
            "ok": True,
            "itemSelected": True,
            "itemIndex": 4,
            "fruitIndex": 4,
            "item": "Onion",
        }
        with patch.object(collect_samples, "fetch_json", return_value=response) as fetch:
            name = collect_samples.select_item("http://esp32", 4, ["Apple", "Banana", "Tomato", "Orange", "Onion"])
        self.assertEqual(name, "Onion")
        fetch.assert_called_once_with("http://esp32/api/item?id=4")

    def test_rejects_mismatched_device_acknowledgement(self):
        response = {"ok": True, "itemSelected": True, "itemIndex": 3, "fruitIndex": 3, "item": "Orange"}
        with patch.object(collect_samples, "fetch_json", return_value=response):
            with self.assertRaisesRegex(ValueError, "did not confirm"):
                collect_samples.select_item("http://esp32", 4, ["Apple", "Banana", "Tomato", "Orange", "Onion"])

    def test_rejects_malformed_device_acknowledgement(self):
        with patch.object(collect_samples, "fetch_json", return_value=["unexpected"]):
            with self.assertRaisesRegex(ValueError, "did not confirm"):
                collect_samples.select_item("http://esp32", 0, ["Apple"])

    def test_rejects_wrong_item_name_and_unstable_window(self):
        base = {
            "itemSelected": True, "fruitIndex": 4, "item": "Kiwi",
            "mode": "LIVE", "state": "READY", "dataReady": True,
            "dhtValid": True, "measurementStable": True,
            "featuresVersion": 2, "readingAgeMs": 0,
            "rawMq135": 1000, "rawMq3": 1000, "rawMq4": 1000,
            "temperature": 24.0, "humidity": 50.0,
        }
        wrong_name = dict(base, item="Tomato")
        self.assertIn("item name", collect_samples.reading_problem(wrong_name, 4, "Kiwi"))
        unstable = dict(base, measurementStable=False)
        self.assertIn("not marked stable", collect_samples.reading_problem(unstable, 4, "Kiwi"))
        self.assertIsNone(collect_samples.reading_problem(base, 4, "Kiwi"))
        bad_age = dict(base, readingAgeMs=True)
        self.assertIn("stale", collect_samples.reading_problem(bad_age, 4, "Kiwi"))
        bad_adc = dict(base, rawMq135=True)
        self.assertIn("rawMq135", collect_samples.reading_problem(bad_adc, 4, "Kiwi"))

    def test_refuses_unselected_or_changed_status_rows(self):
        self.assertEqual(
            collect_samples.reading_problem({"itemSelected": False, "fruitIndex": 4}, 4, "Kiwi"),
            "no item is selected on the ESP32",
        )
        self.assertEqual(
            collect_samples.reading_problem({"itemSelected": True, "fruitIndex": 3}, 4, "Kiwi"),
            "ESP32 item selection changed; refusing to label this reading",
        )


if __name__ == "__main__":
    unittest.main()
