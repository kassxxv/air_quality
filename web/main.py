from __future__ import annotations

import json
import time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from fastapi import FastAPI
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

# ─── App ──────────────────────────────────────────────────────────────────────

app = FastAPI(title="Air Quality Monitor")

STATIC_DIR = Path(__file__).parent / "static"
LOG_FILE   = Path(__file__).parent / "command_log.json"

# ─── In-memory state ──────────────────────────────────────────────────────────

latest: Optional[dict] = None
history: deque[dict] = deque(maxlen=500)   # readings with server timestamp (ms)
daily: dict[str, list[dict]] = {}          # "YYYY-MM-DD" → [readings]

device_state = {
    "co2_threshold":  1000,
    "pm25_threshold": 55,
    "buzzer":         0,    # 0 = auto (state machine), 1 = force constant tone
}
pending_led: Optional[int] = None          # one-shot, cleared after delivery

command_log: deque[dict] = deque(maxlen=50)

# Load persisted log so entries survive server restarts
if LOG_FILE.exists():
    try:
        command_log.extend(json.loads(LOG_FILE.read_text()))
    except Exception:
        pass

# ─── Helpers ──────────────────────────────────────────────────────────────────

def air_score(co2: float, pm25: float) -> int:
    c = min(100.0, max(0.0, (co2 - 400) / 16))
    p = min(100.0, max(0.0, pm25 / 0.75))
    return round(c * 0.6 + p * 0.4)


def score_label(score: int) -> str:
    if score <= 25:
        return "Good"
    if score <= 60:
        return "Moderate"
    return "Alarm"


def now_ms() -> int:
    return int(time.time() * 1000)


def log_command(msg: str):
    ts = datetime.now(timezone.utc).strftime("%H:%M:%S")
    command_log.appendleft({"ts": ts, "msg": msg})
    try:
        LOG_FILE.write_text(json.dumps(list(command_log)))
    except Exception:
        pass

# ─── Request / response models ────────────────────────────────────────────────

class SensorReading(BaseModel):
    co2:     int
    pm25:    int
    temp:    float
    hum:     float
    pmsTemp: float
    pmsHum:  float


class ControlCommand(BaseModel):
    co2_threshold:  Optional[int]   = None
    pm25_threshold: Optional[int]   = None
    buzzer:         Optional[int]   = None   # 0 or 1
    led:            Optional[int]   = None   # 0 or 1, one-shot

# ─── ESP32 endpoints ──────────────────────────────────────────────────────────

@app.post("/api/data")
def receive_data(reading: SensorReading):
    global latest
    ts = now_ms()
    record = {
        "co2":     reading.co2,
        "pm25":    reading.pm25,
        "temp":    reading.temp,
        "hum":     reading.hum,
        "pmsTemp": reading.pmsTemp,
        "pmsHum":  reading.pmsHum,
        "ts":      ts,
    }
    latest = record
    history.append(record)

    date_key = datetime.fromtimestamp(ts / 1000, tz=timezone.utc).strftime("%Y-%m-%d")
    daily.setdefault(date_key, []).append(record)

    return {"ok": True}


@app.get("/api/commands")
def send_commands():
    global pending_led

    cmd = {
        "co2_threshold":  device_state["co2_threshold"],
        "pm25_threshold": device_state["pm25_threshold"],
        "buzzer":         device_state["buzzer"],
    }
    if pending_led is not None:
        cmd["led"] = pending_led
        pending_led = None

    return cmd

# ─── Dashboard endpoints ──────────────────────────────────────────────────────

@app.get("/api/status")
def get_status():
    score = None
    label = None
    if latest and latest["co2"] > 0:
        score = air_score(latest["co2"], latest["pm25"])
        label = score_label(score)

    return {
        "latest":  latest,
        "score":   score,
        "label":   label,
        "state":   device_state,
        "history": list(history)[-100:],
        "log":     list(command_log),
    }


@app.get("/api/history")
def get_history():
    return {"history": list(history)}


@app.get("/api/daily")
def get_daily():
    result = {}
    for date_key, readings in daily.items():
        valid = [r for r in readings if r["co2"] > 0]
        if not valid:
            continue
        avg_co2  = sum(r["co2"]  for r in valid) / len(valid)
        avg_pm25 = sum(r["pm25"] for r in valid) / len(valid)
        avg_temp = sum(r["temp"] for r in valid) / len(valid)
        avg_hum  = sum(r["hum"]  for r in valid) / len(valid)
        score    = air_score(avg_co2, avg_pm25)
        result[date_key] = {
            "score":    score,
            "label":    score_label(score),
            "avg_co2":  round(avg_co2, 1),
            "avg_pm25": round(avg_pm25, 1),
            "avg_temp": round(avg_temp, 1),
            "avg_hum":  round(avg_hum, 1),
            "count":    len(valid),
        }
    return result


@app.post("/api/control")
def set_control(cmd: ControlCommand):
    global pending_led

    if cmd.co2_threshold is not None:
        device_state["co2_threshold"] = cmd.co2_threshold
        log_command(f"CO₂ threshold → {cmd.co2_threshold} ppm")

    if cmd.pm25_threshold is not None:
        device_state["pm25_threshold"] = cmd.pm25_threshold
        log_command(f"PM2.5 threshold → {cmd.pm25_threshold} μg/m³")

    if cmd.buzzer is not None:
        device_state["buzzer"] = cmd.buzzer
        log_command(f"Buzzer → {'forced ON' if cmd.buzzer else 'auto'}")

    if cmd.led is not None:
        pending_led = cmd.led
        log_command(f"LED one-shot → {'ON' if cmd.led else 'OFF'}")

    return {"ok": True, "state": device_state}

# ─── Serve frontend ───────────────────────────────────────────────────────────

@app.get("/")
def root():
    return FileResponse(STATIC_DIR / "index.html")


app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")
