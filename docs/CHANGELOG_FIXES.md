# Fix log

## 2026-10-15

- The dataset-trained model now uses only fields available from the ESP32 and
  is served as a clearly marked dataset-based estimate for covered produce. The selected
  Tomato readings from the supplied screenshot return `Spoiled`; MQ-3 and MQ-4
  are flagged outside the training dataset's observed range. Real-labeled model
  behavior remains preferred and separate.
- The MIT App Inventor result notice now says the class is a dataset-based estimate
  based on assumed labels/synthetic conditions and is not a food-safety result.
- ESP32 item selection now accepts only a complete decimal ID within the
  available fruit list. The previous `String.toInt()` path accepted malformed
  numeric prefixes such as `1x`.
- The sample collector now selects and verifies `--fruit-index` on the ESP32
  before collection, and rejects rows if the selection is absent or changes.
  This prevents readings from being written under the wrong produce index.
- Wi-Fi credentials were moved from the sketch to the local, ignored
  `firmware/SPOIL_ALERT_ESP32_ML_Ready/secrets.h`. The example header and setup
  instructions describe how to create that file while retaining the current
  local settings.
- Added automated checks for item-ID parsing, collector selection and status
  validation, the live API's UNVERIFIED/stale/raw-ADC contract, and parity
  between the embedded reference table and `reference_sensor_profiles.csv`.

Verification: 13 Python regression tests passed, including the screenshot
Tomato prediction/API, app disclaimer, collector integrity, and data parity;
the C++ item-ID parser test compiled
with MinGW `g++` under `-Wall -Wextra -Werror` and passed. The full ESP32 sketch
was not compiled because Arduino CLI/IDE is not installed in this environment.
No physical sensor, OLED, LED, Wi-Fi, or Android hardware test was performed.
