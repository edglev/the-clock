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
#include "agent_printer.hpp"
#include "agent_ams.hpp"
#include "agent_ring.hpp"
#include "agent_viewer.hpp"
#include "agent_settings.hpp"

static const char *TAG = "agent_viewer";

#define COLOR_CYAN      0x009999
#define COLOR_CYAN_DIM  0x004D4D
#define DIAL_SIZE 206
#define DIAL_CX   (DIAL_SIZE / 2)
#define DIAL_CY   (DIAL_SIZE / 2)
#define NAV_HINT_EDGE_OFFSET 24

static lv_obj_t *canvas;
static lv_draw_buf_t *draw_buf;
static lv_obj_t *label_provider;
static lv_obj_t *label_project;
static lv_obj_t *label_branch;
static lv_obj_t *label_model_effort;
static lv_obj_t *label_stats;
static lv_obj_t *label_agent_count;
static lv_obj_t *label_batt_icon;
static lv_obj_t *label_batt;
static lv_obj_t *status_ring;
static lv_obj_t *tileview;
static lv_obj_t *tile_agent;
static lv_obj_t *tile_instances;
static lv_obj_t *tile_printer;
static lv_obj_t *tile_ams;
static lv_obj_t *tile_settings;
static lv_obj_t *instances_list;

static float pulse_val = 0.0f;
static float pulse_dir = 0.02f;
static int   rot_angle = 0;
static int   success_countdown = 0;
static uint32_t instances_signature = 0;
static uint8_t instances_refresh_tick = 0;
static agent_instance_info_t provider_summary_items[AGENT_MAX_INSTANCES];
static agent_instance_info_t instances_page_items[AGENT_MAX_INSTANCES];
static agent_instance_info_t focused_canvas;
static agent_instance_info_t focused_header;
static agent_instance_info_t focused_instances;
static agent_instance_info_t focused_timer;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static const char *agent_state_label(uint8_t state)
{
    switch (state) {
    case AGENT_STATE_THINKING: return "Thinking";
    case AGENT_STATE_WAITING:  return "Waiting";
    case AGENT_STATE_SUCCESS:  return "Success";
    case AGENT_STATE_IDLE:
    default:                   return "Idle";
    }
}

static lv_color_t agent_state_color(uint8_t state)
{
    switch (state) {
    case AGENT_STATE_THINKING: return lv_color_hex(0x9933FF);
    case AGENT_STATE_WAITING:  return lv_color_hex(0xFFAA00);
    case AGENT_STATE_SUCCESS:  return lv_color_hex(0x00FF00);
    case AGENT_STATE_IDLE:
    default:                   return lv_color_hex(0xFFFFFF);
    }
}

static const char *provider_label(const char *provider)
{
    return (provider && provider[0]) ? provider : "Agent";
}

static void format_model_effort(char *buf, size_t buf_size, const char *model, const char *effort)
{
    if (!buf || buf_size == 0) return;

    bool has_model = model && model[0];
    bool has_effort = effort && effort[0];
    if (has_model && has_effort) {
        snprintf(buf, buf_size, "%s %s", model, effort);
    } else if (has_model) {
        snprintf(buf, buf_size, "%s", model);
    } else if (has_effort) {
        snprintf(buf, buf_size, "%s", effort);
    } else {
        buf[0] = '\0';
    }
}

static void format_provider_summary(char *buf, size_t buf_size, const agent_instance_info_t *focused)
{
    if (!buf || buf_size == 0) return;

    int count = g_ble_connected ? agent_ble_get_instances(provider_summary_items, AGENT_MAX_INSTANCES) : 0;
    bool has_claude = false;
    bool has_codex = false;
    bool mixed = false;
    char first[AGENT_INSTANCE_PROVIDER_LEN + 1] = "";

    for (int i = 0; i < count; i++) {
        const char *provider = provider_label(provider_summary_items[i].provider);
        if (strcmp(provider, "Claude") == 0) has_claude = true;
        if (strcmp(provider, "Codex") == 0) has_codex = true;

        if (first[0] == '\0') {
            strncpy(first, provider, sizeof(first) - 1);
            first[sizeof(first) - 1] = '\0';
        } else if (strcmp(first, provider) != 0) {
            mixed = true;
        }
    }

    if (mixed && has_claude && has_codex) {
        snprintf(buf, buf_size, "Claude + Codex");
    } else if (mixed) {
        snprintf(buf, buf_size, "Mixed CLIs");
    } else if (focused) {
        snprintf(buf, buf_size, "%s", provider_label(focused->provider));
    } else if (first[0]) {
        snprintf(buf, buf_size, "%s", first);
    } else {
        buf[0] = '\0';
    }
}

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

    bool has_focus = g_ble_connected && agent_ble_get_focused_instance(&focused_canvas);

    if (!has_focus) {
        arc_dsc.color = lv_color_hex(0x555555);
        arc_dsc.radius = 30;
        arc_dsc.center.x = cx; arc_dsc.center.y = cy;
        arc_dsc.start_angle = 0; arc_dsc.end_angle = 3600;
        lv_draw_arc(&layer, &arc_dsc);
        draw_filled_circle(&layer, cx, cy, 5, lv_color_hex(0x888888));
        lv_canvas_finish_layer(canvas, &layer);
        lv_obj_invalidate(canvas);
        return;
    }

    switch (focused_canvas.state) {
    case AGENT_STATE_IDLE: {
        arc_dsc.color = lv_color_hex(COLOR_CYAN_DIM);
        arc_dsc.radius = 30;
        arc_dsc.center.x = cx; arc_dsc.center.y = cy;
        arc_dsc.start_angle = 0; arc_dsc.end_angle = 3600;
        lv_draw_arc(&layer, &arc_dsc);
        rad = (rot_angle / 2.0f) * ((float)M_PI / 180.0f);
        lx = cx + (int)(25 * cos(rad));
        ly = cy + (int)(25 * sin(rad));
        draw_line(&layer, cx, cy, lx, ly, lv_color_hex(COLOR_CYAN));
        int r = 4 + (int)(3 * pulse_val);
        draw_filled_circle(&layer, cx, cy, r, lv_color_hex(COLOR_CYAN));
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
        draw_filled_circle(&layer, cx, cy, 20, lv_color_hex(COLOR_CYAN_DIM));
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

    bool has_focus = g_ble_connected && agent_ble_get_focused_instance(&focused_header);
    int count = g_ble_connected ? agent_ble_get_instance_count() : 0;

    if (!g_ble_connected) {
        if (label_provider) lv_label_set_text(label_provider, "");
        if (label_project) lv_label_set_text(label_project, "");
        if (label_branch) lv_label_set_text(label_branch, "");
        if (label_model_effort) lv_label_set_text(label_model_effort, "");
        lv_label_set_text(label_stats, "Waiting for connection");
        lv_obj_set_style_text_color(label_stats, lv_color_hex(0xB0B0B0), 0);
        if (label_agent_count) lv_label_set_text(label_agent_count, "");
    } else if (has_focus) {
        char provider[32];
        const char *status = focused_header.status[0] ? focused_header.status : agent_state_label(focused_header.state);
        format_provider_summary(provider, sizeof(provider), &focused_header);
        if (label_provider) {
            lv_label_set_text(label_provider, provider);
            lv_obj_set_style_text_color(label_provider, lv_color_hex(COLOR_CYAN), 0);
        }
        if (label_project) {
            lv_label_set_text(label_project, focused_header.label);
            lv_obj_set_style_text_color(label_project, lv_color_hex(0xFFFFFF), 0);
        }
        if (label_branch) {
            lv_label_set_text(label_branch, focused_header.branch);
            lv_obj_set_style_text_color(label_branch, lv_color_hex(0xB0B0B0), 0);
        }
        if (label_model_effort) {
            char model_effort[48];
            format_model_effort(model_effort, sizeof(model_effort), focused_header.model, focused_header.effort);
            lv_label_set_text(label_model_effort, model_effort);
            lv_obj_set_style_text_color(label_model_effort, lv_color_hex(0xE0E0E0), 0);
        }
        lv_label_set_text(label_stats, status);
        lv_obj_set_style_text_color(label_stats, agent_state_color(focused_header.state), 0);

        if (label_agent_count) {
            char count_text[32];
            if (count > 1) {
                snprintf(count_text, sizeof(count_text), "%d agents running", count);
            } else {
                count_text[0] = '\0';
            }
            lv_label_set_text(label_agent_count, count_text);
            lv_obj_set_style_text_color(label_agent_count, lv_color_hex(0xB0B0B0), 0);
        }
    } else {
        if (label_provider) lv_label_set_text(label_provider, "");
        if (label_project) lv_label_set_text(label_project, "");
        if (label_branch) lv_label_set_text(label_branch, "");
        if (label_model_effort) lv_label_set_text(label_model_effort, "");
        lv_label_set_text(label_stats, "Waiting for events");
        lv_obj_set_style_text_color(label_stats, lv_color_hex(0xB0B0B0), 0);
        if (label_agent_count) {
            lv_label_set_text(label_agent_count, "Connected");
            lv_obj_set_style_text_color(label_agent_count, lv_color_hex(0xB0B0B0), 0);
        }
    }
    g_ble_stats_changed = false;
}

static void tap_cb(lv_event_t *e) { agent_ble_notify_action(1); }

static void set_status_ring_visible(bool visible)
{
    agent_ring_set_visible(status_ring, visible);
}

static bool tileview_settled_on(lv_obj_t *tile)
{
    if (!tileview || !tile) return false;
    if (lv_obj_is_scrolling(tileview)) return false;

    lv_obj_t *active_tile = lv_tileview_get_tile_active(tileview);
    return active_tile == tile || (active_tile == NULL && tile == tile_agent);
}

static void update_ring_visibility(void)
{
    set_status_ring_visible(tileview_settled_on(tile_agent));
    agent_printer_set_ring_visible(tileview_settled_on(tile_printer));
}

static void tileview_scroll_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL_BEGIN) {
        set_status_ring_visible(false);
        agent_printer_set_ring_visible(false);
    } else if (code == LV_EVENT_SCROLL_END) {
        update_ring_visibility();
    }
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

static void format_age(uint32_t updated_ms, char *buf, size_t buf_size)
{
    uint32_t elapsed_ms = now_ms() - updated_ms;
    uint32_t seconds = elapsed_ms / 1000;
    if (seconds < 60) {
        snprintf(buf, buf_size, "%lus", (unsigned long)seconds);
    } else if (seconds < 3600) {
        snprintf(buf, buf_size, "%lum", (unsigned long)(seconds / 60));
    } else {
        snprintf(buf, buf_size, "%luh", (unsigned long)(seconds / 3600));
    }
}

static void update_instances_page(void)
{
    if (!instances_list) return;

    bool has_focus = g_ble_connected && agent_ble_get_focused_instance(&focused_instances);
    int count = g_ble_connected ? agent_ble_get_instances(instances_page_items, AGENT_MAX_INSTANCES) : 0;

    uint32_t sig = 2166136261u;
    sig = hash_bytes(sig, &g_ble_connected, sizeof(g_ble_connected));
    sig = hash_bytes(sig, &count, sizeof(count));
    if (has_focus) sig = hash_bytes(sig, focused_instances.id, strlen(focused_instances.id));
    for (int i = 0; i < count; i++) {
        sig = hash_bytes(sig, &instances_page_items[i].state, sizeof(instances_page_items[i].state));
        sig = hash_bytes(sig, &instances_page_items[i].updated_ms, sizeof(instances_page_items[i].updated_ms));
        sig = hash_bytes(sig, instances_page_items[i].id, strlen(instances_page_items[i].id));
        sig = hash_bytes(sig, instances_page_items[i].label, strlen(instances_page_items[i].label));
        sig = hash_bytes(sig, instances_page_items[i].status, strlen(instances_page_items[i].status));
        sig = hash_bytes(sig, instances_page_items[i].provider, strlen(instances_page_items[i].provider));
        sig = hash_bytes(sig, instances_page_items[i].branch, strlen(instances_page_items[i].branch));
        sig = hash_bytes(sig, instances_page_items[i].metrics, strlen(instances_page_items[i].metrics));
        sig = hash_bytes(sig, instances_page_items[i].model, strlen(instances_page_items[i].model));
        sig = hash_bytes(sig, instances_page_items[i].effort, strlen(instances_page_items[i].effort));
        sig = hash_bytes(sig, &instances_page_items[i].priority_entered_ms, sizeof(instances_page_items[i].priority_entered_ms));
    }
    if (sig == instances_signature) return;
    instances_signature = sig;

    lv_obj_clean(instances_list);

    if (!g_ble_connected) {
        lv_obj_t *msg = lv_label_create(instances_list);
        lv_label_set_text(msg, "Waiting for connection");
        lv_obj_set_style_text_color(msg, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, 0);
        return;
    }

    if (count == 0) {
        lv_obj_t *msg = lv_label_create(instances_list);
        lv_label_set_text(msg, "No active agents");
        lv_obj_set_style_text_color(msg, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        bool is_focused = has_focus && strcmp(instances_page_items[i].id, focused_instances.id) == 0;
        lv_color_t color = agent_state_color(instances_page_items[i].state);

        lv_obj_t *row = lv_obj_create(instances_list);
        lv_obj_set_size(row, 330, 82);
        lv_obj_set_style_bg_color(row, lv_color_hex(is_focused ? 0x1A1A1A : 0x111111), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, is_focused ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, is_focused ? color : lv_color_hex(0x333333), 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, color, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);

        char title[96];
        snprintf(title, sizeof(title), "%s - %s",
                 provider_label(instances_page_items[i].provider),
                 instances_page_items[i].label);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, title);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, 270);
        lv_obj_set_height(name, 24);
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 34, 7);

        char meta[128];
        char model_effort[48];
        char age[12];
        const char *branch = instances_page_items[i].branch[0] ? instances_page_items[i].branch : "no branch";
        const char *metrics = instances_page_items[i].metrics[0] ? instances_page_items[i].metrics :
                              (instances_page_items[i].status[0] ? instances_page_items[i].status :
                               agent_state_label(instances_page_items[i].state));
        format_model_effort(model_effort, sizeof(model_effort),
                            instances_page_items[i].model,
                            instances_page_items[i].effort);
        format_age(instances_page_items[i].updated_ms, age, sizeof(age));
        snprintf(meta, sizeof(meta), "%s - %s - %s", branch, metrics, age);

        lv_obj_t *model = lv_label_create(row);
        lv_label_set_text(model, model_effort);
        lv_label_set_long_mode(model, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(model, 270);
        lv_obj_set_height(model, 20);
        lv_obj_set_style_text_color(model, lv_color_hex(0xC8C8C8), 0);
        lv_obj_set_style_text_font(model, &lv_font_montserrat_16, 0);
        lv_obj_align(model, LV_ALIGN_TOP_LEFT, 34, 34);

        lv_obj_t *status = lv_label_create(row);
        lv_label_set_text(status, meta);
        lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(status, 270);
        lv_obj_set_height(status, 18);
        lv_obj_set_style_text_color(status, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(status, &lv_font_montserrat_14, 0);
        lv_obj_align(status, LV_ALIGN_TOP_LEFT, 34, 58);
    }
}

static void timer_cb(lv_timer_t *t)
{
    pulse_val += pulse_dir;
    if (pulse_val >= 1.0f || pulse_val <= 0.0f) pulse_dir = -pulse_dir;
    rot_angle = (rot_angle + 6) % 360;
    if (success_countdown > 0) success_countdown--;

    bool has_focus = g_ble_connected && agent_ble_get_focused_instance(&focused_timer);

    lv_color_t ring_color;
    if (!has_focus) {
        ring_color = lv_color_hex(0x555555);
    } else {
        switch (focused_timer.state) {
        case AGENT_STATE_IDLE:    ring_color = lv_color_hex(COLOR_CYAN); break;
        case AGENT_STATE_THINKING: ring_color = lv_color_hex(0x9933FF); break;
        case AGENT_STATE_WAITING:  ring_color = (rot_angle / 15 % 2 == 0) ? lv_color_hex(0xFFAA00) : lv_color_hex(0xFF0000); break;
        case AGENT_STATE_SUCCESS:  ring_color = lv_color_hex(0x00FF00); break;
        default:                   ring_color = lv_color_hex(COLOR_CYAN); break;
        }
    }
    lv_obj_set_style_arc_color(status_ring, ring_color, LV_PART_INDICATOR);

    bool tile_scrolling = tileview && lv_obj_is_scrolling(tileview);
    if (!tile_scrolling) {
        update_ring_visibility();
        update_canvas();
        update_header();
        if (++instances_refresh_tick >= 20) {
            instances_refresh_tick = 0;
            update_instances_page();
        }
        agent_printer_timer_update();
        agent_ams_timer_update();
        agent_settings_timer_update();
    }
}

static void make_indicator_noninteractive(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_nav_hint(lv_obj_t *parent, const char *symbol, lv_align_t align, int x_ofs, int y_ofs)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, symbol);
    lv_obj_set_width(label, 28);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    make_indicator_noninteractive(label);
    lv_obj_align(label, align, x_ofs, y_ofs);
    return label;
}

static void create_page_indicators(void)
{
    create_nav_hint(tile_agent, LV_SYMBOL_DOWN, LV_ALIGN_BOTTOM_MID, 0, -NAV_HINT_EDGE_OFFSET);
    create_nav_hint(tile_agent, LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, -NAV_HINT_EDGE_OFFSET, 0);

    create_nav_hint(tile_instances, LV_SYMBOL_UP, LV_ALIGN_TOP_MID, 0, NAV_HINT_EDGE_OFFSET);

    create_nav_hint(tile_settings, LV_SYMBOL_LEFT, LV_ALIGN_LEFT_MID, NAV_HINT_EDGE_OFFSET, 0);

    create_nav_hint(tile_printer, LV_SYMBOL_LEFT, LV_ALIGN_LEFT_MID, NAV_HINT_EDGE_OFFSET, 0);
    create_nav_hint(tile_printer, LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, -NAV_HINT_EDGE_OFFSET, 0);
    create_nav_hint(tile_printer, LV_SYMBOL_DOWN, LV_ALIGN_BOTTOM_MID, 0, -NAV_HINT_EDGE_OFFSET);

    create_nav_hint(tile_ams, LV_SYMBOL_UP, LV_ALIGN_TOP_MID, 0, NAV_HINT_EDGE_OFFSET);
}

static void create_status_ring(lv_obj_t *parent)
{
    status_ring = agent_ring_create(parent, 0, 360, 360);
    lv_obj_set_style_arc_width(status_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(status_ring, AGENT_RING_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(status_ring, lv_color_hex(COLOR_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(status_ring, LV_OPA_COVER, LV_PART_INDICATOR);
    set_status_ring_visible(true);
}

static void create_page_agent(lv_obj_t *tile)
{
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

    label_provider = lv_label_create(tile);
    lv_label_set_text(label_provider, "");
    lv_obj_set_width(label_provider, 220);
    lv_obj_set_style_text_align(label_provider, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_provider, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_text_font(label_provider, &lv_font_montserrat_20, 0);
    lv_obj_align(label_provider, LV_ALIGN_TOP_MID, 0, 30);

    label_agent_count = lv_label_create(tile);
    lv_label_set_text(label_agent_count, "");
    lv_obj_set_width(label_agent_count, 220);
    lv_obj_set_style_text_align(label_agent_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_agent_count, lv_color_hex(0xB0B0B0), 0);
    lv_obj_set_style_text_font(label_agent_count, &lv_font_montserrat_16, 0);
    lv_obj_align(label_agent_count, LV_ALIGN_TOP_MID, 0, 52);

    label_project = lv_label_create(tile);
    lv_label_set_text(label_project, "");
    lv_label_set_long_mode(label_project, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label_project, 320);
    lv_obj_set_style_text_align(label_project, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_project, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label_project, &lv_font_montserrat_24, 0);
    lv_obj_align(label_project, LV_ALIGN_TOP_MID, 0, 82);

    label_branch = lv_label_create(tile);
    lv_label_set_text(label_branch, "");
    lv_label_set_long_mode(label_branch, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label_branch, 340);
    lv_obj_set_style_text_align(label_branch, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_branch, lv_color_hex(0xB0B0B0), 0);
    lv_obj_set_style_text_font(label_branch, &lv_font_montserrat_20, 0);
    lv_obj_align(label_branch, LV_ALIGN_TOP_MID, 0, 114);

    label_model_effort = lv_label_create(tile);
    lv_label_set_text(label_model_effort, "");
    lv_label_set_long_mode(label_model_effort, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label_model_effort, 340);
    lv_obj_set_style_text_align(label_model_effort, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_model_effort, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(label_model_effort, &lv_font_montserrat_20, 0);
    lv_obj_align(label_model_effort, LV_ALIGN_TOP_MID, 0, 142);

    label_stats = lv_label_create(tile);
    lv_label_set_text(label_stats, "Waiting for connection");
    lv_label_set_long_mode(label_stats, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label_stats, 340);
    lv_obj_set_style_text_align(label_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_stats, lv_color_hex(0xB0B0B0), 0);
    lv_obj_set_style_text_font(label_stats, &lv_font_montserrat_20, 0);
    lv_obj_align(label_stats, LV_ALIGN_TOP_MID, 0, 170);

    lv_obj_t *batt_row = lv_obj_create(tile);
    lv_obj_remove_style_all(batt_row);
    lv_obj_set_size(batt_row, 120, 24);
    lv_obj_align(batt_row, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_obj_clear_flag(batt_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(batt_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(batt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(batt_row, 8, 0);

    label_batt_icon = lv_label_create(batt_row);
    lv_label_set_text(label_batt_icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_color(label_batt_icon, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_batt_icon, &lv_font_montserrat_16, 0);

    label_batt = lv_label_create(batt_row);
    lv_label_set_text(label_batt, "---");
    lv_obj_set_style_text_color(label_batt, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_batt, &lv_font_montserrat_16, 0);
}

static void create_page_instances(lv_obj_t *tile)
{
    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "Agents");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    instances_list = lv_obj_create(tile);
    lv_obj_set_size(instances_list, 360, 318);
    lv_obj_align(instances_list, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_opa(instances_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(instances_list, 0, 0);
    lv_obj_set_style_outline_width(instances_list, 0, 0);
    lv_obj_set_style_shadow_width(instances_list, 0, 0);
    lv_obj_set_style_radius(instances_list, 0, 0);
    lv_obj_set_style_pad_all(instances_list, 8, 0);
    lv_obj_set_style_pad_row(instances_list, 10, 0);
    lv_obj_set_scroll_dir(instances_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(instances_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(instances_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(instances_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    update_instances_page();
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

    tile_agent = lv_tileview_add_tile(tileview, 0, 0, (lv_dir_t)(LV_DIR_RIGHT | LV_DIR_BOTTOM));
    lv_obj_set_style_bg_opa(tile_agent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile_agent, 0, 0);
    create_status_ring(tile_agent);
    create_page_agent(tile_agent);

    tile_instances = lv_tileview_add_tile(tileview, 0, 1, LV_DIR_TOP);
    lv_obj_set_style_bg_opa(tile_instances, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile_instances, 0, 0);
    create_page_instances(tile_instances);

    tile_printer = lv_tileview_add_tile(tileview, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT | LV_DIR_BOTTOM));
    lv_obj_set_style_bg_opa(tile_printer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile_printer, 0, 0);
    agent_printer_create(tile_printer);

    tile_ams = lv_tileview_add_tile(tileview, 1, 1, LV_DIR_TOP);
    lv_obj_set_style_bg_opa(tile_ams, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile_ams, 0, 0);
    agent_ams_create(tile_ams);

    tile_settings = lv_tileview_add_tile(tileview, 2, 0, LV_DIR_LEFT);
    lv_obj_set_style_bg_opa(tile_settings, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile_settings, 0, 0);
    agent_settings_create(tile_settings);

    create_page_indicators();
    lv_tileview_set_tile(tileview, tile_agent, LV_ANIM_OFF);
    update_ring_visibility();

    lv_obj_add_flag(tileview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tileview, tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(tileview, tileview_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(tileview, tileview_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    lv_timer_create(timer_cb, 25, NULL);
    update_canvas();
    update_header();
    update_instances_page();
    agent_printer_timer_update();
    agent_ams_timer_update();

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
    lv_obj_set_style_border_color(modal_cont, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_border_opa(modal_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(modal_cont, 12, 0);
    lv_obj_set_style_pad_all(modal_cont, 20, 0);

    lv_obj_t *title = lv_label_create(modal_cont);
    lv_label_set_text(title, "Pairing Request");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CYAN), 0);
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
