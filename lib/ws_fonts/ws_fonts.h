#pragma once

// =============================================================================
// ws_fonts.h — shared LVGL font assets (Cyrillic-capable Montserrat).
//
// WHY THIS EXISTS
//   LVGL's built-in `lv_font_montserrat_*` fonts cover ASCII + a FontAwesome
//   subset only: they contain no Cyrillic glyphs, so Russian text would render as
//   missing-glyph boxes. The UI needs RU and EN from the same asset, therefore
//   these fonts are generated from the Montserrat typeface (SIL OFL, the same
//   typeface LVGL uses for its built-ins) with the ranges:
//
//     0x20-0x7F   ASCII
//     0x00AB, 0x00BB, 0x00B0            « » °
//     0x2013-0x2014, 0x2018-0x201D, 0x2026   – — ‘ ’ “ ” …
//     0x0410-0x044F, 0x0401, 0x0451     А-я and Ё/ё
//
//   The SAME assets are used by the PC simulator and (once LVGL is linked) by the
//   ESP32 build — see README in this directory for provenance and regeneration.
//
// WHY THE MACRO
//   The firmware is built with DISPLAY_BACKEND=0 and does not link LVGL at all.
//   PlatformIO compiles every file under lib/, so these translation units must
//   compile to nothing unless the build really has LVGL. `WS_LVGL_FONTS` is
//   defined by the simulator target today and by the future ESP32+LVGL env.
// =============================================================================

#if defined(WS_LVGL_FONTS)

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* small / regular / large — the three sizes the shared UI uses. */
extern const lv_font_t ws_font_14;
extern const lv_font_t ws_font_20;
extern const lv_font_t ws_font_28;

/* FontAwesome subset still lives in LVGL's built-in Montserrat fonts: the icons
   that are drawn as glyphs (the water droplet, LV_SYMBOL_TINT) use this one. */
extern const lv_font_t lv_font_montserrat_28;

#ifdef __cplusplus
}
#endif

#endif /* WS_LVGL_FONTS */
