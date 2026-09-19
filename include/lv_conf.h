/**
 * lv_conf.h — LVGL configuration SHARED by the PC simulator and the ESP32.
 *
 * One file, two targets: `include/` is on the include path of the PlatformIO
 * ESP32 build (-DLV_CONF_INCLUDE_SIMPLE) and is passed to the simulator as
 * -DLV_BUILD_CONF_DIR. Fonts, colour depth and the widget set are therefore
 * literally the same on both, which is what keeps simulator geometry equal to
 * device geometry.
 *
 * Target switch: the simulator defines WS_PC_SIMULATOR (CMake), the firmware
 * does not — that is the only difference, and it only enables the SDL driver.
 *
 * Keep the enabled widget set minimal: everything here costs flash on the ESP32.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ------------------------------------------------------------------ target */
#if defined(WS_PC_SIMULATOR)
    /* LVGL's official SDL desktop driver (window + mouse + keyboard). */
    #define LV_USE_SDL 1
    /* The official SDL2 VC package ships a flat include directory. */
    #define LV_SDL_INCLUDE_PATH <SDL.h>
    #define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
    /* Software renderer: works with the dummy video driver (headless CI runs)
     * and keeps screenshots deterministic across machines. */
    #define LV_SDL_ACCELERATED 0
#else
    #define LV_USE_SDL 0
#endif

/* ------------------------------------------------------------------ basics */
/* Same pixel format as the future ILI9341 panel (RGB565). */
#define LV_COLOR_DEPTH 16
/* Single-threaded on both targets: lv_timer_handler() drives everything, so
 * animation timing and redraw order are identical. */
#define LV_USE_OS 0

/* ------------------------------------------------------------------- fonts */
/* The SAME LVGL font assets on the PC and on the device (no desktop fonts). */
#define LV_FONT_MONTSERRAT_14 1   /* small  — axis/state labels */
#define LV_FONT_MONTSERRAT_20 1   /* normal — titles, units   */
#define LV_FONT_MONTSERRAT_28 1   /* large  — metric values   */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* ----------------------------------------------------------------- widgets */
/* Only what the shared UI (include/lvgl_ui.h) actually uses. BUTTON is needed
 * for the mode selector (mouse + keyboard focus); it is a thin wrapper around
 * lv_obj, so the flash cost is small. */
#define LV_USE_LABEL 1
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_LINE 1
#define LV_USE_BUTTON 1

/* ---------------------------------------------------------------- themes */
/* The shared UI styles every widget explicitly, so only LVGL's default theme
 * (the base look for buttons/bars) is kept. MONO and SIMPLE are unused: turning
 * them off removes dead code from the ESP32 image AND the C5287 warnings MSVC
 * 14.5x reports inside them, so both builds stay warning-free. */
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_MONO 0
#define LV_USE_THEME_SIMPLE 0

/* ------------------------------------------------------------- diagnostics */
/* Asserts are off on both targets: the same code path, no debug-only layouts. */
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

#endif /* LV_CONF_H */
