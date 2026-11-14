"""Train and evaluate a grouped baseline model from real, labelled samples.

Required training_data.csv columns:
fruitIndex,temperature,humidity,rawMq135,rawMq3,rawMq4,label,sample_id
All rows from one physical sample are kept in the same train/test split.
"""
from pathlib import Path
import sys
from datetime import datetime, timezone

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.impute import SimpleImputer
from sklearn.metrics import accuracy_score, balanced_accuracy_score, classification_report, confusion_matrix
from sklearn.model_selection import GroupShuffleSplit
from sklearn.pipeline import Pipeline

ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = ROOT.parent
DATA = PROJECT_ROOT / "data" / "training_data.csv"
MODEL_OUT = ROOT / "models" / "spoil_alert_model.joblib"
FEATURES = ["fruitIndex", "temperature", "humidity", "rawMq135", "rawMq3", "rawMq4"]
LABELS = ["Fresh", "Early", "Medium", "Spoiled"]
MODEL_VERSION = "grouped-rf-2-raw-adc"
BOUNDS = {
    "fruitIndex": (0, 9),
    "temperature": (-20, 100),
    "humidity": (0, 100),
    "rawMq135": (20, 4080),
    "rawMq3": (20, 4080),
    "rawMq4": (20, 4080),
}


def fail(message: str):
    sys.exit(message)


if not DATA.exists():
    fail("Missing data/training_data.csv. Collect real readings with independently verified labels; do not train on data/training_template.csv.")

try:
    df = pd.read_csv(DATA)
except Exception as exc:
    fail(f"Could not read training_data.csv: {exc}")

required = FEATURES + ["label", "sample_id"]
missing = [column for column in required if column not in df.columns]
if missing:
    fail(f"Missing required columns: {missing}")
if df.empty:
    fail("training_data.csv has no data rows.")

df = df[required].copy()
null_counts = df.isna().sum()
missing_values = {key: int(value) for key, value in null_counts.items() if value}
if missing_values:
    fail(f"Missing values by column: {missing_values}. Correct or remove those rows explicitly.")

for column in FEATURES:
    df[column] = pd.to_numeric(df[column], errors="coerce")
    bad = df[column].isna() | ~np.isfinite(df[column].to_numpy(dtype=float))
    if bad.any():
        fail(f"Invalid/non-numeric values in {column}: rows {list(df.index[bad] + 2)[:20]}")
    low, high = BOUNDS[column]
    outside = (df[column] < low) | (df[column] > high)
    if outside.any():
        fail(f"Out-of-range values in {column} (expected {low}..{high}): rows {list(df.index[outside] + 2)[:20]}")
if not np.equal(df["fruitIndex"].to_numpy(dtype=float) % 1, 0).all():
    fail("fruitIndex must be an integer item index from 0 through 9 (the current ESP32 item list).")
df["fruitIndex"] = df["fruitIndex"].astype(int)
df["label"] = df["label"].astype(str).str.strip().str.title()
df["sample_id"] = df["sample_id"].astype(str).str.strip()
if (df["sample_id"] == "").any():
    fail("sample_id cannot be empty; it must identify the physical fruit/sample.")
unknown = sorted(set(df["label"]) - set(LABELS))
if unknown:
    fail(f"Unsupported labels: {unknown}. Use only {LABELS}.")
absent = sorted(set(LABELS) - set(df["label"]))
if absent:
    fail(f"No real labelled rows found for classes {absent}; all four documented classes are required.")
if len(df) < 40:
    fail(f"Only {len(df)} rows found. Collect more independently labelled real data before training (software minimum: 40 rows).")

groups = df["sample_id"]
group_counts = df.groupby("label")["sample_id"].nunique()
too_few = {label: int(group_counts.get(label, 0)) for label in LABELS if group_counts.get(label, 0) < 2}
if too_few:
    fail(f"Need at least two distinct physical sample_id values per class for a grouped holdout split; found: {too_few}")

X, y = df[FEATURES], df["label"]
splitter = GroupShuffleSplit(n_splits=100, test_size=0.25, random_state=42)
selected = None
for train_idx, test_idx in splitter.split(X, y, groups):
    if set(y.iloc[train_idx]) == set(LABELS) and set(y.iloc[test_idx]) == set(LABELS):
        selected = train_idx, test_idx
        break
if selected is None:
    fail("Could not create a grouped split containing all four classes in train and test. Collect more distinct physical samples per class.")
train_idx, test_idx = selected
X_train, X_test = X.iloc[train_idx], X.iloc[test_idx]
y_train, y_test = y.iloc[train_idx], y.iloc[test_idx]

model = Pipeline([
    ("imputer", SimpleImputer(strategy="median")),
    ("classifier", RandomForestClassifier(n_estimators=300, class_weight="balanced", random_state=42, min_samples_leaf=2)),
])
model.fit(X_train, y_train)
pred = model.predict(X_test)
train_pred = model.predict(X_train)
train_accuracy = float(accuracy_score(y_train, train_pred))
test_accuracy = float(accuracy_score(y_test, pred))
train_balanced_accuracy = float(balanced_accuracy_score(y_train, train_pred))
test_balanced_accuracy = float(balanced_accuracy_score(y_test, pred))
accuracy_gap = train_accuracy - test_accuracy
print(f"Grouped holdout by physical sample_id; train rows={len(train_idx)}, test rows={len(test_idx)}")
print(f"Training accuracy={train_accuracy:.3f}; grouped holdout accuracy={test_accuracy:.3f}; gap={accuracy_gap:+.3f}")
print(f"Training balanced accuracy={train_balanced_accuracy:.3f}; grouped holdout balanced accuracy={test_balanced_accuracy:.3f}")
print("Confusion matrix class order:", LABELS)
print(confusion_matrix(y_test, pred, labels=LABELS))
print("Per-class precision, recall, and F1:")
print(classification_report(y_test, pred, labels=LABELS, zero_division=0))

model.fit(X, y)
bundle = {
    "model": model,
    "metadata": {
        "modelVersion": MODEL_VERSION,
        "modelType": "real_labeled_prototype",
        "features": FEATURES,
        "classes": LABELS,
        "trainedAtUtc": datetime.now(timezone.utc).isoformat(),
        "trainingRows": int(len(df)),
        "physicalSamples": int(groups.nunique()),
        "evaluation": {
            "split": "physical_sample_id_group_holdout",
            "trainAccuracy": train_accuracy,
            "holdoutAccuracy": test_accuracy,
            "accuracyGap": accuracy_gap,
            "trainBalancedAccuracy": train_balanced_accuracy,
            "holdoutBalancedAccuracy": test_balanced_accuracy,
        },
    },
}
MODEL_OUT.parent.mkdir(parents=True, exist_ok=True)
joblib.dump(bundle, MODEL_OUT)
print(f"Saved model: {MODEL_OUT}")
