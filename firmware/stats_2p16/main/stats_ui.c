#include "stats_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "catvision_display.h"
#include "catvision_ui.h"
#include "stats_signature.h"

LV_FONT_DECLARE(lv_font_card_24_bold);
LV_FONT_DECLARE(lv_font_card_22_bold);
LV_FONT_DECLARE(lv_font_card_16);
LV_FONT_DECLARE(lv_font_card_16_bold);

typedef struct {
    lv_obj_t *panel;
    lv_obj_t *name;
    catvision_image_t *avatar;
    catvision_image_t *badges[2];
    lv_obj_t *badge_x_labels[2];
    lv_obj_t *badge_counts[2];
    bool avatar_loaded;
} cat_card_t;

typedef struct {
    const char *cat_id;
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} embedded_avatar_t;

typedef struct {
    const char *box_id;
    const uint8_t *start;
    const uint8_t *end;
} embedded_box_t;

extern const uint8_t qiuqiu_png_start[] asm("_binary_qiuqiu_png_start");
extern const uint8_t qiuqiu_png_end[] asm("_binary_qiuqiu_png_end");
extern const uint8_t lvlv_png_start[] asm("_binary_lvlv_png_start");
extern const uint8_t lvlv_png_end[] asm("_binary_lvlv_png_end");
extern const uint8_t bobo_png_start[] asm("_binary_bobo_png_start");
extern const uint8_t bobo_png_end[] asm("_binary_bobo_png_end");
extern const uint8_t baobao_png_start[] asm("_binary_baobao_png_start");
extern const uint8_t baobao_png_end[] asm("_binary_baobao_png_end");
extern const uint8_t box_1_png_start[] asm("_binary_box_1_png_start");
extern const uint8_t box_1_png_end[] asm("_binary_box_1_png_end");
extern const uint8_t box_2_png_start[] asm("_binary_box_2_png_start");
extern const uint8_t box_2_png_end[] asm("_binary_box_2_png_end");
extern const uint8_t box_3_png_start[] asm("_binary_box_3_png_start");
extern const uint8_t box_3_png_end[] asm("_binary_box_3_png_end");
extern const uint8_t box_4_png_start[] asm("_binary_box_4_png_start");
extern const uint8_t box_4_png_end[] asm("_binary_box_4_png_end");

static const embedded_avatar_t avatars[] = {
    {"3c4fafeb-b168-4136-b9f2-99c401872019", "球球", qiuqiu_png_start, qiuqiu_png_end},
    {"4c9c4c30-6fa1-4a81-b8da-3d0c12d78976", "驴驴", lvlv_png_start, lvlv_png_end},
    {"80ad9f7d-2d75-4605-89ae-7d43cd333305", "啵啵", bobo_png_start, bobo_png_end},
    {"034dc402-51ea-4ff0-9085-2a6abbe422c8", "宝宝", baobao_png_start, baobao_png_end},
};

static const embedded_box_t boxes[] = {
    {"box-1", box_1_png_start, box_1_png_end},
    {"box-2", box_2_png_start, box_2_png_end},
    {"box-3", box_3_png_start, box_3_png_end},
    {"box-4", box_4_png_start, box_4_png_end},
};

static lv_obj_t *root;
static lv_obj_t *status;
static catvision_image_t *live_frame;
static cat_card_t cards[4];

static esp_err_t load_embedded_avatar(catvision_image_t *image, const char *cat_id)
{
    if (!image || !cat_id) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int index = 0; index < 4; index++) {
        const embedded_avatar_t *avatar = &avatars[index];
        if (strcmp(avatar->cat_id, cat_id) == 0) {
            return catvision_image_update(image, avatar->start, avatar->end - avatar->start, 72, 72);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t load_embedded_box(catvision_image_t *image, const char *box_id)
{
    if (!image || !box_id) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int index = 0; index < 4; index++) {
        const embedded_box_t *box = &boxes[index];
        if (strcmp(box->box_id, box_id) == 0) {
            return catvision_image_update(image, box->start, box->end - box->start, 28, 28);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static uint32_t displayed_content_crc;

static void hide_badges_from(cat_card_t *card, int first_hidden)
{
    for (int badge = first_hidden; badge < 2; badge++) {
        lv_obj_add_flag(card->badges[badge]->object, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card->badge_x_labels[badge], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card->badge_counts[badge], LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_badges(cat_card_t *card)
{
    hide_badges_from(card, 0);
}

static int count_text_width(const char *text)
{
    int width = 0;
    for (size_t index = 0; text[index]; index++) {
        uint32_t next = text[index + 1] ? (uint8_t)text[index + 1] : 0;
        width += lv_font_get_glyph_width(&lv_font_card_22_bold, (uint8_t)text[index], next);
    }
    return width;
}

static int count_label_x(int badge_x)
{
    return badge_x + 28 + 2 * lv_font_get_glyph_width(&lv_font_card_16_bold, ' ', 'X');
}

static int count_number_x(int badge_x)
{
    return count_label_x(badge_x) + lv_font_get_glyph_width(&lv_font_card_16_bold, 'X', 0) +
           lv_font_get_glyph_width(&lv_font_card_16_bold, ' ', '2');
}

static void align_badges(cat_card_t *card)
{
    const char *first_count = lv_label_get_text(card->badge_counts[0]);
    int space_width = 2 * lv_font_get_glyph_width(&lv_font_card_22_bold, ' ', 'X');
    int text_width = count_text_width(first_count);
    int second_x = lv_obj_get_x(card->badge_counts[0]) + text_width + space_width;
    lv_obj_set_x(card->badges[1]->object, second_x);
    lv_obj_set_x(card->badge_x_labels[1], count_label_x(second_x));
    lv_obj_set_x(card->badge_counts[1], count_number_x(second_x));
    ESP_LOGI("stats_ui", "badge_layout second_x=%d space=%d text_width=%d", second_x, space_width, text_width);
}

static void create_card(int index)
{
    cat_card_t *card = &cards[index];
    int x = 10 + (index % 2) * 235;
    int y = 246 + (index / 2) * 112;
    card->panel = catvision_panel(root, x, y, 225, 106, 0x080c10);
    lv_obj_set_style_bg_opa(card->panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(card->panel, 0, LV_PART_MAIN);
    card->avatar = catvision_image_create(card->panel, 2, 16, 72, 72);
    card->name = catvision_label(card->panel, avatars[index].name, 88, 16, 24, 0xf4f6f8);
    lv_obj_set_style_text_font(card->name, &lv_font_card_24_bold, LV_PART_MAIN);
    lv_obj_set_width(card->name, 137);
    lv_label_set_long_mode(card->name, LV_LABEL_LONG_MODE_DOTS);
    for (int badge = 0; badge < 2; badge++) {
        int badge_x = 88;
        int badge_y = 58;
        card->badges[badge] = catvision_image_create(card->panel, badge_x, badge_y, 28, 28);
        card->badge_x_labels[badge] = catvision_label(card->panel, "X", count_label_x(badge_x), badge_y + 7, 16, 0xaab4c0);
        lv_obj_set_style_text_font(card->badge_x_labels[badge], &lv_font_card_16_bold, LV_PART_MAIN);
        card->badge_counts[badge] = catvision_label(card->panel, "0", count_number_x(badge_x), badge_y + 2, 22, 0xaab4c0);
        lv_obj_set_width(card->badge_counts[badge], 34);
        lv_obj_set_style_text_font(card->badge_counts[badge], &lv_font_card_22_bold, LV_PART_MAIN);
        lv_obj_set_style_text_align(card->badge_counts[badge], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }
    hide_badges(card);
}

esp_err_t stats_ui_init(lv_obj_t *parent, lv_obj_t *status_label)
{
    live_frame = catvision_image_create(parent, 0, 0, 480, 240);
    if (!live_frame) {
        return ESP_ERR_NO_MEM;
    }
    root = catvision_panel(parent, 0, 0, 480, 480, 0x080c10);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    status = status_label;
    for (int index = 0; index < 4; index++) {
        create_card(index);
    }
    lv_obj_move_foreground(status_label);
    return ESP_OK;
}

void stats_ui_load_defaults(void)
{
    static const char preview_json[] = "{"
        "\"unknown_count\":0,\"cats\":["
        "{\"cat_id\":\"3c4fafeb-b168-4136-b9f2-99c401872019\",\"name\":\"球球\",\"today_count\":5,\"box_counts\":[{\"box_id\":\"box-1\",\"count\":3},{\"box_id\":\"box-3\",\"count\":2}]},"
        "{\"cat_id\":\"4c9c4c30-6fa1-4a81-b8da-3d0c12d78976\",\"name\":\"驴驴\",\"today_count\":0,\"box_counts\":[]},"
        "{\"cat_id\":\"80ad9f7d-2d75-4605-89ae-7d43cd333305\",\"name\":\"啵啵\",\"today_count\":2,\"box_counts\":[{\"box_id\":\"box-4\",\"count\":2}]},"
        "{\"cat_id\":\"034dc402-51ea-4ff0-9085-2a6abbe422c8\",\"name\":\"宝宝\",\"today_count\":4,\"box_counts\":[{\"box_id\":\"box-1\",\"count\":1},{\"box_id\":\"box-2\",\"count\":1},{\"box_id\":\"box-3\",\"count\":1},{\"box_id\":\"box-4\",\"count\":1}]}"
        "]}";
    for (int index = 0; index < 4; index++) {
        if (cards[index].avatar_loaded) {
            continue;
        }
        esp_err_t result = load_embedded_avatar(cards[index].avatar, avatars[index].cat_id);
        if (result == ESP_OK) {
            cards[index].avatar_loaded = true;
            ESP_LOGI("stats_ui", "embedded_avatar_loaded=%s", avatars[index].cat_id);
        } else {
            ESP_LOGW("stats_ui", "embedded_avatar_failed=%s error=%d", avatars[index].cat_id, result);
        }
    }
    if (stats_ui_update(preview_json) == ESP_OK) {
        catvision_display_lock();
        lv_label_set_text(status, "CARD PREVIEW");
        catvision_display_unlock();
    }
}

esp_err_t stats_ui_update_frame(const uint8_t *data, size_t size)
{
    return catvision_image_update_rgb565(live_frame, data, size, 480, 240);
}

esp_err_t stats_ui_update_frame_zlib(const uint8_t *data, size_t size)
{
    return catvision_image_update_zlib(live_frame, data, size, 480, 240);
}

esp_err_t stats_ui_update_frame_delta(const uint8_t *data, size_t size, bool delta, uint32_t expected_crc)
{
    return catvision_image_update_zlib_delta_native(live_frame, data, size, delta, expected_crc, 480, 240);
}

uint32_t stats_ui_frame_crc(void)
{
    return catvision_image_crc(live_frame);
}

catvision_image_t *stats_ui_live_image(void)
{
    return live_frame;
}

esp_err_t stats_ui_update(const char *json)
{
    cJSON *document = cJSON_Parse(json);
    if (!document) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t content_crc = stats_content_crc(document);
    if (content_crc == displayed_content_crc) {
        cJSON_Delete(document);
        return ESP_OK;
    }
    cJSON *cats = cJSON_GetObjectItem(document, "cats");
    int count = cJSON_GetArraySize(cats);
    for (int index = 0; index < count && index < 4; index++) {
        cat_card_t *card = &cards[index];
        if (!card->panel) {
            catvision_display_lock();
            create_card(index);
            catvision_display_unlock();
        }
        cJSON *cat = cJSON_GetArrayItem(cats, index);
        const char *cat_id = cJSON_GetStringValue(cJSON_GetObjectItem(cat, "cat_id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(cat, "name"));
        int today = cJSON_GetNumberValue(cJSON_GetObjectItem(cat, "today_count"));
        if (!card->avatar_loaded) {
            esp_err_t result = load_embedded_avatar(card->avatar, cat_id);
            if (result == ESP_OK) {
                card->avatar_loaded = true;
                ESP_LOGI("stats_ui", "embedded_avatar_loaded=%s", cat_id);
            } else {
                ESP_LOGW("stats_ui", "embedded_avatar_failed=%s error=%d", cat_id ? cat_id : "missing", result);
            }
        }
        catvision_display_lock();
        lv_label_set_text(card->name, name ?: "--");
        catvision_display_unlock();

        cJSON *box_counts = cJSON_GetObjectItem(cat, "box_counts");
        int box_total = cJSON_GetArraySize(box_counts);
        int visible = 0;
        for (int box = 0; box < box_total && visible < 2; box++) {
            cJSON *entry = cJSON_GetArrayItem(box_counts, box);
            const char *box_id = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "box_id"));
            int box_count = cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "count"));
            if (box_count <= 0 || load_embedded_box(card->badges[visible], box_id) != ESP_OK) {
                continue;
            }
            char count_text[16];
            snprintf(count_text, sizeof(count_text), "%d", box_count);
            catvision_display_lock();
            lv_label_set_text(card->badge_counts[visible], count_text);
            lv_obj_set_width(card->badge_counts[visible], count_text_width(count_text));
            if (visible == 1) {
                align_badges(card);
            }
            lv_obj_clear_flag(card->badges[visible]->object, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(card->badge_x_labels[visible], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(card->badge_counts[visible], LV_OBJ_FLAG_HIDDEN);
            catvision_display_unlock();
            ESP_LOGI("stats_ui", "embedded_box_loaded=%s count=%d", box_id, box_count);
            visible++;
        }
        hide_badges_from(card, visible);
        ESP_LOGI("stats_ui", "card_rendered=%s today=%d boxes=%d", cat_id ? cat_id : "missing", today, visible);
    }
    int unknown = cJSON_GetNumberValue(cJSON_GetObjectItem(document, "unknown_count"));
    char status_text[48];
    snprintf(status_text, sizeof(status_text), "%d CATS  |  UNKNOWN %d", count, unknown);
    catvision_display_lock();
    lv_label_set_text(status, status_text);
    catvision_display_unlock();
    displayed_content_crc = content_crc;
    cJSON_Delete(document);
    return ESP_OK;
}
