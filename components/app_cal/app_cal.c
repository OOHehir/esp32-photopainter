#include "app_cal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_trust.h"
#include "cal_parse.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "miniz.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "app_cal";

#define NVS_NS         "app_cal"
#define NVS_KEY_COUNT  "n"
#define NVS_KEY_CACHE  "cache"
#define NVS_KEY_CACHE_T "cache_t"
#define CACHE_VERSION  2
#define READ_CHUNK     4096
#define MAX_REDIRECTS  3

typedef struct {
    uint32_t version;
    uint32_t count;
    int64_t  fetched_utc;
    uint8_t  sources_hash[8];   /* invalidates the cache when URLs change */
} cache_header_t;

/* The list is edited from the httpd task while the main task fetches; s_lock guards
 * s_src, s_count and s_gen, and a fetch works on a snapshot taken under it. */
static SemaphoreHandle_t s_lock;
static app_cal_source_t s_src[CAL_MAX_CALENDARS];
static size_t s_count;
static uint32_t s_gen;          /* bumped on every edit; a fetch that straddles one is not cached */
static app_cal_source_t s_snap[CAL_MAX_CALENDARS];
/* Every calendar's events, merged before the cap: capping per calendar in list order
 * would drop calendar 1's today in favor of calendar 0's next week. */
#define MERGE_MAX (APP_CAL_MAX_EVENTS * CAL_MAX_CALENDARS)
static cal_event_t *s_events;   /* MERGE_MAX, in PSRAM */
static cal_event_t *s_scratch;  /* one calendar's parse output */

static const struct { const char *name; uint8_t code; } COLOURS[] = {
    { "black", 0 }, { "yellow", 2 }, { "red", 3 }, { "blue", 5 }, { "green", 6 },
};

int app_cal_colour_from_name(const char *name)
{
    for (size_t i = 0; i < sizeof(COLOURS) / sizeof(COLOURS[0]); i++) {
        if (name && strcmp(name, COLOURS[i].name) == 0) return COLOURS[i].code;
    }
    return -1;
}

const char *app_cal_colour_name(uint8_t colour)
{
    for (size_t i = 0; i < sizeof(COLOURS) / sizeof(COLOURS[0]); i++) {
        if (COLOURS[i].code == colour) return COLOURS[i].name;
    }
    return "?";
}

void app_cal_url_host(const char *url, char *out, size_t out_len)
{
    out[0] = '\0';
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    size_t n = strcspn(p, "/:?#");
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* No plain http://: the URL is a credential and would cross the network in clear.
 * webcal:// is fetched as https://. */
static bool url_ok(const char *url)
{
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "webcal://", 9) == 0;
}

/* FNV-1a over every URL. Only has to notice a change, not resist one. */
static void sources_hash(const app_cal_source_t *src, size_t count, uint8_t out[8])
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < count; i++) {
        for (const char *p = src[i].url; *p; p++) {
            h = (h ^ (uint8_t)*p) * 1099511628211ULL;
        }
        h = (h ^ 0xFF) * 1099511628211ULL;
    }
    memcpy(out, &h, 8);
}

/* Caller holds s_lock. */
static esp_err_t save_sources(void)
{
    s_gen++;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_COUNT, (uint8_t)s_count);
    for (size_t i = 0; err == ESP_OK && i < s_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "src%u", (unsigned)i);
        err = nvs_set_blob(h, key, &s_src[i], sizeof(s_src[i]));
    }
    /* The cache belongs to the old list; drop it rather than show one
     * calendar's events under another's name and color. */
    if (err == ESP_OK) {
        esp_err_t e = nvs_erase_key(h, NVS_KEY_CACHE);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) err = e;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* CONFIG_APP_CAL_SEED is whitespace-separated `name,color,url` triples.
 * Google's secret URLs contain neither commas nor whitespace. */
static void seed_from_kconfig(void)
{
    char *seed = strdup(CONFIG_APP_CAL_SEED);
    if (!seed) return;
    char *save = NULL;
    for (char *tok = strtok_r(seed, " \t\r\n", &save); tok && s_count < CAL_MAX_CALENDARS;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        char *c1 = strchr(tok, ',');
        char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
        if (!c2) {
            ESP_LOGW(TAG, "CONFIG_APP_CAL_SEED entry without name,color,url -- skipped");
            continue;
        }
        *c1 = *c2 = '\0';
        int col = app_cal_colour_from_name(c1 + 1);
        if (col < 0 || !url_ok(c2 + 1) || strlen(c2 + 1) >= APP_CAL_URL_MAX) {
            ESP_LOGW(TAG, "CONFIG_APP_CAL_SEED entry \"%s\" invalid -- skipped", tok);
            continue;
        }
        app_cal_source_t *s = &s_src[s_count++];
        snprintf(s->name, sizeof(s->name), "%s", tok);
        s->colour = (uint8_t)col;
        snprintf(s->url, sizeof(s->url), "%s", c2 + 1);
    }
    free(seed);
    if (s_count) {
        ESP_LOGI(TAG, "seeded %u calendar(s) from CONFIG_APP_CAL_SEED", (unsigned)s_count);
        save_sources();
    }
}

esp_err_t app_cal_init(void)
{
    if (!s_events) {
        s_lock = xSemaphoreCreateMutex();
        s_events = heap_caps_calloc(MERGE_MAX, sizeof(cal_event_t), MALLOC_CAP_SPIRAM);
        s_scratch = heap_caps_calloc(APP_CAL_MAX_EVENTS, sizeof(cal_event_t), MALLOC_CAP_SPIRAM);
        if (!s_lock || !s_events || !s_scratch) return ESP_ERR_NO_MEM;
    }
    s_count = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    bool stored = false;
    if (err == ESP_OK) {
        uint8_t n = 0;
        if (nvs_get_u8(h, NVS_KEY_COUNT, &n) == ESP_OK) {
            stored = true;
            for (size_t i = 0; i < n && i < CAL_MAX_CALENDARS; i++) {
                char key[16];
                size_t len = sizeof(s_src[0]);
                snprintf(key, sizeof(key), "src%u", (unsigned)i);
                if (nvs_get_blob(h, key, &s_src[s_count], &len) == ESP_OK &&
                    len == sizeof(s_src[0])) {
                    s_src[s_count].name[APP_CAL_NAME_MAX - 1] = '\0';
                    s_src[s_count].url[APP_CAL_URL_MAX - 1] = '\0';
                    s_count++;
                }
            }
        }
        nvs_close(h);
    }
    /* A stored count of zero is a list the user emptied, not a first boot: seeding
     * again would bring back a calendar they removed. */
    if (stored) {
        ESP_LOGI(TAG, "%u calendar(s) configured", (unsigned)s_count);
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    seed_from_kconfig();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

size_t app_cal_sources(app_cal_source_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_count;
    memcpy(out, s_src, sizeof(s_src[0]) * n);
    xSemaphoreGive(s_lock);
    return n;
}

static esp_err_t set_source_locked(size_t idx, const char *name, uint8_t colour,
                                   const char *url);

esp_err_t app_cal_set_source(size_t idx, const char *name, uint8_t colour, const char *url)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = set_source_locked(idx, name, colour, url);
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t set_source_locked(size_t idx, const char *name, uint8_t colour,
                                   const char *url)
{
    if (idx > s_count || idx >= CAL_MAX_CALENDARS || !url) return ESP_ERR_INVALID_ARG;
    if (url[0] == '\0') {
        if (idx == s_count) return ESP_ERR_INVALID_ARG;
        memmove(&s_src[idx], &s_src[idx + 1], sizeof(s_src[0]) * (s_count - idx - 1));
        s_count--;
        return save_sources();
    }
    if (!url_ok(url) || strlen(url) >= APP_CAL_URL_MAX || !name || !name[0] ||
        strlen(name) >= APP_CAL_NAME_MAX || app_cal_colour_name(colour)[0] == '?') {
        return ESP_ERR_INVALID_ARG;
    }
    app_cal_source_t *s = &s_src[idx];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->colour = colour;
    snprintf(s->url, sizeof(s->url), "%s", url);
    if (idx == s_count) s_count++;
    return save_sources();
}

/* --------------------------------------------------------------------- fetching */

/* Streaming gunzip in front of the parser. Google's iCal endpoint honors
 * Accept-Encoding: gzip, which cuts a feed about ninefold (71 396 -> 7 660 bytes on a
 * public Google calendar); it offers no date filter, ETag or Last-Modified, so the whole
 * feed comes down on every wake and size is the only thing left to save. The inflater is
 * miniz's, in the ESP32-S3 ROM; its 32 KB output buffer doubles as the LZ77 window. */
typedef struct {
    tinfl_decompressor inf;
    uint8_t  dict[TINFL_LZ_DICT_SIZE];
    size_t   dict_ofs;
    uint8_t  hdr[10];
    size_t   hdr_n;
    uint8_t  flags;
    int      skip_state;   /* 0 header, 1 extra-len, 2 extra, 3 name, 4 comment, 5 hcrc, 6 body, 7 done */
    uint32_t skip_left;
} gunzip_t;

static gunzip_t *gunzip_new(void)
{
    gunzip_t *g = heap_caps_calloc(1, sizeof(*g), MALLOC_CAP_SPIRAM);
    if (g) tinfl_init(&g->inf);
    return g;
}

/* Advance through the gzip header (RFC 1952 section 2.3). Returns bytes
 * consumed; sets *err for a stream that is not gzip/deflate. */
static size_t gunzip_header(gunzip_t *g, const uint8_t *in, size_t len, esp_err_t *err)
{
    size_t used = 0;
    while (used < len && g->skip_state < 6) {
        uint8_t c = in[used++];
        switch (g->skip_state) {
        case 0:
            g->hdr[g->hdr_n++] = c;
            if (g->hdr_n == 10) {
                if (g->hdr[0] != 0x1F || g->hdr[1] != 0x8B || g->hdr[2] != 8) {
                    *err = ESP_ERR_INVALID_RESPONSE;
                    return used;
                }
                g->flags = g->hdr[3];
                g->skip_state = (g->flags & 0x04) ? 1 : (g->flags & 0x08) ? 3
                              : (g->flags & 0x10) ? 4 : (g->flags & 0x02) ? 5 : 6;
                g->skip_left = (g->skip_state == 1) ? 2 : (g->skip_state == 5) ? 2 : 0;
                g->hdr_n = 0;
            }
            break;
        case 1:
            g->hdr[g->hdr_n++] = c;
            if (g->hdr_n == 2) {
                g->skip_left = g->hdr[0] | (g->hdr[1] << 8);
                g->skip_state = g->skip_left ? 2 : 3;
            }
            break;
        case 2:
            if (--g->skip_left == 0) g->skip_state = 3;
            break;
        case 3:
            if (!(g->flags & 0x08) || c == 0) {
                if (!(g->flags & 0x08)) used--;
                g->skip_state = 4;
            }
            break;
        case 4:
            if (!(g->flags & 0x10) || c == 0) {
                if (!(g->flags & 0x10)) used--;
                g->skip_state = 5;
                g->skip_left = 2;
            }
            break;
        case 5:
            if (!(g->flags & 0x02)) { used--; g->skip_state = 6; break; }
            if (--g->skip_left == 0) g->skip_state = 6;
            break;
        }
    }
    return used;
}

/* The first bytes of the decoded body, to check it is iCalendar. */
typedef struct {
    char   head[24];
    size_t n;
} sniff_t;

static void sniff_add(sniff_t *s, const char *d, size_t len)
{
    while (len-- && s->n < sizeof(s->head) - 1) s->head[s->n++] = *d++;
    s->head[s->n] = '\0';
}

static bool sniff_is_ical(const sniff_t *s)
{
    const char *h = s->head;
    if (strncmp(h, "\xEF\xBB\xBF", 3) == 0) h += 3;   /* UTF-8 BOM */
    return strncmp(h, "BEGIN:VCALENDAR", 15) == 0;
}

/* Inflate `len` compressed bytes and feed the output to the parser. */
static esp_err_t gunzip_feed(gunzip_t *g, cal_parser_t *p, sniff_t *sniff, const uint8_t *in,
                             size_t len, size_t *plain)
{
    esp_err_t err = ESP_OK;
    size_t used = gunzip_header(g, in, len, &err);
    in += used;
    len -= used;
    while (err == ESP_OK && g->skip_state == 6 && (len > 0)) {
        size_t in_n = len, out_n = TINFL_LZ_DICT_SIZE - g->dict_ofs;
        tinfl_status st = tinfl_decompress(&g->inf, in, &in_n, g->dict, g->dict + g->dict_ofs,
                                           &out_n, TINFL_FLAG_HAS_MORE_INPUT);
        in += in_n;
        len -= in_n;
        sniff_add(sniff, (const char *)g->dict + g->dict_ofs, out_n);
        if (out_n && cal_parser_feed(p, (const char *)g->dict + g->dict_ofs, out_n) != 0) {
            return ESP_ERR_NO_MEM;
        }
        *plain += out_n;
        g->dict_ofs = (g->dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) {
            g->skip_state = 7;   /* CRC32 and length trailer follow; TLS already guards integrity */
        } else if (st < 0) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else if (in_n == 0 && out_n == 0) {
            break;
        }
    }
    return err;
}

/* Streams one calendar through the parser; the body is never held whole,
 * because a Google feed carries every event since the calendar was created and runs to
 * megabytes. */
static esp_err_t fetch_one(size_t idx, const cal_window_t *win, size_t *n_out)
{
    char url[APP_CAL_URL_MAX + 8];
    const char *u = s_snap[idx].url;
    if (strncmp(u, "webcal://", 9) == 0) {
        snprintf(url, sizeof(url), "https://%s", u + 9);
    } else {
        snprintf(url, sizeof(url), "%s", u);
    }
    char host[64];
    app_cal_url_host(url, host, sizeof(host));

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CONFIG_APP_CAL_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = app_trust_public_bundle_attach(),
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");

    esp_err_t err = ESP_OK;
    int status = 0;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) break;
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status >= 300 && status < 400 && hop < MAX_REDIRECTS) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        break;
    }
    if (err == ESP_OK && status != 200) {
        ESP_LOGW(TAG, "calendar %u (%s): HTTP %d", (unsigned)idx, host, status);
        err = ESP_ERR_INVALID_RESPONSE;
    }

    cal_parser_t *p = NULL;
    char *buf = NULL;
    gunzip_t *gz = NULL;
    size_t total = 0, plain = 0;
    sniff_t sniff = { 0 };
    if (err == ESP_OK) {
        p = cal_parser_new(win);
        buf = malloc(READ_CHUNK);
        if (!p || !buf) err = ESP_ERR_NO_MEM;
    }
    while (err == ESP_OK) {
        int r = esp_http_client_read(client, buf, READ_CHUNK);
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            if (!esp_http_client_is_complete_data_received(client)) {
                /* A truncated feed would silently drop the events after
                 * the cut, which reads as "nothing on" rather than as a failure. */
                ESP_LOGW(TAG, "calendar %u (%s): body ended early after %u bytes",
                         (unsigned)idx, host, (unsigned)total);
                err = ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        /* Sniffed from the magic bytes rather than Content-Encoding:
         * esp_http_client_get_header() reads request headers, not the response's. */
        if (total == 0 && r >= 2 && (uint8_t)buf[0] == 0x1F && (uint8_t)buf[1] == 0x8B) {
            gz = gunzip_new();
            if (!gz) { err = ESP_ERR_NO_MEM; break; }
        }
        total += (size_t)r;
        if (gz) {
            err = gunzip_feed(gz, p, &sniff, (const uint8_t *)buf, (size_t)r, &plain);
        } else {
            plain += (size_t)r;
            sniff_add(&sniff, buf, (size_t)r);
            if (cal_parser_feed(p, buf, (size_t)r) != 0) err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK && gz && gz->skip_state != 7) {
        ESP_LOGW(TAG, "calendar %u (%s): gzip stream ended early", (unsigned)idx, host);
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && !sniff_is_ical(&sniff)) {
        /* An HTML error page or an undecoded body parses to zero events,
         * which would otherwise be drawn -- and cached -- as an empty week. */
        ESP_LOGW(TAG, "calendar %u (%s): body is not iCalendar", (unsigned)idx, host);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        cal_stats_t st;
        size_t n = 0;
        if (cal_parser_finish(p, s_scratch, APP_CAL_MAX_EVENTS, &n, &st) != 0) {
            err = ESP_ERR_NO_MEM;
        } else {
            *n_out = n;
            ESP_LOGI(TAG, "calendar %u (%s): %u bytes (%u %s), %u VEVENTs, %u in window"
                     " (private hidden %u, with COLOR %u, unsupported rules %u,"
                     " unknown zones %u, dropped %u)",
                     (unsigned)idx, host, (unsigned)plain, (unsigned)total,
                     gz ? "gzipped" : "on the wire", (unsigned)st.vevents, (unsigned)n,
                     (unsigned)st.hidden_private, (unsigned)st.with_colour,
                     (unsigned)st.unsupported_rrule, (unsigned)st.tz_fallback,
                     (unsigned)st.dropped_full);
        }
    } else {
        ESP_LOGW(TAG, "calendar %u (%s): %s", (unsigned)idx, host, esp_err_to_name(err));
    }
    if (p) cal_parser_free(p);
    free(buf);
    heap_caps_free(gz);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static int cmp_event(const void *a, const void *b)
{
    const cal_event_t *x = a, *y = b;
    if (x->start_day != y->start_day) return x->start_day < y->start_day ? -1 : 1;
    if (x->all_day != y->all_day) return x->all_day ? -1 : 1;
    if (x->start_min != y->start_min) return x->start_min < y->start_min ? -1 : 1;
    if (x->calendar != y->calendar) return x->calendar < y->calendar ? -1 : 1;
    return strcmp(x->title, y->title);
}

/* Reads the cached events for the calendars in `mask` that overlap the
 * window into s_events[*count...]. */
static bool cache_load(const uint8_t sh[8], uint8_t mask, int32_t first_day, int32_t last_day,
                       size_t *count, int64_t *fetched_utc)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = false;
    void *blob = NULL;
    if (nvs_get_blob(h, NVS_KEY_CACHE, NULL, &len) == ESP_OK && len >= sizeof(cache_header_t)) {
        blob = malloc(len);
        if (blob && nvs_get_blob(h, NVS_KEY_CACHE, blob, &len) == ESP_OK) {
            cache_header_t hd;
            memcpy(&hd, blob, sizeof(hd));
            if (hd.version == CACHE_VERSION && memcmp(hd.sources_hash, sh, 8) == 0 &&
                len == sizeof(hd) + hd.count * sizeof(cal_event_t)) {
                const cal_event_t *ev = (const cal_event_t *)((uint8_t *)blob + sizeof(hd));
                for (uint32_t i = 0; i < hd.count && *count < MERGE_MAX; i++) {
                    if ((mask >> ev[i].calendar) & 1 && ev[i].end_day >= first_day &&
                        ev[i].start_day <= last_day) {
                        s_events[(*count)++] = ev[i];
                    }
                }
                *fetched_utc = hd.fetched_utc;
                int64_t t;
                if (nvs_get_i64(h, NVS_KEY_CACHE_T, &t) == ESP_OK) *fetched_utc = t;
                ok = true;
            }
        }
    }
    free(blob);
    nvs_close(h);
    return ok;
}

static void cache_store(const uint8_t sh[8], size_t count, int64_t now_utc)
{
    size_t len = sizeof(cache_header_t) + count * sizeof(cal_event_t);
    uint8_t *blob = malloc(len);
    if (!blob) return;
    cache_header_t hd = { .version = CACHE_VERSION, .count = count, .fetched_utc = now_utc };
    memcpy(hd.sources_hash, sh, 8);
    memcpy(blob, &hd, sizeof(hd));
    memcpy(blob + sizeof(hd), s_events, count * sizeof(cal_event_t));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        /* Skip identical writes: this runs every wake, and most wakes find
         * the same week. */
        size_t old_len = 0;
        bool same = false;
        if (nvs_get_blob(h, NVS_KEY_CACHE, NULL, &old_len) == ESP_OK && old_len == len) {
            uint8_t *old = malloc(len);
            if (old && nvs_get_blob(h, NVS_KEY_CACHE, old, &old_len) == ESP_OK) {
                cache_header_t oh;
            memcpy(&oh, old, sizeof(oh));
            same = memcmp(oh.sources_hash, sh, 8) == 0 &&
                   memcmp(old + sizeof(hd), blob + sizeof(hd), len - sizeof(hd)) == 0;
            }
            free(old);
        }
        if (!same) {
            esp_err_t err = nvs_set_blob(h, NVS_KEY_CACHE, blob, len);
            if (err == ESP_OK) err = nvs_commit(h);
            ESP_LOGI(TAG, "cache: %u events written (%s)", (unsigned)count, esp_err_to_name(err));
        }
        /* The fetch time is kept apart from the blob so it advances on every good fetch
         * without rewriting 11 KB; "last updated" means last fetched, not last changed. */
        nvs_set_i64(h, NVS_KEY_CACHE_T, now_utc);
        nvs_commit(h);
        nvs_close(h);
    }
    free(blob);
}

esp_err_t app_cal_fetch(const cal_tz_t *local, int64_t from_utc, int64_t to_utc, bool online,
                        app_cal_week_t *out)
{
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_count;
    uint32_t gen = s_gen;
    memcpy(s_snap, s_src, sizeof(s_src[0]) * count);
    xSemaphoreGive(s_lock);
    if (count == 0) return ESP_ERR_NOT_FOUND;
    uint8_t sh[8];
    sources_hash(s_snap, count, sh);
    int64_t t0 = esp_timer_get_time();

    size_t total = 0;
    uint8_t all = (uint8_t)((1u << count) - 1);
    uint8_t failed = online ? 0 : all;
    for (size_t i = 0; online && i < count; i++) {
        cal_window_t win = { .from_utc = from_utc, .to_utc = to_utc, .local = local,
                             .calendar = (uint8_t)i,
                             .hide_private = CONFIG_APP_CAL_HIDE_PRIVATE };
        size_t n = 0;
        if (fetch_one(i, &win, &n) != ESP_OK) {
            failed |= (uint8_t)(1u << i);
            continue;
        }
        for (size_t k = 0; k < n && total < MERGE_MAX; k++) {
            s_events[total++] = s_scratch[k];
        }
    }

    /* The window as local day numbers, the frame cal_event_t uses. */
    int32_t first_day = (int32_t)((from_utc + cal_tz_offset_at_utc(local, from_utc)) / 86400);
    int32_t last_day = (int32_t)((to_utc - 1 + cal_tz_offset_at_utc(local, to_utc - 1)) / 86400);

    if (failed) {
        /* Stale even with no cache to fall back on: a calendar is then simply missing,
         * and the panel must say so rather than show its days as free. */
        out->stale = true;
        int64_t when = 0;
        if (cache_load(sh, failed, first_day, last_day, &total, &when)) {
            out->stale_since_utc = when;
        } else if (failed == all) {
            return ESP_FAIL;
        }
    }
    qsort(s_events, total, sizeof(cal_event_t), cmp_event);
    if (total > APP_CAL_MAX_EVENTS) {
        ESP_LOGW(TAG, "%u events in the week; showing the earliest %u", (unsigned)total,
                 APP_CAL_MAX_EVENTS);
        total = APP_CAL_MAX_EVENTS;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool edited = gen != s_gen;
    xSemaphoreGive(s_lock);
    if (!failed && !edited) {
        cache_store(sh, total, (int64_t)time(NULL));
    }
    out->events = s_events;
    out->count = total;
    out->failed_mask = failed;
    out->fetch_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}
