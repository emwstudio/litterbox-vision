/*******************************************************************************
 * Size: 16 px
 * Bpp: 4
 * Opts: --font .superpowers/sdd/2026-11-12-2p16-cat-cards/NotoEmoji-Regular.ttf --size 16 --bpp 4 --format lvgl --symbols 💩 --no-compress --lv-include lvgl.h --lv-font-name lv_font_poop_16 -o firmware/common/components/catvision_ui/lv_font_poop_16.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef LV_FONT_POOP_16
#define LV_FONT_POOP_16 1
#endif

#if LV_FONT_POOP_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+1F4A9 "💩" */
    0x0, 0x0, 0x0, 0x4, 0x20, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x1c, 0xf5, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0xc, 0xff, 0xc2, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x2, 0xff, 0xff, 0xfd, 0x70,
    0x0, 0x0, 0x0, 0x0, 0x5f, 0xff, 0xff, 0xff,
    0x60, 0x0, 0x0, 0x0, 0x9f, 0xff, 0xff, 0xff,
    0xf9, 0x0, 0x0, 0x0, 0x3f, 0xff, 0xff, 0xff,
    0xff, 0xd3, 0x0, 0x0, 0x6, 0xff, 0xaa, 0xff,
    0xd9, 0xef, 0xe0, 0x0, 0x1, 0xbf, 0x74, 0x38,
    0xe2, 0x62, 0xff, 0x50, 0x2, 0xef, 0xf4, 0xba,
    0x5c, 0x3f, 0x1e, 0xff, 0x60, 0xaf, 0xff, 0xb1,
    0x1c, 0xf4, 0x6, 0xff, 0xfe, 0xc, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xf1, 0xaf, 0xff,
    0xf5, 0x34, 0x43, 0x2e, 0xff, 0xff, 0x4, 0xff,
    0xff, 0x90, 0x0, 0x4, 0xff, 0xff, 0x80, 0x8,
    0xff, 0xff, 0xa4, 0x37, 0xff, 0xff, 0xc0, 0x0,
    0x5, 0xef, 0xff, 0xff, 0xff, 0xff, 0x80, 0x0,
    0x0, 0x0, 0x49, 0xbd, 0xdc, 0xa6, 0x10, 0x0,
    0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 325, .box_w = 17, .box_h = 17, .ofs_x = 2, .ofs_y = -3}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 128169, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t lv_font_poop_16 = {
#else
lv_font_t lv_font_poop_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 17,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 10,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LV_FONT_POOP_16*/

