# Релізи UA Smart Box

| Файл | Пристрій | Як ставити |
|---|---|---|
| `UASmartBox-1.0.4.bin` | GeekMagic SmallTV Pro (ESP32) | `http://<IP>/update` або заводська сторінка оновлення |
| `UASmartBox-SD-1.0.0.bin` | JUZIPi SD PRO (ESP8266) | `http://<IP>/update` (з UA Smart Box SD або із завантажувача) |
| `SDP_UASmartBox-SD-loader.bin` | JUZIPi SD PRO, перший крок із заводської прошивки | заводська сторінка «Firmware Update (OTA)», потім `UASmartBox-SD-*.bin` |

Не плутайте файли. UA Smart Box SD відхиляє прошивку для ESP32, SmallTV Pro відхиляє прошивку для
ESP8266, але завантажувач (`SDP_…-loader.bin`) і заводські прошивки таких перевірок не мають: чужий
файл, залитий через них, зробить пристрій непрацездатним.
Подробиці кожної версії — у [`../CHANGELOG.md`](../CHANGELOG.md).
