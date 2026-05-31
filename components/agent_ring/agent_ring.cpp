#include "agent_ring.hpp"

static void set_noninteractive(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *agent_ring_create(lv_obj_t *parent, int min_value, int max_value, int value)
{
    lv_obj_t *ring = lv_arc_create(parent);
    lv_obj_set_size(ring, AGENT_RING_SIZE, AGENT_RING_SIZE);
    lv_obj_center(ring);
    lv_arc_set_rotation(ring, AGENT_RING_ROTATION);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_range(ring, min_value, max_value);
    lv_arc_set_value(ring, value);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_size(ring, 0, 0, LV_PART_KNOB);
    lv_obj_set_style_opa(ring, LV_OPA_TRANSP, LV_PART_KNOB);
    set_noninteractive(ring);
    lv_obj_move_background(ring);
    return ring;
}

void agent_ring_set_visible(lv_obj_t *ring, bool visible)
{
    if (!ring) return;

    bool hidden = lv_obj_has_flag(ring, LV_OBJ_FLAG_HIDDEN);
    if (visible && hidden) {
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_HIDDEN);
    } else if (!visible && !hidden) {
        lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
    }
}
