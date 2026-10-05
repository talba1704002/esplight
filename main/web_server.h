// Asynchronous web portal (esp_http_server runs in its own task, independent
// of the Matter task and the scheduler task) + JSON API:
//   GET  /              embedded HTML portal
//   GET  /api/status    state, RTC time, schedule list
//   POST /api/cmd       c=ON|OFF|AUTO
//   POST /api/schedule  list="onH,onM,offH,offM,en;..."
//   POST /api/sync      y,mo,d,h,mi,s
//   GET/POST /api/adv   advanced Wi-Fi / button settings
#pragma once
#include "esp_err.h"
esp_err_t web_server_start(void);
