"""Train a dataset-based estimate model from spoilalert_training_dataset.csv.

This dataset has assumed freshness labels and generated environment columns.
The output is intentionally separate from the live ESP32 model artifact.
"""
from datetime import datetime, timezone
from pathlib import Path

import joblib
import pandas as pd
from sklearn.compose import ColumnTransformer
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, balanced_accuracy_score, classification_report, confusion_matrix
from sklearn.model_selection import GroupShuffleSplit
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import OneHotEncoder

ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = ROOT.parent
DATA = PROJECT_ROOT / "data" / "spoilalert_training_dataset.csv"
SOURCE_MAP = PROJECT_ROOT / "data" / "source_file_mapping.csv"
OUT = ROOT / "models" / "spoilalert_dataset_estimate_model.joblib"
TARGET = "Freshness_Label"
NUMERIC = ["MQ3", "MQ4", "MQ135", "Temperature_C", "Humidity_pct"]
LABELS = ["Fresh", "Early", "Medium", "Spoiled"]

if not DATA.is_file():
    raise SystemExit(f"Dataset not found: {DATA}")
df = pd.read_csv(DATA)
required = ["Fruit", *NUMERIC, TARGET]
missing = [column for column in required if column not in df.columns]
if missing:
    raise SystemExit(f"Dataset is missing columns: {missing}")
df = df[required].dropna().copy()
df[TARGET] = df[TARGET].astype(str).str.strip().str.title()
if sorted(df[TARGET].unique()) != sorted(LABELS):
    raise SystemExit(f"Expected labels {LABELS}; found {sorted(df[TARGET].unique())}")
for column in NUMERIC:
    df[column] = pd.to_numeric(df[column], errors="coerce")
df = df.dropna(subset=NUMERIC)

X = df[["Fruit", *NUMERIC]]
y = df[TARGET]
if not SOURCE_MAP.is_file():
    raise SystemExit(f"Source recording map not found: {SOURCE_MAP}")
source_map = pd.read_csv(SOURCE_MAP)
map_columns = {"source_file", "rows_in_training_csv", "fruit", "assigned_freshness_label"}
missing_map_columns = sorted(map_columns - set(source_map.columns))
if missing_map_columns:
    raise SystemExit(f"Source map is missing columns: {missing_map_columns}")
row_counts = pd.to_numeric(source_map["rows_in_training_csv"], errors="coerce")
if row_counts.isna().any() or (row_counts <= 0).any() or not (row_counts % 1 == 0).all():
    raise SystemExit("Source map rows_in_training_csv must contain positive integer row counts.")
row_counts = row_counts.astype(int)
if int(row_counts.sum()) != len(df):
    raise SystemExit(
        f"Source map accounts for {int(row_counts.sum())} rows, but the dataset has {len(df)}."
    )

# The CSV is assembled in source_file order, with each recording kept as one
# contiguous block. Validate that assumption before using source_file as group.
groups = []
offset = 0
for source in source_map.itertuples(index=False):
    count = int(source.rows_in_training_csv)
    recording = df.iloc[offset:offset + count]
    expected_label = str(source.assigned_freshness_label).strip().title()
    if set(recording["Fruit"].astype(str).str.strip()) != {str(source.fruit).strip()}:
        raise SystemExit(f"Dataset row block for {source.source_file} does not match its mapped fruit.")
    if set(recording[TARGET]) != {expected_label}:
        raise SystemExit(f"Dataset row block for {source.source_file} does not match its mapped label.")
    groups.extend([str(source.source_file)] * count)
    offset += count
groups = pd.Series(groups, index=df.index, name="source_file")

selected = None
all_fruits = set(X["Fruit"])
for train_idx, test_idx in GroupShuffleSplit(
    n_splits=500, test_size=0.30, random_state=42
).split(X, y, groups):
    if (
        set(y.iloc[train_idx]) == set(LABELS)
        and set(y.iloc[test_idx]) == set(LABELS)
        and set(X.iloc[train_idx]["Fruit"]) == all_fruits
        and set(X.iloc[test_idx]["Fruit"]) == all_fruits
    ):
        selected = train_idx, test_idx
        break
if selected is None:
    raise SystemExit("Could not make a source-recording holdout containing all classes and fruits.")
train_idx, test_idx = selected
X_train, X_test = X.iloc[train_idx], X.iloc[test_idx]
y_train, y_test = y.iloc[train_idx], y.iloc[test_idx]
groups_train = groups.iloc[train_idx]


def make_model(params, n_estimators):
    preprocess = ColumnTransformer(
        [("fruit", OneHotEncoder(handle_unknown="ignore"), ["Fruit"])],
        remainder="passthrough",
    )
    classifier_params = {
        "n_estimators": n_estimators,
        "class_weight": "balanced",
        "random_state": 42,
        "n_jobs": -1,
        **params,
    }
    return Pipeline([
        ("features", preprocess),
        ("classifier", RandomForestClassifier(**classifier_params)),
    ])


# Tune only inside the outer training recordings; preserve the outer holdout
# for the final grouped evaluation.
candidates = [
    ("strong_regularization", {"max_depth": 8, "min_samples_leaf": 100, "min_samples_split": 200}),
    ("moderate_regularization", {"max_depth": 12, "min_samples_leaf": 40, "min_samples_split": 80}),
    ("baseline", {"max_depth": None, "min_samples_leaf": 2, "min_samples_split": 2}),
]
inner_splits = []
for inner_train_idx, inner_valid_idx in GroupShuffleSplit(
    n_splits=200, test_size=0.25, random_state=17
).split(X_train, y_train, groups_train):
    if (
        set(y_train.iloc[inner_train_idx]) == set(LABELS)
        and set(y_train.iloc[inner_valid_idx]) == set(LABELS)
    ):
        inner_splits.append((inner_train_idx, inner_valid_idx))
        if len(inner_splits) == 3:
            break
if len(inner_splits) < 3:
    raise SystemExit("Could not make three grouped tuning splits containing all classes.")

candidate_scores = {}
for name, params in candidates:
    scores = []
    for inner_train_idx, inner_valid_idx in inner_splits:
        candidate_model = make_model(params, n_estimators=120)
        candidate_model.fit(X_train.iloc[inner_train_idx], y_train.iloc[inner_train_idx])
        validation_prediction = candidate_model.predict(X_train.iloc[inner_valid_idx])
        scores.append(float(balanced_accuracy_score(y_train.iloc[inner_valid_idx], validation_prediction)))
    candidate_scores[name] = float(sum(scores) / len(scores))
best_name = max(candidate_scores, key=candidate_scores.get)
best_params = dict(dict(candidates)[best_name])
print("Grouped inner validation balanced accuracy:")
for name, score in candidate_scores.items():
    print(f"  {name}: {score:.4f}")
print(f"Selected configuration: {best_name}")

model = make_model(best_params, n_estimators=300)
model.fit(X_train, y_train)
pred = model.predict(X_test)
train_pred = model.predict(X_train)
train_accuracy = float(accuracy_score(y_train, train_pred))
test_accuracy = float(accuracy_score(y_test, pred))
train_balanced_accuracy = float(balanced_accuracy_score(y_train, train_pred))
test_balanced_accuracy = float(balanced_accuracy_score(y_test, pred))
accuracy_gap = train_accuracy - test_accuracy
print("TRAINING DATA ESTIMATE MODEL — this is not the live ESP32 model.")
print(f"Rows used={len(df)}, train rows={len(X_train)}, holdout rows={len(X_test)}")
print(f"Source recordings: train={groups.iloc[train_idx].nunique()}, holdout={groups.iloc[test_idx].nunique()}")
print(f"Training accuracy: {train_accuracy:.4f}; grouped holdout accuracy: {test_accuracy:.4f}; gap: {accuracy_gap:+.4f}")
print(f"Training balanced accuracy: {train_balanced_accuracy:.4f}; grouped holdout balanced accuracy: {test_balanced_accuracy:.4f}")
print("Confusion matrix class order:", LABELS)
print(confusion_matrix(y_test, pred, labels=LABELS))
print(classification_report(y_test, pred, labels=LABELS, zero_division=0))

model.fit(X, y)
OUT.parent.mkdir(parents=True, exist_ok=True)
joblib.dump({"model": model, "metadata": {
    "modelVersion": "spoilalert-dataset-estimate-rf-3-group-tuned",
    "modelType": "training_dataset_estimate",
    "features": ["Fruit", *NUMERIC],
    "classes": LABELS,
    "trainedAtUtc": datetime.now(timezone.utc).isoformat(),
    "trainingRows": int(len(df)),
    "evaluation": {
        "split": "source_file_group_holdout",
        "trainAccuracy": train_accuracy,
        "holdoutAccuracy": test_accuracy,
        "accuracyGap": accuracy_gap,
        "trainBalancedAccuracy": train_balanced_accuracy,
        "holdoutBalancedAccuracy": test_balanced_accuracy,
        "selectedConfiguration": best_name,
        "selectedParameters": best_params,
        "innerGroupedBalancedAccuracy": candidate_scores,
        "trainingRecordings": int(groups.iloc[train_idx].nunique()),
        "holdoutRecordings": int(groups.iloc[test_idx].nunique()),
    },
    "fruits": sorted(str(value) for value in df["Fruit"].unique()),
    "numericFeatureRanges": {
        column: {"min": float(df[column].min()), "max": float(df[column].max())}
        for column in NUMERIC
    },
    "dataSource": "data/spoilalert_training_dataset.csv",
    "warning": "Dataset-based estimate only. Freshness labels are inferred from recording day; temperature and humidity are generated; the original sensor calibration may differ. Metrics use source-file grouped holdouts and are not validated for food-safety decisions.",
}}, OUT)
print(f"Saved training dataset estimate model: {OUT}")
