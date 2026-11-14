# Data files

- `reference_sensor_profiles.csv`: simulated profiles used only to check
  consistency with the firmware's embedded reference table. They are not
  physical sensor measurements and cannot train the live model.
- `training_template.csv`: header-only template for real sample collection.
  Copy it to `training_data.csv` and add independently labeled readings.
- `training_data.csv`: created by the project owner when real samples are
  collected. It is intentionally not included yet.
- `logs/unlabelled_readings_v3_raw_adc.csv`: current raw-ADC API telemetry.
  Older `unlabelled_readings*.csv` files use legacy scaled features and should
  not be mixed into the new training template. These readings have no
  independent freshness labels and must not be used as training truth.
- Optional image-training data is not included in this archive. The separate
  `ml_api/train_image_model.py` script expects
  `Unified_Dataset/<produce>/{fresh,rotten}/` image folders. No reference
  notebook or image files are bundled. This image model is separate from live
  sensor predictions.

Required real-training fields:
`fruitIndex,temperature,humidity,rawMq135,rawMq3,rawMq4,label,sample_id`.
Use one `sample_id` per physical sample, repeated for that sample's readings.
MQ columns are ADC counts (not ppm). Train and infer with the same sensors,
divider circuitry, ESP32 ADC settings and firmware feature version.
