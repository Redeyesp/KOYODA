KOYODA Wi-Fi Clean Step 1 — GitHub Ready
=======================================

BASE
----
Confirmed-working KOYODA Tap Wake Happy.

UPLOAD / REPLACE
----------------
main/main.c
main/CMakeLists.txt
main/koyoda_wifi.c
main/koyoda_wifi.h
.github/workflows/build-koyoda.yml

IMPORTANT
---------
Do NOT restore any older Wi-Fi UI/status files.

This patch intentionally contains:
- NO LVGL calls in koyoda_wifi.c
- NO face_img access
- NO Battery page access
- NO swipe/current_page access
- NO on-screen Wi-Fi status
- NO wifi-preserved.sha256

Wi-Fi starts about 1.5 seconds after KOYODA UI is ready, in a priority-1
background task.

Repository Secrets expected:
- KOYODA_WIFI_SSID
- KOYODA_WIFI_PASSWORD

EXPECTED SERIAL LOG
-------------------
KOYODA_WIFI: Startup scheduled; UI untouched
KOYODA_WIFI: Station started; connecting...
KOYODA_WIFI: Connected; IP=...
KOYODA_WIFI: SSID=... RSSI=... dBm

CHECK AFTER FLASH
-----------------
1. Face blinks normally.
2. Face <-> Battery swipe still works.
3. No Face/Battery overlay.
4. Tap wake still performs Closed -> Half -> Idle -> Happy -> Idle.
5. Power-off dialog still works.
6. Wi-Fi is confirmed only from Serial Monitor for this step.
