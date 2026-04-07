#pragma once
// ============================================================
//  notifier.h — ntfy.sh push notification interface
// ============================================================

#include <stdbool.h>
#include <stddef.h>

#define NOTIFIER_TOPIC_MAX 64   // max ntfy topic string length incl. null

// Call once from app_main after NVS is available.
void notifier_init(void);

// Get/set the ntfy topic (persisted to NVS).
// notifier_get_topic() copies into buf (up to len bytes incl. null).
void notifier_get_topic(char *buf, size_t len);
void notifier_set_topic(const char *topic);

// Send asynchronously — spawns a one-shot task, returns immediately.
// Safe to call from any task. No-op if topic is not configured.
void notifier_send(const char *title, const char *message);

// Send synchronously — blocks until the POST completes or times out.
// Returns the HTTP status code (e.g. 200), or -1 on error.
// Only use from a task with adequate stack (>=16KB); the test handler uses this.
int notifier_send_sync(const char *title, const char *message);
