#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "agent_ble.hpp"
#include "agent_pmic.hpp"
#include "agent_viewer.hpp"
#include "agent_settings.hpp"

static const char *TAG = "agent_viewer";

#define DIAL_SIZE 206
#define DIAL_CX   (DIAL_SIZE / 2)
#define DIAL_CY   (DIAL_SIZE / 2)

static lv_obj_t *canvas;
static lv_draw_buf_t *draw_buf;
static lv_obj_t *label_stats;
static lv_obj_t *label_batt;
static lv_obj_t *label_ble;
static lv_obj_t *status_ring;
static lv_obj_t *tileview;
static lv_obj_t *tile_agent;
static lv_obj_t *tile_settings;

static float pulse_val = 0.0f;
static float pulse_dir = 0.02f;
static int   rot_angle = 0;
static int   success_countdown = 0;

static void draw_filled_circle(lv_layer_t *layer, int cx, int cy, int r, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = lv_pct(50);
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(layer, &dsc, &a);
}

static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t color)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = 2;
    dsc.p1.x = x1 * 256;
    dsc.p1.y = y1 * 256;
    dsc.p2.x = x2 * 256;
    dsc.p2.y = y2 * 256;
    lv_draw_line(layer, &dsc);
}

static void draw_triangle(lv_layer_t *layer, int x1, int y1, int x2, int y2, int x3, int y3, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    int mx = LV_MIN(LV_MIN(x1, x2), x3);
    int my = LV_MIN(LV_MIN(y1, y2), y3);
    int Mx = LV_MAX(LV_MAX(x1, x2), x3);
    int My = LV_MAX(LV_MAX(y1, y2), y3);
    lv_area_t a = { mx, my, Mx, My };
    lv_draw_rect(layer, &dsc, &a);
}

static void update_canvas(void)
{
    if (draw_buf == NULL) return;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_COVER);

    int cx = DIAL_CX, cy = DIAL_CY;
    float rad;
    int lx, ly;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.width = 2;

    switch (g_ble_state) {
    case AGENT_STATE_IDLE: {
        arc_dsc.color = lv_color_hex(0x007777);
        arc_dsc.radius = 30;
        arc_dsc.center.x = cx; arc_dsc.center.y = cy;
        arc_dsc.start_angle = 0; arc_dsc.end_angle = 3600;
        lv_draw_arc(&layer, &arc_dsc);
        rad = (rot_angle / 2.0f) * ((float)M_PI / 180.0f);
        lx = cx + (int)(25 * cos(rad));
        ly = cy + (int)(25 * sin(rad));
        draw_line(&layer, cx, cy, lx, ly, lv_color_hex(0x00FFFF));
        int r = 4 + (int)(3 * pulse_val);
        draw_filled_circle(&layer, cx, cy, r, lv_color_hex(0x00FFFF));
        break;
    }
    case AGENT_STATE_THINKING: {
        rad = (rot_angle * 1.5f) * ((float)M_PI / 180.0f);
        lx = cx + (int)(25 * cos(rad));
        ly = cy + (int)(25 * sin(rad));
        draw_line(&layer, cx, cy, lx, ly, lv_color_hex(0x9933FF));
        int r = 3 + (int)(5 * pulse_val);
        draw_filled_circle(&layer, cx, cy, r, lv_color_hex(0x9933FF));
        break;
    }
    case AGENT_STATE_WAITING: {
        lv_color_t c = (rot_angle / 30 % 2 == 0) ? lv_color_hex(0xFFAA00) : lv_color_hex(0xFF0000);
        draw_triangle(&layer, cx, cy - 14, cx - 14, cy + 12, cx + 14, cy + 12, c);
        break;
    }
    case AGENT_STATE_SUCCESS: {
        draw_filled_circle(&layer, cx, cy, 20, lv_color_hex(0x007777));
        draw_filled_circle(&layer, cx, cy, 15, lv_color_hex(0x00FF00));
        draw_line(&layer, cx - 7, cy, cx - 2, cy + 5, lv_color_hex(0xFFFFFF));
        draw_line(&layer, cx - 2, cy + 5, cx + 7, cy - 5, lv_color_hex(0xFFFFFF));
        if (success_countdown == 0) success_countdown = 160;
        break;
    }
    }

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_invalidate(canvas);
}

static void update_header(void)
{
    int batt = agent_pmic_get_battery_percent();
    bool charging = agent_pmic_is_charging();
    char buf[16];
    if (batt >= 0) snprintf(buf, sizeof(buf), "%d%%%s", batt, charging ? " +" : "");
    else snprintf(buf, sizeof(buf), "---");
    lv_label_set_text(label_batt, buf);

    if (g_ble_connected) {
        lv_label_set_text(label_ble, "BLE");
        lv_obj_set_style_text_color(label_ble, lv_color_hex(0x00FFFF), 0);
    } else {
        lv_label_set_text(label_ble, "---");
        lv_obj_set_style_text_color(label_ble, lv_color_hex(0x888888), 0);
    }
    if (g_ble_stats_changed) {
        g_ble_stats_changed = false;
        lv_label_set_text(label_stats, g_ble_stats_text);
    }
}

static void tap_cb(lv_event_t *e) { agent_ble_notify_action(1); }

static void timer_cb(lv_timer_t *t)
{
    pulse_val += pulse_dir;
    if (pulse_val >= 1.0f || pulse_val <= 0.0f) pulse_dir = -pulse_dir;
    rot_angle = (rot_angle + 6) % 360;
    if (success_countdown > 0) {
        success_countdown--;
        if (success_countdown == 0) g_ble_state = AGENT_STATE_IDLE;
    }

    lv_color_t ring_color;
    switch (g_ble_state) {
    case AGENT_STATE_IDLE:    ring_color = lv_color_hex(0x00FFFF); break;
    case AGENT_STATE_THINKING: ring_color = lv_color_hex(0x9933FF); break;
    case AGENT_STATE_WAITING:  ring_color = (rot_angle / 15 % 2 == 0) ? lv_color_hex(0xFFAA00) : lv_color_hex(0xFF0000); break;
    case AGENT_STATE_SUCCESS:  ring_color = lv_color_hex(0x00FF00); break;
    default:                   ring_color = lv_color_hex(0x00FFFF); break;
    }
    lv_obj_set_style_arc_color(status_ring, ring_color, LV_PART_INDICATOR);

    update_canvas();
    update_header();
    agent_settings_timer_update();
}

static void create_page_agent(lv_obj_t *tile)
{
    status_ring = lv_arc_create(tile);
    lv_obj_remove_flag(status_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(status_ring, 456, 456);
    lv_obj_center(status_ring);
    lv_arc_set_rotation(status_ring, 270);
    lv_arc_set_bg_angles(status_ring, 0, 360);
    lv_arc_set_range(status_ring, 0, 360);
    lv_arc_set_value(status_ring, 360);
    lv_obj_set_style_bg_opa(status_ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(status_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(status_ring, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(status_ring, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(status_ring, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_size(status_ring, 0, 0, LV_PART_KNOB);
    lv_obj_set_style_opa(status_ring, LV_OPA_TRANSP, LV_PART_KNOB);

    uint32_t stride = lv_draw_buf_width_to_stride(DIAL_SIZE, LV_COLOR_FORMAT_RGB565);
    uint32_t buf_size = stride * DIAL_SIZE;
    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    draw_buf = (lv_draw_buf_t *)heap_caps_malloc(sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "canvas buf=%p draw_buf=%p size=%d", buf, draw_buf, buf_size);
    if (draw_buf && buf) {
        lv_draw_buf_init(draw_buf, DIAL_SIZE, DIAL_SIZE, LV_COLOR_FORMAT_RGB565, stride, buf, buf_size);
    }

    canvas = lv_canvas_create(tile);
    lv_obj_set_size(canvas, DIAL_SIZE, DIAL_SIZE);
    lv_obj_center(canvas);
    if (draw_buf && buf) {
        lv_canvas_set_draw_buf(canvas, draw_buf);
    }

    label_ble = lv_label_create(tile);
    lv_label_set_text(label_ble, "---");
    lv_obj_set_style_text_color(label_ble, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_ble, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(label_ble, 140, 20);

    label_batt = lv_label_create(tile);
    lv_label_set_text(label_batt, "---");
    lv_obj_set_style_text_color(label_batt, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_batt, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(label_batt, 310, 20);

    label_stats = lv_label_create(tile);
    lv_label_set_text(label_stats, "Agent ready");
    lv_obj_set_style_text_color(label_stats, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label_stats, &lv_font_montserrat_20, 0);
    lv_obj_align(label_stats, LV_ALIGN_BOTTOM_MID, 0, -40);
}

void agent_viewer_init(void)
{
    ESP_LOGI(TAG, "Creating UI");

    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    tileview = lv_tileview_create(scr);
    lv_obj_remove_style_all(tileview);
    lv_obj_set_size(tileview, 466, 466);
    lv_obj_center(tileview);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);

    tile_agent = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_RIGHT);
    lv_obj_set_style_bg_opa(tile_agent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile_agent, 0, 0);
    create_page_agent(tile_agent);

    tile_settings = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_LEFT);
    lv_obj_set_style_bg_opa(tile_settings, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile_settings, 0, 0);
    agent_settings_create(tile_settings);

    lv_obj_add_flag(tileview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tileview, tap_cb, LV_EVENT_CLICKED, NULL);

    lv_timer_create(timer_cb, 25, NULL);
    update_canvas();
    update_header();

    bsp_display_unlock();
    ESP_LOGI(TAG, "UI ready");
}

static lv_obj_t *modal_cont = NULL;
static agent_viewer_pairing_cb_t modal_cb = NULL;

static void modal_accept_cb(lv_event_t *e)
{
    if (modal_cb) modal_cb(true);
    agent_viewer_hide_pairing_modal();
}

static void modal_reject_cb(lv_event_t *e)
{
    if (modal_cb) modal_cb(false);
    agent_viewer_hide_pairing_modal();
}

void agent_viewer_show_pairing_modal(uint32_t passkey, agent_viewer_pairing_cb_t cb)
{
    modal_cb = cb;
    if (bsp_display_lock(5000) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock display for pairing modal");
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    modal_cont = lv_obj_create(scr);
    lv_obj_set_size(modal_cont, 300, 220);
    lv_obj_center(modal_cont);
    lv_obj_set_style_bg_color(modal_cont, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(modal_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modal_cont, 2, 0);
    lv_obj_set_style_border_color(modal_cont, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_border_opa(modal_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(modal_cont, 12, 0);
    lv_obj_set_style_pad_all(modal_cont, 20, 0);

    lv_obj_t *title = lv_label_create(modal_cont);
    lv_label_set_text(title, "Pairing Request");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    char code[32];
    snprintf(code, sizeof(code), "Code: %06" PRIu32, passkey);
    lv_obj_t *code_label = lv_label_create(modal_cont);
    lv_label_set_text(code_label, code);
    lv_obj_set_style_text_color(code_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(code_label, &lv_font_montserrat_24, 0);
    lv_obj_align(code_label, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *hint = lv_label_create(modal_cont);
    lv_label_set_text(hint, "Does this code match\nwhat's on your PC?");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 75);

    lv_obj_t *yes = lv_btn_create(modal_cont);
    lv_obj_set_size(yes, 100, 40);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0x007700), 0);
    lv_obj_add_event_cb(yes, modal_accept_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes_l = lv_label_create(yes);
    lv_label_set_text(yes_l, "Accept");
    lv_obj_center(yes_l);

    lv_obj_t *no = lv_btn_create(modal_cont);
    lv_obj_set_size(no, 100, 40);
    lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(no, lv_color_hex(0x770000), 0);
    lv_obj_add_event_cb(no, modal_reject_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *no_l = lv_label_create(no);
    lv_label_set_text(no_l, "Reject");
    lv_obj_center(no_l);

    lv_obj_invalidate(scr);
    bsp_display_unlock();
}

void agent_viewer_hide_pairing_modal(void)
{
    if (!modal_cont) return;
    if (bsp_display_lock(5000) != ESP_OK) return;
    lv_obj_del(modal_cont);
    modal_cont = NULL;
    modal_cb = NULL;
    lv_obj_invalidate(lv_scr_act());
    bsp_display_unlock();
}
