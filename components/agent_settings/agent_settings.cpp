#include <string.h>
#include <inttypes.h>
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "agent_ble.hpp"
#include "agent_bambu.hpp"
#include "agent_pmic.hpp"
#include "agent_orientation.hpp"

static lv_obj_t *page_main = NULL;
static lv_obj_t *page_bluetooth = NULL;
static lv_obj_t *page_wifi = NULL;
static lv_obj_t *label_ble_status = NULL;
static lv_obj_t *label_ble_summary = NULL;
static lv_obj_t *label_battery_status = NULL;
static lv_obj_t *label_brightness_val = NULL;
static lv_obj_t *label_orientation_status = NULL;
static lv_obj_t *orientation_lock_switch = NULL;
static lv_obj_t *label_wifi_summary = NULL;
static lv_obj_t *label_wifi_state = NULL;
static lv_obj_t *label_wifi_ssid = NULL;
static lv_obj_t *label_wifi_ip = NULL;
static lv_obj_t *label_wifi_cloud = NULL;
static lv_obj_t *label_wifi_printer = NULL;
static lv_obj_t *wifi_list_cont = NULL;
static int current_brightness = 100;
static lv_obj_t *bond_list_cont = NULL;
static lv_obj_t *pair_btn = NULL;
static int last_bond_count = -1;
static int last_wifi_count = -1;

#define COLOR_CYAN       0x009999
#define COLOR_CYAN_DARK  0x00384D

static void show_page(lv_obj_t *page)
{
    if (page_main) lv_obj_add_flag(page_main, LV_OBJ_FLAG_HIDDEN);
    if (page_bluetooth) lv_obj_add_flag(page_bluetooth, LV_OBJ_FLAG_HIDDEN);
    if (page_wifi) lv_obj_add_flag(page_wifi, LV_OBJ_FLAG_HIDDEN);
    if (page) lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
}

static void show_main_cb(lv_event_t *e) { show_page(page_main); }
static void show_bluetooth_cb(lv_event_t *e) { show_page(page_bluetooth); }
static void show_wifi_cb(lv_event_t *e) { show_page(page_wifi); }

static lv_obj_t *create_page(lv_obj_t *tile, bool hidden)
{
    lv_obj_t *page = lv_obj_create(tile);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 466, 466);
    lv_obj_center(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, (lv_obj_flag_t)(LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    if (hidden) {
        lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    }
    return page;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 400);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static lv_obj_t *create_back_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 54, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, -94, 46);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, show_main_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *create_list_container(lv_obj_t *parent, int y)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 360, 150);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 6, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return cont;
}

static lv_obj_t *create_list_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 340, 34);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x444444), 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return row;
}

static lv_obj_t *create_nav_row(lv_obj_t *parent, const char *title, lv_obj_t **summary, int y, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 360, 56);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title_l = lv_label_create(row);
    lv_label_set_text(title_l, title);
    lv_obj_set_style_text_color(title_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_l, &lv_font_montserrat_16, 0);
    lv_obj_align(title_l, LV_ALIGN_LEFT_MID, 8, -9);

    *summary = lv_label_create(row);
    lv_label_set_text(*summary, "---");
    lv_obj_set_width(*summary, 280);
    lv_label_set_long_mode(*summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(*summary, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(*summary, &lv_font_montserrat_14, 0);
    lv_obj_align(*summary, LV_ALIGN_LEFT_MID, 8, 13);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_16, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -8, 0);
    return row;
}

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

static void update_orientation_status(void)
{
    if (!label_orientation_status) return;

    char buf[24];
    int angle = agent_orientation_get_angle_deg();
    if (agent_orientation_is_locked()) {
        snprintf(buf, sizeof(buf), "Locked %d deg", angle);
    } else {
        snprintf(buf, sizeof(buf), "Auto %d deg", angle);
    }
    lv_label_set_text(label_orientation_status, buf);
    lv_obj_set_style_text_color(label_orientation_status,
                                agent_orientation_is_locked() ? lv_color_hex(COLOR_CYAN) : lv_color_hex(0x888888),
                                0);
}

static void orientation_lock_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    bool locked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    agent_orientation_set_locked(locked);
    update_orientation_status();
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

static void forget_wifi_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    agent_bambu_delete_wifi_network(index);
    last_wifi_count = -1;
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

        lv_obj_t *row = create_list_row(bond_list_cont);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, display_str);
        lv_obj_set_width(lbl, 240);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 64, 26);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x770000), 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(btn, forget_bond_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *btn_l = lv_label_create(btn);
        lv_label_set_text(btn_l, "Forget");
        lv_obj_set_style_text_font(btn_l, &lv_font_montserrat_12, 0);
        lv_obj_center(btn_l);
    }

    if (count < 4) {
        lv_obj_t *clear_btn = lv_btn_create(bond_list_cont);
        lv_obj_set_size(clear_btn, 180, 30);
        lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0x550000), 0);
        lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(clear_btn, forget_all_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *clear_l = lv_label_create(clear_btn);
        lv_label_set_text(clear_l, "Clear All");
        lv_obj_set_style_text_font(clear_l, &lv_font_montserrat_14, 0);
        lv_obj_center(clear_l);
    }
}

static const char *wifi_state_text(agent_bambu_state_t state)
{
    switch (state) {
    case AGENT_BAMBU_NOT_CONFIGURED: return "Not configured";
    case AGENT_BAMBU_STANDBY: return "Standby";
    case AGENT_BAMBU_WIFI_CONNECTING: return "Wi-Fi connecting";
    case AGENT_BAMBU_WIFI_CONNECTED: return "Wi-Fi connected";
    case AGENT_BAMBU_CLOUD_CONNECTING: return "Cloud connecting";
    case AGENT_BAMBU_CLOUD_CONNECTED: return "Cloud connected";
    case AGENT_BAMBU_ERROR: return "Error";
    default: return "Unknown";
    }
}

static void update_wifi_status(void)
{
    agent_bambu_status_t status = {};
    agent_bambu_get_status(&status);

    const char *state = wifi_state_text(status.state);
    if (label_wifi_summary) {
        lv_label_set_text(label_wifi_summary, state);
        lv_obj_set_style_text_color(label_wifi_summary,
                                    status.cloud_connected ? lv_color_hex(0x00FF00) :
                                    status.configured ? lv_color_hex(0xCCCCCC) : lv_color_hex(0x888888),
                                    0);
    }
    if (!label_wifi_state) return;

    char buf[80];
    snprintf(buf, sizeof(buf), "State: %s", state);
    lv_label_set_text(label_wifi_state, buf);
    lv_obj_set_style_text_color(label_wifi_state,
                                status.state == AGENT_BAMBU_ERROR ? lv_color_hex(0xFF5555) :
                                status.cloud_connected ? lv_color_hex(0x00FF00) : lv_color_hex(0xCCCCCC),
                                0);

    snprintf(buf, sizeof(buf), "SSID: %s", status.ssid[0] ? status.ssid : "--");
    lv_label_set_text(label_wifi_ssid, buf);
    snprintf(buf, sizeof(buf), "IP: %s", status.ip[0] ? status.ip : "--");
    lv_label_set_text(label_wifi_ip, buf);
    snprintf(buf, sizeof(buf), "Cloud: %s", status.detail[0] ? status.detail : "--");
    lv_label_set_text(label_wifi_cloud, buf);
    snprintf(buf, sizeof(buf), "Printer: %s", status.printer[0] ? status.printer : "--");
    lv_label_set_text(label_wifi_printer, buf);
}

static void rebuild_wifi_list(void)
{
    if (!wifi_list_cont) return;
    lv_obj_clean(wifi_list_cont);

    agent_bambu_wifi_network_t networks[AGENT_BAMBU_MAX_WIFI_NETWORKS] = {};
    int count = agent_bambu_get_wifi_networks(networks, AGENT_BAMBU_MAX_WIFI_NETWORKS);
    if (count > AGENT_BAMBU_MAX_WIFI_NETWORKS) count = AGENT_BAMBU_MAX_WIFI_NETWORKS;
    last_wifi_count = count;

    if (count == 0) {
        lv_obj_t *no = lv_label_create(wifi_list_cont);
        lv_label_set_text(no, "No saved Wi-Fi");
        lv_obj_set_style_text_color(no, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(no, &lv_font_montserrat_14, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t *row = create_list_row(wifi_list_cont);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, networks[i].ssid);
        lv_obj_set_width(lbl, 240);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 64, 26);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x770000), 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(btn, forget_wifi_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *btn_l = lv_label_create(btn);
        lv_label_set_text(btn_l, "Remove");
        lv_obj_set_style_text_font(btn_l, &lv_font_montserrat_12, 0);
        lv_obj_center(btn_l);
    }
}

static void create_main_page(lv_obj_t *tile)
{
    page_main = create_page(tile, false);

    create_label(page_main, "Settings", 30, &lv_font_montserrat_24, COLOR_CYAN);

    lv_obj_t *bright_label = lv_label_create(page_main);
    lv_label_set_text(bright_label, "Brightness");
    lv_obj_set_style_text_color(bright_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(bright_label, &lv_font_montserrat_16, 0);
    lv_obj_align(bright_label, LV_ALIGN_TOP_MID, -95, 84);

    label_brightness_val = lv_label_create(page_main);
    lv_label_set_text(label_brightness_val, "100%");
    lv_obj_set_width(label_brightness_val, 110);
    lv_obj_set_style_text_align(label_brightness_val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(label_brightness_val, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_brightness_val, &lv_font_montserrat_16, 0);
    lv_obj_align(label_brightness_val, LV_ALIGN_TOP_MID, 100, 84);

    lv_obj_t *slider = lv_slider_create(page_main);
    lv_obj_set_size(slider, 360, 20);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 118);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, current_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_CYAN), LV_PART_INDICATOR);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *batt_label = lv_label_create(page_main);
    lv_label_set_text(batt_label, "Battery");
    lv_obj_set_style_text_color(batt_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(batt_label, &lv_font_montserrat_16, 0);
    lv_obj_align(batt_label, LV_ALIGN_TOP_MID, -95, 155);

    label_battery_status = lv_label_create(page_main);
    lv_label_set_text(label_battery_status, "---");
    lv_obj_set_width(label_battery_status, 180);
    lv_label_set_long_mode(label_battery_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label_battery_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(label_battery_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_battery_status, &lv_font_montserrat_16, 0);
    lv_obj_align(label_battery_status, LV_ALIGN_TOP_MID, 100, 155);

    lv_obj_t *orientation_row = lv_obj_create(page_main);
    lv_obj_set_size(orientation_row, 360, 56);
    lv_obj_align(orientation_row, LV_ALIGN_TOP_MID, 0, 205);
    lv_obj_set_style_bg_color(orientation_row, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_opa(orientation_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(orientation_row, 1, 0);
    lv_obj_set_style_border_color(orientation_row, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(orientation_row, 8, 0);
    lv_obj_set_style_pad_all(orientation_row, 8, 0);
    lv_obj_clear_flag(orientation_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(orientation_row, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *orientation_title = lv_label_create(orientation_row);
    lv_label_set_text(orientation_title, "Orientation lock");
    lv_obj_set_style_text_color(orientation_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(orientation_title, &lv_font_montserrat_16, 0);
    lv_obj_align(orientation_title, LV_ALIGN_LEFT_MID, 8, -9);

    label_orientation_status = lv_label_create(orientation_row);
    lv_label_set_text(label_orientation_status, "Auto 0 deg");
    lv_obj_set_width(label_orientation_status, 220);
    lv_label_set_long_mode(label_orientation_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(label_orientation_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_orientation_status, &lv_font_montserrat_14, 0);
    lv_obj_align(label_orientation_status, LV_ALIGN_LEFT_MID, 8, 13);

    orientation_lock_switch = lv_switch_create(orientation_row);
    lv_obj_align(orientation_lock_switch, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(orientation_lock_switch, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(orientation_lock_switch, orientation_lock_cb, LV_EVENT_VALUE_CHANGED, NULL);

    create_nav_row(page_main, "Bluetooth", &label_ble_summary, 275, show_bluetooth_cb);
    create_nav_row(page_main, "Wi-Fi", &label_wifi_summary, 345, show_wifi_cb);
    update_orientation_status();
}

static void create_bluetooth_page(lv_obj_t *tile)
{
    page_bluetooth = create_page(tile, true);

    create_back_button(page_bluetooth);
    create_label(page_bluetooth, "Bluetooth", 38, &lv_font_montserrat_24, COLOR_CYAN);
    label_ble_status = create_label(page_bluetooth, "Disconnected", 90, &lv_font_montserrat_20, 0xFF5555);

    lv_obj_t *paired_label = create_label(page_bluetooth, "Paired Devices", 132, &lv_font_montserrat_16, 0xFFFFFF);

    bond_list_cont = create_list_container(page_bluetooth, 164);

    pair_btn = lv_btn_create(page_bluetooth);
    lv_obj_set_size(pair_btn, 220, 34);
    lv_obj_align(pair_btn, LV_ALIGN_TOP_MID, 0, 382);
    lv_obj_set_style_bg_color(pair_btn, lv_color_hex(COLOR_CYAN_DARK), 0);
    lv_obj_add_flag(pair_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(pair_btn, pair_new_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pair_l = lv_label_create(pair_btn);
    lv_label_set_text(pair_l, "Pair New Device");
    lv_obj_center(pair_l);

    (void)paired_label;
}

static void create_wifi_page(lv_obj_t *tile)
{
    page_wifi = create_page(tile, true);

    create_back_button(page_wifi);
    create_label(page_wifi, "Wi-Fi", 38, &lv_font_montserrat_24, COLOR_CYAN);
    label_wifi_state = create_label(page_wifi, "State: --", 82, &lv_font_montserrat_14, 0xCCCCCC);
    label_wifi_ssid = create_label(page_wifi, "SSID: --", 108, &lv_font_montserrat_14, 0xCCCCCC);
    label_wifi_ip = create_label(page_wifi, "IP: --", 134, &lv_font_montserrat_14, 0xCCCCCC);
    label_wifi_cloud = create_label(page_wifi, "Cloud: --", 160, &lv_font_montserrat_14, 0xCCCCCC);
    label_wifi_printer = create_label(page_wifi, "Printer: --", 186, &lv_font_montserrat_14, 0xCCCCCC);

    lv_obj_t *saved_label = create_label(page_wifi, "Saved Networks", 222, &lv_font_montserrat_16, 0xFFFFFF);
    (void)saved_label;

    wifi_list_cont = create_list_container(page_wifi, 250);
}

void agent_settings_create(lv_obj_t *tile)
{
    create_main_page(tile);
    create_bluetooth_page(tile);
    create_wifi_page(tile);
    update_battery_status();
    update_orientation_status();
    update_wifi_status();
    rebuild_bond_list();
    rebuild_wifi_list();
}

void agent_settings_timer_update(void)
{
    update_battery_status();
    update_orientation_status();
    update_wifi_status();

    if (label_ble_status) {
        lv_label_set_text(label_ble_status, g_ble_connected ? "Connected" : "Disconnected");
        lv_obj_set_style_text_color(label_ble_status, g_ble_connected ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF5555), 0);
    }
    if (label_ble_summary) {
        lv_label_set_text(label_ble_summary, g_ble_connected ? "Connected" : "Disconnected");
        lv_obj_set_style_text_color(label_ble_summary, g_ble_connected ? lv_color_hex(0x00FF00) : lv_color_hex(0x888888), 0);
    }

    int bond_count = agent_ble_get_bond_count();
    if (bond_count != last_bond_count) {
        rebuild_bond_list();
    }

    int wifi_count = agent_bambu_get_wifi_networks(NULL, 0);
    if (wifi_count != last_wifi_count) {
        rebuild_wifi_list();
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
