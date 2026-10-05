// Pushes local relay changes (schedule / button / web) into the Matter OnOff attribute
// so Google Home updates immediately, without polling.
#pragma once
#include <stdint.h>
#include <stdbool.h>

void matter_sync_init(uint16_t endpoint_id);   // before esp_matter::start()
void matter_sync_start(void);                  // after esp_matter::start()
// Any task, never blocks. Safe before matter_sync_start() (ignored; start() syncs).
void matter_sync_notify_relay(bool on);
// True while matter_sync itself writes the attribute. The attribute-update
// callback MUST ignore such writes, otherwise a schedule change would be
// turned into a FORCE_ON/FORCE_OFF override.
bool matter_sync_is_internal_update(void);
// Re-sends the current OnOff value to subscribers (esp_matter::attribute::report).
void matter_sync_force_report(void);
