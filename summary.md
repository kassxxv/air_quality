ESP32 Air Quality Monitor — Handoff Summary

HARDWARE
- ESP32 on screw-terminal expansion board
- SCD40 (I2C, 0x62) → GPIO21 (SDA), GPIO22 (SCL), 3.3V
- PMS5003T (UART) → GPIO16 (RX), GPIO17 (TX), 5V
- SSD1306 OLED 128x64 (I2C, 0x3C) → same bus as SCD40
- LEDs: Green=GPIO18, Yellow=GPIO19, Red=GPIO23
- Passive buzzer → GPIO15 (uses tone())

LIBRARIES
- ArduinoJson (Benoit Blanchon)
- SensirionI2cScd4x
- Adafruit SSD1306 + Adafruit GFX

DATA
- Reads CO2 (ppm), PM2.5 (ug/m3), weighted avg temp (SCD40×0.6 + PMS×0.4), weighted avg hum
- POSTs JSON every 5s to AZURE_POST: { co2, pm25, temp, hum, pmsTemp, pmsHum }
- GETs commands every 2s from AZURE_GET, expects: { led, buzzer, co2_threshold, pm25_threshold }

LOGIC
- Green LED: CO2 < 800 AND PM2.5 < 35
- Yellow LED: between thresholds
- Red LED + buzzer (1000Hz tone): CO2 ≥ 1000 OR PM2.5 ≥ 55
- All thresholds overridable from backend
- Red LED and buzzer also manually triggerable via commands endpoint
- WiFi failure is non-blocking, runs offline

CLOUD COMMUNICATION — PURPOSE
The device is a dumb sensor + actuator. It pushes readings to the cloud and pulls
operational commands (override LED, override buzzer, change alarm thresholds).
The backend owns: persistence, history, dashboards, alerting, and any logic that
needs more state than a 5-second loop. The device owns: sensor I/O, immediate
local automation (LED/buzzer based on thresholds), and OLED display.

TRANSPORT
- Plain HTTP/HTTPS over WiFi (depends on AZURE_POST / AZURE_GET URL scheme)
- WiFi credentials hardcoded in WIFI_SSID / WIFI_PASS macros (must be set before flashing)
- connectWiFi() in setup(): up to 20 retries at 500ms intervals (~10s budget), then gives up
- If WiFi drops mid-session, loop() retries WiFi.begin() every 5 minutes (WIFI_RETRY_INTERVAL)
- No TLS pinning, no client cert, no auth header on either endpoint (add at backend if needed)
- No device ID is sent — backend currently cannot distinguish multiple devices

UPLINK — POST {AZURE_POST}
Purpose: push the latest sensor reading. Fire-and-forget telemetry.

  Cadence:    every 5000ms (SEND_INTERVAL), driven by loop()
  Method:     HTTP POST
  Headers:    Content-Type: application/json
  Gating:     only fires when WiFi.status() == WL_CONNECTED
  Retries:    none (lost packets are lost; no on-device queue)
  Response:   status code is logged to Serial as "[POST] <code>" but otherwise ignored

  Body schema (all fields always present):
    {
      "co2":     <uint16, ppm>,           // SCD40 CO2 reading; 0 means "no reading yet"
      "pm25":    <uint16, ug/m3>,         // PMS5003T PM2.5 mass concentration
      "temp":    <float, degC, 1 decimal>,// weighted average: 0.6*SCD40 + 0.4*PMS (or SCD-only if PMS not yet reported)
      "hum":     <float, %RH, 1 decimal>, // weighted average, same formula
      "pmsTemp": <float, degC, 1 decimal>,// raw PMS5003T temperature, included for diagnostics
      "pmsHum":  <float, %RH, 1 decimal>  // raw PMS5003T humidity, included for diagnostics
    }

  Example:
    {"co2":612,"pm25":18,"temp":22.7,"hum":48.3,"pmsTemp":23.1,"pmsHum":47.5}

  Notes for backend:
  - The SCD40 raw temp/hum are NOT sent separately — they are folded into temp/hum.
    If you want both raw streams later, add scdTemp/scdHum to the payload.
  - During the first ~10s after boot, co2 may be 0 and pmsTemp may be 0 (sensors warming up).
    Treat 0 as "no data yet" rather than "actually zero".
  - Backend should timestamp on receipt; the device does not send a timestamp.

DOWNLINK — GET {AZURE_GET}
Purpose: backend pushes commands and threshold overrides to the device.

  Cadence:    every 2000ms (COMMAND_INTERVAL), driven by loop()
  Method:     HTTP GET (no body)
  Gating:     only fires when WiFi.status() == WL_CONNECTED
  Accepted:   only HTTP 200 is parsed; other codes are silently dropped
  Headers:    none (no auth)

  Expected response body — JSON object, all fields OPTIONAL:
    {
      "led":            0 | 1,    // force red LED off (0) or on (1) — overrides automation until next command
      "buzzer":         0 | 1,    // 0 = follow normal beep state machine; 1 = force constant 1000Hz tone
      "co2_threshold":  <int, ppm>,    // alarm trips when co2 >= this value (default 1000)
      "pm25_threshold": <int, ug/m3>   // alarm trips when pm25 >= this value (default 55)
    }

  Field semantics:
  - Only fields present in the response are applied (uses ArduinoJson containsKey).
    Omit fields you don't want to change.
  - "led": 1 forces the red LED on by writing it directly. The LED automation in
    handleAutomation() will overwrite this on the next loop iteration unless the
    underlying alarm condition is also active. (Practical behavior: the LED flickers
    unless you actually want a manual override that lasts; consider this a known quirk.)
  - "buzzer": 1 produces a steady tone, bypassing the burst/cooldown state machine.
    "buzzer": 0 returns control to the state machine (which only beeps if alarm is active).
  - Threshold changes take effect immediately on the next loop iteration.

  Examples:
    Just nudge the CO2 threshold:        {"co2_threshold": 800}
    Silence everything:                  {"buzzer": 0, "led": 0}
    Manual alarm test:                   {"led": 1, "buzzer": 1}
    No-op (heartbeat):                   {}

OFFLINE BEHAVIOR
- postData() and fetchCommands() both silently no-op when WiFi is down
- Sensor reads, OLED, LED automation, and buzzer state machine continue running normally
- No on-device buffering: readings produced during an outage are lost
- Last-known thresholds and last-known manual overrides persist in RAM across the outage,
  but are reset to defaults on power cycle / reboot

THINGS THE BACKEND SHOULD PROBABLY DO
- Always respond to POSTs with 2xx (the device doesn't care about the body, only the code)
- For GET, return {} when there are no commands to send — easier than 204
- Add a device identifier scheme (header, query param, or body field) before deploying >1 device
- Consider rate-limiting: the device WILL hit POST every 5s and GET every 2s, forever
- Decide whether you want HTTPS — if so, the device may need a CA bundle update
