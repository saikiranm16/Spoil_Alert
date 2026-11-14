SPOILALERT TRAINING DATASET PACKAGE
====================================

Files
-----
1. spoilalert_training_dataset.csv
   A cleaned, downsampled dataset for experimenting with a multi-class gas-sensor model.
   Target column: Freshness_Label
   Classes: Fresh, Early, Medium, Spoiled

2. class_distribution.csv
   Number of rows assigned to each experimental class.

3. source_file_mapping.csv
   Shows which original fruit/day recording files were used and how experimental labels were assigned.

IMPORTANT DATA QUALITY / SAFETY NOTICE
--------------------------------------
This is an experimental prototype dataset, not a scientifically validated food-safety dataset.

- The original ZIP provided gas-sensor readings (MQ2, MQ3, MQ4, MQ5, MQ6, MQ7, MQ8, MQ9, MQ135) and timestamps.
- The original CSVs do not contain measured DHT22 temperature/humidity columns or verified Fresh/Early/Medium/Spoiled target labels.
- Temperature_C values (25.1 to 31.9 C) and Humidity_pct values (40 to 65%) were synthetically generated for software-pipeline testing. They are NOT real DHT22 measurements and are not associated with actual environmental observations.
- Files named D1, D2, D3, D4 and D5 were mapped to Fresh, Early, Medium, Spoiled and Spoiled respectively. This mapping is an assumption, not verified ground truth. Confirm the original dataset paper/protocol before using these labels for research conclusions.
- Files that mix multiple fruits but do not have a D1-D5 day label were excluded because their sample-level freshness labels could not be established safely.
- The readings were downsampled evenly within each included source file to a maximum of 1,800 rows per file, giving a manageable dataset.

Recommended use
---------------
Use this package to test your preprocessing and model-training code only. Do not claim accuracy from it as evidence of real spoilage detection.
For a valid model, collect synchronized MQ sensor + DHT22 measurements and manually verified freshness labels for each sample over time. Split train/test data by fruit sample or recording session, not randomly by row, because adjacent time-series rows are highly correlated.
Do not use Ticks, file day, or source filename as model input. In the provided training CSV, those fields are intentionally omitted.
For first experiments, try RandomForestClassifier or ExtraTreesClassifier and evaluate with a grouped split. Since Fruit is categorical, encode it (for example, OneHotEncoder) or omit it if the model should generalize to unseen fruits.
The model's output is an experimental classification, not a food-safety certification. Do not use it to decide whether food is safe to eat.
