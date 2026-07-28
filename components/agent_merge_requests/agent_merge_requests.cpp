#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "agent_ble.hpp"
#include "agent_ring.hpp"
#include "agent_merge_requests.hpp"

#define COLOR_GITLAB 0xFC6D26
#define COLOR_READY  0x22C55E

static lv_obj_t *status_ring;
static lv_obj_t *label_count;
static lv_obj_t *requests_list;
static agent_merge_request_status_t page_status;
static uint32_t page_signature;
static uint32_t observed_sequence;
static bool unseen;

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

static lv_color_t request_status_color(const char *status)
{
    if (status && strcmp(status, "Ready") == 0) return lv_color_hex(COLOR_READY);
    if (status && strcmp(status, "Needs approval") == 0) return lv_color_hex(0xF59E0B);
    return lv_color_hex(0x94A3B8);
}

static void show_message(const char *text)
{
    lv_obj_clean(requests_list);
    lv_obj_t *message = create_label(requests_list, text, &lv_font_montserrat_20,
                                     lv_color_hex(0x888888), 320, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_line_space(message, 8, 0);
}

static void rebuild_list(const agent_merge_request_status_t *status)
{
    if (!status || !status->valid) {
        lv_label_set_text(label_count, "Waiting for GitLab");
        show_message("Connecting...");
        return;
    }

    if (status->auth_invalid) {
        lv_label_set_text(label_count, "Reauthenticate");
        show_message("PAT rejected by GitLab\nReauthenticate from host");
        return;
    }

    char count_text[40];
    if (status->count == 0) {
        lv_label_set_text(label_count, "0 need review");
        show_message("All caught up");
        return;
    }
    snprintf(count_text, sizeof(count_text), "%u need review", status->count);
    lv_label_set_text(label_count, count_text);
    lv_obj_clean(requests_list);

    for (int i = 0; i < status->count && i < AGENT_MR_MAX_COUNT; i++) {
        const agent_merge_request_t *mr = &status->items[i];
        if (!mr->id[0]) continue;

        lv_obj_t *row = lv_obj_create(requests_list);
        lv_obj_set_size(row, 342, 68);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x333333), 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        set_noninteractive(row);

        lv_obj_t *reference = create_label(row,
            mr->reference[0] ? mr->reference : "Merge request",
            &lv_font_montserrat_14, lv_color_hex(COLOR_GITLAB), 215, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(reference, LV_ALIGN_TOP_LEFT, 12, 7);

        lv_obj_t *state = create_label(row, mr->status, &lv_font_montserrat_14,
                                       request_status_color(mr->status), 100, LV_TEXT_ALIGN_RIGHT);
        lv_obj_align(state, LV_ALIGN_TOP_RIGHT, -12, 7);

        lv_obj_t *title = create_label(row, mr->title, &lv_font_montserrat_16,
                                       lv_color_hex(0xFFFFFF), 318, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(title, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    }
}

void agent_merge_requests_create(lv_obj_t *tile)
{
    status_ring = agent_ring_create(tile, 0, 360, 360);
    lv_obj_set_style_arc_width(status_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(status_ring, AGENT_RING_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(status_ring, lv_color_hex(COLOR_GITLAB), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(status_ring, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_t *title = create_label(tile, "Merge Requests", &lv_font_montserrat_28,
                                   lv_color_hex(COLOR_GITLAB), 300, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    label_count = create_label(tile, "Waiting for GitLab", &lv_font_montserrat_16,
                               lv_color_hex(0xD0D0D0), 280, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label_count, LV_ALIGN_TOP_MID, 0, 68);

    requests_list = lv_obj_create(tile);
    lv_obj_set_size(requests_list, 366, 318);
    lv_obj_align(requests_list, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_opa(requests_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(requests_list, 0, 0);
    lv_obj_set_style_outline_width(requests_list, 0, 0);
    lv_obj_set_style_shadow_width(requests_list, 0, 0);
    lv_obj_set_style_pad_all(requests_list, 6, 0);
    lv_obj_set_style_pad_row(requests_list, 8, 0);
    lv_obj_set_scroll_dir(requests_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(requests_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(requests_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(requests_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    agent_merge_requests_timer_update();
}

void agent_merge_requests_timer_update(void)
{
    agent_merge_request_status_t next = {};
    bool valid = agent_ble_get_merge_requests(&next);
    if (valid && next.sequence != observed_sequence) {
        observed_sequence = next.sequence;
        if (next.new_items) unseen = true;
    }

    uint32_t sig = 2166136261u;
    sig = hash_bytes(sig, &valid, sizeof(valid));
    if (valid) sig = hash_bytes(sig, &next, sizeof(next));
    if (sig != page_signature) {
        page_signature = sig;
        page_status = next;
        rebuild_list(valid ? &page_status : NULL);
    }

    if (status_ring) {
        lv_obj_set_style_arc_color(status_ring,
            unseen && agent_merge_requests_alert_on() ? lv_color_hex(0xFFFFFF) : lv_color_hex(COLOR_GITLAB),
            LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(status_ring,
            unseen && !agent_merge_requests_alert_on() ? LV_OPA_30 : LV_OPA_COVER,
            LV_PART_INDICATOR);
    }
}

void agent_merge_requests_set_ring_visible(bool visible)
{
    agent_ring_set_visible(status_ring, visible);
}

void agent_merge_requests_mark_seen(void)
{
    unseen = false;
}

bool agent_merge_requests_has_unseen(void)
{
    return unseen;
}

bool agent_merge_requests_alert_on(void)
{
    return ((now_ms() / 400u) % 2u) == 0;
}
