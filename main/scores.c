#include "scores.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "scores";

#define NVS_NAMESPACE "stealth"
#define KEY_RECORDS   "recs"
#define KEY_INITIALS  "who"
#define KEY_SET_ID    "setid"

typedef struct {
    uint16_t centis;      // 0 = unset
    char     who[4];      // 2 initials + NUL (padded)
} record_t;

static record_t s_recs[SCORES_MAX_LEVELS];
static bool     s_ready;

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, KEY_RECORDS, s_recs, sizeof(s_recs));
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t scores_init(uint32_t set_id)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    memset(s_recs, 0, sizeof(s_recs));

    nvs_handle_t h;
    bool wipe = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(h, KEY_SET_ID, &stored) != ESP_OK || stored != set_id) {
            // Different stage table. Keeping the times would show a record for
            // a map that no longer exists at that index.
            ESP_LOGW(TAG, "stage set changed (%08lx -> %08lx), clearing records",
                     (unsigned long)stored, (unsigned long)set_id);
            wipe = true;
        } else {
            size_t len = sizeof(s_recs);
            if (nvs_get_blob(h, KEY_RECORDS, s_recs, &len) != ESP_OK) {
                ESP_LOGI(TAG, "no saved records yet");
            }
        }
        nvs_close(h);
    }

    if (wipe) {
        memset(s_recs, 0, sizeof(s_recs));
    }
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_SET_ID, set_id);
        if (wipe) nvs_set_blob(h, KEY_RECORDS, s_recs, sizeof(s_recs));
        nvs_commit(h);
        nvs_close(h);
    }

    int have = 0;
    for (int i = 0; i < SCORES_MAX_LEVELS; i++) if (s_recs[i].centis) have++;
    ESP_LOGI(TAG, "records loaded: %d stage(s) with a time", have);

    s_ready = true;
    return ESP_OK;
}

bool scores_get(int level, uint16_t *centis, char who[4])
{
    if (!s_ready || level < 0 || level >= SCORES_MAX_LEVELS) return false;
    if (s_recs[level].centis == 0) return false;
    if (centis) *centis = s_recs[level].centis;
    if (who) {
        memcpy(who, s_recs[level].who, 3);
        who[3] = '\0';
    }
    return true;
}

bool scores_submit(int level, float seconds, const char *who)
{
    if (!s_ready || level < 0 || level >= SCORES_MAX_LEVELS) return false;

    if (seconds < 0.0f) seconds = 0.0f;
    uint32_t c = (uint32_t)(seconds * 100.0f + 0.5f);
    if (c == 0) c = 1;              // 0 is the "unset" marker
    if (c > 65535) c = 65535;       // ~10.9 minutes, and it still counts

    const uint16_t centis = (uint16_t)c;
    if (s_recs[level].centis != 0 && centis >= s_recs[level].centis) return false;

    s_recs[level].centis = centis;
    memset(s_recs[level].who, 0, sizeof(s_recs[level].who));
    if (who) strncpy(s_recs[level].who, who, 2);
    persist();
    return true;
}

void scores_load_initials(char out[3])
{
    strcpy(out, "AA");
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 4;
    char buf[4] = {0};
    if (nvs_get_str(h, KEY_INITIALS, buf, &len) == ESP_OK && buf[0]) {
        out[0] = buf[0];
        out[1] = buf[1] ? buf[1] : 'A';
        out[2] = '\0';
    }
    nvs_close(h);
}

void scores_save_initials(const char *in)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char buf[4] = {0};
    strncpy(buf, in, 2);
    nvs_set_str(h, KEY_INITIALS, buf);
    nvs_commit(h);
    nvs_close(h);
}
