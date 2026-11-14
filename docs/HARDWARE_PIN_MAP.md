# Hardware pin map and electrical notes

The pin map matches `firmware/SPOIL_ALERT_ESP32_ML_Ready/SPOIL_ALERT_ESP32_ML_Ready.ino`.

| Component / signal | ESP32 connection | Firmware symbol |
| --- | --- | --- |
| MQ-135 analog output | GPIO 34 (ADC1) | `MQ135_PIN` |
| MQ-4 analog output | GPIO 35 (ADC1) | `MQ4_PIN` |
| MQ-3 analog output | GPIO 32 (ADC1) | `MQ3_PIN` |
| DHT22 data | GPIO 4 | `DHT_PIN` |
| OLED SDA | GPIO 21 | `OLED_SDA` |
| OLED SCL | GPIO 22 | `OLED_SCL` |
| Red LED | GPIO 25 through series resistor | `RED_LED` |
| Yellow LED | GPIO 26 through series resistor | `YELLOW_LED` |
| Green LED | GPIO 27 through series resistor | `GREEN_LED` |
| Optional DS18B20 data | GPIO 18 with 4.7 kOhm pull-up to 3.3 V | `DS18B20_PIN` |

## Power and sensor cautions

- MQ heater modules normally use 5 V and may draw substantial current. Use a regulated supply sized for the modules and connect its ground to ESP32 ground.
- Never connect an MQ module analog output directly to an ESP32 input if it can exceed 3.3 V. Calculate divider output at maximum sensor output and supply voltage, include resistor tolerances, and meter the ADC node before attaching it. A 10 kOhm upper / 20 kOhm lower divider gives about 3.33 V from 5 V and is not a safe guaranteed limit.
- DHT22 data needs a pull-up to 3.3 V (typically 4.7–10 kOhm). Check module pin labels before power-up.
- The DHT22 measures ambient air at its mounting point. Use the optional waterproof DS18B20 probe when a produce temperature feature is required; keep the training temperature source consistent with the firmware setting.
- MQ outputs are raw ADC counts, not calibrated ppm values.
