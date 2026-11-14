/* cal_value -- readers for iCalendar scalar values (RFC 5545 section 3.3). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Read a DATE ("20261114") or DATE-TIME ("20261114T090000", optional "Z").
 * @param s              Value; need not be NUL-terminated.
 * @param len            Bytes in s; trailing bytes other than the value are an error.
 * @param[out] wall      Wall-clock seconds since 1970-01-01T00:00; midnight for a DATE.
 * @param[out] is_date   True for a DATE.
 * @param[out] is_utc    True when the value ends in "Z".
 * @return true on success; outputs are unspecified otherwise.
 */
bool cal_value_datetime(const char *s, size_t len, int64_t *wall, bool *is_date,
                        bool *is_utc);

/**
 * @brief Read a DURATION such as "P1D", "PT1H30M", "P1W" or "-PT15M".
 *
 * Weeks and days are nominal (calendar days, so a day across a DST change is 23 or 25
 * hours); hours, minutes and seconds are exact. RFC 5545 section 3.3.6.
 *
 * @param s           NUL-terminated value.
 * @param[out] days   Nominal days, weeks included; negative for a negative duration.
 * @param[out] secs   Exact seconds; negative for a negative duration.
 * @return true on success.
 */
bool cal_value_duration(const char *s, int32_t *days, int32_t *secs);

/**
 * @brief Read a UTC-OFFSET such as "+0100", "-0500" or "+053000".
 * @param s          NUL-terminated value.
 * @param[out] secs  Seconds east of UTC.
 * @return true on success.
 */
bool cal_value_utc_offset(const char *s, int32_t *secs);
