/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Glyphs: LV_SYMBOL_LEFT and LV_SYMBOL_RIGHT only -- bit-for-bit the ones in LVGL's built-in
 * lv_font_montserrat_14, so they look exactly as before without carrying the
 * whole Montserrat font. line_height/base_line are copied from that font so
 * the glyphs sit at the same height they always did.
 * Opts: lv_font_conv --no-compress --no-prefilter --bpp 4 --size 14
 *   --font Montserrat-Medium.ttf -r 0x20 --font FontAwesome5-Solid+Brands+Regular.woff
 *   -r 61523,61524 --format lvgl --force-fast-kern-format
 *   (both source fonts ship with LVGL in scripts/built_in_font)
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include <lvgl.h>
#else
#include <lvgl.h>
#endif

#ifndef PANEL_SYMBOLS_14
#define PANEL_SYMBOLS_14 1
#endif

#if PANEL_SYMBOLS_14

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */

    /* U+F053 "" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3f,
    0x90, 0x0, 0x0, 0x3f, 0xfc, 0x0, 0x0, 0x3f,
    0xfd, 0x10, 0x0, 0x3f, 0xfd, 0x10, 0x0, 0x3f,
    0xfd, 0x10, 0x0, 0x1f, 0xfd, 0x10, 0x0, 0x0,
    0xcf, 0xf4, 0x0, 0x0, 0x0, 0xcf, 0xf4, 0x0,
    0x0, 0x0, 0xcf, 0xf4, 0x0, 0x0, 0x0, 0xcf,
    0xf4, 0x0, 0x0, 0x0, 0xcf, 0xe0, 0x0, 0x0,
    0x0, 0xa4, 0x0,

    /* U+F054 "" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0xcd, 0x10, 0x0,
    0x0, 0x1f, 0xfd, 0x10, 0x0, 0x0, 0x3f, 0xfd,
    0x10, 0x0, 0x0, 0x3f, 0xfd, 0x10, 0x0, 0x0,
    0x3f, 0xfd, 0x10, 0x0, 0x0, 0x3f, 0xfd, 0x0,
    0x0, 0x8, 0xff, 0x90, 0x0, 0x8, 0xff, 0x90,
    0x0, 0x8, 0xff, 0x90, 0x0, 0x8, 0xff, 0x90,
    0x0, 0x2, 0xff, 0x90, 0x0, 0x0, 0x7, 0x80,
    0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 60, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 140, .box_w = 9, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 59, .adv_w = 140, .box_w = 9, .box_h = 13, .ofs_x = 0, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0xf033, 0xf034
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 61493, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 3, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
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
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t panel_symbols_14 = {
#else
lv_font_t panel_symbols_14 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
};



#endif /*#if PANEL_SYMBOLS_14*/

