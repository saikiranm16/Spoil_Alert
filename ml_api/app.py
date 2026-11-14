"""SPOIL ALERT prediction API for readings forwarded by the Android app."""
from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import csv
from typing import Literal

import joblib
import pandas as pd
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, ConfigDict, Field, model_validator

ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = ROOT.parent
MODEL_PATH = ROOT / "models" / "spoil_alert_model.joblib"
DATASET_ESTIMATE_MODEL_PATH = ROOT / "models" / "spoilalert_dataset_estimate_model.joblib"
# Raw-ADC feature schema v2 gets a new log so old scaled-feature CSV rows retain
# their original column meanings.
LOG_PATH = PROJECT_ROOT / "data" / "logs" / "unlabelled_readings_v3_raw_adc.csv"
FEATURES = ["fruitIndex", "temperature", "humidity", "rawMq135", "rawMq3", "rawMq4"]
DATASET_ESTIMATE_FEATURES = ["Fruit", "MQ3", "MQ4", "MQ135", "Temperature_C", "Humidity_pct"]
SUPPORTED_LABELS = ["Fresh", "Early", "Medium", "Spoiled"]
ITEMS = [
    "Banana", "Blueberry", "Grape", "Green Apple", "Kiwi", "Mushroom", "Pear", "Red Apple", "Strawberry", "Tomato",
]
VEGETABLE_INDICES = {5, 9}
# The firmware verifies a sensor window about every 120 seconds. Allow a small
# transfer/scheduling margin, but reject a cached verified window after 150 s.
MAX_READING_AGE_MS = 150_000
DATASET_ESTIMATE_FRUIT_ALIASES = {"Grapes": "Grape"}
app = FastAPI(title="SPOIL ALERT ML API", version="2.0.0")


def _public_model_type(value):
    return "training_dataset_estimate" if value == "dataset_demo" else value


def _public_model_version(value):
    if value == "spoilalert-dataset-demo-rf-3-group-tuned":
        return "spoilalert-dataset-estimate-rf-3-group-tuned"
    return value


class Reading(BaseModel):
    model_config = ConfigDict(extra="ignore")

    fruitIndex: int = Field(ge=0, le=9)
    itemSelected: bool
    item: str = Field(default="", max_length=64)
    itemType: Literal["Fruit", "Vegetable"] | None = None
    temperature: float = Field(ge=-20, le=100, allow_inf_nan=False)
    humidity: float = Field(ge=0, le=100, allow_inf_nan=False)
    # Display fields are raw ADC counts too; kept for old app component keys.
    mq135: float = Field(ge=20, le=4080, allow_inf_nan=False)
    mq3: float = Field(ge=20, le=4080, allow_inf_nan=False)
    mq4: float = Field(ge=20, le=4080, allow_inf_nan=False)
    # Raw counts are the model features. They are not ppm; train and infer
    # with the same sensor board, wiring, divider and ADC configuration.
    rawMq135: int = Field(ge=20, le=4080)
    rawMq3: int = Field(ge=20, le=4080)
    rawMq4: int = Field(ge=20, le=4080)
    dataReady: bool
    dhtValid: bool
    mode: str
    state: str
    featuresVersion: int
    sensorFeatureScale: Literal["raw_adc"]
    fruit: str | None = None
    # Milliseconds since the firmware last published a verified LIVE window.
    readingAgeMs: int = Field(ge=0)

    @model_validator(mode="after")
    def display_gas_values_match_adc(self):
        pairs = ((self.mq135, self.rawMq135), (self.mq3, self.rawMq3), (self.mq4, self.rawMq4))
        if any(abs(display_value - raw_value) > 0.01 for display_value, raw_value in pairs):
            raise ValueError("MQ display values must match raw ADC counts in featuresVersion=2.")
        return self


def _load_model():
    try:
        bundle = joblib.load(MODEL_PATH)
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"Trained model could not be loaded: {exc}") from exc
    if not isinstance(bundle, dict) or "model" not in bundle or "metadata" not in bundle:
        raise HTTPException(status_code=503, detail="Model file has an old or invalid format. Retrain it with ml_api/train_model.py.")
    metadata = bundle["metadata"]
    model_type = metadata.get("modelType")
    if model_type not in {"synthetic_demo", "real_labeled_prototype"}:
        raise HTTPException(status_code=503, detail="Model provenance is missing or unsupported. Retrain using ml_api/train_model.py.")
    if model_type != "synthetic_demo" and metadata.get("features") != FEATURES:
        raise HTTPException(status_code=503, detail="Model feature order does not match the API. Retrain with ml_api/train_model.py.")
    return bundle


def _load_dataset_estimate_model():
    try:
        bundle = joblib.load(DATASET_ESTIMATE_MODEL_PATH)
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"Dataset estimate model could not be loaded: {exc}") from exc
    if not isinstance(bundle, dict) or "model" not in bundle or "metadata" not in bundle:
        raise HTTPException(status_code=503, detail="Dataset estimate model has an invalid format. Retrain it with ml_api/train_dataset_estimate_model.py.")
    metadata = bundle["metadata"]
    # Accept the pre-rename artifact until the dataset model is retrained.
    if metadata.get("modelType") not in {"training_dataset_estimate", "dataset_demo"} or metadata.get("features") != DATASET_ESTIMATE_FEATURES:
        raise HTTPException(status_code=503, detail="Dataset estimate model schema is invalid. Retrain it with ml_api/train_dataset_estimate_model.py.")
    return bundle


def append_unlabelled(data: dict):
    fields = ["loggedAtUtc", "itemSelected", "item", "itemType", "mq135", "mq3", "mq4", *FEATURES,
              "fruit", "mode", "dataReady", "dhtValid", "state", "featuresVersion", "readingAgeMs"]
    exists = LOG_PATH.exists()
    row = {"loggedAtUtc": datetime.now(timezone.utc).isoformat(), **data}
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with LOG_PATH.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        if not exists:
            writer.writeheader()
        writer.writerow(row)


@app.get("/")
def root():
    return {"service": "SPOIL ALERT ML API", "endpoints": ["/health", "/predict"]}


@app.get("/health")
def health():
    version = None
    model_loaded = False
    live_enabled = False
    model_type = None
    dataset_estimate_enabled = False
    if MODEL_PATH.exists():
        try:
            bundle = _load_model()
            version = bundle["metadata"].get("modelVersion")
            model_type = bundle["metadata"].get("modelType")
            model_loaded = True
            live_enabled = model_type == "real_labeled_prototype"
        except HTTPException:
            pass
    if not live_enabled and DATASET_ESTIMATE_MODEL_PATH.exists():
        try:
            estimate_bundle = _load_dataset_estimate_model()
            model_loaded = True
            dataset_estimate_enabled = True
            if model_type is None:
                model_type = "training_dataset_estimate"
                version = estimate_bundle["metadata"].get("modelVersion")
        except HTTPException:
            pass
    return {
        "ok": True,
        "modelLoaded": model_loaded,
        "livePredictionEnabled": live_enabled,
        "datasetEstimateEnabled": dataset_estimate_enabled,
        "modelType": _public_model_type(model_type),
        "modelVersion": _public_model_version(version),
        "predictionStatus": "ready" if live_enabled else "dataset_estimates_only" if dataset_estimate_enabled else "real_labeled_model_required",
    }


@app.post("/predict")
def predict(reading: Reading):
    data = reading.model_dump(mode="json")

    if not reading.itemSelected:
        raise HTTPException(status_code=422, detail="Select a fruit or vegetable before requesting a prediction.")
    if not reading.item or reading.itemType is None:
        raise HTTPException(status_code=422, detail="Selected item name and itemType are required.")
    if reading.item != ITEMS[reading.fruitIndex]:
        raise HTTPException(status_code=422, detail="Selected item name does not match fruitIndex.")
    expected_type = "Vegetable" if reading.fruitIndex in VEGETABLE_INDICES else "Fruit"
    if reading.itemType != expected_type:
        raise HTTPException(status_code=422, detail="Selected item type does not match fruitIndex.")
    if reading.mode.strip().upper() != "LIVE" or reading.state.strip().upper() != "READY":
        raise HTTPException(status_code=422, detail="Prediction requires ESP32 mode=LIVE and state=READY.")
    if not reading.dataReady or not reading.dhtValid:
        raise HTTPException(status_code=422, detail="Prediction requires dataReady=true and dhtValid=true.")
    if reading.featuresVersion != 2:
        raise HTTPException(status_code=422, detail="Unsupported ESP32 featuresVersion.")
    if reading.readingAgeMs > MAX_READING_AGE_MS:
        raise HTTPException(status_code=422, detail="Sensor reading is stale; the last verified LIVE window is over 150 seconds old.")

    # Save only sensor-valid, explicitly selected, LIVE requests. This is still
    # unlabeled telemetry and never serves as training input.
    append_unlabelled(data)

    bundle = None
    metadata = {}
    if MODEL_PATH.exists():
        bundle = _load_model()
        metadata = bundle["metadata"]
    if bundle is not None and metadata.get("modelType") == "real_labeled_prototype":
        # Preserve the training column names when passing one prediction row
        # into the fitted raw-ADC pipeline.
        x = pd.DataFrame([[getattr(reading, key) for key in FEATURES]], columns=FEATURES)
        model = bundle["model"]
        prediction = str(model.predict(x)[0])
        confidence = None
        if hasattr(model, "predict_proba"):
            confidence = round(float(max(model.predict_proba(x)[0])) * 100, 1)
        return {
            "prediction": prediction,
            "confidence": confidence,
            "modelVersion": _public_model_version(metadata["modelVersion"]),
            "modelType": _public_model_type(metadata.get("modelType")),
            "predictionSource": "real_labeled_experimental_ml",
            "predictionTimestampUtc": datetime.now(timezone.utc).isoformat(),
            "item": reading.item,
            "itemType": reading.itemType,
            "shelfLifeDaysEstimate": None,
            "note": "Experimental model trained on real labeled sensor samples; not a food-safety determination.",
        }

    estimate_bundle = None
    if DATASET_ESTIMATE_MODEL_PATH.exists():
        estimate_bundle = _load_dataset_estimate_model()
    if estimate_bundle is not None:
        estimate_metadata = estimate_bundle["metadata"]
        estimate_fruit = DATASET_ESTIMATE_FRUIT_ALIASES.get(reading.item, reading.item)
        supported_fruits = estimate_metadata.get("fruits", [])
        if estimate_fruit in supported_fruits:
            x = pd.DataFrame([[
                estimate_fruit,
                reading.rawMq3,
                reading.rawMq4,
                reading.rawMq135,
                reading.temperature,
                reading.humidity,
            ]], columns=DATASET_ESTIMATE_FEATURES)
            prediction = str(estimate_bundle["model"].predict(x)[0])
            numeric_values = {
                "MQ3": reading.rawMq3,
                "MQ4": reading.rawMq4,
                "MQ135": reading.rawMq135,
                "Temperature_C": reading.temperature,
                "Humidity_pct": reading.humidity,
            }
            ranges = estimate_metadata.get("numericFeatureRanges", {})
            outside_range = [
                key for key, value in numeric_values.items()
                if key in ranges and (value < ranges[key]["min"] or value > ranges[key]["max"])
            ]
            range_note = (
                " Input outside this training dataset's observed range for: " + ", ".join(outside_range) + "."
                if outside_range else ""
            )
            return {
                "prediction": prediction,
                "confidence": None,
                "modelVersion": _public_model_version(estimate_metadata.get("modelVersion")),
                "modelType": "training_dataset_estimate",
                "predictionSource": "training_dataset_estimate",
                "inputOutsideTrainingRange": bool(outside_range),
                "inputOutsideTrainingRangeFeatures": outside_range,
                "predictionTimestampUtc": datetime.now(timezone.utc).isoformat(),
                "item": reading.item,
                "itemType": reading.itemType,
                "shelfLifeDaysEstimate": None,
                "note": (
                    "Dataset-based estimate from recording-day label assumptions and generated temperature/humidity. "
                    "Sensor calibration may differ; not a food-safety determination." + range_note
                ),
            }
        dataset_note = (
            f"No reference rows for {reading.item} are present in the training dataset; "
            "no class was inferred."
        )
    else:
        dataset_note = "No compatible training-dataset or real-labeled model is installed; no class was inferred."

    if bundle is None or metadata.get("modelType") in {"synthetic_demo", "dataset_demo"}:
        return {
            "prediction": "UNVERIFIED",
            "confidence": None,
            "modelVersion": metadata.get("modelVersion"),
            "modelType": _public_model_type(metadata.get("modelType", "no_real_model")),
            "predictionSource": "real_labeled_model_unavailable",
            "predictionTimestampUtc": datetime.now(timezone.utc).isoformat(),
            "item": reading.item,
            "itemType": reading.itemType,
            "shelfLifeDaysEstimate": None,
            "note": dataset_note,
        }
