import unittest
from types import SimpleNamespace
from unittest.mock import patch

from fastapi import HTTPException
from pydantic import ValidationError

from ml_api import app as api


def valid_live_reading():
    return {
        "fruitIndex": 4,
        "itemSelected": True,
        "item": "Kiwi",
        "itemType": "Fruit",
        "temperature": 24.0,
        "humidity": 48.0,
        "mq135": 1200.0,
        "mq3": 1300.0,
        "mq4": 1400.0,
        "rawMq135": 1200,
        "rawMq3": 1300,
        "rawMq4": 1400,
        "dataReady": True,
        "dhtValid": True,
        "measurementStable": True,
        "mode": "LIVE",
        "state": "READY",
        "featuresVersion": 2,
        "sensorFeatureScale": "raw_adc",
        "readingAgeMs": 0,
    }


class ApiContractTests(unittest.TestCase):
    def test_live_reading_without_real_model_stays_unverified(self):
        reading = api.Reading.model_validate(valid_live_reading())
        with (
            patch.object(api, "append_unlabelled") as append,
            patch.object(api, "MODEL_PATH", SimpleNamespace(exists=lambda: False)),
            patch.object(api, "DATASET_ESTIMATE_MODEL_PATH", SimpleNamespace(exists=lambda: False)),
        ):
            result = api.predict(reading)
        self.assertEqual(result["prediction"], "UNVERIFIED")
        self.assertIsNone(result["shelfLifeDaysEstimate"])
        append.assert_called_once()

    def test_live_prediction_rejects_unstable_window(self):
        payload = valid_live_reading()
        payload["dataReady"] = False
        reading = api.Reading.model_validate(payload)
        with patch.object(api, "append_unlabelled"):
            with self.assertRaises(HTTPException) as raised:
                api.predict(reading)
        self.assertEqual(raised.exception.status_code, 422)
        self.assertIn("dataReady", raised.exception.detail)

    def test_mismatched_display_and_raw_adc_is_rejected(self):
        payload = valid_live_reading()
        payload["mq135"] = 1201.0
        with self.assertRaises(ValidationError):
            api.Reading.model_validate(payload)

    def test_stale_reading_is_rejected(self):
        reading_data = valid_live_reading()
        reading_data["readingAgeMs"] = api.MAX_READING_AGE_MS + 1
        reading = api.Reading.model_validate(reading_data)
        with patch.object(api, "append_unlabelled"):
            with self.assertRaises(HTTPException) as raised:
                api.predict(reading)
        self.assertEqual(raised.exception.status_code, 422)
        self.assertIn("stale", raised.exception.detail)

    def test_supported_fruit_returns_caveated_dataset_estimate(self):
        payload = valid_live_reading()
        payload.update({"fruitIndex": 9, "item": "Tomato", "itemType": "Vegetable"})
        reading = api.Reading.model_validate(payload)

        class PredictionStub:
            def predict(self, frame):
                self.frame = frame.copy()
                return ["Early"]

        model = PredictionStub()
        bundle = {"model": model, "metadata": {
            "modelType": "training_dataset_estimate",
            "modelVersion": "test-dataset-estimate",
            "features": api.DATASET_ESTIMATE_FEATURES,
            "fruits": ["Tomato"],
            "numericFeatureRanges": {
                "MQ3": {"min": 62, "max": 898},
                "MQ4": {"min": 49, "max": 178},
                "MQ135": {"min": 447, "max": 2975},
                "Temperature_C": {"min": 25.1, "max": 31.9},
                "Humidity_pct": {"min": 40, "max": 65},
            },
        }}
        with (
            patch.object(api, "append_unlabelled"),
            patch.object(api, "MODEL_PATH", SimpleNamespace(exists=lambda: False)),
            patch.object(api, "DATASET_ESTIMATE_MODEL_PATH", SimpleNamespace(exists=lambda: True)),
            patch.object(api, "_load_dataset_estimate_model", return_value=bundle),
        ):
            result = api.predict(reading)

        self.assertEqual(result["prediction"], "Early")
        self.assertEqual(result["predictionSource"], "training_dataset_estimate")
        self.assertTrue(result["inputOutsideTrainingRange"])
        self.assertIn("assumptions", result["note"])
        self.assertEqual(model.frame.loc[0, "Fruit"], "Tomato")
        self.assertEqual(list(model.frame.columns), api.DATASET_ESTIMATE_FEATURES)

    def test_unsupported_fruit_stays_unverified(self):
        reading = api.Reading.model_validate(valid_live_reading())
        bundle = {"model": object(), "metadata": {
            "modelType": "training_dataset_estimate",
            "features": api.DATASET_ESTIMATE_FEATURES,
            "fruits": ["Tomato"],
        }}
        with (
            patch.object(api, "append_unlabelled"),
            patch.object(api, "MODEL_PATH", SimpleNamespace(exists=lambda: False)),
            patch.object(api, "DATASET_ESTIMATE_MODEL_PATH", SimpleNamespace(exists=lambda: True)),
            patch.object(api, "_load_dataset_estimate_model", return_value=bundle),
        ):
            result = api.predict(reading)
        self.assertEqual(result["prediction"], "UNVERIFIED")
        self.assertIn("No reference rows", result["note"])

    @unittest.skipUnless(api.DATASET_ESTIMATE_MODEL_PATH.is_file(), "trained dataset estimate artifact is not present")
    def test_screenshot_tomato_reading_returns_a_dataset_class(self):
        payload = valid_live_reading()
        payload.update({
            "fruitIndex": 9,
            "item": "Tomato",
            "itemType": "Vegetable",
            "temperature": 31.4,
            "humidity": 59.1,
            "mq135": 1068.0,
            "rawMq135": 1068,
            "mq3": 1238.0,
            "rawMq3": 1238,
            "mq4": 241.0,
            "rawMq4": 241,
        })
        reading = api.Reading.model_validate(payload)
        with patch.object(api, "append_unlabelled"):
            result = api.predict(reading)
        self.assertEqual(result["prediction"], "Spoiled")
        self.assertEqual(result["modelType"], "training_dataset_estimate")
        self.assertTrue(result["inputOutsideTrainingRange"])
        self.assertIn("MQ3", result["inputOutsideTrainingRangeFeatures"])
        self.assertIn("MQ4", result["inputOutsideTrainingRangeFeatures"])

    @unittest.skipUnless(api.DATASET_ESTIMATE_MODEL_PATH.is_file(), "trained dataset estimate artifact is not present")
    def test_health_distinguishes_dataset_estimate_from_real_live_model(self):
        with (
            patch.object(api, "MODEL_PATH", SimpleNamespace(exists=lambda: False)),
        ):
            result = api.health()
        self.assertTrue(result["modelLoaded"])
        self.assertTrue(result["datasetEstimateEnabled"])
        self.assertFalse(result["livePredictionEnabled"])
        self.assertEqual(result["predictionStatus"], "dataset_estimates_only")


if __name__ == "__main__":
    unittest.main()
