#include "ui_ambient.h"
#include "audio_es8311.h"
#include "chord_data.h"
#include "fender_img.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Page 2 uses only the Fender image and one canvas.  The chord diagram,
 * VU meter and environment card are drawn into the canvas to avoid creating
 * dozens of LVGL children on the 48 KB LVGL heap. */

namespace {

constexpr lv_coord_t CANVAS_W = 232;
constexpr lv_coord_t CANVAS_H = 274;
constexpr lv_coord_t GUITAR_W = 160;
constexpr lv_coord_t GUITAR_H = CANVAS_H;

uint32_t g_rock_level = 18;
uint8_t  g_chord_idx = 0;
ChordGroup g_chord_group = CHORD_GROUP_OPEN;
float    g_temp = 25.0f;
float    g_humi = 50.0f;

lv_obj_t   *g_canvas = nullptr;
lv_obj_t   *g_guitar = nullptr;
lv_color_t *g_canvas_buf = nullptr;
lv_color_t *g_guitar_buf = nullptr;
lv_img_dsc_t g_guitar_stretched = {};

lv_color_t ink(void)
{
    return lv_color_black();
}

lv_color_t paper(void)
{
    return lv_color_white();
}

void draw_rect(lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
               lv_color_t fill, lv_opa_t fill_opa,
               lv_color_t border, lv_coord_t border_width, lv_coord_t radius = 0)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = fill;
    dsc.bg_opa = fill_opa;
    dsc.border_color = border;
    dsc.border_opa = border_width ? LV_OPA_COVER : LV_OPA_TRANSP;
    dsc.border_width = border_width;
    dsc.radius = radius;
    lv_canvas_draw_rect(g_canvas, x, y, w, h, &dsc);
}

void draw_line(lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2,
               lv_coord_t width = 1)
{
    lv_point_t points[2] = {{x1, y1}, {x2, y2}};
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = ink();
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 0;
    dsc.round_end = 0;
    lv_canvas_draw_line(g_canvas, points, 2, &dsc);
}

void draw_text(lv_coord_t x, lv_coord_t y, lv_coord_t width, const char *text,
               const lv_font_t *font = &lv_font_montserrat_14,
               lv_text_align_t align = LV_TEXT_ALIGN_LEFT,
               bool reverse = false)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = reverse ? paper() : ink();
    dsc.font = font;
    dsc.align = align;
    lv_canvas_draw_text(g_canvas, x, y, width, &dsc, text);
}

void draw_dot(lv_coord_t cx, lv_coord_t cy, lv_coord_t radius)
{
    draw_rect(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1,
              ink(), LV_OPA_COVER, ink(), 0, LV_RADIUS_CIRCLE);
}

void draw_chord(void)
{
    const ChordData &chord = chord_data_get(g_chord_idx);
    constexpr lv_coord_t grid_x = 12;
    constexpr lv_coord_t grid_y = 116;
    constexpr lv_coord_t string_gap = 18;
    constexpr lv_coord_t fret_gap = 15;

    draw_text(10, 60, 102, chord.name, &lv_font_montserrat_20);

    for(int string = 0; string < 6; ++string) {
        lv_coord_t x = grid_x + string * string_gap;
        const char *status = chord.frets[string] < 0 ? "X"
                             : (chord.frets[string] == 0 ? "O" : "");
        draw_text(x - 7, 82, 14, status,
                  &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
        draw_text(x - 8, 98, 16, chord.string_notes[string],
                  &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
        draw_line(x, grid_y, x, grid_y + fret_gap * 5, 1);
    }

    for(int fret = 0; fret <= 5; ++fret) {
        lv_coord_t y = grid_y + fret * fret_gap;
        draw_line(grid_x, y, grid_x + string_gap * 5, y, fret == 0 ? 2 : 1);
        if(fret > 0) {
            char number[2] = {(char)('0' + fret), '\0'};
            draw_text(0, y - 8, 10, number, &lv_font_montserrat_14,
                      LV_TEXT_ALIGN_RIGHT);
        }
    }

    for(int string = 0; string < 6; ++string) {
        int8_t fret = chord.frets[string];
        lv_coord_t x = grid_x + string * string_gap;
        if(fret > 0) {
            lv_coord_t y = grid_y + (fret * fret_gap) - fret_gap / 2;
            draw_dot(x, y, 6);
            if(chord.fingers[string] > 0) {
                char finger[2] = {(char)('0' + chord.fingers[string]), '\0'};
                draw_text(x - 6, y - 7, 12, finger, &lv_font_montserrat_14,
                          LV_TEXT_ALIGN_CENTER, true);
            }
        }
    }
}

void draw_chord_info(void)
{
    const ChordData &chord = chord_data_get(g_chord_idx);
    uint8_t first = chord_data_group_first(g_chord_group);
    uint8_t size = chord_data_group_size(g_chord_group);
    uint8_t position = g_chord_idx - first;
    uint8_t next_idx = first + (position + 1) % size;

    char progress[20];
    snprintf(progress, sizeof(progress), "%s  %u/%u",
             chord_data_group_name(g_chord_group), position + 1, size);
    draw_text(115, 63, 108, progress, &lv_font_montserrat_14,
              LV_TEXT_ALIGN_CENTER);
    draw_text(115, 83, 108, chord.quality, &lv_font_montserrat_20,
              LV_TEXT_ALIGN_CENTER);

    draw_text(117, 111, 104, "FORMULA", &lv_font_montserrat_14,
              LV_TEXT_ALIGN_CENTER);
    draw_text(115, 129, 108, chord.formula, &lv_font_montserrat_14,
              LV_TEXT_ALIGN_CENTER);
    draw_text(117, 151, 104, "NOTES", &lv_font_montserrat_14,
              LV_TEXT_ALIGN_CENTER);
    draw_text(113, 169, 112, chord.notes, &lv_font_montserrat_14,
              LV_TEXT_ALIGN_CENTER);

    char next[24];
    snprintf(next, sizeof(next), "NEXT: %s", chord_data_get(next_idx).name);
    draw_text(115, 192, 108, next, &lv_font_montserrat_14,
              LV_TEXT_ALIGN_CENTER);
}

void draw_thermometer(void)
{
    draw_rect(10, 236, 9, 22, paper(), LV_OPA_COVER, ink(), 2, 4);
    draw_line(14, 241, 14, 257, 2);
    draw_rect(7, 254, 15, 15, paper(), LV_OPA_COVER, ink(), 2,
              LV_RADIUS_CIRCLE);
    draw_dot(14, 261, 3);
}

void draw_drop(void)
{
    lv_point_t points[5] = {{134, 238}, {126, 253}, {126, 260},
                            {134, 268}, {142, 260}};
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = ink();
    dsc.width = 2;
    dsc.round_start = 1;
    dsc.round_end = 1;
    lv_canvas_draw_line(g_canvas, points, 5, &dsc);
    draw_line(142, 260, 134, 238, 2);
    draw_line(135, 264, 139, 260, 1);
}

void redraw(void)
{
    if(!g_canvas) return;

    lv_canvas_fill_bg(g_canvas, paper(), LV_OPA_COVER);

    draw_rect(0, 0, CANVAS_W, 48, ink(), LV_OPA_COVER, ink(), 0, 3);
    char rock[32];
    snprintf(rock, sizeof(rock), "ROCK LEVEL %04lu", (unsigned long)g_rock_level);
    draw_text(9, 13, CANVAS_W - 18, rock, &lv_font_montserrat_20,
              LV_TEXT_ALIGN_CENTER, true);

    draw_rect(0, 54, CANVAS_W, 158, paper(), LV_OPA_COVER, ink(), 2, 2);
    draw_chord();
    draw_chord_info();

    draw_rect(0, 218, CANVAS_W, 56, paper(), LV_OPA_COVER, ink(), 2, 2);
    draw_thermometer();
    draw_drop();
    draw_line(116, 231, 116, 263, 2);

    char temp[24];
    char humi[24];
    snprintf(temp, sizeof(temp), "TEMP %.0f°C", g_temp);
    snprintf(humi, sizeof(humi), "HUMI %.0f%%", g_humi);
    draw_text(27, 239, 84, temp, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    draw_text(148, 239, 80, humi, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
}

bool build_stretched_guitar(void)
{
    const lv_coord_t src_w = fender_strat_map.header.w;
    const lv_coord_t src_h = fender_strat_map.header.h;
    if(src_w != GUITAR_W || src_h <= 0 ||
       fender_strat_map.header.cf != LV_IMG_CF_TRUE_COLOR) {
        return false;
    }

    const size_t row_bytes = (size_t)GUITAR_W * sizeof(lv_color_t);
    g_guitar_buf = static_cast<lv_color_t *>(
        heap_caps_malloc(row_bytes * GUITAR_H,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if(!g_guitar_buf) {
        return false;
    }

    const uint8_t *source = fender_strat_map.data;
    uint8_t *target = reinterpret_cast<uint8_t *>(g_guitar_buf);
    for(lv_coord_t y = 0; y < GUITAR_H; ++y) {
        lv_coord_t src_y = (lv_coord_t)((int32_t)y * src_h / GUITAR_H);
        memcpy(target + (size_t)y * row_bytes,
               source + (size_t)src_y * row_bytes, row_bytes);
    }

    g_guitar_stretched.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_guitar_stretched.header.w = GUITAR_W;
    g_guitar_stretched.header.h = GUITAR_H;
    g_guitar_stretched.data_size = row_bytes * GUITAR_H;
    g_guitar_stretched.data = reinterpret_cast<const uint8_t *>(g_guitar_buf);
    return true;
}

} // namespace

void ui_ambient_tap(void)
{
    ++g_rock_level;
    redraw();
    audio_play_chord(g_chord_idx);
}

void ui_ambient_next_chord(void)
{
    uint8_t first = chord_data_group_first(g_chord_group);
    uint8_t size = chord_data_group_size(g_chord_group);
    g_chord_idx = first + (g_chord_idx - first + 1) % size;
    redraw();
    audio_play_chord(g_chord_idx);
}

void ui_ambient_next_group(void)
{
    g_chord_group = g_chord_group == CHORD_GROUP_OPEN
                    ? CHORD_GROUP_SEVENTH : CHORD_GROUP_OPEN;
    g_chord_idx = chord_data_group_first(g_chord_group);
    redraw();
    audio_play_chord(g_chord_idx);
}

uint8_t ui_ambient_current_chord(void)
{
    return g_chord_idx;
}

void ui_ambient_update_env(float temp, float humi)
{
    g_temp = temp;
    g_humi = humi;
    redraw();
}

void ui_ambient_init(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    g_guitar = lv_img_create(parent);
    lv_img_set_src(g_guitar, build_stretched_guitar()
                              ? &g_guitar_stretched
                              : &fender_strat_map);
    lv_obj_set_pos(g_guitar, 2, 3);

    g_canvas_buf = static_cast<lv_color_t *>(
        heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(CANVAS_W, CANVAS_H),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if(!g_canvas_buf) {
        lv_obj_t *error = lv_label_create(parent);
        lv_label_set_text(error, "Page 2 canvas allocation failed");
        lv_obj_set_pos(error, 166, 120);
        return;
    }

    g_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(g_canvas, g_canvas_buf, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(g_canvas, 164, 3);
    redraw();

    /* Deliberately no audio_play_chord() here: audio is only triggered by keys. */
}

void ui_ambient_start_radar(void) {}

void ui_ambient_on_show(void)
{
    redraw();
}
