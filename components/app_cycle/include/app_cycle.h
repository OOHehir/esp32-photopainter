/* app_cycle -- one wake: connect, fetch, draw, maybe update, sleep.
 *
 * The device is built for a battery, so it spends nearly all its time in deep sleep and
 * every boot is a fresh start from app_main(). State that must survive a sleep lives in
 * RTC memory (wake count, last update check) or NVS (calendars, cache, frame hash).
 *
 * Wake sources: the RTC timer on the next half hour (CONFIG_APP_CYCLE_INTERVAL_MIN),
 * skipping the quiet hours, and the KEY button. A short press forces a redraw; holding
 * it for 3 s keeps the device awake for CONFIG_APP_CYCLE_AWAKE_WINDOW_S with its web
 * page up, for setup, an update or a reflash over USB.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the wake cycle. Never returns: ends in deep sleep, or on USB power with
 *        CONFIG_APP_CYCLE_SLEEP_ON_USB=n, loops forever awake.
 */
void app_cycle_run(void);

#ifdef __cplusplus
}
#endif
