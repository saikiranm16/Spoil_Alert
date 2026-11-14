"""Collect labelled training rows straight from the ESP32 (no extra packages).

Example (collect 6 verified readings of one physical onion you judged "Fresh"):

    python ml_api/collect_samples.py --esp http://192.168.43.50 \
        --fruit-index 4 --label Fresh --sample-id onion_01 --count 6

Rules this script enforces so the training file stays trustworthy:
  * only LIVE + READY + dataReady + dhtValid readings with featuresVersion 2,
  * each ESP32 verified window (the `report` counter) is saved only once,
  * the label is what YOU decided from inspecting the physical produce
    (look, smell, firmness, date bought...). Never use a model/OLED result.
  * use ONE sample_id per physical piece of produce.

Output columns match train_model.py:
fruitIndex,temperature,humidity,rawMq135,rawMq3,rawMq4,label,sample_id
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = PROJECT_ROOT / "data" / "training_data.csv"
FIELDS = ["fruitIndex", "temperature", "humidity", "rawMq135", "rawMq3", "rawMq4", "label", "sample_id"]
LABELS = ["Fresh", "Early", "Medium", "Spoiled"]
MAX_READING_AGE_MS = 150_000      # same limit the API uses
ADC_MIN, ADC_MAX = 20, 4080       # same limits as the firmware/API/trainer


def fetch_json(url: str, timeout: float = 6.0):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def select_item(base: str, fruit_index: int, names: list[str]) -> str:
    """Select the requested ESP32 item and verify its acknowledgement."""
    if not isinstance(names, list) or any(not isinstance(name, str) for name in names):
        raise ValueError("ESP32 returned an invalid item list")
    if not 0 <= fruit_index < len(names):
        raise ValueError(f"fruit index {fruit_index} is not present in the ESP32 item list")
    expected_name = names[fruit_index]
    result = fetch_json(f"{base}/api/item?id={fruit_index}")
    if (
        not isinstance(result, dict)
        or result.get("ok") is not True
        or result.get("itemSelected") is not True
        or result.get("itemIndex") != fruit_index
        or result.get("fruitIndex") != fruit_index
        or result.get("item") != expected_name
    ):
        raise ValueError("ESP32 did not confirm the requested item selection")
    return expected_name


def reading_problem(d: dict, expected_fruit_index: int, expected_item: str | None = None) -> str | None:
    """Return why a status reading must be skipped, or None when it is usable."""
    if d.get("itemSelected") is not True:
        return "no item is selected on the ESP32"
    reported_index = d.get("fruitIndex")
    if isinstance(reported_index, bool) or not isinstance(reported_index, int) or reported_index != expected_fruit_index:
        return "ESP32 item selection changed; refusing to label this reading"
    if expected_item is not None and d.get("item") != expected_item:
        return "ESP32 item name does not match the requested selection"
    if str(d.get("mode", "")).upper() != "LIVE":
        return "ESP32 is not in LIVE mode"
    if str(d.get("state", "")).upper() != "READY":
        return f"state is {d.get('state')} (waiting for READY)"
    if d.get("dataReady") is not True or d.get("dhtValid") is not True:
        return "dataReady/dhtValid is false"
    if d.get("measurementStable") is not True:
        return "measurement window is not marked stable"
    if d.get("featuresVersion") != 2:
        return "featuresVersion is not 2"
    age = d.get("readingAgeMs")
    if isinstance(age, bool) or not isinstance(age, int) or not 0 <= age <= MAX_READING_AGE_MS:
        return "reading is stale or no verified window yet"
    for key in ("rawMq135", "rawMq3", "rawMq4"):
        v = d.get(key)
        if isinstance(v, bool) or not isinstance(v, int) or not ADC_MIN <= v <= ADC_MAX:
            return f"{key} out of range ({v})"
    for key, lo, hi in (("temperature", -20, 100), ("humidity", 0, 100)):
        v = d.get(key)
        if not isinstance(v, (int, float)) or not lo <= v <= hi:
            return f"{key} out of range ({v})"
    return None


def append_row(out: Path, row: dict) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    new_file = not out.exists() or out.stat().st_size == 0
    with out.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        if new_file:
            writer.writeheader()
        writer.writerow(row)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--esp", required=True, help="ESP32 base URL, e.g. http://192.168.43.50")
    ap.add_argument("--fruit-index", type=int, required=True, help="0-9 item index (see /api/items on the ESP32)")
    ap.add_argument("--label", required=True, help="Fresh | Early | Medium | Spoiled (your independent judgement)")
    ap.add_argument("--sample-id", required=True, help="one id per physical produce sample, e.g. onion_01")
    ap.add_argument("--count", type=int, default=6, help="number of verified windows to save (default 6)")
    ap.add_argument("--poll", type=float, default=5.0, help="seconds between polls (default 5)")
    ap.add_argument("--timeout-min", type=float, default=60.0, help="give up after this many minutes")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args(argv)

    label = args.label.strip().title()
    if label not in LABELS:
        return print(f"--label must be one of {LABELS}") or 2
    if not 0 <= args.fruit_index <= 9:
        return print("--fruit-index must be 0..9") or 2
    sample_id = args.sample_id.strip()
    if not sample_id:
        return print("--sample-id cannot be empty") or 2
    if args.count < 1:
        return print("--count must be at least 1") or 2
    if args.poll <= 0:
        return print("--poll must be greater than 0 seconds") or 2
    if args.timeout_min <= 0:
        return print("--timeout-min must be greater than 0 minutes") or 2

    base = args.esp.rstrip("/")
    try:
        names = fetch_json(f"{base}/api/items")
        item_name = select_item(base, args.fruit_index, names)
        print(f"Item {args.fruit_index} = {item_name!r} | label={label} | sample_id={sample_id}")
    except (urllib.error.URLError, OSError, ValueError, IndexError, TypeError) as exc:
        return print(f"Cannot reach the ESP32 at {base}: {exc}") or 1

    saved, last_report, last_msg = 0, None, None
    deadline = time.time() + args.timeout_min * 60
    while saved < args.count and time.time() < deadline:
        try:
            d = fetch_json(f"{base}/api/status")
        except (urllib.error.URLError, OSError, ValueError) as exc:
            msg = f"waiting: ESP32 unreachable ({exc})"
        else:
            problem = reading_problem(d, args.fruit_index, item_name)
            report = d.get("report")
            if problem:
                msg = f"waiting: {problem}"
            elif not isinstance(report, int) or isinstance(report, bool) or report < 1:
                msg = "waiting: ESP32 report counter is missing or invalid"
            elif report == last_report:
                msg = "waiting for the next verified window"
            else:
                last_report = report
                append_row(args.out, {
                    "fruitIndex": args.fruit_index,
                    "temperature": round(float(d["temperature"]), 2),
                    "humidity": round(float(d["humidity"]), 2),
                    "rawMq135": d["rawMq135"], "rawMq3": d["rawMq3"], "rawMq4": d["rawMq4"],
                    "label": label, "sample_id": sample_id,
                })
                saved += 1
                msg = (f"saved {saved}/{args.count}: T={d['temperature']} H={d['humidity']} "
                       f"MQ135={d['rawMq135']} MQ3={d['rawMq3']} MQ4={d['rawMq4']}")
                print(msg)
                last_msg = None
                if saved < args.count:
                    time.sleep(args.poll)
                continue
        if msg != last_msg:
            print(msg)
            last_msg = msg
        time.sleep(args.poll)

    print(f"Done: {saved} row(s) written to {args.out}")
    return 0 if saved == args.count else 1


if __name__ == "__main__":
    sys.exit(main())
