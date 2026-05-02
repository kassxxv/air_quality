# Air Quality Monitor — Filip's Work Summary

## What Was Built

### Backend — `web/main.py` (FastAPI, Python 3.11)
- `POST /api/data` — receives sensor readings from ESP32 every 5s (co2, pm25, temp, hum, pmsTemp, pmsHum), timestamps on server, stores last 500 readings in memory
- `GET /api/commands` — returns thresholds + buzzer state to ESP32 every 2s; LED is one-shot (sent once, then cleared)
- `GET /api/status` — live data + history for dashboard
- `GET /api/history` — full 500-point history for History tab charts
- `GET /api/daily` — daily averages for calendar (air score per day)
- `POST /api/control` — dashboard sends LED/buzzer overrides and threshold changes
- `GET /` — serves the dashboard

### Frontend — `web/static/index.html` (vanilla JS + Chart.js)
Three-tab SPA, warm cream design:
- **Overview** — Air Score badge, CO₂ + PM2.5 with sparklines, Temperature + Humidity cards, gradient range bars
- **History** — monthly calendar (colored by daily score), 4 stacked line charts (CO₂/PM2.5/Temp/Humidity) with time filters (30s/1m/2m/All), Time of Day bar chart
- **Controls** — Red LED override (AUTO/ON/OFF), Buzzer force toggle, CO₂ + PM2.5 alarm threshold sliders, Device Status table, Command Log

### Cloud — Azure App Service
- Python 3.11, Linux, Free tier
- Auto-deploys from GitHub `main` branch via GitHub Actions
- Startup command: `gunicorn -w 2 -k uvicorn.workers.UvicornWorker main:app`
- Live URL already set in `air_quality.ino`

---

## What Partner Needs to Do

### 1. Set WiFi credentials in `air_quality.ino`
```cpp
#define WIFI_SSID  "your_wifi_name"
#define WIFI_PASS  "your_wifi_password"
```

### 2. Install Arduino libraries (Arduino IDE → Library Manager)
- ArduinoJson — Benoit Blanchon
- SensirionI2cScd4x
- Adafruit SSD1306
- Adafruit GFX

### 3. Flash `air_quality.ino` to ESP32
Board: ESP32 Dev Module, Port: whichever COM port the ESP32 shows up on.

Once flashed — device connects to WiFi, data appears on the dashboard automatically.
