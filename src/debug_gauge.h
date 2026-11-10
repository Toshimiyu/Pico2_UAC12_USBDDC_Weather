/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/


/*
 * Diagnostics page: buffer water level, USB/audio state, and runtime counters.
 * Drawing is queued one SPI operation at a time and gated on audio headroom.
 */
#ifndef _DEBUG_GAUGE_H_
#define _DEBUG_GAUGE_H_

#include <stdint.h>

#ifndef DEBUG_GAUGE_ENABLE
#define DEBUG_GAUGE_ENABLE (1)
#endif

#if DEBUG_GAUGE_ENABLE
// Call once when entering the diagnostics page.
extern void debug_gauge_init(void);
// Reset diagnostic counters and restart the displayed measurement window.
extern void debug_gauge_reset(void);
// Update the diagnostics-page reset countdown; zero restores the normal hint.
extern void debug_gauge_set_reset_countdown(uint8_t seconds_remaining);
// Call every main-loop pass while the diagnostics page is active.
extern void debug_gauge_service(void);
// Redraw the log header row after the mode-switch countdown overlay clears.
extern void debug_gauge_restore_log_header(void);
#else
static inline void debug_gauge_init(void) {}
static inline void debug_gauge_reset(void) {}
static inline void debug_gauge_set_reset_countdown(uint8_t seconds_remaining) {(void)seconds_remaining;}
static inline void debug_gauge_service(void) {}
static inline void debug_gauge_restore_log_header(void) {}
#endif

#endif /* _DEBUG_GAUGE_H_ */
