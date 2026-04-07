#pragma once

// ============================================================
//  relay.h — Relay output management
// ============================================================

#include <stdbool.h>
#include "esp_err.h"

// Relay identifiers  (display order: Heater first, then Humidifier)
typedef enum {
    RELAY_HEATER = 0,
    RELAY_HUMIDIFIER,
    RELAY_FAN,
    RELAY_PANEL_FAN,    // Cabinet cooling fan (overtemp protection)
    RELAY_LIGHT,
    RELAY_COUNT
} relay_id_t;

typedef struct {
    bool  state;          // true = ON
    bool  locked_off;     // safety interlock — relay cannot turn ON
    char  name[16];
} relay_status_t;

esp_err_t relay_init(void);
esp_err_t relay_set(relay_id_t relay, bool on);
bool      relay_get(relay_id_t relay);
void      relay_lock_off(relay_id_t relay, bool lock);
bool      relay_is_locked(relay_id_t relay);
void      relay_all_off(void);
void      relay_get_all_status(relay_status_t out[RELAY_COUNT]);
