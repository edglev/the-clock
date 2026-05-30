#include <string.h>
#include <inttypes.h>
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "agent_ble.hpp"
#include "agent_pmic.hpp"

static lv_obj_t *label_ble_status = NULL;
static lv_obj_t *label_battery_status = NULL;
static lv_obj_t *label_brightness_val = NULL;
static int current_brightness = 100;
static lv_obj_t *bond_list_cont = NULL;
static lv_obj_t *pair_btn = NULL;
static int last_bond_count = -1;

#define COLOR_CYAN       0x009999
#define COLOR_CYAN_DARK  0x00384D

static void brightness_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    current_brightness = val;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", val);
    lv_label_set_text(label_brightness_val, buf);
    bsp_display_brightness_set(val);
}

static void forget_bond_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    agent_ble_delete_bond(index);
    last_bond_count = -1;
}

static void pair_new_cb(lv_event_t *e)
{
    agent_ble_enable_pairing_mode();
    lv_label_set_text(lv_obj_get_child(pair_btn, 0), "Pairing...");
    lv_obj_set_style_bg_color(pair_btn, lv_color_hex(0x007700), 0);
}

static void forget_all_cb(lv_event_t *e)
{
    agent_ble_delete_all_bonds();
    last_bond_count = -1;
}

static void update_battery_status(void)
{
    if (!label_battery_status) return;

    int percent = agent_pmic_get_battery_percent();
    bool charging = agent_pmic_is_charging();

    char buf[32];
    if (percent >= 0) {
        snprintf(buf, sizeof(buf), "%d%% %s", percent, charging ? "Charging" : "On battery");
    } else {
        snprintf(buf, sizeof(buf), "---");
    }

    lv_label_set_text(label_battery_status, buf);
    lv_obj_set_style_text_color(label_battery_status,
                                charging ? lv_color_hex(0x00FF00) : lv_color_hex(0x888888),
                                0);
}

static void rebuild_bond_list(void)
{
    if (!bond_list_cont) return;
    lv_obj_clean(bond_list_cont);

    int count = agent_ble_get_bond_count();
    last_bond_count = count;

    if (count == 0) {
        lv_obj_t *no = lv_label_create(bond_list_cont);
        lv_label_set_text(no, "No paired devices");
        lv_obj_set_style_text_color(no, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(no, &lv_font_montserrat_14, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        uint8_t addr[6];
        agent_ble_get_bond(i, addr);
        const char *name = agent_ble_get_bond_name(i);
        char display_str[40];
        if (name[0]) {
            snprintf(display_str, sizeof(display_str), "%s", name);
        } else {
            snprintf(display_str, sizeof(display_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        }

        lv_obj_t *row = lv_obj_create(bond_list_cont);
        lv_obj_set_size(row, 420, 34);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x444444), 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, display_str);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 60, 26);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x770000), 0);
        lv_obj_add_event_cb(btn, forget_bond_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *btn_l = lv_label_create(btn);
        lv_label_set_text(btn_l, "Forget");
        lv_obj_set_style_text_font(btn_l, &lv_font_montserrat_12, 0);
        lv_obj_center(btn_l);
    }

    lv_obj_t *clear_btn = lv_btn_create(bond_list_cont);
    lv_obj_set_size(clear_btn, 200, 30);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0x550000), 0);
    lv_obj_add_event_cb(clear_btn, forget_all_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_l = lv_label_create(clear_btn);
    lv_label_set_text(clear_l, "Clear All Bonds");
    lv_obj_set_style_text_font(clear_l, &lv_font_montserrat_14, 0);
    lv_obj_center(clear_l);
}

void agent_settings_create(lv_obj_t *tile)
{
    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *bright_label = lv_label_create(tile);
    lv_label_set_text(bright_label, "Brightness");
    lv_obj_set_style_text_color(bright_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(bright_label, &lv_font_montserrat_16, 0);
    lv_obj_align(bright_label, LV_ALIGN_TOP_MID, -100, 80);

    label_brightness_val = lv_label_create(tile);
    lv_label_set_text(label_brightness_val, "100%");
    lv_obj_set_style_text_color(label_brightness_val, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_brightness_val, &lv_font_montserrat_16, 0);
    lv_obj_align(label_brightness_val, LV_ALIGN_TOP_MID, 100, 80);

    lv_obj_t *slider = lv_slider_create(tile);
    lv_obj_set_size(slider, 360, 20);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 115);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, current_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_CYAN), LV_PART_INDICATOR);

    lv_obj_t *batt_label = lv_label_create(tile);
    lv_label_set_text(batt_label, "Battery");
    lv_obj_set_style_text_color(batt_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(batt_label, &lv_font_montserrat_16, 0);
    lv_obj_align(batt_label, LV_ALIGN_TOP_MID, -100, 150);

    label_battery_status = lv_label_create(tile);
    lv_label_set_text(label_battery_status, "---");
    lv_obj_set_style_text_color(label_battery_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_battery_status, &lv_font_montserrat_16, 0);
    lv_obj_align(label_battery_status, LV_ALIGN_TOP_MID, 100, 150);

    lv_obj_t *bt_label = lv_label_create(tile);
    lv_label_set_text(bt_label, "Bluetooth");
    lv_obj_set_style_text_color(bt_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(bt_label, &lv_font_montserrat_16, 0);
    lv_obj_align(bt_label, LV_ALIGN_TOP_MID, -100, 185);

    label_ble_status = lv_label_create(tile);
    lv_label_set_text(label_ble_status, "Disconnected");
    lv_obj_set_style_text_color(label_ble_status, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(label_ble_status, &lv_font_montserrat_16, 0);
    lv_obj_align(label_ble_status, LV_ALIGN_TOP_MID, 100, 185);

    lv_obj_t *paired_label = lv_label_create(tile);
    lv_label_set_text(paired_label, "Paired Devices");
    lv_obj_set_style_text_color(paired_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(paired_label, &lv_font_montserrat_16, 0);
    lv_obj_align(paired_label, LV_ALIGN_TOP_MID, 0, 240);

    bond_list_cont = lv_obj_create(tile);
    lv_obj_set_size(bond_list_cont, 440, 160);
    lv_obj_align(bond_list_cont, LV_ALIGN_TOP_MID, 0, 270);
    lv_obj_set_style_bg_color(bond_list_cont, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(bond_list_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bond_list_cont, 0, 0);
    lv_obj_set_style_pad_all(bond_list_cont, 6, 0);
    lv_obj_clear_flag(bond_list_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bond_list_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bond_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bond_list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    pair_btn = lv_btn_create(tile);
    lv_obj_set_size(pair_btn, 440, 34);
    lv_obj_align(pair_btn, LV_ALIGN_TOP_MID, 0, 440);
    lv_obj_set_style_bg_color(pair_btn, lv_color_hex(COLOR_CYAN_DARK), 0);
    lv_obj_add_event_cb(pair_btn, pair_new_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pair_l = lv_label_create(pair_btn);
    lv_label_set_text(pair_l, "Pair New Device");
    lv_obj_center(pair_l);

    update_battery_status();
    rebuild_bond_list();
}

void agent_settings_timer_update(void)
{
    update_battery_status();

    if (label_ble_status) {
        lv_label_set_text(label_ble_status, g_ble_connected ? "Connected" : "Disconnected");
        lv_obj_set_style_text_color(label_ble_status, g_ble_connected ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF0000), 0);
    }

    int bond_count = agent_ble_get_bond_count();
    if (bond_count != last_bond_count) {
        rebuild_bond_list();
    }

    if (pair_btn) {
        bool pairing = agent_ble_is_pairing_mode();
        lv_obj_t *lbl = lv_obj_get_child(pair_btn, 0);
        if (pairing && strcmp(lv_label_get_text(lbl), "Pairing...") != 0) {
            lv_label_set_text(lbl, "Pairing...");
            lv_obj_set_style_bg_color(pair_btn, lv_color_hex(0x007700), 0);
        } else if (!pairing && strcmp(lv_label_get_text(lbl), "Pairing...") == 0) {
            lv_label_set_text(lbl, "Pair New Device");
            lv_obj_set_style_bg_color(pair_btn, lv_color_hex(COLOR_CYAN_DARK), 0);
        }
    }
}
