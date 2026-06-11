#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "agent_ble.hpp"
#include "agent_ring.hpp"
#include "agent_pmic.hpp"
#include "agent_printer.hpp"

#define COLOR_GREEN 0x22C55E
#define COLOR_IDLE  0x888888
#define COLOR_SKY   0x87CEEB
#define MDI_CLOCK  "\xF3\xB1\x91\x8E"
#define MDI_NOZZLE "\xF3\xB0\xB9\x9B"
#define MDI_BED    "\xF3\xB1\xA1\x9B"

extern "C" {
extern const lv_font_t mdi_40;
extern const lv_image_dsc_t bambuicon_small;
}

static lv_obj_t *status_arc;
static lv_obj_t *label_progress;
static lv_obj_t *label_state;
static lv_obj_t *label_batt_icon;
static lv_obj_t *label_batt;
static lv_obj_t *logo_image;
static lv_obj_t *label_nozzle_icon;
static lv_obj_t *label_nozzle_value;
static lv_obj_t *label_bed_icon;
static lv_obj_t *label_bed_value;
static lv_obj_t *label_layer;
static lv_obj_t *label_detail;
static lv_obj_t *label_source;
static lv_obj_t *label_eta_icon;
static lv_obj_t *label_eta;

static uint32_t printer_signature = 0;
static uint32_t last_ring_main_hex = 0;
static uint32_t last_ring_indicator_hex = 0;
static agent_printer_status_t printer_page_status;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

static void set_noninteractive(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, lv_align_t align, int x, int y,
                              const lv_font_t *font, lv_color_t color, int width,
                              lv_text_align_t text_align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, text_align, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, align, x, y);
    set_noninteractive(label);
    return label;
}

static lv_color_t printer_state_color(const char *state)
{
    if (!state) return lv_color_hex(0x888888);
    if (strcmp(state, "printing") == 0) return lv_color_hex(COLOR_GREEN);
    if (strcmp(state, "paused") == 0) return lv_color_hex(0xFFAA00);
    if (strcmp(state, "error") == 0) return lv_color_hex(0xFF3333);
    if (strcmp(state, "idle") == 0) return lv_color_hex(COLOR_IDLE);
    if (strcmp(state, "offline") == 0) return lv_color_hex(0x888888);
    return lv_color_hex(0xB0B0B0);
}

static uint32_t printer_state_hex(const char *state)
{
    if (!state) return 0x888888;
    if (strcmp(state, "printing") == 0) return COLOR_GREEN;
    if (strcmp(state, "paused") == 0) return 0xFFAA00;
    if (strcmp(state, "error") == 0) return 0xFF3333;
    if (strcmp(state, "idle") == 0) return COLOR_IDLE;
    if (strcmp(state, "offline") == 0) return 0x888888;
    return 0xB0B0B0;
}

static const char *printer_state_label(const char *state)
{
    if (!state) return "unknown";
    if (strcmp(state, "printing") == 0) return "printing";
    if (strcmp(state, "paused") == 0) return "paused";
    if (strcmp(state, "error") == 0) return "failed";
    if (strcmp(state, "idle") == 0) return "idle";
    if (strcmp(state, "offline") == 0) return "offline";
    return "unknown";
}

static uint32_t freshness_bucket(uint32_t updated_ms)
{
    uint32_t elapsed_ms = now_ms() - updated_ms;
    if (elapsed_ms < 60000) return 0;
    return elapsed_ms / 60000;
}

static void format_freshness(uint32_t updated_ms, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    uint32_t elapsed_ms = now_ms() - updated_ms;
    if (elapsed_ms < 60000) {
        snprintf(buf, buf_size, "just now");
        return;
    }

    uint32_t minutes = elapsed_ms / 60000;
    if (minutes < 60) {
        snprintf(buf, buf_size, "%lum ago", (unsigned long)minutes);
    } else {
        snprintf(buf, buf_size, "%luh ago", (unsigned long)(minutes / 60));
    }
}

static void format_eta(int32_t seconds, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    if (seconds < 0) {
        snprintf(buf, buf_size, "--");
    } else if (seconds == 0) {
        snprintf(buf, buf_size, "Done");
    } else if (seconds < 60) {
        snprintf(buf, buf_size, "<1m");
    } else {
        int minutes = seconds / 60;
        if (minutes < 60) {
            snprintf(buf, buf_size, "%dm", minutes);
        } else {
            snprintf(buf, buf_size, "%dh %02dm", minutes / 60, minutes % 60);
        }
    }
}

static void format_layers(const agent_printer_status_t *status, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    if (!status) {
        snprintf(buf, buf_size, "Layer: -- / --");
    } else if (status->layer > 0 && status->layers > 0) {
        snprintf(buf, buf_size, "Layer: %d / %d", status->layer, status->layers);
    } else if (status->layer > 0) {
        snprintf(buf, buf_size, "Layer: %d / --", status->layer);
    } else {
        snprintf(buf, buf_size, "Layer: -- / --");
    }
}

static const char *temp_value(const char *value)
{
    return (value && value[0]) ? value : "--";
}

static void set_arc_visual(uint32_t main_hex, uint32_t indicator_hex, int progress)
{
    if (!status_arc) return;
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    lv_arc_set_value(status_arc, progress);

    if (main_hex != last_ring_main_hex) {
        lv_obj_set_style_arc_color(status_arc, lv_color_hex(main_hex), LV_PART_MAIN);
        last_ring_main_hex = main_hex;
    }
    if (indicator_hex != last_ring_indicator_hex) {
        lv_obj_set_style_arc_color(status_arc, lv_color_hex(indicator_hex), LV_PART_INDICATOR);
        last_ring_indicator_hex = indicator_hex;
    }
}

static void update_battery_widget(void)
{
    if (!label_batt_icon || !label_batt) return;

    int batt = agent_pmic_get_battery_percent();
    bool charging = agent_pmic_is_charging();
    char buf[16];
    char icon_buf[16];
    const char *icon = LV_SYMBOL_BATTERY_EMPTY;

    if (batt >= 90) {
        icon = LV_SYMBOL_BATTERY_FULL;
    } else if (batt >= 65) {
        icon = LV_SYMBOL_BATTERY_3;
    } else if (batt >= 35) {
        icon = LV_SYMBOL_BATTERY_2;
    } else if (batt >= 10) {
        icon = LV_SYMBOL_BATTERY_1;
    }

    if (batt >= 0) snprintf(buf, sizeof(buf), "%d%%", batt);
    else snprintf(buf, sizeof(buf), "---");
    snprintf(icon_buf, sizeof(icon_buf), "%s%s", icon, charging ? " " LV_SYMBOL_CHARGE : "");

    lv_label_set_text(label_batt_icon, icon_buf);
    lv_obj_set_style_text_color(label_batt_icon,
                                charging ? lv_color_hex(0x00FF00) :
                                (batt >= 0 && batt < 10) ? lv_color_hex(0xFF0000) : lv_color_hex(0x888888),
                                0);
    lv_label_set_text(label_batt, buf);
}

static void apply_empty_state(const char *state, const char *detail, bool connected)
{
    uint32_t color = connected ? printer_state_hex("offline") : 0x666666;
    set_arc_visual(0x101010, color, 0);
    lv_label_set_text(label_progress, "--%");
    lv_obj_set_style_text_color(label_progress, lv_color_hex(color), 0);
    lv_label_set_text(label_state, state);
    lv_obj_set_style_text_color(label_state, lv_color_hex(color), 0);
    lv_label_set_text(label_nozzle_value, "--C");
    lv_label_set_text(label_bed_value, "--C");
    lv_label_set_text(label_layer, "Layer: -- / --");
    lv_label_set_text(label_detail, detail);
    lv_label_set_text(label_source, connected ? "No printer data" : "");
    lv_label_set_text(label_eta, "--");
}

void agent_printer_create(lv_obj_t *tile)
{
    status_arc = agent_ring_create(tile, 0, 100, 0);
    lv_obj_set_style_arc_width(status_arc, AGENT_RING_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(status_arc, AGENT_RING_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(status_arc, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_arc_color(status_arc, lv_color_hex(0x888888), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(status_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(status_arc, true, LV_PART_INDICATOR);

    label_progress = create_label(tile, "--%", LV_ALIGN_CENTER, 0, -178, &lv_font_montserrat_32,
                                  lv_color_hex(0xFFFFFF), 180, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *batt_row = lv_obj_create(tile);
    lv_obj_remove_style_all(batt_row);
    lv_obj_set_size(batt_row, 120, 24);
    lv_obj_align(batt_row, LV_ALIGN_CENTER, 0, -140);
    set_noninteractive(batt_row);
    lv_obj_set_flex_flow(batt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(batt_row, 8, 0);

    label_batt_icon = lv_label_create(batt_row);
    lv_label_set_text(label_batt_icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_color(label_batt_icon, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_batt_icon, &lv_font_montserrat_16, 0);
    set_noninteractive(label_batt_icon);

    label_batt = lv_label_create(batt_row);
    lv_label_set_text(label_batt, "---");
    lv_obj_set_style_text_color(label_batt, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_batt, &lv_font_montserrat_16, 0);
    set_noninteractive(label_batt);

    label_state = create_label(tile, "offline", LV_ALIGN_CENTER, 0, -92, &lv_font_montserrat_28,
                               lv_color_hex(0x888888), 260, LV_TEXT_ALIGN_CENTER);

    logo_image = lv_image_create(tile);
    lv_image_set_src(logo_image, &bambuicon_small);
    lv_image_set_scale(logo_image, 183);
    lv_image_set_antialias(logo_image, true);
    lv_obj_align(logo_image, LV_ALIGN_CENTER, 0, -8);
    set_noninteractive(logo_image);

    label_nozzle_icon = create_label(tile, MDI_NOZZLE, LV_ALIGN_CENTER, -152, -10, &mdi_40,
                                     lv_color_hex(0xFFFFFF), 48, LV_TEXT_ALIGN_CENTER);
    label_nozzle_value = create_label(tile, "--C", LV_ALIGN_CENTER, -82, -10, &lv_font_montserrat_24,
                                      lv_color_hex(0xFFFFFF), 94, LV_TEXT_ALIGN_LEFT);
    label_bed_icon = create_label(tile, MDI_BED, LV_ALIGN_CENTER, 152, -10, &mdi_40,
                                  lv_color_hex(0xFFFFFF), 48, LV_TEXT_ALIGN_CENTER);
    label_bed_value = create_label(tile, "--C", LV_ALIGN_CENTER, 78, -10, &lv_font_montserrat_24,
                                   lv_color_hex(0xFFFFFF), 96, LV_TEXT_ALIGN_RIGHT);

    label_layer = create_label(tile, "Layer: -- / --", LV_ALIGN_CENTER, 0, 62, &lv_font_montserrat_24,
                               lv_color_hex(0xDDDDDD), 300, LV_TEXT_ALIGN_CENTER);
    label_detail = create_label(tile, "No printer data", LV_ALIGN_CENTER, 0, 98, &lv_font_montserrat_16,
                                lv_color_hex(0xFFFFFF), 330, LV_TEXT_ALIGN_CENTER);
    label_source = create_label(tile, "", LV_ALIGN_CENTER, 0, 126, &lv_font_montserrat_14,
                                lv_color_hex(0x94A3B8), 310, LV_TEXT_ALIGN_CENTER);
    label_eta_icon = create_label(tile, MDI_CLOCK, LV_ALIGN_CENTER, -56, 170, &mdi_40,
                                  lv_color_hex(COLOR_SKY), 48, LV_TEXT_ALIGN_CENTER);
    label_eta = create_label(tile, "--", LV_ALIGN_CENTER, 52, 170, &lv_font_montserrat_28,
                             lv_color_hex(COLOR_SKY), 150, LV_TEXT_ALIGN_LEFT);

    agent_printer_timer_update();
}

void agent_printer_set_ring_visible(bool visible)
{
    agent_ring_set_visible(status_arc, visible);
}

void agent_printer_timer_update(void)
{
    if (!label_state) return;
    update_battery_widget();

    bool has_status = agent_ble_get_printer_status(&printer_page_status);
    uint32_t age_bucket = has_status ? freshness_bucket(printer_page_status.updated_ms) : 0;
    uint32_t sig = 2166136261u;
    sig = hash_bytes(sig, &g_ble_connected, sizeof(g_ble_connected));
    sig = hash_bytes(sig, &has_status, sizeof(has_status));
    sig = hash_bytes(sig, &age_bucket, sizeof(age_bucket));
    if (has_status) {
        sig = hash_bytes(sig, printer_page_status.state, strlen(printer_page_status.state));
        sig = hash_bytes(sig, printer_page_status.job, strlen(printer_page_status.job));
        sig = hash_bytes(sig, printer_page_status.material, strlen(printer_page_status.material));
        sig = hash_bytes(sig, printer_page_status.nozzle_c, strlen(printer_page_status.nozzle_c));
        sig = hash_bytes(sig, printer_page_status.bed_c, strlen(printer_page_status.bed_c));
        sig = hash_bytes(sig, printer_page_status.source, strlen(printer_page_status.source));
        sig = hash_bytes(sig, &printer_page_status.progress_percent, sizeof(printer_page_status.progress_percent));
        sig = hash_bytes(sig, &printer_page_status.eta_seconds, sizeof(printer_page_status.eta_seconds));
        sig = hash_bytes(sig, &printer_page_status.layer, sizeof(printer_page_status.layer));
        sig = hash_bytes(sig, &printer_page_status.layers, sizeof(printer_page_status.layers));
    }
    if (sig == printer_signature) return;
    printer_signature = sig;

    if (!has_status) {
        if (g_ble_connected) {
            apply_empty_state("offline", "Run bambu-login", true);
        } else {
            apply_empty_state("waiting", "Waiting for printer data", false);
        }
        return;
    }

    uint32_t indicator_hex = printer_state_hex(printer_page_status.state);
    int progress = printer_page_status.progress_percent >= 0 ? printer_page_status.progress_percent : 0;
    set_arc_visual(0x101010, indicator_hex, progress);

    char buf[128];
    if (printer_page_status.progress_percent >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", printer_page_status.progress_percent);
    } else {
        snprintf(buf, sizeof(buf), "--%%");
    }
    lv_label_set_text(label_progress, buf);
    lv_obj_set_style_text_color(label_progress, lv_color_hex(indicator_hex), 0);

    lv_label_set_text(label_state, printer_state_label(printer_page_status.state));
    lv_obj_set_style_text_color(label_state, printer_state_color(printer_page_status.state), 0);

    snprintf(buf, sizeof(buf), "%sC", temp_value(printer_page_status.nozzle_c));
    lv_label_set_text(label_nozzle_value, buf);
    snprintf(buf, sizeof(buf), "%sC", temp_value(printer_page_status.bed_c));
    lv_label_set_text(label_bed_value, buf);

    format_layers(&printer_page_status, buf, sizeof(buf));
    lv_label_set_text(label_layer, buf);

    lv_label_set_text(label_detail, printer_page_status.job[0] ? printer_page_status.job : "No active job");

    char fresh[20];
    format_freshness(printer_page_status.updated_ms, fresh, sizeof(fresh));
    const char *source = printer_page_status.source[0] ? printer_page_status.source : "Cloud";
    if (printer_page_status.material[0]) {
        snprintf(buf, sizeof(buf), "%s - %s - %s", printer_page_status.material, source, fresh);
    } else {
        snprintf(buf, sizeof(buf), "%s - %s", source, fresh);
    }
    lv_label_set_text(label_source, buf);

    format_eta(printer_page_status.eta_seconds, buf, sizeof(buf));
    lv_label_set_text(label_eta, buf);
}
