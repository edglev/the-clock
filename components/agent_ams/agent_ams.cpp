#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "agent_ble.hpp"
#include "agent_ams.hpp"

#define COLOR_CYAN  0x009999
#define COLOR_GREEN 0x22C55E
#define TRAY_CARD_WIDTH 158
#define TRAY_CARD_HEIGHT 96

static lv_obj_t *label_status;
static lv_obj_t *tray_cards[AGENT_AMS_TRAY_COUNT];
static lv_obj_t *tray_fills[AGENT_AMS_TRAY_COUNT];
static lv_obj_t *tray_slot_labels[AGENT_AMS_TRAY_COUNT];
static lv_obj_t *tray_material_labels[AGENT_AMS_TRAY_COUNT];
static lv_obj_t *tray_remaining_labels[AGENT_AMS_TRAY_COUNT];

static uint32_t ams_signature = 0;
static agent_ams_status_t ams_status;

static void set_noninteractive(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              lv_color_t color, int width, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    set_noninteractive(label);
    return label;
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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_color(const char *text, uint32_t *out)
{
    if (!text || !out) return false;
    if (text[0] == '#') text++;
    if (strlen(text) < 6) return false;

    uint32_t value = 0;
    for (int i = 0; i < 6; i++) {
        int nibble = hex_value(text[i]);
        if (nibble < 0) return false;
        value = (value << 4) | (uint32_t)nibble;
    }
    *out = value;
    return true;
}

static lv_color_t text_color_for_fill(uint32_t color)
{
    uint8_t red = (color >> 16) & 0xff;
    uint8_t green = (color >> 8) & 0xff;
    uint8_t blue = color & 0xff;
    uint32_t brightness = (red * 299u + green * 587u + blue * 114u) / 1000u;
    return lv_color_hex(brightness > 145 ? 0x000000 : 0xFFFFFF);
}

static const char *tray_material_text(const agent_ams_tray_t *tray)
{
    if (!tray || !tray->loaded) return "Empty";
    return tray->material[0] ? tray->material : "Loaded";
}

static void tray_remaining_text(const agent_ams_tray_t *tray, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    if (!tray || !tray->loaded) {
        snprintf(buf, buf_size, "--");
    } else if (tray->remaining_percent >= 0) {
        snprintf(buf, buf_size, "%d%%", tray->remaining_percent);
    } else {
        snprintf(buf, buf_size, "--");
    }
}

void agent_ams_create(lv_obj_t *tile)
{
    lv_obj_t *title = create_label(tile, "AMS", &lv_font_montserrat_28,
                                   lv_color_hex(COLOR_CYAN), 180, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 38);

    label_status = create_label(tile, "No AMS data", &lv_font_montserrat_20,
                                lv_color_hex(0xB0B0B0), 300, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 74);

    for (int i = 0; i < AGENT_AMS_TRAY_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;
        int x = col == 0 ? -86 : 86;
        int y = row == 0 ? -32 : 78;

        lv_obj_t *card = lv_obj_create(tile);
        tray_cards[i] = card;
        lv_obj_set_size(card, TRAY_CARD_WIDTH, TRAY_CARD_HEIGHT);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x333333), 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_align(card, LV_ALIGN_CENTER, x, y);
        set_noninteractive(card);

        tray_fills[i] = lv_obj_create(card);
        lv_obj_remove_style_all(tray_fills[i]);
        lv_obj_set_size(tray_fills[i], 0, TRAY_CARD_HEIGHT);
        lv_obj_set_style_radius(tray_fills[i], 6, 0);
        lv_obj_set_style_bg_color(tray_fills[i], lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_opa(tray_fills[i], LV_OPA_80, 0);
        lv_obj_align(tray_fills[i], LV_ALIGN_LEFT_MID, 0, 0);
        set_noninteractive(tray_fills[i]);

        char slot[4];
        snprintf(slot, sizeof(slot), "A%d", i + 1);
        tray_slot_labels[i] = create_label(card, slot, &lv_font_montserrat_20,
                                           lv_color_hex(COLOR_CYAN), 48, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(tray_slot_labels[i], LV_ALIGN_TOP_LEFT, 12, 8);

        tray_material_labels[i] = create_label(card, "Empty", &lv_font_montserrat_20,
                                               lv_color_hex(0xDDDDDD), 112, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(tray_material_labels[i], LV_ALIGN_TOP_LEFT, 12, 38);

        tray_remaining_labels[i] = create_label(card, "--", &lv_font_montserrat_16,
                                                lv_color_hex(0x9CA3AF), 54, LV_TEXT_ALIGN_RIGHT);
        lv_obj_align(tray_remaining_labels[i], LV_ALIGN_TOP_RIGHT, -12, 12);
    }

    agent_ams_timer_update();
}

void agent_ams_timer_update(void)
{
    if (!label_status) return;

    bool has_status = agent_ble_get_ams_status(&ams_status);

    uint32_t sig = 2166136261u;
    sig = hash_bytes(sig, &g_ble_connected, sizeof(g_ble_connected));
    sig = hash_bytes(sig, &has_status, sizeof(has_status));
    if (has_status) {
        sig = hash_bytes(sig, &ams_status.active_slot, sizeof(ams_status.active_slot));
        for (int i = 0; i < AGENT_AMS_TRAY_COUNT; i++) {
            sig = hash_bytes(sig, ams_status.trays[i].material, strlen(ams_status.trays[i].material));
            sig = hash_bytes(sig, ams_status.trays[i].color, strlen(ams_status.trays[i].color));
            sig = hash_bytes(sig, &ams_status.trays[i].remaining_percent, sizeof(ams_status.trays[i].remaining_percent));
            sig = hash_bytes(sig, &ams_status.trays[i].loaded, sizeof(ams_status.trays[i].loaded));
        }
        sig = hash_bytes(sig, ams_status.humidity, strlen(ams_status.humidity));
    }
    if (sig == ams_signature) return;
    ams_signature = sig;

    if (!has_status) {
        lv_label_set_text(label_status, g_ble_connected ? "No AMS data" : "Waiting for AMS data");
    } else if (ams_status.active_slot >= 0) {
        char active[40];
        if (ams_status.humidity[0]) {
            snprintf(active, sizeof(active), "Active A%d  H%s", ams_status.active_slot + 1, ams_status.humidity);
        } else {
            snprintf(active, sizeof(active), "Active A%d", ams_status.active_slot + 1);
        }
        lv_label_set_text(label_status, active);
    } else if (ams_status.humidity[0]) {
        char humidity[24];
        snprintf(humidity, sizeof(humidity), "Humidity H%s", ams_status.humidity);
        lv_label_set_text(label_status, humidity);
    } else {
        lv_label_set_text(label_status, "No active slot");
    }

    for (int i = 0; i < AGENT_AMS_TRAY_COUNT; i++) {
        const agent_ams_tray_t *tray = has_status ? &ams_status.trays[i] : NULL;
        bool active = has_status && ams_status.active_slot == i;
        bool loaded = tray && tray->loaded;
        uint32_t fill_hex = 0x333333;
        int fill_width = 0;
        bool has_fill_color = false;

        if (loaded && tray->remaining_percent >= 0) {
            fill_width = (TRAY_CARD_WIDTH * tray->remaining_percent) / 100;
            if (fill_width < 1 && tray->remaining_percent > 0) fill_width = 1;
            if (!parse_color(tray->color, &fill_hex)) {
                fill_hex = 0x888888;
            }
            has_fill_color = fill_width > 0;
        }
        lv_obj_set_width(tray_fills[i], fill_width);
        lv_obj_set_style_bg_color(tray_fills[i], lv_color_hex(fill_hex), 0);

        lv_obj_set_style_border_width(tray_cards[i], active ? 2 : 1, 0);
        lv_obj_set_style_border_color(tray_cards[i],
                                      lv_color_hex(active ? COLOR_GREEN : 0x333333), 0);
        lv_obj_set_style_bg_color(tray_cards[i],
                                  lv_color_hex(active ? 0x132018 : 0x111111), 0);
        lv_color_t tray_text_color = has_fill_color ? text_color_for_fill(fill_hex) : lv_color_hex(0x888888);
        lv_obj_set_style_text_color(tray_slot_labels[i],
                                    has_fill_color ? tray_text_color : lv_color_hex(active ? COLOR_GREEN : COLOR_CYAN), 0);
        lv_obj_set_style_text_color(tray_material_labels[i], tray_text_color, 0);
        lv_label_set_text(tray_material_labels[i], tray_material_text(tray));

        char remaining[8];
        tray_remaining_text(tray, remaining, sizeof(remaining));
        lv_obj_set_style_text_color(tray_remaining_labels[i],
                                    has_fill_color ? tray_text_color : lv_color_hex(0x666666), 0);
        lv_label_set_text(tray_remaining_labels[i], remaining);
    }
}
