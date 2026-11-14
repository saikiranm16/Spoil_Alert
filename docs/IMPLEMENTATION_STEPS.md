# SPOIL ALERT implementation and field setup

## 1. Hardware and wiring

Use an ESP32 Dev Module, DHT22, MQ-135, MQ-3, MQ-4, OLED, and optional LEDs.
Current firmware pins:

| Part | ESP32 pin |
| --- | --- |
| MQ-135 analog output | GPIO 34 |
| MQ-4 analog output | GPIO 35 |
| MQ-3 analog output | GPIO 32 |
| DHT22 data | GPIO 4 |
| OLED SDA / SCL | GPIO 21 / GPIO 22 |
| Red / yellow / green LEDs | GPIO 25 / 26 / 27, each with a series resistor |

Power the MQ heaters from a regulated 5 V supply with enough current for all
three modules. Join the supply ground to ESP32 ground. Never connect a 5 V MQ
analog output directly to an ESP32 pin. Use a divider on each analog output
and calculate its worst-case output before connecting it. A 10 kOhm upper / 20
kOhm lower divider produces about 3.33 V from 5 V, slightly above 3.3 V; choose
a safer ratio and meter the ADC node before connecting. DHT22 data needs
a pull-up to 3.3 V (typically 4.7-10 kOhm). Check your particular sensor-board
pin labels before powering it.

**Optional produce-temperature probe (DS18B20).** Wire the DS18B20 data pin to
GPIO 18 with a 4.7 kOhm pull-up to 3.3 V (VDD to 3.3 V, GND to GND). In the
sketch set `#define USE_DS18B20_PROBE 1` and install the Arduino libraries
**OneWire** and **DallasTemperature**. The `temperature` value sent to the API
is then the probe temperature; humidity still comes from the DHT22. A model
must be trained and used with the same temperature source, so choose before
collecting training data.

The DHT22 reports ambient air temperature and humidity where it is mounted. It
does not measure produce surface or internal temperature. A produce temperature
measurement requires a separate food-safe probe, such as a waterproof DS18B20,
plus matching firmware and API changes.

MQ sensors need a long initial conditioning/burn-in period according to their
module datasheets. Firmware allows 100 seconds of stabilization after boot;
that does not replace initial burn-in or guarantee calibration.

## 2. Upload the live ESP32 firmware

1. Open `firmware/SPOIL_ALERT_ESP32_ML_Ready/SPOIL_ALERT_ESP32_ML_Ready.ino`
   in Arduino IDE. The folder and sketch names match.
2. Install the ESP32 board package and libraries used by the sketch: DHT sensor
   library by Adafruit, Adafruit GFX, and Adafruit SH110X (or SSD1306 if you
   change `OLED_SH1106`). Select **ESP32 Dev Module** and the correct COM port.
3. Copy `firmware/SPOIL_ALERT_ESP32_ML_Ready/secrets.example.h` to
   `secrets.h` in the same folder, then enter the phone hotspot credentials
   in `secrets.h`. Keep the hotspot on 2.4 GHz and let both the PC and ESP32
   join it. The local `secrets.h` is excluded by `.gitignore`.
4. Upload. Open Serial Monitor at **115200 baud** and wait for Wi-Fi connected
   plus the ESP32's current IP address. The app defaults to `http://172.16.20.155`; confirm that the reported address matches, since DHCP can assign a different address after reconnecting.
5. Check `http://<ESP32-IP>/api/status` from a browser on the PC. Wait through
   the five-minute stabilization and the next verified sensor window. Readings
   are raw ADC counts; no simulated status is generated in live mode.

## 3. Start the API on the PC

Use PowerShell in the project folder. The launcher script prefers the Python
3.12 `py` launcher and falls back to `python` if `py` is not installed. If
neither command is available, install Python 3.12 and reopen PowerShell.

```powershell
.\start_api.ps1
```

If PowerShell reports that scripts are not digitally signed, run this local
launcher with a process-scoped policy override (it does not change the machine
or user policy):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\start_api.ps1
```

Keep this terminal open. Check `http://127.0.0.1:8000/health` on the PC. The
phone and PC must remain on the same hotspot. If Windows Firewall asks, allow
Python on the Private network. On the phone, `http://172.16.18.199:8000/health`
should open while the API process is running.

The health response reports both `datasetEstimateEnabled` and
`livePredictionEnabled`. The training-dataset model can return a clearly
caveated estimate for supported items; it does not enable `livePredictionEnabled`. Only
a model trained from real, independently labeled sensor data does that.

## 4. Configure and use the MIT app

1. Import `mit_app/SPOILALERT_Updated.aia` into MIT App Inventor.
2. Set **ESP32 SENSOR URL** to `http://172.16.20.155` (no `/api/status`).
3. Set **ML API SERVER** to `http://172.16.18.199:8000` (no `/predict`). The app
   appends `/predict` itself. Both fields can be edited in the app.
4. Build a new APK and install it; older installed APKs do not contain these
   changes.
5. Select the item, wait for a verified live sensor window, and watch the
   connection/result labels. The API only receives a selected, ready, fresh,
   live reading. Items represented in the training dataset show a
   **dataset-based estimate**;
   unsupported items remain `UNVERIFIED`. Real-labeled training is required
   before enabling experimental live classification.

## 5. Collect data before enabling a classifier

The app logs unlabeled readings to `data/logs/`; these logs are not training
labels. For each physical sample, record readings over time and independently
assign its freshness label using a documented method. Do not use generated
reference data, model predictions, OLED status, or telemetry timestamps as ground truth.

1. Collect rows with the helper (ESP32 must be LIVE and READY; each saved row
   is one verified window, about every 2 minutes). Run it once per physical
   piece of produce, with the label you judged from the real item:

   ```powershell
   & "$PWD\.venv\Scripts\python.exe" ml_api\collect_samples.py --esp http://<ESP32-IP> `
       --fruit-index 4 --label Fresh --sample-id onion_01 --count 6
   ```

   It creates `data/training_data.csv` automatically. (Manual alternative: copy
   `data/training_template.csv` and fill `fruitIndex`, temperature, humidity,
   raw MQ ADC counts, label and a stable `sample_id` yourself.)
2. Use a different `--sample-id` for every physical sample and re-measure the
   same pieces as they age. Aim for at least 6 distinct samples per class so
   the grouped holdout split can work (3 per class was too few in testing).
3. Keep the same board, sensor modules, voltage dividers, ADC settings, and
   feature names. Do not mix raw ADC data with workbook-scaled values.
4. Collect every class from multiple distinct physical samples; use the
   trainer's grouped holdout report to assess the prototype.
5. Run `& "$PWD\.venv\Scripts\python.exe" ml_api\train_model.py`. It requires
   all four labels, at least 40 rows, and at least two distinct sample IDs per
   class. Review the grouped holdout metrics before deployment.
6. Restart the API and confirm `/health` reports
   `"livePredictionEnabled": true` and `"modelType": "real_labeled_prototype"`.

Even after training, this is an experimental classifier, not a food-safety
device. Shelf-life estimation is not implemented.
