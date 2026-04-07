#pragma once

// ============================================================
//  web_server.h — HTTP server + browser dashboard
//  Endpoints:
//    GET  /            → HTML dashboard (auto-refreshes)
//    GET  /api/status  → JSON: sensor readings + relay states
//    POST /api/setpoints → JSON body: update setpoints
//    POST /api/relay   → JSON body: manual relay toggle
// ============================================================

#include "esp_err.h"

esp_err_t web_server_start(void);
void      web_server_stop(void);
