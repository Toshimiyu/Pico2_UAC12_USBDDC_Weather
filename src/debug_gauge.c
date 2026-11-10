/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/


/*
 * Debug gauge (see debug_gauge.h). Independent of display_rta.c.
 */

#include "debug_gauge.h"

#if DEBUG_GAUGE_ENABLE

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "common.h"
#include "transmit_to_dac.h"
#include "usb_device_control.h"
#include "usb_audio_config.h"

// ---- Layout (320x240) -------------------------------------------------------------------------------
#define G_HOST_X 48
#define G_HOST_Y 42
#define G_FB_X 48
#define G_FB_Y 54

#define G_TEXT_Y 88
#define G_STATE_Y 30
#define G_UAC_X 136
#define G_FORMAT_X 180
#define G_FORMAT_Y 30
#define G_FORMAT_CHARS 14

#define G_FRAME_X 40
#define G_FRAME_Y 70
#define G_FRAME_W 132
#define G_FRAME_H 12

#define G_IN_X (G_FRAME_X + 1)
#define G_IN_Y (G_FRAME_Y + 1)
#define G_IN_W (G_FRAME_W - 2)
#define G_IN_H (G_FRAME_H - 2)

#define G_PCT_X   48
#define G_SIZE_X  160
#define G_PKT_X   248
#define G_STATE_X 56

#define G_DIAG_COL0_X 180
#define G_DIAG_COL1_X 248
#define G_DIAG_ROW0_Y 42
#define G_DIAG_ROW_STEP 12
#define G_DIAG_FIELDS 8
#define G_DIAG_CHARS 5

#define G_LOG_X 12
#define G_LOG_Y 120
#define G_LOG_ROWS 8
#define G_LOG_COLS 3
#define G_LOG_ROW_STEP 12
#define G_LOG_CHARS 12
#define G_LOG_COL_STEP 104


#define G_UPDATE_US 500000u // 2 Hz
#define G_EMA_TAU_S 0.25f

// ---- Colors (RGB565, same convention as display_rta.c) ----------------------------------------------
#define C_BG 0x0000
#define C_FRAME 0x4208
#define C_TITLE 0x03B0
#define C_LABEL 0xFD20 // amber (same as the frequency labels)
#define C_MARK 0xFFFF
#define C_GREEN 0x07E0
#define C_YELLOW 0xFFE0
#define C_ORANGE 0xFC00
#define C_RED 0xF800
#define C_GRAY 0x7BEF

// DISPLAY_MIN_AUDIO_HEADROOM_SAMPLES is defined in common.h (follows the FB target level)

// Show Ring level MIN / MAX (collected by Core1 at every fetch) in place of S/N.
// Set to 0 to show S/N packet information instead.
#ifndef GAUGE_SHOW_MINMAX
#define GAUGE_SHOW_MINMAX (1)
#endif

// Diagnostics counters, clipped to 3 digits:
//   O ring-overflow drop  F FIFO-resync drop  S dropped while switching  C cleared by restart / forced stop
//   U underruns while STREAMING   a / b longest main-loop gap [us] of Core0 / Core1 (since STREAMING)   T lone tail blocks
// Healthy playback: O, F, U, T stay 0 (green); S and C grow by design at every rate change / (re)open.
#ifndef GAUGE_SHOW_DIAGROW
#define GAUGE_SHOW_DIAGROW (1)
#endif

extern volatile uint32_t audio_last_data_us;
extern volatile bool audio_data_seen;

// ---- 8x8 font (copy of the table in display_rta.c, MSB = left pixel) --------------------------------
static const uint8_t gauge_font8x8[95][8] = {
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

// ---- Minimal LCD routines ----------------------------------------------------------------------------
static uint8_t fill_chunk[256];

static void lcd_cmd(uint8_t cmd)
{
	gpio_put(LCD_DC_PIN, 0);
	gpio_put(LCD_CS_PIN, 0);
	spi_write_blocking(spi0, &cmd, 1);
	gpio_put(LCD_CS_PIN, 1);
}

static void lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	uint8_t xd[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
	uint8_t yd[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
	lcd_cmd(0x2A);
	gpio_put(LCD_DC_PIN, 1);
	gpio_put(LCD_CS_PIN, 0);
	spi_write_blocking(spi0, xd, 4);
	gpio_put(LCD_CS_PIN, 1);
	lcd_cmd(0x2B);
	gpio_put(LCD_DC_PIN, 1);
	gpio_put(LCD_CS_PIN, 0);
	spi_write_blocking(spi0, yd, 4);
	gpio_put(LCD_CS_PIN, 1);
	lcd_cmd(0x2C);
}

static void lcd_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	if (x >= 320 || y >= 240 || w == 0 || h == 0)
		return;
	if (x + w > 320)
		w = 320 - x;
	if (y + h > 240)
		h = 240 - y;

	for (uint32_t i = 0; i < sizeof(fill_chunk); i += 2)
	{
		fill_chunk[i] = (uint8_t)(color >> 8);
		fill_chunk[i + 1] = (uint8_t)color;
	}
	lcd_window(x, y, x + w - 1, y + h - 1);
	gpio_put(LCD_DC_PIN, 1);
	gpio_put(LCD_CS_PIN, 0);
	uint32_t remaining = (uint32_t)w * h * 2u;
	while (remaining > 0)
	{
		const uint32_t n = remaining > sizeof(fill_chunk) ? sizeof(fill_chunk) : remaining;
		spi_write_blocking(spi0, fill_chunk, n);
		remaining -= n;
	}
	gpio_put(LCD_CS_PIN, 1);
}

static void lcd_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
	if (c < ' ' || c > '~')
		c = ' ';
	const uint8_t *glyph = gauge_font8x8[c - ' '];
	uint8_t buf[8 * 8 * 2];
	int n = 0;
	for (int row = 0; row < 8; row++)
		for (int col = 0; col < 8; col++)
		{
			const uint16_t px = (glyph[row] & (0x80 >> col)) ? fg : bg;
			buf[n++] = (uint8_t)(px >> 8);
			buf[n++] = (uint8_t)px;
		}
	lcd_window(x, y, x + 7, y + 7);
	gpio_put(LCD_DC_PIN, 1);
	gpio_put(LCD_CS_PIN, 0);
	spi_write_blocking(spi0, buf, sizeof(buf));
	gpio_put(LCD_CS_PIN, 1);
}

// ---- One-operation-per-call queue ----------------------------------------------------------------------
typedef struct
{
	uint8_t is_char;
	char c;
	uint16_t x, y, w, h, fg, bg;
} gauge_op_t;

#define Q_SIZE 384
static gauge_op_t queue[Q_SIZE];
static uint16_t q_head, q_tail, q_count;

static void q_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	if (q_count >= Q_SIZE)
		return;
	queue[q_tail] = (gauge_op_t){.is_char = 0, .x = x, .y = y, .w = w, .h = h, .fg = color};
	q_tail = (q_tail + 1) % Q_SIZE;
	q_count++;
}

// returns false when the queue is full (the character was NOT queued)
static bool q_char(uint16_t x, uint16_t y, char c, uint16_t fg)
{
	if (q_count >= Q_SIZE)
		return false;
	queue[q_tail] = (gauge_op_t){.is_char = 1, .c = c, .x = x, .y = y, .fg = fg, .bg = C_BG};
	q_tail = (q_tail + 1) % Q_SIZE;
	q_count++;
	return true;
}

// ---- Gauge state -------------------------------------------------------------------------------------------
static bool inited = false;
static float level_ema = 0.0f;
static uint32_t last_sample_us = 0, last_update_us = 0;
static uint32_t log_scroll_offset = 0;
static bool log_refresh_pending = false;
static bool log_clear_pending = false;
static uint16_t log_clear_row = 0;
static bool history_mode = false;
static uint8_t reset_status = 0;
static bool reset_status_pending = false;
static bool reset_clear_pending = false;
static uint16_t reset_clear_row = 0;

static display_button_t button_a;
static display_button_t button_b;

// what is currently drawn (forces a full draw at the beginning)
static int shown_fill = -1;
static uint16_t shown_color = 0;
static char shown_pct[6]   = "     ";
static char shown_host[8]  = "       ";
static char shown_fb[8]    = "       ";
static char shown_size[6] = "     ";
static char shown_pkt[6]  = "     ";
static char shown_state[5] = "    ";
static char shown_format[G_FORMAT_CHARS + 1] = "              ";
static char shown_diag[G_DIAG_FIELDS][G_DIAG_CHARS + 1];
static uint16_t shown_diag_color[G_DIAG_FIELDS][G_DIAG_CHARS];
static char shown_log[G_LOG_ROWS][G_LOG_COLS][G_LOG_CHARS + 1];
static uint16_t shown_log_color[G_LOG_ROWS][G_LOG_COLS][G_LOG_CHARS];

static uint16_t shown_state_color = 0;

static const char *state_text(usb_audio_state_t st)
{
	switch (st)
	{
	case USB_AUDIO_BUFFERING:
		return "BUFF";
	case USB_AUDIO_SWITCHING:
		return "SWCH";
	case USB_AUDIO_STREAMING:
		return "STRM";
	case USB_AUDIO_OVERFLOW_FATAL:
		return "OVF!";
	case USB_AUDIO_UNDERFLOW_FATAL:
		return "UDF!";
	case USB_AUDIO_IDLE:
	default:
		return "IDLE";
	}
}

// green: normal / yellow: BUFFERING / orange: near-or-in UNDERFLOW / red: near-or-in OVERFLOW / gray: IDLE
static uint16_t state_color(usb_audio_state_t st, float level)
{
	switch (st)
	{
	case USB_AUDIO_BUFFERING:
		return C_YELLOW;
	case USB_AUDIO_SWITCHING:
		return C_YELLOW;
	case USB_AUDIO_OVERFLOW_FATAL:
		return C_RED;
	case USB_AUDIO_UNDERFLOW_FATAL:
		return C_ORANGE;
	case USB_AUDIO_STREAMING:
		if (level >= (float)SIZE_AUDIO_RING_FRAMES * 7.0f / 8.0f)
			return C_RED;
		if (level <= (float)SIZE_AUDIO_RING_FRAMES / 8.0f)
			return C_ORANGE;
		return C_GREEN;
	case USB_AUDIO_IDLE:
	default:
		return C_GRAY;
	}
}

static void queue_text_at(uint16_t x, uint16_t y, const char *s, int len, uint16_t color)
{
	for (int i = 0; i < len; i++)
		q_char(x + 8 * i, y, s[i], color);
}

static void draw_log_header(void)
{
	q_rect(8, 108, 304, 10, C_BG);
	queue_text_at(8, 108, history_mode ? "RECENT AUDIO HISTORY" : "USB COMMUNICATION LOG",
				  history_mode ? 20 : 21, C_LABEL);
}

static inline uint16_t target_px(void)
{
	return (uint16_t)(((uint32_t)G_IN_W * SIZE_BUFFER_FB_THRESHOLD) / SIZE_AUDIO_RING_FRAMES);
}

static void poll_log_buttons(uint32_t now)
{
	const bool a_pressed = display_button_pressed(&button_a, DISPLAY_BUTTON_A_PIN, now);
	const bool b_pressed = display_button_pressed(&button_b, DISPLAY_BUTTON_B_PIN, now);

	if ((a_pressed && !button_b.stable_high) || (b_pressed && !button_a.stable_high))
	{
		history_mode = !history_mode;
		log_scroll_offset = 0;
		memset(shown_log, ' ', sizeof(shown_log));
		log_clear_pending = true;
		log_clear_row = 0;
		log_refresh_pending = true;
		return;
	}

	if (a_pressed && !b_pressed)
	{
		if (history_mode)
		{
			usb_audio_history_sample_t sample;
			if (usb_audio_get_history_sample(log_scroll_offset + 1u, &sample))
			{
				log_scroll_offset++;
				log_refresh_pending = true;
			}
		}
		else
		{
			char event[8];
			usb_audio_get_event(log_scroll_offset + 1u, event);
			if (event[0] != ' ')
			{
				log_scroll_offset++;
				log_refresh_pending = true;
			}
		}
	}
	else if (b_pressed && !a_pressed && log_scroll_offset > 0)
	{
		log_scroll_offset--;
		log_refresh_pending = true;
	}
}

void debug_gauge_restore_log_header(void)
{
	if (inited && q_count + 2 < Q_SIZE)
		draw_log_header();
}

void debug_gauge_set_reset_countdown(uint8_t seconds_remaining)
{
	if (seconds_remaining > 3u)
		return;

	reset_status = seconds_remaining;
	reset_status_pending = true;
}

void debug_gauge_init(void)
{
	q_head = 0;
	q_tail = 0;
	q_count = 0;
	log_scroll_offset = 0;
	log_refresh_pending = true;
	log_clear_pending = false;
	log_clear_row = 0;
	reset_status = 0;
	reset_status_pending = false;
	reset_clear_pending = false;

	gpio_init(DISPLAY_BUTTON_A_PIN);
	gpio_set_dir(DISPLAY_BUTTON_A_PIN, GPIO_IN);
	gpio_pull_up(DISPLAY_BUTTON_A_PIN);
	gpio_init(DISPLAY_BUTTON_B_PIN);
	gpio_set_dir(DISPLAY_BUTTON_B_PIN, GPIO_IN);
	gpio_pull_up(DISPLAY_BUTTON_B_PIN);
	const uint32_t buttons_initialized_us = time_us_32();
	button_a.raw_high = gpio_get(DISPLAY_BUTTON_A_PIN);
	button_a.stable_high = button_a.raw_high;
	button_a.raw_changed_us = buttons_initialized_us;
	button_b.raw_high = gpio_get(DISPLAY_BUTTON_B_PIN);
	button_b.stable_high = button_b.raw_high;
	button_b.raw_changed_us = buttons_initialized_us;

	queue_text_at(8, 12, "SYSTEM DIAGNOSTICS", 18, C_LABEL);
	queue_text_at(160, 12, "Pico2 UAC12 USB DDC", 19, C_TITLE);
	q_rect(8, 22, 304, 1, C_FRAME);
	queue_text_at(8, G_STATE_Y, "STATE", 5, C_LABEL);
#if USB_AUDIO_CLASS_VERSION == 1
	queue_text_at(G_UAC_X, G_FORMAT_Y, "UAC1", 4, C_TITLE);
#else
	queue_text_at(G_UAC_X, G_FORMAT_Y, "UAC2", 4, C_TITLE);
#endif
	queue_text_at(8, G_HOST_Y, "HOST", 4, C_LABEL);
	queue_text_at(8, G_FB_Y, "FB", 2, C_LABEL);
	queue_text_at(8, G_FRAME_Y + 2, "RING", 4, C_LABEL);
	queue_text_at(8, G_TEXT_Y, "LEVEL", 5, C_LABEL);
	queue_text_at(128, G_TEXT_Y, "MIN", 3, C_LABEL);
	queue_text_at(216, G_TEXT_Y, "MAX", 3, C_LABEL);
	q_rect(8, 102, 304, 1, C_FRAME);
	queue_text_at(8, 108, "USB COMMUNICATION LOG", 21, C_LABEL);
	q_rect(8, 214, 304, 1, C_FRAME);
	queue_text_at(12, 224, "A+B VIEW A:OLD B:NEW B3 EXIT Y3 RESET", 37, C_LABEL);

	q_rect(G_FRAME_X, G_FRAME_Y, G_FRAME_W, 1, C_FRAME);
	q_rect(G_FRAME_X, G_FRAME_Y + G_FRAME_H - 1, G_FRAME_W, 1, C_FRAME);
	q_rect(G_FRAME_X, G_FRAME_Y, 1, G_FRAME_H, C_FRAME);
	q_rect(G_FRAME_X + G_FRAME_W - 1, G_FRAME_Y, 1, G_FRAME_H, C_FRAME);
	q_rect(G_IN_X, G_IN_Y, G_IN_W, G_IN_H, C_BG);
	q_rect(G_IN_X + target_px(), G_IN_Y, 1, G_IN_H, C_MARK);

    shown_fill = 0;
    shown_color = C_BG;

    memcpy(shown_pct,   "     ",    6);
    memcpy(shown_host,  "       ",   8);
    memcpy(shown_fb,    "       ",   8);
    memcpy(shown_size,  "    ",     5);
    memcpy(shown_pkt,   "    ",     5);
    memcpy(shown_state, "    ",     5);
    memcpy(shown_format, "              ", G_FORMAT_CHARS + 1);
    memset(shown_diag, ' ', sizeof(shown_diag));
    memset(shown_diag_color, 0, sizeof(shown_diag_color));
    memset(shown_log, ' ', sizeof(shown_log));
    memset(shown_log_color, 0, sizeof(shown_log_color));
    for (int i = 0; i < G_DIAG_FIELDS; i++)
        shown_diag[i][G_DIAG_CHARS] = '\0';
    for (int row = 0; row < G_LOG_ROWS; row++)
        for (int col = 0; col < G_LOG_COLS; col++)
            shown_log[row][col][G_LOG_CHARS] = '\0';

    shown_state_color = 0;

    last_sample_us = time_us_32();
    last_update_us = last_sample_us;
    level_ema = (float)audio_ring_total_frames();
    inited = true;
}

void debug_gauge_reset(void)
{
	usb_audio_reset_diagnostics();
	history_mode = false;
	debug_gauge_init();
	reset_status = 4;
	reset_status_pending = true;
	reset_clear_pending = true;
	reset_clear_row = 0;
}

static void queue_reset_status(void)
{
	static const char *const status_text[] = {
		"A+B VIEW A:OLD B:NEW B3 EXIT Y3 RESET",
		"HOLD Y TO RESET: 1",
		"HOLD Y TO RESET: 2",
		"HOLD Y TO RESET: 3",
		"DIAGNOSTICS RESET COMPLETE"
	};
	static const uint8_t text_lengths[] = {37, 18, 18, 18, 26};

	q_rect(8, 224, 304, 8, C_BG);
	queue_text_at(12, 224, status_text[reset_status],
		text_lengths[reset_status], C_LABEL);
	reset_status_pending = false;
}

static void refresh_usb_log(void)
{
	for (int row = 0; row < G_LOG_ROWS; row++)
	{
		for (int col = 0; col < G_LOG_COLS; col++)
		{
			const uint32_t age = log_scroll_offset + (uint32_t)(row * G_LOG_COLS + col);
			char event[8];
			char line[G_LOG_CHARS + 1];
			uint16_t color = C_LABEL;
			uint32_t event_time_us;
			usb_audio_get_event_entry(age, event, &event_time_us);

			if (event[0] == ' ')
			{
				memset(line, ' ', G_LOG_CHARS);
				line[G_LOG_CHARS] = '\0';
			}
			else
			{
				const uint32_t uptime_seconds = (event_time_us / 1000000u) % 1000u;
				snprintf(line, sizeof(line), "%03lu %s", (unsigned long)uptime_seconds, event);
				color = event[6] == '-' ? C_RED : C_GREEN;
			}

			for (int char_index = 0; char_index < G_LOG_CHARS; char_index++)
			{
				if (line[char_index] == ' ' && shown_log[row][col][char_index] == ' ')
				{
					shown_log_color[row][col][char_index] = color;
					continue;
				}

				if (line[char_index] != shown_log[row][col][char_index] ||
					color != shown_log_color[row][col][char_index])
				{
					if (q_char(G_LOG_X + G_LOG_COL_STEP * col + 8 * char_index,
							   G_LOG_Y + G_LOG_ROW_STEP * row, line[char_index], color))
					{
						shown_log[row][col][char_index] = line[char_index];
						shown_log_color[row][col][char_index] = color;
					}
				}
			}
		}
	}
}

static void refresh_audio_history(void)
{
	for (int row = 0; row < G_LOG_ROWS; row++)
	{
		for (int col = 0; col < G_LOG_COLS; col++)
		{
			const uint32_t age = log_scroll_offset + (uint32_t)(row * G_LOG_COLS + col);
			char line[G_LOG_CHARS + 1];
			uint16_t color = C_LABEL;
			usb_audio_history_sample_t sample;

			if (usb_audio_get_history_sample(age, &sample))
			{
				const char state = sample.state == USB_AUDIO_STREAMING ? 'S' :
					sample.state == USB_AUDIO_BUFFERING ? 'B' :
					sample.state == USB_AUDIO_OVERFLOW_FATAL ? 'O' :
					sample.state == USB_AUDIO_UNDERFLOW_FATAL ? 'U' :
					sample.state == USB_AUDIO_SWITCHING ? 'C' : 'I';
				const uint32_t seconds = (sample.timestamp_us / 1000000u) % 1000u;
				const uint32_t ring = sample.ring_frames > 9999u ? 9999u : sample.ring_frames;
				const uint32_t dma = sample.dma_frames > 999u ? 999u : sample.dma_frames;
				snprintf(line, sizeof(line), "%03lu%c%04lu%c%03lu",
						 (unsigned long)seconds, state, (unsigned long)ring,
						 sample.output_active ? '*' : '-', (unsigned long)dma);
				if (sample.state == USB_AUDIO_OVERFLOW_FATAL ||
					sample.state == USB_AUDIO_UNDERFLOW_FATAL)
					color = C_RED;
				else if (!sample.output_active && sample.state == USB_AUDIO_STREAMING)
					color = C_ORANGE;
			}
			else
			{
				memset(line, ' ', G_LOG_CHARS);
				line[G_LOG_CHARS] = '\0';
			}

			for (int char_index = 0; char_index < G_LOG_CHARS; char_index++)
			{
				if (line[char_index] == ' ' && shown_log[row][col][char_index] == ' ')
				{
					shown_log_color[row][col][char_index] = color;
					continue;
				}

				if (line[char_index] != shown_log[row][col][char_index] ||
					color != shown_log_color[row][col][char_index])
				{
					if (q_char(G_LOG_X + G_LOG_COL_STEP * col + 8 * char_index,
							   G_LOG_Y + G_LOG_ROW_STEP * row, line[char_index], color))
					{
						shown_log[row][col][char_index] = line[char_index];
						shown_log_color[row][col][char_index] = color;
					}
				}
			}
		}
	}
}

static void refresh(float level, usb_audio_state_t st)
{
	// --- bar ---
	int fill = (int)(level * (float)G_IN_W / (float)SIZE_AUDIO_RING_FRAMES + 0.5f);
	if (fill < 0)
		fill = 0;
	if (fill > G_IN_W)
		fill = G_IN_W;
	const uint16_t color = state_color(st, level);
	const uint16_t mark_x = G_IN_X + target_px();

	if (color != shown_color)
	{
		// repaint the whole interior with the new color
		if (fill > 0)
			q_rect(G_IN_X, G_IN_Y, (uint16_t)fill, G_IN_H, color);
		if (fill < G_IN_W)
			q_rect(G_IN_X + (uint16_t)fill, G_IN_Y, (uint16_t)(G_IN_W - fill), G_IN_H, C_BG);
		q_rect(mark_x, G_IN_Y, 1, G_IN_H, C_MARK);
		shown_fill = fill;
		shown_color = color;
	}
	else if (fill > shown_fill)
	{
		q_rect(G_IN_X + (uint16_t)shown_fill, G_IN_Y, (uint16_t)(fill - shown_fill), G_IN_H, color);
		q_rect(mark_x, G_IN_Y, 1, G_IN_H, C_MARK);
		shown_fill = fill;
	}
	else if (fill < shown_fill)
	{
		q_rect(G_IN_X + (uint16_t)fill, G_IN_Y, (uint16_t)(shown_fill - fill), G_IN_H, C_BG);
		q_rect(mark_x, G_IN_Y, 1, G_IN_H, C_MARK);
		shown_fill = fill;
	}

	// Buffer percentage, QMI divider, and derived flash clock share this row.
	float pct = level * 100.0f / (float)SIZE_AUDIO_RING_FRAMES;
	if (pct > 99.9f)
		pct = 99.9f;
	if (pct < 0.0f)
		pct = 0.0f;
	char pct_text[6];
	const int tenths = (int)(pct * 10.0f + 0.5f);
	pct_text[0] = (tenths >= 100) ? (char)('0' + tenths / 100) : ' ';
	pct_text[1] = (char)('0' + (tenths / 10) % 10);
	pct_text[2] = '.';
	pct_text[3] = (char)('0' + tenths % 10);
	pct_text[4] = '%';
	pct_text[5] = '\0';
	for (int i = 0; i < 5; i++)
		if (pct_text[i] != shown_pct[i])
		{
			q_char(G_PCT_X + 8 * i, G_TEXT_Y, pct_text[i], C_LABEL);
			shown_pct[i] = pct_text[i];
		}

	char host_text[8];
	char fb_text[8];
	snprintf(host_text, sizeof(host_text), "H%06lu", (unsigned long)usb_audio_get_host_rate_hz());
	snprintf(fb_text, sizeof(fb_text), "F%06lu", (unsigned long)usb_audio_get_feedback_hz());

	for (int i = 0; i < 7; i++)
	{
		if (host_text[i] != shown_host[i])
		{
			if (q_char(G_HOST_X + 8 * i, G_HOST_Y, host_text[i], C_LABEL))
				shown_host[i] = host_text[i];
		}
		if (fb_text[i] != shown_fb[i])
		{
			if (q_char(G_FB_X + 8 * i, G_FB_Y, fb_text[i], C_LABEL))
				shown_fb[i] = fb_text[i];
		}
	}

	uint32_t bit_depth = audio_state.bit_depth;
	uint32_t rate_tenth_khz = (audio_state.freq + 50u) / 100u;
	char format_text[G_FORMAT_CHARS + 1] = "              ";
	if (bit_depth > 99u)
		bit_depth = 99u;
	if (rate_tenth_khz > 9999u)
		rate_tenth_khz = 9999u;
	format_text[0] = bit_depth >= 10u ? (char)('0' + bit_depth / 10u) : ' ';
	format_text[1] = (char)('0' + bit_depth % 10u);
	format_text[2] = 'b';
	format_text[3] = 'i';
	format_text[4] = 't';
	format_text[5] = ' ';
	const uint32_t rate_khz = rate_tenth_khz / 10u;
	if (rate_khz >= 100u)
		format_text[6] = (char)('0' + rate_khz / 100u);
	if (rate_khz >= 10u)
		format_text[7] = (char)('0' + (rate_khz / 10u) % 10u);
	format_text[8] = (char)('0' + rate_khz % 10u);
	format_text[9] = '.';
	format_text[10] = (char)('0' + rate_tenth_khz % 10u);
	format_text[11] = 'k';
	format_text[12] = 'H';
	format_text[13] = 'z';
	for (int i = 0; i < G_FORMAT_CHARS; i++)
	{
		if (format_text[i] != shown_format[i] &&
			q_char(G_FORMAT_X + 8 * i, G_FORMAT_Y, format_text[i], C_TITLE))
		{
			shown_format[i] = format_text[i];
		}
	}

/* ===== S/N display ===== */

    char size_text[6];
    char pkt_text[6];

#if GAUGE_SHOW_MINMAX
    {
        const usb_audio_stats_t *gs = usb_audio_get_stats();
        const int32_t gmin = gs->level_min;
        const int32_t gmax = gs->level_max;

        if (gmin == INT32_MAX)
            snprintf(size_text, sizeof(size_text), "n----");
        else
            snprintf(size_text, sizeof(size_text), "n%04ld", (long)(gmin < 0 ? 0 : (gmin > 9999 ? 9999 : gmin)));

        snprintf(pkt_text, sizeof(pkt_text), "x%04ld", (long)(gmax < 0 ? 0 : (gmax > 9999 ? 9999 : gmax)));
    }
#else
    snprintf(
        size_text,
        sizeof(size_text),
        "S%03lu",
        (unsigned long)usb_audio_get_avg_packet_bytes());

    snprintf(
        pkt_text,
        sizeof(pkt_text),
        "N%03lu",
        (unsigned long)usb_audio_get_packets_per_sec());
#endif

    for (int i = 0; i < 5; i++)
    {
        if (size_text[i] != shown_size[i])
        {
            q_char(
                G_SIZE_X + 8 * i,
                G_TEXT_Y,
                size_text[i],
                C_LABEL);

            shown_size[i] = size_text[i];
        }
    }

    for (int i = 0; i < 5; i++)
    {
        if (pkt_text[i] != shown_pkt[i])
        {
            q_char(
                G_PKT_X + 8 * i,
                G_TEXT_Y,
                pkt_text[i],
                C_LABEL);

            shown_pkt[i] = pkt_text[i];
        }
    }

#if GAUGE_SHOW_DIAGROW
    {
        const usb_audio_stats_t *d = usb_audio_get_stats();
        const uint32_t vo = d->drop_ovf_frames, vf = d->drop_fifo_frames, vs = d->drop_switch_frames;
        const uint32_t vc = d->drop_clear_frames, vu = d->output_stop_count;
        const uint32_t va = d->loop0_max_us, vb = d->loop1_max_us, vt = d->tail_block_count;
        struct
        {
            char label;
            uint32_t value;
            uint8_t digits;
            uint16_t color;
        } fields[G_DIAG_FIELDS] = {
            {'O', vo, 3, vo ? C_RED : C_GREEN},
            {'F', vf, 3, vf ? C_RED : C_GREEN},
            {'S', vs, 3, C_LABEL},
            {'C', vc, 3, C_LABEL},
            {'U', vu, 3, vu ? C_RED : C_GREEN},
            {'a', va, 4, va > LOOP0_WARN_US ? C_ORANGE : C_LABEL},
            {'b', vb, 4, vb > LOOP1_WARN_US ? C_ORANGE : C_LABEL},
            {'T', vt, 2, vt ? C_ORANGE : C_GREEN},
        };
        const uint16_t col_x[] = {G_DIAG_COL0_X, G_DIAG_COL1_X};

        for (int field = 0; field < G_DIAG_FIELDS; field++)
        {
            char text[G_DIAG_CHARS + 1];
            const uint32_t max_value = fields[field].digits == 4 ? 9999u :
                                       fields[field].digits == 3 ? 999u : 99u;
            const uint32_t value = fields[field].value > max_value ? max_value : fields[field].value;
            if (fields[field].digits == 4)
                snprintf(text, sizeof(text), "%c%04lu", fields[field].label, (unsigned long)value);
            else if (fields[field].digits == 3)
                snprintf(text, sizeof(text), "%c%03lu", fields[field].label, (unsigned long)value);
            else
                snprintf(text, sizeof(text), "%c%02lu", fields[field].label, (unsigned long)value);
            const uint16_t x = col_x[field % 2];
            const uint16_t y = G_DIAG_ROW0_Y + G_DIAG_ROW_STEP * (field / 2);

            for (int i = 0; i < G_DIAG_CHARS; i++)
            {
                if (text[i] != shown_diag[field][i] ||
                    fields[field].color != shown_diag_color[field][i])
                {
                    if (q_char(x + 8 * i, y, text[i], fields[field].color))
                    {
                        shown_diag[field][i] = text[i];
                        shown_diag_color[field][i] = fields[field].color;
                    }
                }
            }
        }
    }
#endif

    // --- state code (colored like the bar) ---
    const char *stxt = state_text(st);

    for (int i = 0; i < 4; i++)
    {
        if (stxt[i] != shown_state[i] ||
            color != shown_state_color)
        {
            q_char(
                G_STATE_X + 8 * i,
                G_STATE_Y,
                stxt[i],
                color);

            shown_state[i] = stxt[i];
        }
    }

    shown_state_color = color;

	if (history_mode)
		refresh_audio_history();
	else
		refresh_usb_log();
}



// Same rule as display_rta_render_service(): draw only when the audio side has headroom
static bool audio_has_slack(void)
{
	const uint32_t now = time_us_32();
	const bool audio_active = audio_data_seen && (uint32_t)(now - audio_last_data_us) < 200000u;
	if (audio_active)
		return audio_ring_total_frames() >= DISPLAY_MIN_AUDIO_HEADROOM_SAMPLES ||
			   dma_tx.using >= (DEPTH_DMA_TX_BUFFER / 2);
	return !enable_output;
}

void debug_gauge_service(void)
{
	if (!inited)
		return;

	// smoothed level (sampled on every call, independent of drawing)
	const uint32_t now = time_us_32();
	const float dt = (float)(uint32_t)(now - last_sample_us) * 1e-6f;
	last_sample_us = now;
	const float alpha = dt / (G_EMA_TAU_S + dt);
	level_ema += ((float)audio_ring_total_frames() - level_ema) * alpha;
	poll_log_buttons(now);

	if (reset_clear_pending)
	{
		if (!audio_has_slack())
			return;

		lcd_rect(0, reset_clear_row++, 320, 1, C_BG);
		if (reset_clear_row >= 240)
			reset_clear_pending = false;
		return;
	}

	if (q_count == 0)
	{
		if (reset_status_pending)
		{
			queue_reset_status();
		}
		else if (log_clear_pending)
		{
			if (!audio_has_slack())
				return;

			const uint16_t clear_height =
				G_LOG_ROW_STEP * (G_LOG_ROWS - 1) + 8;
			lcd_rect(G_LOG_X - 4, G_LOG_Y + log_clear_row, 312, 1, C_BG);
			log_clear_row++;
			if (log_clear_row >= clear_height)
			{
				log_clear_pending = false;
				draw_log_header();
			}
			return;
		}
		else if (log_refresh_pending || (uint32_t)(now - last_update_us) >= G_UPDATE_US)
		{
			last_update_us = now;
			log_refresh_pending = false;
			refresh(level_ema, usb_audio_get_state());
		}

		if (q_count == 0)
			return;
	}

	if (!audio_has_slack())
		return;

	// exactly one SPI operation per call
	const gauge_op_t op = queue[q_head];
	q_head = (q_head + 1) % Q_SIZE;
	q_count--;
	if (op.is_char)
		lcd_char(op.x, op.y, op.c, op.fg, op.bg);
	else
		lcd_rect(op.x, op.y, op.w, op.h, op.fg);
}

#endif /* DEBUG_GAUGE_ENABLE */
