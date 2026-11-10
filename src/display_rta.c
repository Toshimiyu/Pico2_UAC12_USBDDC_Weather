/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/


#include "display_rta.h"
#include "common.h"
#include "usb_audio_config.h"
#include "transmit_to_dac.h"
#include "ringbuffer.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>

// ===================================================
// Color theme definitions (collaboration: GEMINI Amber x COP Navy).
// ===================================================
#define COLOR_BG         0x0000 // Background: black.
#define COLOR_TITLE      0x03B0 // Main title (COP color: navy with a touch of cyan).
#define COLOR_DIVIDER    0x0112 // Divider (COP color: dark navy).
#define COLOR_L_THEME    0xFA00 // Normal L/R meter color (GEMINI color: deep orange).
#define COLOR_R_THEME    0xFA00 // Normal L/R meter color (GEMINI color: deep orange).
#define COLOR_PEAK_WARN  0xFD20 // Warning: bright amber.
#define COLOR_PEAK_CLIP  0xF800 // Clipping: red.
#define COLOR_FRAME_L    0x4200 // L/R bar frame: dark deep orange.
#define COLOR_FRAME_R    0x4200 // L/R bar frame: dark deep orange.
#define COLOR_FREQ_TEXT  0xFD20 // Frequency text (GEMINI color: bright amber).
#define COLOR_FMT_TEXT   0x03B0 // Format display (COP color: navy with a touch of cyan).
#define COLOR_PEAK_DOT   0xFFFF // Peak hold: white.
#define COLOR_CH_LABEL   0xFD20 // Channel label (GEMINI color: bright amber).

// ===================================================
// Peak bar and RTA layout parameters.
// ===================================================
#define PEAK_HOLD_US      300000u
#define DIAGNOSTICS_RESET_HOLD_US 3000000u
#define PEAK_FALL_SPEED   4     // Peak marker fall distance every 50 ms (shared by FFT and L/R meters).
#define METER_PEAK_FALL_SPEED PEAK_FALL_SPEED
#define RTA_BAR_RELEASE_PIXELS_PER_SEC 100u // Bar fall speed (drops the full height in about one second).
#define PEAK_NONE_Y       0xFFFF// Invalid value indicating that the peak is hidden.

#define RTA_BAR_W         10
#define RTA_CELL_W        13
#define RTA_BAR_H         100
#define RTA_BAR_Y         65
#define RTA_L_X_START     10
#define RTA_R_X_START     167

#define RTA_LABEL_Y_TOP     (RTA_BAR_Y + RTA_BAR_H + 3)   // 168 px (top row: Y=168-175).
#define RTA_LABEL_Y_BOTTOM  (RTA_BAR_Y + RTA_BAR_H + 14)  // 179 px (bottom row: Y=179-186).
#define FONT_CHAR_WIDTH     8                             // Actual character width (8 px).

// 8x8 bitmap font covering all printable ASCII characters (0x20 ' ' through 0x7E '~').
static const uint8_t font8x8[95][8] = {
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
{0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // '"'
{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // '#'
{0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // '$'
{0x00,0x63,0x66,0x0C,0x18,0x33,0x63,0x00}, // '%'
{0x1C,0x36,0x1C,0x3B,0x6E,0x66,0x3B,0x00}, // '&'
{0x06,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // '''
{0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
{0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
{0x00,0x0C,0x0C,0x3E,0x0C,0x0C,0x00,0x00}, // '+'
{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x18}, // ','
{0x00,0x00,0x00,0x3E,0x00,0x00,0x00,0x00}, // '-'
{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
{0x00,0x03,0x06,0x0C,0x18,0x30,0x60,0x00}, // '/'
{0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, // '0'
{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1'
{0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, // '2'
{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // '3'
{0x06,0x0E,0x1E,0x36,0x66,0x7F,0x06,0x00}, // '4'
{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // '5'
{0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00}, // '6'
{0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00}, // '7'
{0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // '8'
{0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}, // '9'
{0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // ':'
{0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, // ';'
{0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // '<'
{0x00,0x00,0x3E,0x00,0x3E,0x00,0x00,0x00}, // '='
{0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // '>'
{0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}, // '?'
{0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00}, // '@'
{0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, // 'A'
{0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // 'B'
{0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // 'C'
{0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // 'D'
{0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00}, // 'E'
{0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00}, // 'F'
{0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00}, // 'G'
{0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 'H'
{0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'I'
{0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, // 'J'
{0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // 'K'
{0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // 'L'
{0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // 'M'
{0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // 'N'
{0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'O'
{0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // 'P'
{0x3C,0x66,0x66,0x66,0x66,0x3C,0x0E,0x00}, // 'Q'
{0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // 'R'
{0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // 'S'
{0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 'T'
{0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'U'
{0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // 'V'
{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'W'
{0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // 'X'
{0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // 'Y'
{0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // 'Z'
{0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
{0x00,0x60,0x30,0x18,0x0C,0x06,0x03,0x00}, // ''
{0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
{0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, // '^'
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
{0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
{0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, // 'a'
{0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // 'b'
{0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, // 'c'
{0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, // 'd'
{0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, // 'e'
{0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00}, // 'f'
{0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, // 'g'
{0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, // 'h'
{0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i'
{0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x0C,0x38}, // 'j'
{0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, // 'k'
{0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l'
{0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, // 'm'
{0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, // 'n'
{0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, // 'o'
{0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, // 'p'
{0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, // 'q'
{0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, // 'r'
{0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, // 's'
{0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00}, // 't'
{0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, // 'u'
{0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, // 'v'
{0x00,0x00,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'w'
{0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, // 'x'
{0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, // 'y'
{0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, // 'z'
{0x0E,0x18,0x18,0x30,0x18,0x18,0x0E,0x00}, // '{'
{0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
{0x70,0x18,0x18,0x0C,0x18,0x18,0x70,0x00}, // '}'
{0x3B,0x6E,0x00,0x00,0x00,0x00,0x00,0x00}  // ''
};   

// Glyphs for the weather page; character codes start at 0x80.
#include "weather_font.h"
#define WEATHER_JP_GLYPH_COUNT (sizeof(weather_jp_glyphs) / sizeof(weather_jp_glyphs[0]))

// Frequency label information structure.
typedef struct {
uint8_t index;
const char *text;
} freq_label_t;   

static const freq_label_t freq_labels[] = {
{ 0,  "45"  },
{ 1,  "90"  },
{ 2,  "125" },
{ 3,  "360" },
{ 4,  "700" },
{ 5,  "1k4" },
{ 6,  "2k8" },
{ 7,  "5k6" },
{ 8,  "9k8" },
{ 9,  "18k" },
{ 10, "22k" }
};
#define NUM_FREQ_LABELS (sizeof(freq_labels) / sizeof(freq_labels[0]))   

// Dynamic drawing data.
static uint8_t cur_l_bands[SPECTRUM_BAND_COUNT] = {0};
static uint8_t cur_r_bands[SPECTRUM_BAND_COUNT] = {0};
static uint8_t spectrum_l[SPECTRUM_BAND_COUNT];
static volatile uint8_t input_l_bands[SPECTRUM_BAND_COUNT] = {0};
static volatile uint8_t input_r_bands[SPECTRUM_BAND_COUNT] = {0};
static uint8_t cur_l_peak = 0;
static uint8_t cur_r_peak = 0;
static volatile uint8_t input_l_peak = 0;
static volatile uint8_t input_r_peak = 0;

static uint16_t prev_l_bands[SPECTRUM_BAND_COUNT] = {0}; // Height of the currently drawn bars (px).
static uint16_t prev_r_bands[SPECTRUM_BAND_COUNT] = {0};
static uint32_t bar_last_update_us[2][SPECTRUM_BAND_COUNT] = {{0}};
static uint16_t prev_l_meter_width = 0;
static uint16_t prev_r_meter_width = 0;
static uint16_t peak_l_meter_width = 0;
static uint16_t peak_r_meter_width = 0;
static uint32_t peak_l_meter_hold_until = 0;
static uint32_t peak_r_meter_hold_until = 0;
static uint32_t peak_l_meter_next_fall = 0;
static uint32_t peak_r_meter_next_fall = 0;
static uint16_t last_l_meter_marker = PEAK_NONE_Y;
static uint16_t last_r_meter_marker = PEAK_NONE_Y;
static uint32_t splash_start_us = 0;
static uint8_t splash_stage = 0;   
static display_page_t current_page = DISPLAY_PAGE_BOOT;
static bool diagnostic_mode_active;
static bool page_clear_pending = false;
static uint16_t page_clear_row = 0;
static bool page_ready = true;

static display_button_t button_x;
static display_button_t button_b;
static display_button_t button_y;
static bool mode_hold_tracking;
static bool mode_switch_latched;
static uint32_t mode_hold_start_us;
static uint8_t mode_last_countdown;
static uint8_t mode_countdown_display;
static bool mode_countdown_dirty;
static weather_snapshot_t weather_data;
static weather_snapshot_t weather_drawn_data;
static bool weather_drawn_valid;
static bool display_audio_has_slack(void);
static bool diagnostics_reset_latched = false;
static bool diagnostics_y_hold_tracking = false;
static uint32_t diagnostics_y_hold_start_us = 0;
static uint8_t diagnostics_last_countdown = 0;

// Buffer for the peak dot animation.
static uint8_t peak_l_dots[SPECTRUM_BAND_COUNT] = {0};
static uint8_t peak_r_dots[SPECTRUM_BAND_COUNT] = {0};
static uint32_t peak_l_hold_until[SPECTRUM_BAND_COUNT] = {0};
static uint32_t peak_r_hold_until[SPECTRUM_BAND_COUNT] = {0};
static uint32_t peak_l_next_fall[SPECTRUM_BAND_COUNT] = {0};
static uint32_t peak_r_next_fall[SPECTRUM_BAND_COUNT] = {0};   

// Buffer tracking the Y coordinates of the peak dots drawn in the previous frame.
static uint16_t last_l_peak_y[SPECTRUM_BAND_COUNT];
static uint16_t last_r_peak_y[SPECTRUM_BAND_COUNT];

static volatile bool reset_peak_dots_pending = false;
static volatile bool format_pending = false;
static volatile bool display_ready = false;
static volatile uint8_t requested_bit_depth = 32;
static volatile uint32_t requested_sample_rate_tenth_khz = 3840;
extern volatile uint32_t audio_last_data_us;
extern volatile bool audio_data_seen;   

static volatile bool display_dirty = false;
static bool render_pending = false;
static uint8_t render_step = 0;
#define FORMAT_TEXT_CHARS 18
static char format_render_text[FORMAT_TEXT_CHARS + 1];
static uint8_t format_render_pos = 0;
static bool format_render_active = false;
// Draw the format display (bit depth / sample rate) at double horizontal scale.
#define FORMAT_TEXT_CHAR_WIDTH 12
#define FORMAT_TEXT_SCALE_Y 1
#define FORMAT_TEXT_Y     203
#define FORMAT_TEXT_X0    (160 - (FORMAT_TEXT_CHARS * FORMAT_TEXT_CHAR_WIDTH) / 2)
// DISPLAY_MIN_AUDIO_HEADROOM_SAMPLES is defined in common.h (follows the FB target level)

static uint8_t lcd_fill_chunk[512];   

typedef enum {
    PAGE_DRAW_FILL,
    PAGE_DRAW_CHAR
} page_draw_type_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint16_t color;
    uint16_t bg;
    uint8_t character;
    uint8_t scale_y;
    page_draw_type_t type;
} page_draw_op_t;

#define PAGE_DRAW_OP_CAPACITY 768
#define PAGE_DRAW_MAX_PIXELS 256
static page_draw_op_t page_draw_ops[PAGE_DRAW_OP_CAPACITY];
static uint16_t page_draw_count = 0;
static uint16_t page_draw_pos = 0;
static bool page_draw_capture = false;
static bool page_draw_pending = false;

static void page_draw_enqueue_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint16_t row = 0;
    while (row < h)
    {
        const uint16_t rows_per_op = w <= PAGE_DRAW_MAX_PIXELS
            ? PAGE_DRAW_MAX_PIXELS / w : 1;
        const uint16_t op_h = (h - row < rows_per_op) ? h - row : rows_per_op;
        uint16_t column = 0;
        while (column < w)
        {
            const uint16_t op_w = (w - column < PAGE_DRAW_MAX_PIXELS)
                ? w - column : PAGE_DRAW_MAX_PIXELS;
            if (page_draw_count >= PAGE_DRAW_OP_CAPACITY)
            {
                panic("LCD page draw queue overflow");
            }

            page_draw_ops[page_draw_count++] = (page_draw_op_t){
                .x = x + column,
                .y = y + row,
                .w = op_w,
                .h = op_h,
                .color = color,
                .type = PAGE_DRAW_FILL
            };
            column += op_w;
        }
        row += op_h;
    }
}

static void page_draw_begin_capture(void)
{
    page_draw_count = 0;
    page_draw_pos = 0;
    page_draw_capture = true;
}

static void page_draw_end_capture(void)
{
    page_draw_capture = false;
    page_draw_pending = page_draw_count > 0;
    page_ready = !page_draw_pending;
}

static void lcd_write_cmd(uint8_t cmd) {
gpio_put(LCD_DC_PIN, 0);
gpio_put(LCD_CS_PIN, 0);
spi_write_blocking(spi0, &cmd, 1);
gpio_put(LCD_CS_PIN, 1);
}   

static void lcd_write_data(uint8_t data) {
gpio_put(LCD_DC_PIN, 1);
gpio_put(LCD_CS_PIN, 0);
spi_write_blocking(spi0, &data, 1);
gpio_put(LCD_CS_PIN, 1);
}   

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
lcd_write_cmd(0x2A); // Column Address Set
gpio_put(LCD_DC_PIN, 1);
gpio_put(LCD_CS_PIN, 0);
uint8_t x_data[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
spi_write_blocking(spi0, x_data, 4);
gpio_put(LCD_CS_PIN, 1);   

lcd_write_cmd(0x2B); // Row Address Set
gpio_put(LCD_DC_PIN, 1);
gpio_put(LCD_CS_PIN, 0);
uint8_t y_data[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
spi_write_blocking(spi0, y_data, 4);
gpio_put(LCD_CS_PIN, 1);

lcd_write_cmd(0x2C); // Memory Write
}

static void lcd_fill_color(uint16_t color) {
lcd_set_window(0, 0, 319, 239);
gpio_put(LCD_DC_PIN, 1);
gpio_put(LCD_CS_PIN, 0);
uint8_t data[2] = {color >> 8, color & 0xFF};
for (int i = 0; i < 320 * 240; i++) {
spi_write_blocking(spi0, data, 2);
}
gpio_put(LCD_CS_PIN, 1);
}

static void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
if (x >= 320 || y >= 240 || w == 0 || h == 0) return;
if (x + w > 320) w = 320 - x;
if (y + h > 240) h = 240 - y;

if (page_draw_capture) {
page_draw_enqueue_fill(x, y, w, h, color);
return;
}

lcd_set_window(x, y, x + w - 1, y + h - 1);
gpio_put(LCD_DC_PIN, 1);
gpio_put(LCD_CS_PIN, 0);

const uint8_t hi = (uint8_t)(color >> 8);
const uint8_t lo = (uint8_t)color;
for (uint32_t i = 0; i < sizeof(lcd_fill_chunk); i += 2) {
lcd_fill_chunk[i] = hi;
lcd_fill_chunk[i + 1] = lo;
}
uint32_t remaining = (uint32_t)w * h * 2u;
while (remaining > 0) {
const uint32_t chunk = remaining > sizeof(lcd_fill_chunk)
? sizeof(lcd_fill_chunk) : remaining;
spi_write_blocking(spi0, lcd_fill_chunk, chunk);
remaining -= chunk;
}
gpio_put(LCD_CS_PIN, 1);
}

static void lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
lcd_fill_rect(x, y, w, 1, color);
lcd_fill_rect(x, y + h - 1, w, 1, color);
lcd_fill_rect(x, y, 1, h, color);
lcd_fill_rect(x + w - 1, y, 1, h, color);
}

static void lcd_draw_char_scaled_width(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg,
                                       uint16_t width, uint8_t scale_y) {
const uint8_t *glyph = NULL;
const uint16_t *jp_glyph = NULL;
const uint8_t code = (uint8_t)c;
if (code >= 0x80 && code < 0x80 + WEATHER_JP_GLYPH_COUNT) {
jp_glyph = weather_jp_glyphs[code - 0x80].rows;
} else {
if (c < ' ' || c > '~') c = ' ';
glyph = font8x8[c - ' '];
}
// Japanese glyphs are 16x16, so scale_y 2 is their native height.
const uint8_t glyph_rows = jp_glyph ? 16 : 8;
const uint8_t row_repeat = jp_glyph ? (scale_y / 2 ? scale_y / 2 : 1) : scale_y;
uint16_t height = glyph_rows * row_repeat;

if (x + width > 320 || y + height > 240) return;

if (page_draw_capture) {
if (page_draw_count >= PAGE_DRAW_OP_CAPACITY) {
panic("LCD page draw queue overflow");
}
page_draw_ops[page_draw_count++] = (page_draw_op_t){
    .x = x,
    .y = y,
    .w = width,
    .h = height,
    .color = color,
    .bg = bg,
    .character = code,
    .scale_y = scale_y,
    .type = PAGE_DRAW_CHAR
};
return;
}

lcd_set_window(x, y, x + width - 1, y + height - 1);
gpio_put(LCD_DC_PIN, 1);
gpio_put(LCD_CS_PIN, 0);

uint8_t buf[24 * 24 * 2];
int buf_idx = 0;

for (int row = 0; row < glyph_rows; row++) {
for (int sy = 0; sy < row_repeat; sy++) {
for (uint16_t col = 0; col < width; col++) {
const bool on = jp_glyph ? (jp_glyph[row] & (0x8000u >> (col * 16 / width)))
                         : (glyph[row] & (0x80 >> (col * 8 / width)));
uint16_t px_color = on ? color : bg;
uint8_t high = px_color >> 8;
uint8_t low = px_color & 0xFF;
buf[buf_idx++] = high;
buf[buf_idx++] = low;
}
}
}

spi_write_blocking(spi0, buf, buf_idx);
gpio_put(LCD_CS_PIN, 1);
}

static void lcd_draw_char_scaled(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg,
                                 uint8_t scale_x, uint8_t scale_y) {
lcd_draw_char_scaled_width(x, y, c, color, bg, 8 * scale_x, scale_y);
}

static void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale) {
lcd_draw_char_scaled(x, y, c, color, bg, scale, scale);
}

static void lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale) {
while (*str) {
lcd_draw_char(x, y, *str, color, bg, scale);
x += 8 * scale;
str++;
}
}

static void lcd_draw_string_centered(uint16_t center_x, uint16_t y, const char *str,
                                     uint16_t color, uint16_t bg, uint8_t scale) {
	const uint16_t width = (uint16_t)(strlen(str) * 8u * scale);
	lcd_draw_string(center_x > width / 2 ? center_x - width / 2 : 0, y, str, color, bg, scale);
}

static void lcd_draw_string_scaled(uint16_t x, uint16_t y, const char *str,
                                   uint16_t color, uint16_t bg, uint8_t scale_x, uint8_t scale_y) {
	while (*str) {
		lcd_draw_char_scaled(x, y, *str, color, bg, scale_x, scale_y);
		x += 8u * scale_x;
		str++;
	}
}

static void lcd_draw_string_centered_scaled(uint16_t center_x, uint16_t y, const char *str,
                                            uint16_t color, uint16_t bg,
                                            uint8_t scale_x, uint8_t scale_y) {
	const uint16_t width = (uint16_t)(strlen(str) * 8u * scale_x);
	lcd_draw_string_scaled(center_x > width / 2 ? center_x - width / 2 : 0,
		y, str, color, bg, scale_x, scale_y);
}

static void lcd_draw_utf8_string(uint16_t x, uint16_t y, const char *text,
                                 uint16_t color, uint16_t bg, uint8_t scale_y) {
	while (*text != '\0') {
		size_t glyph_index;
		for (glyph_index = 0; glyph_index < WEATHER_JP_GLYPH_COUNT; ++glyph_index) {
			const size_t bytes = strlen(weather_jp_glyphs[glyph_index].utf8);
			if (strncmp(text, weather_jp_glyphs[glyph_index].utf8, bytes) == 0) {
				lcd_draw_char_scaled_width(x, y, (char)(0x80 + glyph_index),
					color, bg, 16, scale_y);
				x += 16;
				text += bytes;
				break;
			}
		}
		if (glyph_index == WEATHER_JP_GLYPH_COUNT && (uint8_t)*text <= 0x7Eu) {
			lcd_draw_char_scaled_width(x, y, *text, color, bg, 8, scale_y);
			x += 8;
			text++;
		} else if (glyph_index == WEATHER_JP_GLYPH_COUNT) {
			text++;
		}
	}
}

static uint16_t lcd_utf8_string_width(const char *text)
{
	uint16_t width = 0;
	while (*text != '\0') {
		size_t glyph_index;
		for (glyph_index = 0; glyph_index < WEATHER_JP_GLYPH_COUNT; ++glyph_index) {
			const size_t bytes = strlen(weather_jp_glyphs[glyph_index].utf8);
			if (strncmp(text, weather_jp_glyphs[glyph_index].utf8, bytes) == 0) {
				width += 16;
				text += bytes;
				break;
			}
		}
		if (glyph_index == WEATHER_JP_GLYPH_COUNT) {
			if ((uint8_t)*text <= 0x7Eu)
				width += 8;
			text++;
		}
	}
	return width;
}

static void lcd_draw_utf8_string_centered(uint16_t center_x, uint16_t y, const char *text,
                                          uint16_t color, uint16_t bg, uint8_t scale_y)
{
	const uint16_t width = lcd_utf8_string_width(text);
	lcd_draw_utf8_string(center_x > width / 2 ? center_x - width / 2 : 0,
		y, text, color, bg, scale_y);
}

static uint16_t lcd_utf8_string_width_with_large_digits(const char *text, uint8_t digit_width)
{
	uint16_t width = 0;
	while (*text != '\0') {
		size_t glyph_index;
		for (glyph_index = 0; glyph_index < WEATHER_JP_GLYPH_COUNT; ++glyph_index) {
			const size_t bytes = strlen(weather_jp_glyphs[glyph_index].utf8);
			if (strncmp(text, weather_jp_glyphs[glyph_index].utf8, bytes) == 0) {
				width += 16;
				text += bytes;
				break;
			}
		}
		if (glyph_index == WEATHER_JP_GLYPH_COUNT) {
			if ((uint8_t)*text <= 0x7Eu)
				width += (*text >= '0' && *text <= '9') ? digit_width : 8;
			text++;
		}
	}
	return width;
}

static void lcd_draw_utf8_temperature_centered(uint16_t center_x, uint16_t y, const char *text,
                                               uint16_t color, uint16_t bg)
{
	uint8_t digit_width = 12;
	uint16_t width = lcd_utf8_string_width_with_large_digits(text, digit_width);
	if (width > 144) {
		digit_width = 8;
		width = lcd_utf8_string_width_with_large_digits(text, digit_width);
	}

	uint16_t x = center_x > width / 2 ? center_x - width / 2 : 0;
	while (*text != '\0') {
		size_t glyph_index;
		for (glyph_index = 0; glyph_index < WEATHER_JP_GLYPH_COUNT; ++glyph_index) {
			const size_t bytes = strlen(weather_jp_glyphs[glyph_index].utf8);
			if (strncmp(text, weather_jp_glyphs[glyph_index].utf8, bytes) == 0) {
				lcd_draw_char_scaled_width(x, y, (char)(0x80 + glyph_index),
					color, bg, 16, 2);
				x += 16;
				text += bytes;
				break;
			}
		}
		if (glyph_index == WEATHER_JP_GLYPH_COUNT && (uint8_t)*text <= 0x7Eu) {
			const uint16_t char_width =
				(*text >= '0' && *text <= '9') ? digit_width : 8;
			lcd_draw_char_scaled_width(x, y, *text, color, bg, char_width, 2);
			x += char_width;
			text++;
		} else if (glyph_index == WEATHER_JP_GLYPH_COUNT) {
			text++;
		}
	}
}

static void format_audio_info(char *buf, uint8_t bit_depth, float sample_rate_khz) {
int ptr = 0;
if (bit_depth >= 10) buf[ptr++] = '0' + (bit_depth / 10);
buf[ptr++] = '0' + (bit_depth % 10);

const char *s1 = "bit / ";
while (*s1) buf[ptr++] = *s1++;

int int_part = (int)sample_rate_khz;
int frac_part = (int)((sample_rate_khz - (float)int_part) * 10.0f + 0.5f);
if (frac_part >= 10) { int_part++; frac_part = 0; }

if (int_part >= 100) {
buf[ptr++] = '0' + (int_part / 100);
int_part %= 100;
buf[ptr++] = '0' + (int_part / 10);
buf[ptr++] = '0' + (int_part % 10);
} else if (int_part >= 10) {
buf[ptr++] = '0' + (int_part / 10);
buf[ptr++] = '0' + (int_part % 10);
} else {
buf[ptr++] = '0' + int_part;
}

buf[ptr++] = '.';
buf[ptr++] = '0' + frac_part;
buf[ptr++] = 'k'; buf[ptr++] = 'H'; buf[ptr++] = 'z';
buf[ptr] = '\0';
}

void display_rta_set_format(uint8_t bit_depth, float sample_rate_khz) {
requested_bit_depth = bit_depth;
requested_sample_rate_tenth_khz = (uint32_t)(sample_rate_khz * 10.0f + 0.5f);
format_pending = true;
}

static void prepare_requested_format(uint8_t bit_depth, uint32_t sample_rate_tenth_khz) {
char buf[32];
format_audio_info(buf, bit_depth, (float)sample_rate_tenth_khz / 10.0f);
int len = 0; while (buf[len]) len++;
if (len > FORMAT_TEXT_CHARS) len = FORMAT_TEXT_CHARS;
memset(format_render_text, ' ', sizeof(format_render_text) - 1);
format_render_text[sizeof(format_render_text) - 1] = '\0';
const uint8_t start = (uint8_t)((FORMAT_TEXT_CHARS - len) / 2);
memcpy(&format_render_text[start], buf, (size_t)len);
format_render_pos = 0;
format_render_active = true;
}

void display_rta_set_bands(const uint8_t l_bands[SPECTRUM_BAND_COUNT], const uint8_t r_bands[SPECTRUM_BAND_COUNT]) {
bool changed = false;
for (int i = 0; i < SPECTRUM_BAND_COUNT; i++) {
const uint8_t left = (l_bands[i] > 100) ? 100 : l_bands[i];
const uint8_t right = (r_bands[i] > 100) ? 100 : r_bands[i];
if (input_l_bands[i] != left || input_r_bands[i] != right) changed = true;
input_l_bands[i] = left;
input_r_bands[i] = right;
}
if (changed) display_dirty = true;
}

void display_rta_set_peak(uint8_t l_peak, uint8_t r_peak) {
const uint8_t left = (l_peak > 100) ? 100 : l_peak;
const uint8_t right = (r_peak > 100) ? 100 : r_peak;
if (input_l_peak != left || input_r_peak != right) display_dirty = true;
input_l_peak = left;
input_r_peak = right;
}

void display_rta_reset_levels(void) {
reset_peak_dots_pending = true;
display_dirty = true;
}

void display_rta_set_levels(const uint8_t l_bands[SPECTRUM_BAND_COUNT], const uint8_t r_bands[SPECTRUM_BAND_COUNT], uint8_t l_peak, uint8_t r_peak) {
display_rta_set_bands(l_bands, r_bands);
display_rta_set_peak(l_peak, r_peak);
}

static void update_horizontal_meter(
uint16_t y,
uint8_t new_val,
uint16_t *prev_width,
uint16_t *peak_width,
uint32_t *hold_until,
uint32_t *next_fall,
uint16_t *last_marker,
uint16_t normal_color)
{
const uint16_t max_w = 284;
const uint16_t block_w = 2;
const uint16_t raw_w = (new_val * max_w) / 100;
const uint16_t prev_w = *prev_width;
uint16_t new_w = prev_w;

if (raw_w > prev_w + block_w / 2) {
new_w = (uint16_t)(((raw_w + block_w / 2) / block_w) * block_w);
} else if (raw_w + block_w / 2 < prev_w) {
new_w = (uint16_t)(((raw_w + block_w / 2) / block_w) * block_w);
}
if (new_w > max_w) new_w = max_w;

uint16_t warn_x = (((max_w * 74) / 100) / block_w) * block_w;
uint16_t clip_x = (((max_w * 96) / 100) / block_w) * block_w;

if (new_w > prev_w) {
if (prev_w < warn_x) {
uint16_t start = prev_w;
uint16_t end = (new_w < warn_x) ? new_w : warn_x;

    if (end > start) {
        lcd_fill_rect(23 + start, y, end - start, 4, normal_color);
    }
}

if (new_w > warn_x && prev_w < clip_x) {
    uint16_t start = (prev_w > warn_x) ? prev_w : warn_x;
    uint16_t end = (new_w < clip_x) ? new_w : clip_x;

    if (end > start) {
        lcd_fill_rect(23 + start, y, end - start, 4, COLOR_PEAK_WARN);
    }
}

if (new_w > clip_x) {
    uint16_t start = (prev_w > clip_x) ? prev_w : clip_x;

    if (new_w > start) {
        lcd_fill_rect(23 + start, y, new_w - start, 4, COLOR_PEAK_CLIP);
    }
}
} else if (new_w < prev_w) {
lcd_fill_rect(23 + new_w, y, prev_w - new_w, 4, COLOR_BG);
}

const uint32_t now = time_us_32();
if (new_w >= *peak_width && new_w > 0) {
*peak_width = new_w;
*hold_until = now + PEAK_HOLD_US;
*next_fall = *hold_until;
} else if (*peak_width > new_w &&
(int32_t)(now - *hold_until) >= 0 &&
(int32_t)(now - *next_fall) >= 0) {
*peak_width = (*peak_width > METER_PEAK_FALL_SPEED)
? (uint16_t)(*peak_width - METER_PEAK_FALL_SPEED) : 0;
if (*peak_width < new_w) *peak_width = new_w;
*next_fall = now + 25000u;
}

const uint16_t desired_marker = (*peak_width > 0)
? (uint16_t)(*peak_width - 1u) : PEAK_NONE_Y;
if (*last_marker != PEAK_NONE_Y &&
(*last_marker != desired_marker || new_w != prev_w)) {
const uint16_t marker_x = *last_marker;
uint16_t restore_color = COLOR_BG;
if (marker_x < new_w) {
restore_color = (marker_x >= clip_x) ? COLOR_PEAK_CLIP :
((marker_x >= warn_x) ? COLOR_PEAK_WARN : normal_color);
}
lcd_fill_rect(23 + marker_x, y, 1, 4, restore_color);
}
if (desired_marker != PEAK_NONE_Y &&
(*last_marker != desired_marker || new_w != prev_w)) {
lcd_fill_rect(23 + desired_marker, y, 1, 4, COLOR_PEAK_DOT);
}
*last_marker = desired_marker;
*prev_width = new_w;
}

static void update_vertical_bar(
uint16_t x,
uint8_t new_val,
uint16_t *prev_height,
uint32_t *last_update_us,
uint8_t *peak_dot,
uint32_t *hold_until,
uint32_t *next_fall,
uint16_t *last_peak_y,
uint16_t color)
{
uint16_t max_h = 98;
uint16_t y_base = 163;

const uint32_t now = time_us_32();
const uint16_t target_h = (new_val * max_h) / 100;
const uint16_t prev_h = *prev_height;
uint16_t new_h;
if (target_h >= prev_h) {
// Apply upward movement immediately.
new_h = target_h;
*last_update_us = now;
} else {
// Make downward movement proportional to elapsed time, independent of the render interval.
const uint32_t elapsed_us = now - *last_update_us;
const uint32_t fall_px = (uint32_t)(((uint64_t)elapsed_us * RTA_BAR_RELEASE_PIXELS_PER_SEC) / 1000000u);
new_h = prev_h;
if (fall_px > 0) {
const uint16_t remaining = prev_h - target_h;
new_h = (fall_px >= remaining) ? target_h : (uint16_t)(prev_h - fall_px);
*last_update_us = now;
}
}

if (target_h >= *peak_dot && target_h > 0) {
*peak_dot = target_h;
*hold_until = now + PEAK_HOLD_US;
*next_fall = *hold_until;
} else if (*peak_dot > 0 &&
(int32_t)(now - *hold_until) >= 0 &&
(int32_t)(now - *next_fall) >= 0) {
if (*peak_dot > PEAK_FALL_SPEED) {
*peak_dot -= PEAK_FALL_SPEED;
} else {
*peak_dot = 0;
}
*next_fall = now + 25000u;
}
if (*peak_dot < target_h) *peak_dot = target_h;

uint16_t new_bar_top = (new_h > 0) ? (y_base - new_h + 1) : (y_base + 1);
uint16_t new_peak_y = (*peak_dot > 0) ? (y_base - *peak_dot + 1) : PEAK_NONE_Y;

if (new_h > prev_h) {
lcd_fill_rect(x + 1, y_base - new_h + 1, 8, new_h - prev_h, color);
} else if (new_h < prev_h) {
lcd_fill_rect(x + 1, y_base - prev_h + 1, 8, prev_h - new_h, COLOR_BG);
}
*prev_height = new_h;

const bool marker_visible = new_peak_y != PEAK_NONE_Y &&
new_peak_y >= 66 && new_peak_y <= 163 &&
(new_peak_y < new_bar_top || new_h == 0);
if (*last_peak_y != PEAK_NONE_Y &&
(!marker_visible || *last_peak_y != new_peak_y)) {
if (*last_peak_y >= 66 && *last_peak_y <= 163) {
const uint16_t restore_color =
(new_h > 0 && *last_peak_y >= new_bar_top && *last_peak_y <= y_base)
? color : COLOR_BG;
lcd_fill_rect(x + 1, *last_peak_y, 8, 1, restore_color);
}
*last_peak_y = PEAK_NONE_Y;
}

if (marker_visible && *last_peak_y != new_peak_y) {
lcd_fill_rect(x + 1, new_peak_y, 8, 1, COLOR_PEAK_DOT);
*last_peak_y = new_peak_y;
}
}

void display_rta_clear_screen(void) {
lcd_fill_color(COLOR_BG);
}

void display_rta_draw_splash_screen(void) {
static const char *const boot_steps[6] = {
    "Display Initialization",
    "DAC Initialization",
    "Core1 Audio Engine",
    "FFT Initialization",
    "USB Audio Ready",
    "System Ready"
};

#ifndef BUILD_TIMESTAMP
    #define BUILD_TIMESTAMP "Unknown Build"
    #endif

    lcd_draw_rect(8, 8, 304, 224, COLOR_DIVIDER);
    lcd_draw_rect(10, 10, 300, 220, COLOR_DIVIDER);

    lcd_draw_string(20, 20, "SYSTEM BOOT: Quad Heritage", COLOR_TITLE, COLOR_BG, 1);
    lcd_fill_rect(20, 32, 280, 1, COLOR_DIVIDER);

    lcd_draw_string(20, 44, "Pico2 UAC12 USB DDC with Weather", COLOR_TITLE, COLOR_BG, 1);
    lcd_draw_string(20, 58, "Copilot x GEMINI x ChatGPT x Claude", COLOR_CH_LABEL, COLOR_BG, 1);
    lcd_draw_string(20, 72, "Created by Toshimiyu", COLOR_L_THEME, COLOR_BG, 1);

    // --- Center information box ---
    lcd_fill_rect(20, 88, 280, 70, 0x0081);
    lcd_draw_rect(20, 88, 280, 70, COLOR_DIVIDER);

    lcd_draw_string(28, 95,  "Architecture : Pico2 & PCM5100A", COLOR_FREQ_TEXT, 0x0081, 1);
    lcd_draw_string(28, 109, "Version      : v" FW_VERSION,   COLOR_FREQ_TEXT, 0x0081, 1);

#if USB_AUDIO_CLASS_VERSION == 1
    lcd_draw_string(28, 123, "USB Audio    : UAC1", COLOR_FREQ_TEXT, 0x0081, 1);
#else
    lcd_draw_string(28, 123, "USB Audio    : UAC2", COLOR_FREQ_TEXT, 0x0081, 1);
#endif

    char build_buf[40];
    snprintf(build_buf, sizeof(build_buf), "Build        : %s", BUILD_TIMESTAMP);
    lcd_draw_string(28, 137, build_buf, COLOR_FREQ_TEXT, 0x0081, 1);
    // -----------------------------------------------------------------


    lcd_fill_rect(20, 162, 280, 1, COLOR_DIVIDER);
    lcd_draw_string(20, 174, splash_stage >= 5 ? "System Ready" : "Initializing Subsystems",
        COLOR_TITLE, COLOR_BG, 1);

    splash_start_us = time_us_32();

    lcd_draw_string(240, 174, ".", COLOR_TITLE, COLOR_BG, 1);
    lcd_draw_string(20, 190, boot_steps[splash_stage <= 5 ? splash_stage : 0], COLOR_DIVIDER, COLOR_BG, 1);
    lcd_fill_rect(20, 210, 280, 1, COLOR_DIVIDER);
    lcd_draw_string(32, 218, "X:NEXT PAGE", COLOR_CH_LABEL, COLOR_BG, 1);
}

void display_rta_set_boot_stage(uint8_t stage)
{
    static const char *const boot_steps[6] = {
        "Display Initialization",
        "DAC Initialization",
        "Core1 Audio Engine",
        "FFT Initialization",
        "USB Audio Ready",
        "System Ready"
    };

    if(stage > 5)
    {
        stage = 5;
    }

    splash_stage = stage;
    if (current_page != DISPLAY_PAGE_BOOT || page_clear_pending)
    {
        return;
    }

    lcd_fill_rect(20, 190, 250, 12, COLOR_BG);

    lcd_draw_string(
        20,
        190,
        boot_steps[stage],
        COLOR_DIVIDER,
        COLOR_BG,
        1
    );

    char dots[7] = {0};

    for(uint8_t i = 0; i <= stage; i++)
    {
        dots[i] = '.';
    }

    lcd_fill_rect(240, 174, 48, 8, COLOR_BG);

    lcd_draw_string(
        240,
        174,
        dots,
        COLOR_TITLE,
        COLOR_BG,
        1
    );
}

void display_rta_hw_init(void) {
spi_init(spi0, 40000000);
gpio_set_function(LCD_SCK_PIN, GPIO_FUNC_SPI);
gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);

gpio_init(LCD_CS_PIN);
gpio_set_dir(LCD_CS_PIN, GPIO_OUT);
gpio_put(LCD_CS_PIN, 1);

gpio_init(LCD_DC_PIN);
gpio_set_dir(LCD_DC_PIN, GPIO_OUT);

gpio_init(LCD_BL_PIN);
gpio_set_dir(LCD_BL_PIN, GPIO_OUT);
gpio_put(LCD_BL_PIN, 1);
sleep_ms(100);

// Pico Display Pack 2.0 RGB channels are active-low on GP6, GP7, and GP8.
// Drive all three high so the unused color LED remains off.
for (uint pin = 6; pin <= 8; ++pin) {
    gpio_init(pin);
    gpio_put(pin, true);
    gpio_set_dir(pin, GPIO_OUT);
}

gpio_init(DISPLAY_BUTTON_X_PIN);
gpio_set_dir(DISPLAY_BUTTON_X_PIN, GPIO_IN);
gpio_pull_up(DISPLAY_BUTTON_X_PIN);
gpio_init(DISPLAY_BUTTON_B_PIN);
gpio_set_dir(DISPLAY_BUTTON_B_PIN, GPIO_IN);
gpio_pull_up(DISPLAY_BUTTON_B_PIN);
gpio_init(DISPLAY_BUTTON_Y_PIN);
gpio_set_dir(DISPLAY_BUTTON_Y_PIN, GPIO_IN);
gpio_pull_up(DISPLAY_BUTTON_Y_PIN);
const uint32_t buttons_initialized_us = time_us_32();
button_x.raw_high = gpio_get(DISPLAY_BUTTON_X_PIN);
button_x.stable_high = button_x.raw_high;
button_x.raw_changed_us = buttons_initialized_us;
button_b.raw_high = gpio_get(DISPLAY_BUTTON_B_PIN);
button_b.stable_high = button_b.raw_high;
button_b.raw_changed_us = buttons_initialized_us;
button_y.raw_high = gpio_get(DISPLAY_BUTTON_Y_PIN);
button_y.stable_high = button_y.raw_high;
button_y.raw_changed_us = buttons_initialized_us;

lcd_write_cmd(0x01); // SWRESET
sleep_ms(150);

lcd_write_cmd(0x11); // SLPOUT
sleep_ms(150);

lcd_write_cmd(0x21); // Display Inversion ON
lcd_write_cmd(0x29); // DISPON

lcd_write_cmd(0x3A); // COLMOD
lcd_write_data(0x55); // 16-bit color

lcd_write_cmd(0x36); // MADCTL
lcd_write_data(0x60); // Screen rotation.

lcd_fill_color(COLOR_BG);
}

// Draw frequency labels (staggered layout).
static void draw_rta_labels(uint16_t x_start) {
for (size_t i = 0; i < NUM_FREQ_LABELS; i++) {
uint8_t idx = freq_labels[i].index;
const char *text = freq_labels[i].text;

    uint16_t center_x = x_start + (idx * RTA_CELL_W) + (RTA_BAR_W / 2);
    uint8_t text_len = strlen(text);
    int16_t text_x = center_x - ((text_len * FONT_CHAR_WIDTH) / 2);

    bool is_bottom = (i % 2 != 0);
    uint16_t text_y = is_bottom ? RTA_LABEL_Y_BOTTOM : RTA_LABEL_Y_TOP;

    lcd_draw_string(text_x, text_y, text, COLOR_FREQ_TEXT, COLOR_BG, 1);
}
}

static void draw_playback_screen(void) {
memset(prev_l_bands, 0, sizeof(prev_l_bands));
memset(prev_r_bands, 0, sizeof(prev_r_bands));
memset(peak_l_dots, 0, sizeof(peak_l_dots));
memset(peak_r_dots, 0, sizeof(peak_r_dots));
memset(peak_l_hold_until, 0, sizeof(peak_l_hold_until));
memset(peak_r_hold_until, 0, sizeof(peak_r_hold_until));
memset(peak_l_next_fall, 0, sizeof(peak_l_next_fall));
memset(peak_r_next_fall, 0, sizeof(peak_r_next_fall));
memset(bar_last_update_us, 0, sizeof(bar_last_update_us));
prev_l_meter_width = 0;
prev_r_meter_width = 0;
peak_l_meter_width = 0;
peak_r_meter_width = 0;
last_l_meter_marker = PEAK_NONE_Y;
last_r_meter_marker = PEAK_NONE_Y;
reset_peak_dots_pending = true;
render_pending = false;
render_step = 0;
for (int i = 0; i < SPECTRUM_BAND_COUNT; i++) {
last_l_peak_y[i] = PEAK_NONE_Y;
last_r_peak_y[i] = PEAK_NONE_Y;
}

lcd_draw_string(8, 6, "Pico2 UAC12 USB DDC", COLOR_TITLE, COLOR_BG, 2);
lcd_fill_rect(10, 26, 300, 1, COLOR_DIVIDER);

lcd_draw_string(10, 30, "L", COLOR_CH_LABEL, COLOR_BG, 1);
lcd_draw_rect(22, 30, 288, 6, COLOR_FRAME_L);

lcd_draw_string(10, 39, "R", COLOR_CH_LABEL, COLOR_BG, 1);
lcd_draw_rect(22, 39, 288, 6, COLOR_FRAME_R);

lcd_fill_rect(10, 48, 300, 1, COLOR_DIVIDER);

lcd_draw_string(20, 52, "L-Channel", COLOR_CH_LABEL, COLOR_BG, 1);
lcd_draw_string(180, 52, "R-Channel", COLOR_CH_LABEL, COLOR_BG, 1);
lcd_fill_rect(159, 50, 2, 145, COLOR_DIVIDER);

/* Draw FFT bar frames. */
for (uint8_t i = 0; i < SPECTRUM_BAND_COUNT; ++i) {
const uint16_t lx = RTA_L_X_START + i * RTA_CELL_W;
const uint16_t rx = RTA_R_X_START + i * RTA_CELL_W;

lcd_draw_rect(lx, RTA_BAR_Y, RTA_BAR_W, RTA_BAR_H, COLOR_FRAME_L);
lcd_draw_rect(rx, RTA_BAR_Y, RTA_BAR_W, RTA_BAR_H, COLOR_FRAME_R);
}

/* Draw frequency labels (invoke staggered layout). */
draw_rta_labels(RTA_L_X_START);
draw_rta_labels(RTA_R_X_START);

lcd_fill_rect(10, 198, 300, 1, COLOR_DIVIDER);
lcd_fill_rect(10, 218, 300, 1, COLOR_DIVIDER);
lcd_draw_string(32, 226, "X:NEXT PAGE", COLOR_CH_LABEL, COLOR_BG, 1);
display_ready = true;
}

#define WEATHER_CENTER_X 160
#define WEATHER_LEFT_CENTER_X 84
#define WEATHER_RIGHT_CENTER_X 235
#define WEATHER_RENDER_INTERVAL_US (1000000u / 24u)

static uint32_t weather_next_render_us;
static bool weather_render_rate_started;

static void draw_weather_screen(void)
{
	lcd_draw_string(12, 6, "WEATHER", COLOR_TITLE, COLOR_BG, 2);
	lcd_fill_rect(10, 26, 300, 1, COLOR_DIVIDER);
	lcd_fill_rect(10, 48, 300, 1, COLOR_DIVIDER);
	lcd_fill_rect(159, 48, 1, 130, COLOR_DIVIDER);
	lcd_draw_utf8_string(WEATHER_LEFT_CENTER_X - 16, 66, "今日", COLOR_CH_LABEL, COLOR_BG, 2);
	lcd_draw_utf8_string(WEATHER_RIGHT_CENTER_X - 16, 66, "明日", COLOR_CH_LABEL, COLOR_BG, 2);
	lcd_fill_rect(10, 184, 300, 1, COLOR_DIVIDER);
	lcd_fill_rect(10, 218, 300, 1, COLOR_DIVIDER);
	lcd_draw_string_centered(WEATHER_CENTER_X, 226, "X:NEXT PAGE  B:3S DIAG", COLOR_CH_LABEL, COLOR_BG, 1);
	display_ready = true;
}

static uint16_t weather_condition_color(weather_condition_t condition)
{
	switch (condition)
	{
	case WEATHER_CONDITION_SUNNY: return 0xF800;
	case WEATHER_CONDITION_CLOUDY: return 0x8410;
	case WEATHER_CONDITION_RAIN: return 0x07FF;
	case WEATHER_CONDITION_SNOW: return COLOR_PEAK_DOT;
	default: return COLOR_FREQ_TEXT;
	}
}

static void draw_weather_glyphs(uint16_t *x, uint16_t y, uint16_t color,
	uint8_t first, uint8_t second)
{
	lcd_draw_char_scaled_width(*x, y, (char)first, color, COLOR_BG, 16, 2);
	*x += 16;
	if (second != 0)
	{
		lcd_draw_char_scaled_width(*x, y, (char)second, color, COLOR_BG, 16, 2);
		*x += 16;
	}
}

static void draw_weather_condition(uint16_t *x, uint16_t y, weather_condition_t condition)
{
	const uint16_t color = weather_condition_color(condition);
	switch (condition)
	{
	case WEATHER_CONDITION_SUNNY: draw_weather_glyphs(x, y, color, 0x80, 0x86); break;
	case WEATHER_CONDITION_CLOUDY: draw_weather_glyphs(x, y, color, 0x81, 0x87); break;
	case WEATHER_CONDITION_RAIN: draw_weather_glyphs(x, y, color, 0x82, 0); break;
	case WEATHER_CONDITION_SNOW: draw_weather_glyphs(x, y, color, 0x83, 0); break;
	default: break;
	}
}

static uint16_t weather_condition_width(weather_condition_t condition)
{
	switch (condition)
	{
	case WEATHER_CONDITION_SUNNY:
	case WEATHER_CONDITION_CLOUDY: return 32;
	case WEATHER_CONDITION_RAIN:
	case WEATHER_CONDITION_SNOW: return 16;
	default: return 0;
	}
}

static void draw_weather_phrase(uint16_t center_x, uint16_t y, const weather_day_t *day)
{
	if (!day->valid)
	{
		lcd_draw_string_centered(center_x, y + 4, "NO DATA", COLOR_FREQ_TEXT, COLOR_BG, 1);
		return;
	}

	uint16_t width = weather_condition_width(day->first) +
		weather_condition_width(day->second);
	if (day->connector != WEATHER_CONNECTOR_NONE)
		width += 32;
	uint16_t x = center_x - width / 2;

	draw_weather_condition(&x, y, day->first);
	if (day->connector == WEATHER_CONNECTOR_INTERMITTENT)
		draw_weather_glyphs(&x, y, COLOR_FREQ_TEXT, 0x84, 0x85);
	else if (day->connector == WEATHER_CONNECTOR_THEN)
		draw_weather_glyphs(&x, y, COLOR_FREQ_TEXT, 0x88, 0x89);
	draw_weather_condition(&x, y, day->second);
}

static void draw_weather_day_details(uint16_t center_x, const weather_day_t *day)
{
	char text[48];
	if (day->valid && day->has_temperature)
		snprintf(text, sizeof(text), "最高%d℃/最低%d℃", day->high_c, day->low_c);
	else
		snprintf(text, sizeof(text), "最高--℃/最低--℃");
	lcd_draw_utf8_temperature_centered(center_x, 122, text, COLOR_FREQ_TEXT, COLOR_BG);

	if (day->valid)
	{
		static const char *const fullwidth_digits[] = {
			"０", "１", "２", "３", "４", "５", "６", "７", "８", "９"
		};
		char percentage[4];
		size_t text_length;
		snprintf(percentage, sizeof(percentage), "%u", (unsigned)day->precipitation_percent);
		snprintf(text, sizeof(text), "降水確率");
		text_length = strlen(text);
		for (size_t i = 0; percentage[i] != '\0'; ++i) {
			const char *digit = fullwidth_digits[percentage[i] - '0'];
			const size_t digit_length = strlen(digit);
			memcpy(text + text_length, digit, digit_length);
			text_length += digit_length;
		}
		memcpy(text + text_length, "％", sizeof("％"));
	}
	else
		snprintf(text, sizeof(text), "降水確率--％");
	lcd_draw_utf8_string_centered(center_x, 152, text, COLOR_FREQ_TEXT, COLOR_BG, 1);
}

static void draw_weather_datetime(void)
{
	char text[32];
	lcd_fill_rect(10, 27, 300, 21, COLOR_BG);
	if (weather_data.has_time)
		snprintf(text, sizeof(text), "%02u/%02u/%02u %02u:%02u",
			(unsigned)(weather_data.year % 100u), weather_data.month,
			weather_data.day, weather_data.hour, weather_data.minute);
	else
		snprintf(text, sizeof(text), "--/--/-- --:--");
	lcd_draw_string_centered_scaled(WEATHER_CENTER_X, 33, text,
		COLOR_FREQ_TEXT, COLOR_BG, 2, 1);
}

static void draw_weather_day_phrase(uint16_t center_x, const weather_day_t *day)
{
	lcd_fill_rect(center_x - 72, 84, 144, 34, COLOR_BG);
	draw_weather_phrase(center_x, 90, day);
}

static void draw_weather_day_block(uint16_t center_x, const weather_day_t *day)
{
	lcd_fill_rect(center_x - 72, 118, 144, 60, COLOR_BG);
	draw_weather_day_details(center_x, day);
}

static void draw_weather_pico_temperature(void)
{
	char text[32];
	lcd_fill_rect(12, 188, 144, 24, COLOR_BG);
	if (weather_data.has_pico_temperature)
	{
		const int temperature = weather_data.pico_temperature_tenths_c;
		const int absolute_temperature = temperature < 0 ? -temperature : temperature;
		snprintf(text, sizeof(text), "%s%d.%dC",
			temperature < 0 ? "-" : "", absolute_temperature / 10,
			absolute_temperature % 10);
		const uint16_t temperature_color = temperature >= 600 ? 0xF800 :
			(temperature >= 500 ? 0xFFE0 : COLOR_PEAK_DOT);
		uint8_t temperature_scale_x = 2;
		uint16_t value_width = (uint16_t)(strlen(text) * 8u * temperature_scale_x);
		while (32u + 8u + value_width > 144u && temperature_scale_x > 1) {
			temperature_scale_x--;
			value_width = (uint16_t)(strlen(text) * 8u * temperature_scale_x);
		}
		const uint16_t total_width = 32u + 8u + value_width;
		uint16_t x = WEATHER_LEFT_CENTER_X - total_width / 2u;
		lcd_draw_string_scaled(x, 194, "PICO", temperature_color, COLOR_BG, 1, 1);
		x += 40u;
		lcd_draw_string_scaled(x, 194, text, temperature_color, COLOR_BG,
			temperature_scale_x, 1);
	}
}

static void draw_weather_usb_status(void)
{
	lcd_fill_rect(164, 188, 144, 24, COLOR_BG);
	const char *usb_text = "USB WAIT";
	uint16_t usb_color = 0x8410;
	if (weather_data.usb_status == WEATHER_USB_OK)
	{
		usb_text = "USB OK";
		usb_color = 0x07E0;
	}
	else if (weather_data.usb_status == WEATHER_USB_LOST)
	{
		usb_text = "USB LOST";
		usb_color = 0xF800;
	}
	lcd_draw_string_centered_scaled(WEATHER_RIGHT_CENTER_X, 194, usb_text,
		usb_color, COLOR_BG, 2, 1);
}

// The countdown overlay shares the weather page's bottom status row.
static void render_mode_countdown(void)
{
	if (!mode_countdown_dirty)
		return;
	mode_countdown_dirty = false;
	if (mode_countdown_display == 0)
	{
		weather_drawn_valid = false;
		return;
	}
	char text[24];
	snprintf(text, sizeof(text), "DIAG MODE IN %u", (unsigned)mode_countdown_display);
	lcd_fill_rect(10, 188, 300, 24, COLOR_DIVIDER);
	lcd_draw_string_centered(WEATHER_CENTER_X, 194, text, COLOR_PEAK_DOT, COLOR_DIVIDER, 1);
}

enum {
	WEATHER_ELEMENT_DATETIME,
	WEATHER_ELEMENT_TODAY_PHRASE,
	WEATHER_ELEMENT_TOMORROW_PHRASE,
	WEATHER_ELEMENT_TODAY_DETAILS,
	WEATHER_ELEMENT_TOMORROW_DETAILS,
	WEATHER_ELEMENT_PICO_TEMPERATURE,
	WEATHER_ELEMENT_USB_STATUS,
	WEATHER_ELEMENT_COUNT
};

static uint8_t weather_drawn_mask;

static bool weather_element_dirty(unsigned element)
{
	const weather_snapshot_t *n = &weather_data;
	const weather_snapshot_t *o = &weather_drawn_data;
	if (!(weather_drawn_mask & (1u << element)))
		return true;
	switch (element) {
	case WEATHER_ELEMENT_DATETIME:
		return n->has_time != o->has_time || n->year != o->year ||
			n->month != o->month || n->day != o->day ||
			n->hour != o->hour || n->minute != o->minute;
	case WEATHER_ELEMENT_TODAY_PHRASE:
	case WEATHER_ELEMENT_TODAY_DETAILS:
		return memcmp(&n->today, &o->today, sizeof(n->today)) != 0;
	case WEATHER_ELEMENT_TOMORROW_PHRASE:
	case WEATHER_ELEMENT_TOMORROW_DETAILS:
		return memcmp(&n->tomorrow, &o->tomorrow, sizeof(n->tomorrow)) != 0;
	case WEATHER_ELEMENT_PICO_TEMPERATURE:
		return n->has_pico_temperature != o->has_pico_temperature ||
			n->pico_temperature_tenths_c != o->pico_temperature_tenths_c;
	default:
		return n->usb_status != o->usb_status;
	}
}

// Each phrase and details element tracks its own copy so one can stay dirty while the other is drawn.
static weather_snapshot_t weather_element_drawn[WEATHER_ELEMENT_COUNT];

static void render_weather_page(void)
{
	render_mode_countdown();
	if (mode_countdown_display != 0)
		return;
	if (!weather_drawn_valid)
	{
		weather_drawn_mask = 0;
		weather_drawn_valid = true;
	}
	for (unsigned element = 0; element < WEATHER_ELEMENT_COUNT; ++element)
	{
		weather_drawn_data = weather_element_drawn[element];
		const bool dirty = weather_element_dirty(element);
		if (!dirty)
			continue;
		switch (element) {
		case WEATHER_ELEMENT_DATETIME: draw_weather_datetime(); break;
		case WEATHER_ELEMENT_TODAY_PHRASE:
			draw_weather_day_phrase(WEATHER_LEFT_CENTER_X, &weather_data.today); break;
		case WEATHER_ELEMENT_TOMORROW_PHRASE:
			draw_weather_day_phrase(WEATHER_RIGHT_CENTER_X, &weather_data.tomorrow); break;
		case WEATHER_ELEMENT_TODAY_DETAILS:
			draw_weather_day_block(WEATHER_LEFT_CENTER_X, &weather_data.today); break;
		case WEATHER_ELEMENT_TOMORROW_DETAILS:
			draw_weather_day_block(WEATHER_RIGHT_CENTER_X, &weather_data.tomorrow); break;
		case WEATHER_ELEMENT_PICO_TEMPERATURE: draw_weather_pico_temperature(); break;
		default: draw_weather_usb_status(); break;
		}
		weather_element_drawn[element] = weather_data;
		weather_drawn_mask |= 1u << element;
		return;
	}
}

void display_rta_init(void) {
lcd_fill_color(COLOR_BG);
draw_playback_screen();
}

display_page_t display_rta_get_page(void)
{
    return current_page;
}

void display_rta_set_page(display_page_t page)
{
    if (page > DISPLAY_PAGE_WEATHER ||
        (page == DISPLAY_PAGE_DIAGNOSTICS && !diagnostic_mode_active) ||
        page == current_page)
    {
        return;
    }

    current_page = page;
    weather_drawn_valid = false;
    mode_countdown_display = 0;
    mode_countdown_dirty = false;
    page_clear_row = 0;
    page_clear_pending = true;
    page_draw_pending = false;
    page_draw_count = 0;
    page_draw_pos = 0;
    page_ready = false;
}

bool display_rta_is_diagnostic_mode(void)
{
    return diagnostic_mode_active;
}

void display_rta_set_mode(bool diagnostic_mode)
{
    diagnostic_mode_active = diagnostic_mode;
    display_rta_set_page(diagnostic_mode ? DISPLAY_PAGE_DIAGNOSTICS : DISPLAY_PAGE_WEATHER);
}

void display_rta_set_mode_countdown(uint8_t seconds_remaining)
{
    if (mode_countdown_display == seconds_remaining)
        return;
    mode_countdown_display = seconds_remaining;
    mode_countdown_dirty = true;
}

void display_rta_set_weather_data(const weather_snapshot_t *snapshot)
{
    if (snapshot != NULL)
        weather_data = *snapshot;
}

static bool display_audio_has_slack(void)
{
    const uint32_t now = time_us_32();
    const bool audio_active = audio_data_seen &&
        (uint32_t)(now - audio_last_data_us) < 200000u;
    if (audio_active)
    {
        return audio_ring_total_frames() >= DISPLAY_MIN_AUDIO_HEADROOM_SAMPLES ||
            dma_tx.using >= (DEPTH_DMA_TX_BUFFER / 2);
    }

    return !enable_output;
}

// The weather page has no FFT work, so it only yields while audio is actually flowing.
// display_audio_has_slack() also blocks when the stream is open but idle.
static bool display_weather_has_slack(void)
{
    const uint32_t now = time_us_32();
    const bool audio_active = audio_data_seen &&
        (uint32_t)(now - audio_last_data_us) < 200000u;
    return !audio_active || display_audio_has_slack();
}

bool display_rta_page_service(void)
{
    if (page_clear_pending)
    {
        if (page_clear_row < 240)
        {
            if (!display_audio_has_slack())
            {
                return false;
            }

            memset(lcd_fill_chunk, 0, sizeof(lcd_fill_chunk));
            lcd_set_window(0, page_clear_row, 319, page_clear_row);
            gpio_put(LCD_DC_PIN, 1);
            gpio_put(LCD_CS_PIN, 0);
            spi_write_blocking(spi0, lcd_fill_chunk, sizeof(lcd_fill_chunk));
            spi_write_blocking(spi0, lcd_fill_chunk, 128);
            gpio_put(LCD_CS_PIN, 1);
            page_clear_row++;
            if (page_clear_row < 240)
            {
                return false;
            }
        }

        page_clear_pending = false;
        switch (current_page)
        {
        case DISPLAY_PAGE_BOOT:
            page_draw_begin_capture();
            display_rta_draw_splash_screen();
            display_rta_set_boot_stage(splash_stage);
            page_draw_end_capture();
            break;
        case DISPLAY_PAGE_PLAYBACK:
            page_draw_begin_capture();
            draw_playback_screen();
            page_draw_end_capture();
            break;
        case DISPLAY_PAGE_WEATHER:
            page_draw_begin_capture();
            draw_weather_screen();
            page_draw_end_capture();
            break;
        case DISPLAY_PAGE_DIAGNOSTICS:
            page_ready = true;
            break;
        }
        return true;
    }

    if (page_draw_pending)
    {
        if (!display_audio_has_slack())
        {
            return false;
        }

        const page_draw_op_t op = page_draw_ops[page_draw_pos++];
        if (op.type == PAGE_DRAW_FILL)
        {
            lcd_fill_rect(op.x, op.y, op.w, op.h, op.color);
        }
        else
        {
            lcd_draw_char_scaled_width(op.x, op.y, (char)op.character,
                op.color, op.bg, op.w, op.scale_y);
        }

        if (page_draw_pos >= page_draw_count)
        {
            page_draw_pending = false;
            page_ready = true;
        }
    }

    return false;
}

bool display_rta_page_ready(void)
{
    return page_ready;
}

display_button_action_t display_rta_process_buttons(void)
{
    const uint32_t now = time_us_32();
    const bool x_pressed = display_button_pressed(&button_x, DISPLAY_BUTTON_X_PIN, now);
    display_button_pressed(&button_y, DISPLAY_BUTTON_Y_PIN, now);
    display_button_pressed(&button_b, DISPLAY_BUTTON_B_PIN, now);
    const bool y_raw_down = !button_y.raw_high;
    const bool b_raw_down = !button_b.raw_high;

    // Holding B for 3 seconds switches modes from the Weather or diagnostics screen.
    if (b_raw_down &&
        (current_page == DISPLAY_PAGE_WEATHER || current_page == DISPLAY_PAGE_DIAGNOSTICS))
    {
        if (!mode_hold_tracking)
        {
            mode_hold_tracking = true;
            mode_hold_start_us = button_b.raw_changed_us;
        }

        const uint32_t hold_us = (uint32_t)(now - mode_hold_start_us);
        if (hold_us >= MODE_SWITCH_HOLD_MS * 1000u && !mode_switch_latched)
        {
            mode_switch_latched = true;
            mode_last_countdown = 0;
            return DISPLAY_BUTTON_ACTION_MODE_SWITCH;
        }

        if (!mode_switch_latched)
        {
            const uint8_t seconds_remaining = 3u - (uint8_t)(hold_us / 1000000u);
            if (seconds_remaining != mode_last_countdown)
            {
                mode_last_countdown = seconds_remaining;
                switch (seconds_remaining)
                {
                case 3: return DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_3;
                case 2: return DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_2;
                case 1: return DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_1;
                default: break;
                }
            }
        }
        return DISPLAY_BUTTON_ACTION_NONE;
    }

    if (mode_hold_tracking)
    {
        mode_hold_tracking = false;
        mode_switch_latched = false;
        mode_last_countdown = 0;
        if (mode_countdown_display != 0)
            return DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_CLEARED;
    }

    if (current_page == DISPLAY_PAGE_DIAGNOSTICS)
    {
        if (y_raw_down)
        {
            if (!diagnostics_y_hold_tracking)
            {
                diagnostics_y_hold_tracking = true;
                diagnostics_y_hold_start_us = button_y.raw_changed_us;
            }

            const uint32_t hold_us = (uint32_t)(now - diagnostics_y_hold_start_us);
            if (hold_us >= DIAGNOSTICS_RESET_HOLD_US && !diagnostics_reset_latched)
            {
                diagnostics_reset_latched = true;
                diagnostics_last_countdown = 0;
                return DISPLAY_BUTTON_ACTION_RESET_DIAGNOSTICS;
            }

            if (!diagnostics_reset_latched)
            {
                const uint8_t seconds_remaining =
                    3u - (uint8_t)(hold_us / 1000000u);
                if (seconds_remaining != diagnostics_last_countdown)
                {
                    diagnostics_last_countdown = seconds_remaining;
                    switch (seconds_remaining)
                    {
                    case 3:
                        return DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_3;
                    case 2:
                        return DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_2;
                    case 1:
                        return DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_1;
                    default:
                        break;
                    }
                }
            }

            return DISPLAY_BUTTON_ACTION_NONE;
        }

        const bool countdown_was_active = diagnostics_y_hold_tracking;
        diagnostics_y_hold_tracking = false;
        diagnostics_reset_latched = false;
        diagnostics_last_countdown = 0;
        if (countdown_was_active)
            return DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_CLEARED;

        if (x_pressed)
        {
            return DISPLAY_BUTTON_ACTION_NONE;
        }
        return DISPLAY_BUTTON_ACTION_NONE;
    }

    diagnostics_y_hold_tracking = false;
    diagnostics_reset_latched = false;
    diagnostics_last_countdown = 0;

    if (!x_pressed)
        return DISPLAY_BUTTON_ACTION_NONE;

    const display_page_t next_page = current_page == DISPLAY_PAGE_BOOT
        ? DISPLAY_PAGE_PLAYBACK
        : (current_page == DISPLAY_PAGE_PLAYBACK
            ? DISPLAY_PAGE_WEATHER : DISPLAY_PAGE_BOOT);
    display_rta_set_page(next_page);
    return DISPLAY_BUTTON_ACTION_PAGE_CHANGED;
}

bool display_rta_process_fft_left(void) {
if (!display_ready) return false;

const uint32_t now = time_us_32();
const bool timed_out = audio_data_seen && (uint32_t)(now - audio_last_data_us) >= 200000u;
const bool audio_active = audio_data_seen && !timed_out;
if (!audio_active) return false;

if (dma_tx.using < (DEPTH_DMA_TX_BUFFER / 2) &&
audio_ring_total_frames() < DISPLAY_MIN_AUDIO_HEADROOM_SAMPLES)
return false;

return spectrum_process_left(spectrum_l);
}

void display_rta_process_fft_right(void) {
uint8_t spectrum_r[SPECTRUM_BAND_COUNT];
if (spectrum_process_right(spectrum_r))
display_rta_set_bands(spectrum_l, spectrum_r);
}

static bool peak_decay_due(void) {
const uint32_t now = time_us_32();
if ((peak_l_meter_width > prev_l_meter_width &&
(int32_t)(now - peak_l_meter_hold_until) >= 0 &&
(int32_t)(now - peak_l_meter_next_fall) >= 0) ||
(peak_r_meter_width > prev_r_meter_width &&
(int32_t)(now - peak_r_meter_hold_until) >= 0 &&
(int32_t)(now - peak_r_meter_next_fall) >= 0))
return true;
for (uint8_t i = 0; i < SPECTRUM_BAND_COUNT; ++i) {
if ((peak_l_dots[i] > 0 &&
(int32_t)(now - peak_l_hold_until[i]) >= 0 &&
(int32_t)(now - peak_l_next_fall[i]) >= 0) ||
(peak_r_dots[i] > 0 &&
(int32_t)(now - peak_r_hold_until[i]) >= 0 &&
(int32_t)(now - peak_r_next_fall[i]) >= 0))
return true;
}
return false;
}

void display_rta_render_service(void) {
if (display_ready && page_ready && current_page == DISPLAY_PAGE_DIAGNOSTICS) {
// Countdown overlay on the diagnostics log header row; main.c restores the header when it clears.
if (mode_countdown_dirty && mode_countdown_display != 0 && display_audio_has_slack()) {
mode_countdown_dirty = false;
char text[24];
snprintf(text, sizeof(text), "GADGET MODE IN %u", (unsigned)mode_countdown_display);
lcd_fill_rect(8, 108, 304, 10, COLOR_DIVIDER);
lcd_draw_string_centered(WEATHER_CENTER_X, 109, text, COLOR_PEAK_DOT, COLOR_DIVIDER, 1);
}
return;
}
if (display_ready && page_ready && current_page == DISPLAY_PAGE_WEATHER) {
const uint32_t now = time_us_32();
if (display_weather_has_slack() &&
    (!weather_render_rate_started || (int32_t)(now - weather_next_render_us) >= 0)) {
weather_render_rate_started = true;
weather_next_render_us = now + WEATHER_RENDER_INTERVAL_US;
render_weather_page();
}
return;
}
if (!display_ready || !page_ready || current_page != DISPLAY_PAGE_PLAYBACK) return;

const bool audio_slack = display_audio_has_slack();
const bool format_slack = audio_slack;
if (format_pending || format_render_active) {
if (!format_slack) return;
if (format_pending) {
const uint8_t bit_depth = requested_bit_depth;
const uint32_t sample_rate = requested_sample_rate_tenth_khz;
format_pending = false;
prepare_requested_format(bit_depth, sample_rate);
}
const uint16_t x = (uint16_t)FORMAT_TEXT_X0 + (uint16_t)format_render_pos * FORMAT_TEXT_CHAR_WIDTH;
lcd_draw_char_scaled_width(x, FORMAT_TEXT_Y, format_render_text[format_render_pos],
      COLOR_FMT_TEXT, COLOR_BG, FORMAT_TEXT_CHAR_WIDTH, FORMAT_TEXT_SCALE_Y);
if (++format_render_pos >= FORMAT_TEXT_CHARS) format_render_active = false;
return;
}

if (!audio_slack) return;

if (!display_dirty && !reset_peak_dots_pending && !render_pending && !peak_decay_due()) return;
if (!render_pending) display_dirty = false;
display_rta_update();
}

void display_rta_update(void) {
if (!render_pending) {
if (reset_peak_dots_pending) {
reset_peak_dots_pending = false;
for (int i = 0; i < SPECTRUM_BAND_COUNT; ++i) {
input_l_bands[i] = 0;
input_r_bands[i] = 0;
}
input_l_peak = 0;
input_r_peak = 0;
memset(peak_l_dots, 0, sizeof(peak_l_dots));
memset(peak_r_dots, 0, sizeof(peak_r_dots));
memset(peak_l_hold_until, 0, sizeof(peak_l_hold_until));
memset(peak_r_hold_until, 0, sizeof(peak_r_hold_until));
memset(peak_l_next_fall, 0, sizeof(peak_l_next_fall));
memset(peak_r_next_fall, 0, sizeof(peak_r_next_fall));
}
for (int i = 0; i < SPECTRUM_BAND_COUNT; ++i) {
cur_l_bands[i] = input_l_bands[i];
cur_r_bands[i] = input_r_bands[i];
}
cur_l_peak = input_l_peak;
cur_r_peak = input_r_peak;
render_step = 0;
render_pending = true;
}

if (render_step == 0) {
update_horizontal_meter(31, cur_l_peak, &prev_l_meter_width,
&peak_l_meter_width, &peak_l_meter_hold_until,
&peak_l_meter_next_fall, &last_l_meter_marker,
COLOR_L_THEME);
} else if (render_step == 1) {
update_horizontal_meter(40, cur_r_peak, &prev_r_meter_width,
&peak_r_meter_width, &peak_r_meter_hold_until,
&peak_r_meter_next_fall, &last_r_meter_marker,
COLOR_R_THEME);
} else if (render_step < SPECTRUM_BAND_COUNT + 2) {
/* Render each channel's FFT bars in a separate pass. */
const uint8_t band = render_step - 2;
const uint16_t bar_step = 13;
const uint16_t lx = 10 + band * bar_step;
update_vertical_bar(lx, cur_l_bands[band], &prev_l_bands[band],
&bar_last_update_us[0][band],
&peak_l_dots[band], &peak_l_hold_until[band],
&peak_l_next_fall[band],
&last_l_peak_y[band], COLOR_L_THEME);
} else {
const uint8_t band = render_step - (SPECTRUM_BAND_COUNT + 2);
const uint16_t bar_step = 13;
const uint16_t rx = 167 + band * bar_step;
update_vertical_bar(rx, cur_r_bands[band], &prev_r_bands[band],
&bar_last_update_us[1][band],
&peak_r_dots[band], &peak_r_hold_until[band],
&peak_r_next_fall[band],
&last_r_peak_y[band], COLOR_R_THEME);
}
if (++render_step >= (2 * SPECTRUM_BAND_COUNT) + 2)
render_pending = false;
}
