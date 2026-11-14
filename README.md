# SPOIL ALERT

ESP32 live sensor readings -> local FastAPI service -> MIT App Inventor Android app.

## Project layout

```text
firmware/SPOIL_ALERT_ESP32_ML_Ready/   maintained ESP32 Arduino sketch
mit_app/SPOILALERT_Updated.aia         MIT App Inventor source project
ml_api/app.py                          live API and input validation
ml_api/train_model.py                  trainer for real labeled sensor data
ml_api/collect_samples.py              saves labelled readings from the ESP32 into data/training_data.csv
ml_api/train_dataset_estimate_model.py trainer for the supplied training dataset
ml_api/models/                         model artifacts
data/training_template.csv             empty real-data collection template
data/reference_sensor_profiles.csv     reference profiles used by a firmware consistency check
data/logs/                             unlabeled live readings
docs/HARDWARE_PIN_MAP.md              preserved GPIO and electrical notes
docs/IMPLEMENTATION_STEPS.md           wiring, setup and operating guide
docs/ENGINEERING_AUDIT_REPORT.md       audit findings and engineering decisions
docs/TESTING_VALIDATION_REPORT.md      software tests and hardware test checklist
ml_api/train_image_model.py            optional image trainer (external dataset required)
start_api.ps1 / start_api.sh            API launchers for Windows / macOS / Linux
train.ps1 / train.sh                    real-data training launchers
requirements.txt                       Python dependencies
```

## Live pipeline status

Wi-Fi credentials are intentionally not bundled. Copy
`firmware/SPOIL_ALERT_ESP32_ML_Ready/secrets.example.h` to `secrets.h` and
enter your local network credentials before compiling.

The current firmware timing is not a 60-second end-to-end startup: it uses
100 seconds of software stabilization plus a 120-second verified sampling
window (about 220 seconds before the first normal live result). Do not shorten
these periods without repeatable measurements on the actual sensor modules.

A live prediction is marked ready only when a selected item has a fresh,
stable measurement window. Windows that remain noisy after the retry limit
are retained for diagnostics but are not sent as prediction-ready data.

The firmware sends actual ESP32 ADC counts for MQ-135, MQ-3 and MQ-4 with
`featuresVersion=2`; it no longer stretches those readings to fit workbook
ranges. The API rejects stale, unselected, unready or invalid sensor requests.
The bundled training-dataset model can return a **dataset-based estimate** (`Fresh`, `Early`,
`Medium`, or `Spoiled`) for produce represented in
`data/spoilalert_training_dataset.csv`. Its labels were assigned from recording
days and its temperature/humidity values were generated; it is not validated
for food-safety decisions. Unsupported produce remains `UNVERIFIED`. A
real-labeled compatible model takes precedence when installed, and only that
model enables `livePredictionEnabled` in `/health`.

## Optional image model

The separate `ml_api/train_image_model.py` trains a fresh/rotten photo
classifier from `Unified_Dataset/<produce>/{fresh,rotten}/` image folders.
Install its optional dependencies with `pip install -r requirements-image.txt`,
then run `python ml_api/train_image_model.py data/fruits-and-vegetables-quality/Unified_Dataset`.
The trainer holds out whole produce folders for validation and testing. It
saves `ml_api/models/produce_image_quality.keras` and metadata separately; the
sensor API does not load or serve this image model. The image files themselves
must be provided locally, with at least three produce folders containing both
classes.

The DHT22 reports ambient air temperature and humidity at the sensor location;
it does not measure the inside or surface temperature of the produce. The MIT
app labels this value **AMBIENT AIR TEMP**. To measure produce temperature, add
and wire a suitable probe such as a waterproof DS18B20, then update the firmware
and API schema for that sensor explicitly.

MQ readings are ADC counts, not ppm. Freshness labels cannot be inferred as
reliable without collecting physical produce samples, assigning independent
labels, and training with the same ESP32 board, sensor modules, analog divider,
and firmware feature format used at deployment. This project does not make a
food-safety determination.

## Run

See [docs/HARDWARE_PIN_MAP.md](docs/HARDWARE_PIN_MAP.md) for the preserved
GPIO mapping and electrical cautions. Follow
[docs/IMPLEMENTATION_STEPS.md](docs/IMPLEMENTATION_STEPS.md) in order.
In short: install the required Arduino libraries and upload the maintained
sketch; put the PC and phone on the same phone hotspot; start the API on the PC
with `--host 0.0.0.0 --port 8000`; then enter the ESP32 device URL
(`http://172.16.20.155`) and PC API URL (`http://172.16.18.199:8000`) in the
Android app. Confirm the ESP32 address in Serial Monitor after it joins Wi-Fi.

## Troubleshooting: "UNVERIFIED" and room temperature

**Result is `UNVERIFIED`.** The selected produce may not be represented by the
training dataset, or no compatible model may be installed. The app and ESP32 now
offer only the 10 items in `data/spoilalert_training_dataset.csv`: Banana,
Blueberry, Grape, Green Apple, Kiwi, Mushroom, Pear, Red Apple, Strawberry,
and Tomato. For real experimental
predictions, collect independently labeled sensor samples with
`ml_api/collect_samples.py`, train with `ml_api/train_model.py`, restart the
API, and check that `/health` shows
`"livePredictionEnabled": true`.

**Temperature equals room temperature.** The DHT22 measures the air around it.
Either mount it in the box/fridge with the produce, or enable the optional
DS18B20 probe (`USE_DS18B20_PROBE 1` in the sketch, wiring in
docs/IMPLEMENTATION_STEPS.md). Train and run the model with the same
temperature source.
