# Host tests. No ESP-IDF, no toolchain, no hardware:
#
#     make -C test/host
#
# Only units that are pure C and whose bugs are silent on a device live here. The Wi-Fi
# state machine and the NVS wrappers are deliberately absent -- testing them would mean
# mocking most of ESP-IDF, and the mocks would be what got tested.

# Where cJSON comes from. The default is the copy an `idf.py build` leaves behind, which
# is what a developer who has built the firmware already has -- but that directory is
# gitignored, so a fresh clone does not have it & this Makefile cannot conjure it. Then
# tools/run_tests.sh fetches a pinned copy & passes the path in. Run make directly in a
# tree that has neither & the error below says so rather than reporting a missing rule.
CJSON_DIR ?= ../../managed_components/espressif__cjson/cJSON

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
CPPFLAGS = -I. -I../../components/app_httpd/priv_include \
           -I../../components/app_ota/priv_include \
           -I../../components/app_gfx/include \
           -I$(CJSON_DIR) \
           -I../../components/app_ui/include \
           -I../../components/app_cycle/priv_include
LDFLAGS  = -fsanitize=address,undefined

UNITS = ../../components/app_httpd/json_util.c \
        ../../components/app_httpd/dns_pack.c \
        ../../components/app_ota/update_policy.c \
        ../../components/app_ota/rollback_policy.c \
        ../../components/app_ota/sha_hex.c \
        ../../components/app_gfx/app_gfx.c \
        $(wildcard ../../components/app_gfx/font_*.c) \
        ../../components/app_cycle/cycle_sched.c \
        $(CJSON_DIR)/cJSON.c \
        ../../components/app_ui/app_ui.c
TESTS = main.c test_json_util.c test_dns_pack.c test_update_policy.c \
        test_rollback_policy.c test_sha_hex.c \
        test_app_gfx.c test_app_ui.c test_cycle_sched.c

# app_cal: the iCalendar parser and recurrence expander. Its fixtures are read at run
# time from fixtures/*.ics, relative to this directory.
CPPFLAGS += -I../../components/app_cal/include -I../../components/app_cal/priv_include
UNITS += ../../components/app_cal/cal_tz.c ../../components/app_cal/cal_value.c \
         ../../components/app_cal/cal_rrule.c ../../components/app_cal/cal_parse.c
TESTS += test_cal_tz.c test_cal_parse.c test_cal_rrule.c
CAL_HEADERS = cal_test_util.h $(wildcard ../../components/app_cal/include/*.h) \
              $(wildcard ../../components/app_cal/priv_include/*.h)

BIN = run_tests

.PHONY: all check clean preview
all: check

check: $(BIN)
	./$(BIN)

$(BIN): $(UNITS) $(TESTS) test.h $(CAL_HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(UNITS) $(TESTS) $(LDFLAGS)

$(CJSON_DIR)/cJSON.c:
	@echo "error: no cJSON at $(CJSON_DIR)" >&2
	@echo "       Run ./tools/run_tests.sh, which fetches a pinned copy, or build the" >&2
	@echo "       firmware once so the component manager writes managed_components/." >&2
	@false

# Renders the week, stale, setup and no-calendar screens to PPM files in
# $(PREVIEW_DIR) so the layout can be looked at. See the comment in render_ui.c.
PREVIEW_DIR ?= /tmp
PREVIEW_UNITS = ../../components/app_ui/app_ui.c ../../components/app_gfx/app_gfx.c \
                $(wildcard ../../components/app_gfx/font_*.c)

preview: render_ui.c $(PREVIEW_UNITS)
	$(CC) -std=c11 -Wall -O1 $(CPPFLAGS) -o $(PREVIEW_DIR)/cal-render $^
	$(PREVIEW_DIR)/cal-render $(PREVIEW_DIR)

clean:
	rm -rf $(BIN) $(BIN).dSYM
