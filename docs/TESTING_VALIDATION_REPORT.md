# Testing and validation report

## Scope

This report summarizes project-level software checks described by the repository. It does not record a test on the user's physical ESP32, sensor modules, phone, or Wi-Fi network.

## Recorded checks

| Area | Result / limit |
| --- | --- |
| API request validation and response contract | Covered by repository tests; live predictions require fresh, selected, stable sensor data. |
| Sample collector selection and status validation | Covered by repository tests; collector confirms the selected ESP32 item before saving a row. |
| Embedded firmware reference data parity | Covered by repository test against `data/reference_sensor_profiles.csv`. |
| Request-validation C++ helper | Previously compiled with MinGW `g++` using `-Wall -Wextra -Werror` and passed. |
| Shell launcher syntax | `bash -n start_api.sh` passed. |
| Full Arduino/ESP32 firmware build | Not run; Arduino CLI/IDE and board toolchain were unavailable in the recorded validation. |
| Physical Wi-Fi/API integration | Not run; no hardware/Android integration session was available. |

## Before field use

1. Verify the MQ divider maximum voltage at the ESP32 ADC node; never exceed 3.3 V.
2. Confirm the ESP32's current Wi-Fi IP in Serial Monitor and check `/api/status` from a PC on the same network.
3. Check the API `/health` endpoint from the phone and confirm the PC firewall permits Python on the active private network.
4. Treat dataset estimates as experimental outputs. Collect independently labeled physical samples and review grouped holdout metrics before enabling a real-labeled prototype.
