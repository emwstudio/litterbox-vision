#include "catvision_ui.h"

lv_obj_t *catvision_label(lv_obj_t *parent, const char *text, int x, int y, int size, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    const lv_font_t *font = size <= 16 ? &lv_font_catvision_16 : &lv_font_montserrat_24;
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    return label;
}

lv_obj_t *catvision_button(lv_obj_t *parent, const char *text, int x, int y, int width, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 44);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x25313d), LV_PART_MAIN);
    lv_obj_set_style_radius(button, 10, LV_PART_MAIN);
    lv_obj_t *label = catvision_label(button, text, 0, 0, 16, 0xf4f6f8);
    lv_obj_center(label);
    if (callback) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }
    return button;
}

lv_obj_t *catvision_panel(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x344251), LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}
