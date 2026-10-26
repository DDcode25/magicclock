#pragma once

// HTTP API compatible with the stock firmware and its web UI:
//   /                       embedded stock UI (gzip)
//   /set?...                settings and actions (answers "OK")
//   /v.json /space.json /wifi.json /wifisave?s=&p=
//   /filelist?dir= /doUpload?dir= (POST, field "update") /delete?file=
//   /update                 OTA (GET page, POST field "firmware")
//   /flash?offset=&size=    raw flash dump (backup)
//   anything else           static file from LittleFS (/.sys/*.json, /image/...)
namespace web {

void begin();
void loop();

}  // namespace web
