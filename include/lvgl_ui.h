#pragma once

// ===========================================================================
// lvgl_ui.h — SHARED LVGL UI, compiled by BOTH targets.
//
//   WeatherModel -> PresentationModel -> THIS UI -> display backend
//                                          |             |
//                                          |             +-- PC: SDL2 (simulator)
//                                          |             +-- ESP32: ILI9341 (later)
//                                          |
//                                          +-- ESP32 headless backend (no panel,
//                                              same widget tree + layout checks)
//
// Reused from weather_core.h: metric semantics, data states, semantic colour
// roles, metric ranges, mode ids, icon ids, units and the presentation model.
// NOT reused: HTML/CSS — the Web UI stays an independent renderer, and the
// Liquid Glass *concept* is re-expressed with native LVGL primitives
// (translucent rounded containers, thin borders, soft shadows, accent glow).
//
// The UI is embedded-friendly on purpose:
//   * the widget tree is created ONCE and only mutated afterwards (no per-frame
//     object creation, no dynamic strings in the update path);
//   * geometry comes exclusively from computeLayout() in weather_core.h;
//   * no desktop-only API, no C++ STL containers, no heap churn per update.
// ===========================================================================

#include "lvgl.h"

#include "i18n.h"
#include "weather_core.h"

#if defined(WS_LVGL_FONTS)
#include "ws_fonts.h"
#endif

namespace ws_ui {

// ---------------------------------------------------------------------------
// Fonts: the same LVGL font assets on the PC and on the device.
//
//   WS_LVGL_FONTS defined → the shared generated Montserrat fonts with Cyrillic
//                           coverage (lib/ws_fonts) — required for Russian.
//   otherwise             → LVGL's built-in Montserrat (Latin only), i.e. the
//                           fallback for a build that has no font asset yet.
//
// Icons drawn as glyphs (the water droplet = LV_SYMBOL_TINT) live in the
// FontAwesome subset that only exists in LVGL's built-in fonts, so they use a
// separate accessor and keep working in both cases.
// ---------------------------------------------------------------------------
#if defined(WS_LVGL_FONTS)
inline const lv_font_t* font_small() { return &ws_font_14; }
inline const lv_font_t* font_normal() { return &ws_font_20; }
inline const lv_font_t* font_large() { return &ws_font_28; }
/* Icon glyphs (LV_SYMBOL_*) live in the FontAwesome subset of LVGL's built-in
   fonts. Picked by box size, because a 28 px glyph would be clipped in a 22 px
   icon box and lose the droplet's tip. */
inline const lv_font_t* font_symbols_for(int size) {
  if (size >= 28) { return &lv_font_montserrat_28; }
  if (size >= 20) { return &lv_font_montserrat_20; }
  return &lv_font_montserrat_14;
}
#else
inline const lv_font_t* font_small() { return &lv_font_montserrat_14; }
inline const lv_font_t* font_normal() { return &lv_font_montserrat_20; }
inline const lv_font_t* font_large() { return &lv_font_montserrat_28; }
inline const lv_font_t* font_symbols_for(int size) {
  if (size >= 28) { return &lv_font_montserrat_28; }
  if (size >= 20) { return &lv_font_montserrat_20; }
  return &lv_font_montserrat_14;
}
#endif

// Largest enabled font whose glyph still fits an icon box of `size` pixels.
inline const lv_font_t* font_for_icon(int size) {
  if (size >= 30) { return font_large(); }
  if (size >= 20) { return font_normal(); }
  return font_small();
}

// ---------------------------------------------------------------------------
// Theme layer: semantic token -> LVGL colour / opacity / border / glow.
// This is the ONLY place a pixel value is produced from a semantic token; the
// Web renderer maps the same tokens to its CSS variables.
// ---------------------------------------------------------------------------
inline lv_color_t token_color(ColorToken token) {
  return lv_color_hex(themeRgb(ThemeId::kReferenceDark, token));
}

inline lv_opa_t state_opacity(DataState state) {
  switch (state) {
    case DataState::kFresh: return 255;
    case DataState::kStale: return 165;  // dimmed, never blinking
    default: return 205;
  }
}

struct GlassStyle {
  ColorToken accent;
  DataState state;
  lv_opa_t surface_opa;   // translucent glass surface
  lv_opa_t content_opa;   // dims the whole card when STALE
  bool dashed_border;     // ERROR gets a dashed outline instead of flashing
};

// Shared style transition for interactive widgets: property changes (press,
// focus) are animated by LVGL's own style engine, so button feedback costs no
// animation objects and works identically on the ESP32.
inline const lv_style_transition_dsc_t& trans_fast() {
  static lv_style_transition_dsc_t trans;
  static const lv_style_prop_t props[] = {LV_STYLE_BG_OPA, LV_STYLE_BG_COLOR, LV_STYLE_BORDER_COLOR,
                                         LV_STYLE_BORDER_WIDTH, LV_STYLE_PROP_INV};
  static bool ready = false;
  if (!ready) {
    lv_style_transition_dsc_init(&trans, props, lv_anim_path_ease_out, 140, 0, nullptr);
    ready = true;
  }
  return trans;
}

inline GlassStyle glass_for(DataState state, ColorToken accent) {
  GlassStyle style;
  style.accent = accent;
  style.state = state;
  style.surface_opa = (state == DataState::kError) ? 34 : 48;
  style.content_opa = state_opacity(state);
  style.dashed_border = (state == DataState::kError);
  return style;
}

// A Liquid-Glass card: translucent surface, 1px light border, soft shadow and a
// subtle accent tint. Native LVGL only — no CSS, no images.
//
// Every surface built with this helper is PASSIVE: LVGL's lv_obj_create() sets
// LV_OBJ_FLAG_CLICKABLE by default, and lv_indev_search_obj() returns the topmost
// clickable object, so a decorative container placed above a button silently
// swallows its clicks. Passive containers therefore clear that flag explicitly.
inline void make_passive(lv_obj_t* obj) {
  if (obj == nullptr) { return; }
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

inline void apply_glass(lv_obj_t* obj, const GlassStyle& style) {
  const lv_color_t accent = token_color(style.accent);
  make_passive(obj);

  lv_obj_set_style_radius(obj, 14, LV_PART_MAIN);
  /* Translucent dark base with a faint accent gradient = the Web's layered
   * glass surface, expressed with LVGL style properties only. */
  lv_obj_set_style_bg_color(obj, lv_color_hex(0x0E1726), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(obj, accent, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_opa(obj, style.surface_opa / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, style.surface_opa, LV_PART_MAIN);

  lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(obj, lv_color_hex(0x3A4A66), LV_PART_MAIN);
  lv_obj_set_style_border_opa(obj, LV_OPA_70, LV_PART_MAIN);

  lv_obj_set_style_shadow_width(obj, 18, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(obj, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(obj, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_shadow_offset_y(obj, 6, LV_PART_MAIN);
  lv_obj_set_style_shadow_spread(obj, 0, LV_PART_MAIN);

  /* No padding: the shared layout already provides the insets through `gutter`,
     and LVGL positions children inside the parent's *content* area. Zero padding
     keeps `place_in()` coordinates exactly equal to the layout rectangles, so a
     widget can never be shifted or clipped by the card's own padding. */
  lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_opa(obj, style.content_opa, LV_PART_MAIN);
  if (lv_obj_check_type(obj, &lv_obj_class)) {
    lv_obj_set_style_outline_width(obj, style.dashed_border ? 1 : 0, LV_PART_MAIN);
    lv_obj_set_style_outline_color(obj, lv_color_hex(0xFF6B5E), LV_PART_MAIN);
    lv_obj_set_style_outline_opa(obj, style.dashed_border ? LV_OPA_60 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_outline_pad(obj, 3, LV_PART_MAIN);
  }
}

// ---------------------------------------------------------------------------
// Icons: drawn from LVGL primitives, therefore identical on the PC and on the
// device (same IconId -> same drawing code). The Web keeps its own SVG.
//
// Polyline storage comes from a small static pool: LVGL keeps a pointer to the
// points array, and a shared `static` inside the drawing function would make
// every instance overwrite the previous one.
// ---------------------------------------------------------------------------
inline lv_point_precise_t* icon_points_pool() {
  static lv_point_precise_t pool[8][4];
  static int next = 0;
  lv_point_precise_t* slot = pool[next % 8];
  next += 1;
  return slot;
}

inline void draw_icon(lv_obj_t* parent, IconId icon, const LayoutRect& rect, lv_color_t color) {
  const int size = (rect.w < rect.h ? rect.w : rect.h);
  const int cx = rect.x + rect.w / 2;
  const int cy = rect.y + rect.h / 2;

  switch (icon) {
    case IconId::kTemperature: {
      /* Thermometer: rounded stem + bulb, both inside the icon box. */
      const int stem_w = size * 22 / 100;
      const int bulb_d = size * 50 / 100;
      lv_obj_t* stem = lv_obj_create(parent);
      lv_obj_set_style_radius(stem, stem_w / 2, LV_PART_MAIN);
      lv_obj_set_style_bg_color(stem, color, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(stem, 0, LV_PART_MAIN);
      lv_obj_remove_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_pos(stem, cx - stem_w / 2, cy - size / 2);
      lv_obj_set_size(stem, stem_w, size * 60 / 100);

      lv_obj_t* bulb = lv_obj_create(parent);
      lv_obj_set_style_radius(bulb, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(bulb, color, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(bulb, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(bulb, 0, LV_PART_MAIN);
      lv_obj_remove_flag(bulb, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_pos(bulb, cx - bulb_d / 2, cy + size / 2 - bulb_d);
      lv_obj_set_size(bulb, bulb_d, bulb_d);
      break;
    }
    case IconId::kHumidity: {
      /* Real water drop: LVGL's built-in fonts embed the FontAwesome symbol set,
       * so the droplet is a single glyph. It uses font_symbols() — the generated
       * UI fonts carry Latin+Cyrillic only — which keeps the icon identical on
       * the PC and on the device instead of stacking primitive objects. */
      lv_obj_t* glyph = lv_label_create(parent);
      lv_obj_set_style_text_font(glyph, font_symbols_for(size), LV_PART_MAIN);
      lv_obj_set_style_text_color(glyph, color, LV_PART_MAIN);
      lv_obj_set_style_pad_all(glyph, 0, LV_PART_MAIN);
      lv_label_set_text(glyph, LV_SYMBOL_TINT);
      lv_obj_center(glyph);
      break;
    }
    case IconId::kPressure: {
      /* Barometer: 270° arc + needle. */
      lv_obj_t* arc = lv_arc_create(parent);
      lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
      lv_arc_set_rotation(arc, 135);
      lv_arc_set_bg_angles(arc, 0, 270);
      lv_arc_set_range(arc, 0, 100);
      lv_arc_set_value(arc, 62);
      lv_obj_set_style_arc_width(arc, size / 7, LV_PART_MAIN);
      lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
      lv_obj_set_style_arc_opa(arc, LV_OPA_60, LV_PART_MAIN);
      lv_obj_set_style_arc_width(arc, size / 7, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
      lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
      lv_obj_set_pos(arc, cx - size / 2, cy - size / 2);
      lv_obj_set_size(arc, size, size);

      lv_point_precise_t* needle = icon_points_pool();
      needle[0].x = cx;                       needle[0].y = cy + size / 8;
      needle[1].x = cx + (int)(size * 0.30f); needle[1].y = cy - (int)(size * 0.22f);
      lv_obj_t* line = lv_line_create(parent);
      lv_line_set_points(line, needle, 2);
      lv_obj_set_style_line_color(line, color, LV_PART_MAIN);
      lv_obj_set_style_line_width(line, 2, LV_PART_MAIN);
      lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
      break;
    }
    default: {
      /* Wi-Fi: three arcs of decreasing radius + a dot. */
      const lv_coord_t widths[3] = {(lv_coord_t)size, (lv_coord_t)(size * 2 / 3), (lv_coord_t)(size / 3)};
      for (int i = 0; i < 3; i += 1) {
        lv_obj_t* arc = lv_arc_create(parent);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_arc_set_rotation(arc, 180);
        lv_arc_set_bg_angles(arc, 0, 180);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 100);
        lv_obj_set_style_arc_width(arc, 2, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 2, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_pos(arc, cx - widths[i] / 2, cy - size / 2 + i * (size / 4));
        lv_obj_set_size(arc, widths[i], widths[i]);
      }
      lv_obj_t* dot = lv_obj_create(parent);
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
      lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_pos(dot, cx - 2, cy + size / 2 - 5);
      lv_obj_set_size(dot, 4, 4);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Widget set: ONE tree, repositioned per mode. Mode switching only changes
// geometry + visibility, exactly like the Web UI changes presentation state.
// ---------------------------------------------------------------------------
struct MetricWidgets {
  lv_obj_t* card = nullptr;
  lv_obj_t* icon_host = nullptr;   // container the icon primitives are drawn into
  lv_obj_t* label = nullptr;
  lv_obj_t* value = nullptr;
  lv_obj_t* unit = nullptr;
  lv_obj_t* state = nullptr;
  lv_obj_t* bar = nullptr;         // used by horizontal + bars
  lv_obj_t* gauge_bg = nullptr;    // used by gauges
  lv_obj_t* gauge_fg = nullptr;
  // Icon primitives are re-drawn only when their size or semantic colour
  // changes — never per update, so there is no per-frame object churn.
  int16_t icon_drawn_size = 0;
  ColorToken icon_drawn_token = ColorToken::kDataError;
  // Value tween state: the label text is animated between polls instead of
  // jumping, which also makes the mode/demo data changes readable.
  uint8_t decimals = 0;
  int32_t shown_scaled = 0;   // value * 10^decimals currently on screen
  bool tween_ready = false;   // false until the first real value is shown
};

struct HistoryView {
  bool available = false;
  int32_t count = 0;
  float t_min = 0.0f;
  float t_max = 1.0f;
  /* Human-readable retention window of the buffer, e.g. "10 min" or "24 h".
     The model owns the retention policy; the renderer only prints it. */
  const char* window_label = "";
  /* Three normalised series (0..1 inside the metric's own range). NaN = gap. */
  const float* series[3] = {nullptr, nullptr, nullptr};
  int32_t series_count[3] = {0, 0, 0};
};

// Host-provided status for the header badge and the diagnostics overlay. The
// renderer prints these strings, it does not know where they come from: the PC
// simulator fills them from WinHTTP polling, the device would fill them from its
// Wi-Fi/HTTP state. No URL, no sensor and no network code lives in this file.
struct StatusView {
  const char* source = "DEMO";         // "LIVE" / "DEMO"
  const char* connection = "";         // "ONLINE"/"OFFLINE"/"CONNECTING"/"ERROR"
  const char* endpoint = "";           // "192.168.1.111:80/api/weather"
  int http_status = 0;                 // 0 = no response
  double latency_ms = 0.0;
  const char* last_update = "--:--:--";
  uint32_t age_ms = 0;                 // age of the displayed snapshot
  uint32_t success_count = 0;
  uint32_t error_count = 0;
  uint32_t poll_count = 0;
  int fields_seen = 0;
  int fields_expected = 0;
  const char* note = "";               // last error / contract hint
};

struct Ui {
  lv_obj_t* root = nullptr;
  lv_obj_t* header = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* status = nullptr;
  /* Mode selector: created once, shown in every mode, clickable with a mouse and
     reachable with the keyboard (LVGL focus). On a panel without an input device
     it is still rendered — it simply cannot be activated there yet. */
  lv_obj_t* nav = nullptr;
  lv_obj_t* nav_button[kDisplayModeCount] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  lv_obj_t* nav_label[kDisplayModeCount] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  /* Everything mode-dependent lives in one container so a mode switch can be
     animated (fade + small slide) without touching the header or the nav bar. */
  lv_obj_t* content = nullptr;
  MetricWidgets metrics[3];
  lv_obj_t* unit_marks[3] = {nullptr, nullptr, nullptr};
  lv_obj_t* history_panel = nullptr;
  lv_obj_t* history_lines[3] = {nullptr, nullptr, nullptr};
  lv_obj_t* history_labels[3] = {nullptr, nullptr, nullptr};
  lv_obj_t* history_empty = nullptr;
  /* X-axis ends of the history chart (window start / "now"). */
  lv_obj_t* history_axis_from = nullptr;
  lv_obj_t* history_axis_to = nullptr;
  /* Diagnostics overlay ("status/debug panel"): toggled by the host, filled from
     a StatusView. It is part of the shared UI so a panel build gets it too. */
  lv_obj_t* info_panel = nullptr;
  lv_obj_t* info_title = nullptr;
  lv_obj_t* info_text = nullptr;
  bool info_visible = false;
  /* --------------------------------------------------------------------- i18n
     Application state, identical to the Web UI: the locale starts unset and the
     language-selection screen owns the display until the user picks one. */
  lv_obj_t* lang_screen = nullptr;
  lv_obj_t* lang_title = nullptr;
  lv_obj_t* lang_instruction = nullptr;
  lv_obj_t* lang_button[kLocaleCount] = {nullptr, nullptr};
  lv_obj_t* lang_button_label[kLocaleCount] = {nullptr, nullptr};
  lv_obj_t* lang_hint = nullptr;
  Locale locale = Locale::kNone;
  AppScreen screen = AppScreen::kLanguageSelection;
  lv_group_t* input_group = nullptr;
  /* Host hook: a *user* picked a language (button/keyboard), not the host API. */
  void (*on_locale_change)(void* user_data, Locale locale) = nullptr;
  void* on_locale_change_user = nullptr;

  DisplayGeometry geometry = kGeometry320x240;
  DisplayMode mode = DisplayMode::kNumeric;
  /* Animations are part of the UI, not of the host: the simulator can switch
     them off for deterministic captures, the device keeps them on. */
  bool animations = true;
  /* Reduced motion: same layouts, no movement (fade-only transitions). Honours
     the Windows "show animations" setting when the host asks for it. */
  bool reduced_motion = false;
  /* Debug-only event tracing ([UI][DEBUG] lines). Never enabled in NORMAL mode. */
  bool debug_events = false;
  /* Optional host hook, called after a *user* (button) mode change. */
  void (*on_mode_change)(void* user_data, DisplayMode mode) = nullptr;
  void* on_mode_change_user = nullptr;
};

inline lv_obj_t* make_text(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
  /* The shared layout owns the size of every text box: a label must never grow
   * or wrap beyond its rectangle (LVGL's default is wrap-and-expand-height). */
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
  lv_label_set_text(label, "");
  return label;
}

// Metric index -> icon identity / neutral semantic token (used before the first
// real measurement arrives, so the UI never shows an uncoloured icon).
inline IconId icon_id_for_index(int index) {
  switch (index) {
    case 0: return IconId::kTemperature;
    case 1: return IconId::kHumidity;
    default: return IconId::kPressure;
  }
}

inline ColorToken default_token_for_index(int index) {
  switch (index) {
    case 0: return ColorToken::kTempComfort;
    case 1: return ColorToken::kHumidityComfort;
    default: return ColorToken::kPressureNormal;
  }
}

// Redraws an icon only when its pixel size or semantic colour changed: the
// primitive objects are reused otherwise, so updates stay allocation-free.
inline void ensure_icon(MetricWidgets& widgets, IconId icon, int size, ColorToken token) {
  if (size <= 0) { return; }
  if (widgets.icon_drawn_size == (int16_t)size && widgets.icon_drawn_token == token &&
      lv_obj_get_child_count(widgets.icon_host) > 0) {
    return;
  }
  lv_obj_clean(widgets.icon_host);
  draw_icon(widgets.icon_host, icon, makeRect(0, 0, size, size), token_color(token));
  widgets.icon_drawn_size = (int16_t)size;
  widgets.icon_drawn_token = token;
}

inline void place(lv_obj_t* obj, const LayoutRect& rect) {
  lv_obj_set_pos(obj, rect.x, rect.y);
  lv_obj_set_size(obj, rect.w > 0 ? rect.w : 1, rect.h > 0 ? rect.h : 1);
}

// The shared layout uses absolute screen coordinates, while card widgets are
// children of the card object: convert to parent-relative when placing them.
inline void place_in(lv_obj_t* obj, const LayoutRect& rect, const LayoutRect& parent) {
  lv_obj_set_pos(obj, rect.x - parent.x, rect.y - parent.y);
  lv_obj_set_size(obj, rect.w > 0 ? rect.w : 1, rect.h > 0 ? rect.h : 1);
}

// Rough text metrics: a glyph is about 0.58 em wide in Montserrat at these sizes.
// The estimate uses the same font assets on the PC and on the device, so both
// targets make the same fit decision.
inline int estimate_text_width(const char* text, const lv_font_t* font) {
  if (text == nullptr || font == nullptr) { return 0; }
  return (int)((float)strlen(text) * (float)font->line_height * 0.58f);
}

// Picks the largest font in which `text` fits the box, so a long pressure reading
// cannot overflow a narrow card (LVGL has no auto-fit). Both dimensions matter:
// a value box shorter than the font's line height clips the digits vertically.
// `height <= 0` means "width only".
inline const lv_font_t* font_for_text(const char* text, int width, int height,
                                     const lv_font_t* preferred) {
  const lv_font_t* candidates[3] = {preferred, font_normal(), font_small()};
  for (int i = 0; i < 3; i += 1) {
    const lv_font_t* font = candidates[i];
    if (font == nullptr) { continue; }
    const bool fits_width = estimate_text_width(text, font) <= width;
    const bool fits_height = (height <= 0) || ((int)font->line_height <= height);
    if ((fits_width && fits_height) || i == 2) { return font; }
  }
  return preferred;
}

inline const lv_font_t* font_for_text(const char* text, int width, const lv_font_t* preferred) {
  return font_for_text(text, width, 0, preferred);
}

// 320x240 gives each card label about 58 px, where "Temperature" cannot fit at
// any available font size. The short form keeps the label readable instead of
// clipping it; wider geometries keep the full name.
inline const char* short_label_for(IconId icon) {
  switch (icon) {
    case IconId::kTemperature: return "Temp";
    case IconId::kHumidity: return "Humid";
    default: return "Press";
  }
}

inline const char* label_fitting(const char* full, IconId icon, const lv_font_t* font,
                                int width) {
  if (full != nullptr && estimate_text_width(full, font) <= width) { return full; }
  return short_label_for(icon);
}

// Mode switching = presentation state only: geometry + which parts are visible.
// The MetricWidgets are shared between modes, so there is one UI implementation.
inline void ui_set_mode(Ui& ui, DisplayMode mode);

/* Diagnostics overlay. Declared before ui_build() because the tree builder ends
 * by hiding it. While it is visible the dashboard content is hidden as well: on a
 * 320x240 panel a nine-line status panel and the cards cannot share the screen
 * without turning both into noise. */
inline void ui_set_info_visible(Ui& ui, bool visible) {
  ui.info_visible = visible;
  if (ui.info_panel == nullptr) { return; }
  if (visible) {
    if (ui.content != nullptr) { lv_obj_add_flag(ui.content, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_remove_flag(ui.info_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui.info_panel);
  } else {
    if (ui.content != nullptr) { lv_obj_remove_flag(ui.content, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_add_flag(ui.info_panel, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---------------------------------------------------------------------------
// Animations. Plain lv_anim only — no desktop library, no C++ STL, short
// durations that are just as affordable on the ESP32 as they are on the PC.
// ---------------------------------------------------------------------------
constexpr uint32_t kModeTransitionMs = 180;
constexpr uint32_t kValueTweenMs = 320;
constexpr uint32_t kScaleAnimMs = 420;

// Mode transition: fade the content in while it slides a few pixels. One
// animation drives both properties, so a mode switch costs a single anim slot.
inline void anim_content_in(void* var, int32_t value) {
  lv_obj_t* content = (lv_obj_t*)var;
  lv_obj_set_x(content, (lv_coord_t)(((255 - value) * 14) / 255));
  lv_obj_set_style_opa(content, (lv_opa_t)value, LV_PART_MAIN);
}

inline void ui_animate_mode(Ui& ui) {
  if (!ui.animations || ui.content == nullptr) { return; }
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, ui.content);
  lv_anim_set_values(&anim, ui.reduced_motion ? 120 : 0, 255);
  lv_anim_set_duration(&anim, ui.reduced_motion ? (kModeTransitionMs / 3) : kModeTransitionMs);
  lv_anim_set_exec_cb(&anim, anim_content_in);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
  lv_anim_start(&anim);
}

// Value tween: the metric's own decimals decide the integer scale, so the text
// ends exactly on the value that formatMetricText() would have printed.
inline int32_t value_scale_for(uint8_t decimals) {
  return (decimals == 0) ? 1 : ((decimals == 1) ? 10 : 100);
}

inline void anim_value_text(void* var, int32_t value) {
  MetricWidgets* widgets = (MetricWidgets*)var;
  char text[24];
  const int32_t scale = value_scale_for(widgets->decimals);
  snprintf(text, sizeof(text), "%.*f", (int)widgets->decimals,
           (double)value / (double)scale);
  lv_label_set_text(widgets->value, text);
}

// Replaces the value text through a tween when animations are on and the metric
// has a numeric value; otherwise it is a plain, allocation-free text set.
inline void set_value_text(Ui& ui, MetricWidgets& widgets, const MetricPresentation& metric) {
  lv_anim_delete(&widgets, anim_value_text);
  if (!metric.has_value || isnan(metric.value)) {
    char text[24];
    formatMetricText(metric, text, sizeof(text));
    lv_label_set_text(widgets.value, text);
    widgets.shown_scaled = 0;
    widgets.tween_ready = false;
    return;
  }
  widgets.decimals = metric.decimals;
  const int32_t scale = value_scale_for(metric.decimals);
  const float scaled = metric.value * (float)scale;
  const int32_t target = (int32_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
  if (!ui.animations || !widgets.tween_ready || widgets.shown_scaled == target) {
    widgets.shown_scaled = target;
    widgets.tween_ready = true;
    anim_value_text(&widgets, target);
    return;
  }
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, &widgets);
  lv_anim_set_values(&anim, widgets.shown_scaled, target);
  lv_anim_set_duration(&anim, ui.reduced_motion ? (kValueTweenMs / 3) : kValueTweenMs);
  lv_anim_set_exec_cb(&anim, anim_value_text);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
  lv_anim_start(&anim);
  widgets.shown_scaled = target;
}

// Nav button handler: switching from the UI is a user action, so the host hook
// (the simulator's keyboard/CLI state) is notified after the switch.
inline void nav_button_event_cb(lv_event_t* event) {
  Ui* ui = (Ui*)lv_event_get_user_data(event);
  if (ui == nullptr) { return; }
  lv_obj_t* button = lv_event_get_target_obj(event);
  const int index = (int)(intptr_t)lv_obj_get_user_data(button);
  if (index < 0 || index >= (int)kDisplayModeCount) { return; }
  const DisplayMode mode = displayModeAt((uint8_t)index);
  if (ui->debug_events) { printf("[UI][DEBUG] tab click %s\n", displayModeId(mode)); }
  if (ui->mode == mode) {
    if (ui->debug_events) { printf("[UI][DEBUG] already in %s\n", displayModeId(mode)); }
    return;
  }
  ui_set_mode(*ui, mode);
  if (ui->debug_events) { printf("[UI][DEBUG] mode=%s\n", displayModeId(ui->mode)); }
  if (ui->on_mode_change != nullptr) { ui->on_mode_change(ui->on_mode_change_user, mode); }
}

// ---------------------------------------------------------------------------
// Language selection (first screen on every start) and locale application.
//
// The screen owns the display until a locale is chosen: the dashboard (header,
// mode strip, cards, history) is hidden, exactly like the Web UI hides it. The
// transition into the dashboard is a short fade/slide that also honours reduced
// motion. No translation happens at the call site — every label is looked up by
// key through tr().
// ---------------------------------------------------------------------------
constexpr uint32_t kScreenTransitionMs = 220;

inline void ui_set_screen(Ui& ui, AppScreen screen);
inline void ui_apply_locale(Ui& ui, Locale locale);

inline void language_button_event_cb(lv_event_t* event) {
  Ui* ui = (Ui*)lv_event_get_user_data(event);
  if (ui == nullptr) { return; }
  lv_obj_t* button = lv_event_get_target_obj(event);
  const int index = (int)(intptr_t)lv_obj_get_user_data(button);
  if (index < 0 || index >= (int)kLocaleCount) { return; }
  const Locale chosen = localeAt((uint8_t)index);
  if (ui->debug_events) { printf("[UI][DEBUG] language click %s\n", localeId(chosen)); }
  ui_apply_locale(*ui, chosen);
  if (ui->on_locale_change != nullptr) { ui->on_locale_change(ui->on_locale_change_user, chosen); }
}

/* Screen switch: visibility of the two screens plus the keyboard focus group, so
   TAB/arrows never land on an invisible button. */
inline void ui_set_screen(Ui& ui, AppScreen screen) {
  ui.screen = screen;
  const bool dashboard = (screen == AppScreen::kDashboard);
  /* Layout-owned rectangles: the language panel is placed from the same
     computeLayout() both targets use, never from local arithmetic. */
  if (ui.lang_screen != nullptr) {
    const DisplayLayout layout = computeLayout(ui.mode, PresentationModel{}, ui.geometry);
    place(ui.lang_screen, layout.lang_panel);
    place_in(ui.lang_title, layout.lang_title, layout.lang_panel);
    place_in(ui.lang_instruction, layout.lang_instruction, layout.lang_panel);
    for (uint8_t i = 0; i < kLocaleCount; i += 1) {
      place_in(ui.lang_button[i], layout.lang_button[i], layout.lang_panel);
    }
    place_in(ui.lang_hint, layout.lang_hint, layout.lang_panel);
  }
  if (ui.lang_screen != nullptr) {
    if (dashboard) {
      lv_obj_add_flag(ui.lang_screen, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(ui.lang_screen, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(ui.lang_screen);
    }
  }
  lv_obj_t* dashboard_objects[] = {ui.header, ui.nav, ui.content, ui.info_panel};
  for (size_t i = 0; i < sizeof(dashboard_objects) / sizeof(dashboard_objects[0]); i += 1) {
    if (dashboard_objects[i] == nullptr) { continue; }
    if (i == 3 && !ui.info_visible) { continue; }  /* the overlay keeps its own state */
    if (dashboard) {
      lv_obj_remove_flag(dashboard_objects[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(dashboard_objects[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (ui.input_group != nullptr) {
    for (uint8_t i = 0; i < kLocaleCount; i += 1) {
      if (ui.lang_button[i] == nullptr) { continue; }
      if (dashboard) {
        lv_group_remove_obj(ui.lang_button[i]);
      } else {
        lv_group_add_obj(ui.input_group, ui.lang_button[i]);
      }
    }
    for (uint8_t i = 0; i < kDisplayModeCount; i += 1) {
      if (ui.nav_button[i] == nullptr) { continue; }
      if (dashboard) {
        lv_group_add_obj(ui.input_group, ui.nav_button[i]);
      } else {
        lv_group_remove_obj(ui.nav_button[i]);
      }
    }
    if (!dashboard && ui.lang_button[0] != nullptr) { lv_group_focus_obj(ui.lang_button[0]); }
    if (dashboard && ui.nav_button[0] != nullptr) { lv_group_focus_obj(ui.nav_button[0]); }
  }
}

/* Applies only the TEXT locale (language screen + dashboard labels) without
   changing the visible screen. Used for the golden captures, which need both the
   RU and the EN language screen, and by a device that shows the selector in a
   default language before the user chooses. */
inline void ui_apply_text_locale(Ui& ui, Locale locale) {
  const Locale previous = ui.locale;
  ui.locale = locale;
  ui_apply_locale(ui, locale);
  /* ui_apply_locale() jumps to the dashboard for a real locale: undo that when
     the caller only asked for text, keeping the screen it had. */
  if (previous == Locale::kNone) { ui_set_screen(ui, AppScreen::kLanguageSelection); }
}

/* Applies a locale to every label of the dashboard and (when the locale becomes
   known) moves from the language screen to the dashboard with a short fade. */
inline void ui_apply_locale(Ui& ui, Locale locale) {
  ui.locale = locale;
  const Locale text_locale = (locale == Locale::kNone) ? Locale::kEn : locale;

  /* Language screen itself: the button captions are native names, so they read
     the same in both locales by design. */
  if (ui.lang_title != nullptr) { lv_label_set_text(ui.lang_title, tr(TextKey::kAppTitle, text_locale)); }
  if (ui.lang_instruction != nullptr) {
    lv_label_set_text(ui.lang_instruction, tr(TextKey::kChooseLanguage, text_locale));
  }
  if (ui.lang_hint != nullptr) { lv_label_set_text(ui.lang_hint, tr(TextKey::kLanguageHint, text_locale)); }
  for (uint8_t i = 0; i < kLocaleCount; i += 1) {
    if (ui.lang_button_label[i] == nullptr) { continue; }
    lv_label_set_text(ui.lang_button_label[i],
                      tr((i == 0) ? TextKey::kLanguageRu : TextKey::kLanguageEn, text_locale));
  }

  /* Dashboard chrome. The header is one line wide, so it uses the compact title
     (the full name would collide with the status badge on 320x240); the language
     screen and the status panel have room for the full name. */
  if (ui.title != nullptr) { lv_label_set_text(ui.title, tr(TextKey::kAppTitleShort, text_locale)); }
  for (uint8_t i = 0; i < kDisplayModeCount; i += 1) {
    if (ui.nav_label[i] == nullptr) { continue; }
    const TextKey short_keys[kDisplayModeCount] = {
        TextKey::kTabNumericShort, TextKey::kTabHorizontalShort, TextKey::kTabBarsShort,
        TextKey::kTabGaugesShort, TextKey::kTabHistoryShort};
    lv_label_set_text(ui.nav_label[i], tr(short_keys[i], text_locale));
  }
  if (ui.info_title != nullptr) { lv_label_set_text(ui.info_title, tr(TextKey::kAppTitle, text_locale)); }
  if (ui.history_axis_to != nullptr) { lv_label_set_text(ui.history_axis_to, tr(TextKey::kNow, text_locale)); }

  if (locale != Locale::kNone) { ui_set_screen(ui, AppScreen::kDashboard); }
}

// Short display label for a metric card: full name when it fits the rectangle,
// the compact form otherwise (adaptive typography driven by the layout box, not
// by a per-locale hardcode at the call site).
inline const char* localized_metric_label(TextKey full, TextKey compact, Locale locale,
                                          const lv_font_t* font, int width) {
  const char* candidate = tr(full, locale);
  if (estimate_text_width(candidate, font) <= width) { return candidate; }
  return tr(compact, locale);
}

// Builds the whole tree once. `parent` is usually lv_screen_active().
inline void ui_build(Ui& ui, lv_obj_t* parent, DisplayGeometry geometry) {
  ui.geometry = geometry;
  ui.root = lv_obj_create(parent);
  make_passive(ui.root);
  lv_obj_set_style_radius(ui.root, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(ui.root, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ui.root, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui.root, lv_color_hex(0x0B1220), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(ui.root, lv_color_hex(0x16233C), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(ui.root, LV_GRAD_DIR_VER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_pos(ui.root, 0, 0);
  lv_obj_set_size(ui.root, geometry.width, geometry.height);

  ui.header = lv_obj_create(ui.root);
  make_passive(ui.header);
  lv_obj_set_style_radius(ui.header, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ui.header, 26, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui.header, lv_color_hex(0x8FA3BF), LV_PART_MAIN);
  lv_obj_set_style_border_width(ui.header, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(ui.header, lv_color_hex(0x2A3A55), LV_PART_MAIN);
  lv_obj_set_style_pad_all(ui.header, 4, LV_PART_MAIN);
  ui.title = make_text(ui.header, font_normal(), lv_color_hex(0xEEF4FF));
  lv_label_set_text(ui.title, "Weather Station");
  lv_obj_align(ui.title, LV_ALIGN_LEFT_MID, 6, 0);
  ui.status = make_text(ui.header, font_small(), lv_color_hex(0x9FB2CC));
  lv_label_set_text(ui.status, "FRESH");
  lv_obj_align(ui.status, LV_ALIGN_RIGHT_MID, -6, 0);

  /* ---- mode selector (mouse + keyboard) ---------------------------------- */
  ui.nav = lv_obj_create(ui.root);
  make_passive(ui.nav);
  lv_obj_set_style_bg_opa(ui.nav, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ui.nav, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ui.nav, 0, LV_PART_MAIN);
  for (uint8_t i = 0; i < kDisplayModeCount; i += 1) {
    lv_obj_t* button = lv_button_create(ui.nav);
    lv_obj_set_user_data(button, (void*)(intptr_t)i);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    /* Press/hover feedback is a style *transition*, i.e. LVGL animates the
       property change itself — no extra animation objects to manage. */
    lv_obj_set_style_bg_opa(button, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x8FA3BF), LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0x2A3A55), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, 90, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, 255, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(button, lv_color_hex(0x43D19E), LV_STATE_FOCUSED);
    lv_obj_set_style_transition(button, &trans_fast(), LV_STATE_PRESSED);
    lv_obj_set_style_transition(button, &trans_fast(), LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(button, font_small(), LV_PART_MAIN);
    ui.nav_label[i] = make_text(button, font_small(), lv_color_hex(0xDCE6F5));
    lv_label_set_text(ui.nav_label[i], displayModeShortId(displayModeAt(i)));
    lv_obj_center(ui.nav_label[i]);
    lv_obj_add_event_cb(button, nav_button_event_cb, LV_EVENT_CLICKED, &ui);
    ui.nav_button[i] = button;
  }

  /* ---- mode-dependent content (animated as one unit) --------------------- */
  ui.content = lv_obj_create(ui.root);
  /* Full-screen container above the nav bar: it MUST stay passive, otherwise it
     swallows every click on the mode buttons (root cause of the input defect). */
  make_passive(ui.content);
  lv_obj_set_style_bg_opa(ui.content, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ui.content, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ui.content, 0, LV_PART_MAIN);
  lv_obj_set_pos(ui.content, 0, 0);
  lv_obj_set_size(ui.content, geometry.width, geometry.height);

  for (int i = 0; i < 3; i += 1) {
    MetricWidgets& metric = ui.metrics[i];
    metric.card = lv_obj_create(ui.content);
    apply_glass(metric.card, glass_for(DataState::kFresh, ColorToken::kTempComfort));

    metric.icon_host = lv_obj_create(metric.card);
    make_passive(metric.icon_host);
    lv_obj_set_style_bg_opa(metric.icon_host, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(metric.icon_host, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(metric.icon_host, 0, LV_PART_MAIN);

    metric.label = make_text(metric.card, font_small(), lv_color_hex(0x9FB2CC));
    metric.value = make_text(metric.card, font_large(), lv_color_hex(0xEEF4FF));
    metric.unit = make_text(metric.card, font_small(), lv_color_hex(0x9FB2CC));
    metric.state = make_text(metric.card, font_small(), lv_color_hex(0x43D19E));

    metric.bar = lv_bar_create(metric.card);
    lv_obj_set_style_radius(metric.bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(metric.bar, 40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(metric.bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_radius(metric.bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(metric.bar, 0, 1000);
    /* Bar/arc values animate on their own: LVGL reads this duration whenever
       lv_bar_set_value(..., LV_ANIM_ON) is used. */
    lv_obj_set_style_anim_duration(metric.bar, kScaleAnimMs, LV_PART_INDICATOR);

    metric.gauge_bg = lv_arc_create(metric.card);
    lv_obj_remove_flag(metric.gauge_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(metric.gauge_bg, 135);
    lv_arc_set_bg_angles(metric.gauge_bg, 0, 270);
    lv_arc_set_range(metric.gauge_bg, 0, 1000);
    lv_arc_set_value(metric.gauge_bg, 0);
    lv_obj_set_style_arc_width(metric.gauge_bg, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(metric.gauge_bg, lv_color_hex(0x33415C), LV_PART_MAIN);
    lv_obj_set_style_arc_width(metric.gauge_bg, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(metric.gauge_bg, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(metric.gauge_bg, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_anim_duration(metric.gauge_bg, kScaleAnimMs, LV_PART_INDICATOR);
    metric.gauge_fg = metric.gauge_bg;
  }

  ui.history_panel = lv_obj_create(ui.content);
  apply_glass(ui.history_panel, glass_for(DataState::kFresh, ColorToken::kPressureNormal));
  /* Lines and labels live on the content container, in absolute screen
   * coordinates taken from the shared layout — the panel is only the glass
   * background. */
  for (int i = 0; i < 3; i += 1) {
    ui.history_lines[i] = lv_line_create(ui.content);
    lv_obj_set_style_line_width(ui.history_lines[i], 2, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ui.history_lines[i], true, LV_PART_MAIN);
    ui.history_labels[i] = make_text(ui.content, font_small(), lv_color_hex(0x9FB2CC));
  }
  ui.history_empty = make_text(ui.content, font_small(), lv_color_hex(0x9FB2CC));
  lv_label_set_text(ui.history_empty, "Collecting history...");
  ui.history_axis_from = make_text(ui.content, font_small(), lv_color_hex(0x7F8FA6));
  ui.history_axis_to = make_text(ui.content, font_small(), lv_color_hex(0x7F8FA6));
  lv_label_set_text(ui.history_axis_to, "now");
  for (int i = 0; i < 3; i += 1) {
    ui.unit_marks[i] = make_text(ui.content, font_small(), lv_color_hex(0x9FB2CC));
    lv_obj_add_flag(ui.unit_marks[i], LV_OBJ_FLAG_HIDDEN);
  }

  /* ---- diagnostics overlay (status/debug panel) -------------------------- */
  ui.info_panel = lv_obj_create(ui.root);
  apply_glass(ui.info_panel, glass_for(DataState::kFresh, ColorToken::kTempComfort));
  /* Solid surface: it is a debug/status panel that must stay readable over the
     cards, while keeping the same vocabulary (radius, border, shadow). */
  lv_obj_set_style_bg_grad_opa(ui.info_panel, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui.info_panel, lv_color_hex(0x0E1726), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ui.info_panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(ui.info_panel, LV_OBJ_FLAG_HIDDEN);
  ui.info_title = make_text(ui.info_panel, font_normal(), lv_color_hex(0xEEF4FF));
  lv_label_set_text(ui.info_title, "Simulator status");
  ui.info_text = make_text(ui.info_panel, font_small(), lv_color_hex(0xDCE6F5));
  lv_label_set_long_mode(ui.info_text, LV_LABEL_LONG_MODE_WRAP);
  lv_label_set_text(ui.info_text, "");

  /* ---- language selection (first screen on every start) ------------------ */
  ui.lang_screen = lv_obj_create(ui.root);
  apply_glass(ui.lang_screen, glass_for(DataState::kFresh, ColorToken::kTempComfort));
  lv_obj_set_style_bg_grad_opa(ui.lang_screen, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui.lang_screen, lv_color_hex(0x0E1726), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ui.lang_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ui.lang_screen, 28, LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(ui.lang_screen, LV_OPA_50, LV_PART_MAIN);

  ui.lang_title = make_text(ui.lang_screen, font_normal(), lv_color_hex(0xEEF4FF));
  lv_obj_set_style_text_align(ui.lang_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(ui.lang_title, LV_LABEL_LONG_MODE_DOTS);
  ui.lang_instruction = make_text(ui.lang_screen, font_small(), lv_color_hex(0x9FB2CC));
  lv_obj_set_style_text_align(ui.lang_instruction, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(ui.lang_instruction, LV_LABEL_LONG_MODE_DOTS);
  ui.lang_hint = make_text(ui.lang_screen, font_small(), lv_color_hex(0x7F8FA6));
  lv_obj_set_style_text_align(ui.lang_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(ui.lang_hint, LV_LABEL_LONG_MODE_WRAP);

  for (uint8_t i = 0; i < kLocaleCount; i += 1) {
    lv_obj_t* button = lv_button_create(ui.lang_screen);
    lv_obj_set_user_data(button, (void*)(intptr_t)i);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0x43D19E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2FD3C0), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(button, lv_color_hex(0x43D19E), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(button, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, 210, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, 255, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, 255, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(button, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(button, lv_color_hex(0x2FD3C0), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(button, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_transition(button, &trans_fast(), LV_STATE_PRESSED);
    lv_obj_set_style_transition(button, &trans_fast(), LV_STATE_FOCUSED);
    ui.lang_button_label[i] = make_text(button, font_normal(), lv_color_hex(0x06231E));
    lv_obj_center(ui.lang_button_label[i]);
    lv_obj_add_event_cb(button, language_button_event_cb, LV_EVENT_CLICKED, &ui);
    ui.lang_button[i] = button;
  }

  ui_set_mode(ui, ui.mode);
  ui_set_info_visible(ui, false);
  /* First-run state: no locale yet, so the language screen owns the display and
     the dashboard stays hidden until a choice is made. */
  ui_apply_locale(ui, Locale::kNone);
  ui_set_screen(ui, AppScreen::kLanguageSelection);
}

// Mode switching = presentation state only: geometry + which parts are visible.
// The MetricWidgets are shared between modes, so there is one UI implementation.
inline void ui_set_mode(Ui& ui, DisplayMode mode) {
  ui.mode = mode;

  DisplayLayout layout = computeLayout(mode, PresentationModel{}, ui.geometry);
  /* computeLayout only needs the geometry for positions; values are irrelevant
     to the rectangles, so an empty model is a valid input here. */
  place(ui.header, layout.header);
  lv_obj_align(ui.title, LV_ALIGN_LEFT_MID, 6, 0);
  lv_obj_align(ui.status, LV_ALIGN_RIGHT_MID, -6, 0);

  /* Mode selector: the same rectangles on both targets come from the layout.
     The buttons are children of the nav container, so the coordinates are
     converted to parent-relative (LVGL clips children to their parent). */
  place(ui.nav, layout.nav);
  for (uint8_t i = 0; i < kDisplayModeCount; i += 1) {
    lv_obj_t* button = ui.nav_button[i];
    const DisplayMode candidate = displayModeAt(i);
    const bool active = (candidate == mode);
    place_in(button, layout.nav_button[i], layout.nav);
    lv_obj_set_style_bg_color(button, active ? token_color(ColorToken::kTempComfort)
                                             : lv_color_hex(0x8FA3BF),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, active ? 210 : 24, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, active ? token_color(ColorToken::kTempComfort)
                                                 : lv_color_hex(0x2A3A55),
                                  LV_PART_MAIN);
    lv_obj_set_style_text_color(ui.nav_label[i], active ? lv_color_hex(0x0B1220)
                                                        : lv_color_hex(0xDCE6F5),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(ui.nav_label[i], font_small(), LV_PART_MAIN);
  }

  const bool history = (mode == DisplayMode::kHistory);
  if (history) {
    lv_obj_remove_flag(ui.history_panel, LV_OBJ_FLAG_HIDDEN);
    place(ui.history_panel, layout.history_panel);
    place(ui.history_empty, layout.history_title);
    place(ui.history_axis_from, layout.history_axis);
    place(ui.history_axis_to, layout.history_axis);
    lv_obj_set_style_text_align(ui.history_axis_from, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_align(ui.history_axis_to, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_remove_flag(ui.history_axis_from, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui.history_axis_to, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i += 1) {
      place(ui.history_labels[i], layout.history_label[i]);
      lv_obj_set_style_text_align(ui.history_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }
  } else {
    lv_obj_add_flag(ui.history_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.history_empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.history_axis_from, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.history_axis_to, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i += 1) {
      lv_obj_add_flag(ui.history_lines[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui.history_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  for (int i = 0; i < 3; i += 1) {
    MetricWidgets& metric = ui.metrics[i];
    const MetricLayout& cell = layout.metrics[i];
    if (history) {
      lv_obj_add_flag(metric.card, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(metric.card, LV_OBJ_FLAG_HIDDEN);
    place(metric.card, cell.card);

    const bool gauge_mode = (mode == DisplayMode::kGauges);
    const bool track_mode = (mode == DisplayMode::kHorizontal || mode == DisplayMode::kBars);

    if (gauge_mode) {
      lv_obj_add_flag(metric.bar, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(metric.gauge_bg, LV_OBJ_FLAG_HIDDEN);
      place_in(metric.gauge_bg, cell.gauge, cell.card);
    } else {
      lv_obj_add_flag(metric.gauge_bg, LV_OBJ_FLAG_HIDDEN);
      if (track_mode) {
        lv_obj_remove_flag(metric.bar, LV_OBJ_FLAG_HIDDEN);
        place_in(metric.bar, cell.track, cell.card);
        lv_bar_set_orientation(metric.bar, (mode == DisplayMode::kBars)
                                               ? LV_BAR_ORIENTATION_VERTICAL
                                               : LV_BAR_ORIENTATION_HORIZONTAL);
      } else {
        lv_obj_add_flag(metric.bar, LV_OBJ_FLAG_HIDDEN);
      }
    }
    place_in(metric.icon_host, cell.icon, cell.card);
    place_in(metric.label, cell.label, cell.card);
    place_in(metric.value, cell.value, cell.card);
    place_in(metric.unit, cell.unit, cell.card);
    place_in(metric.state, cell.state, cell.card);

    // Draw the icon primitives once per (size, semantic colour) change.
    const int icon_size = (cell.icon.w < cell.icon.h ? cell.icon.w : cell.icon.h);
    const ColorToken icon_token = (metric.icon_drawn_size == 0)
                                      ? default_token_for_index(i)
                                      : metric.icon_drawn_token;
    ensure_icon(metric, icon_id_for_index(i), icon_size, icon_token);
  }

  /* Mode transition: the content container fades in and slides a few pixels.
     Called from the UI itself (button) and from the host (keyboard/CLI), so the
     animation is identical on the PC and on the device. */
  ui_animate_mode(ui);

  /* Diagnostics overlay follows the same layout on both targets. */
  place(ui.info_panel, layout.info);
  place_in(ui.info_title, layout.info_title, layout.info);
  place_in(ui.info_text, layout.info_text, layout.info);
  /* Bar/arc animation duration follows the reduced-motion preference. */
  for (int i = 0; i < 3; i += 1) {
    lv_obj_set_style_anim_duration(ui.metrics[i].bar, ui.reduced_motion ? 120 : kScaleAnimMs,
                                   LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(ui.metrics[i].gauge_bg, ui.reduced_motion ? 120 : kScaleAnimMs,
                                   LV_PART_INDICATOR);
  }
}

// Host-facing alias: `ui_set_mode` already animates, this only documents intent
// for callers that switch modes from the outside (keyboard, CLI, device input).
inline void ui_set_mode_animated(Ui& ui, DisplayMode mode) { ui_set_mode(ui, mode); }

// Registers the interactive buttons with an LVGL input group, so a keypad/rotary
// input can focus them and activate with ENTER (the SDL keyboard driver is a
// keypad indev). Membership follows the active screen: only the visible screen's
// buttons are in the group, so TAB/arrows never land on an invisible widget.
inline void ui_attach_input_group(Ui& ui, lv_group_t* group) {
  if (group == nullptr) { return; }
  ui.input_group = group;
  ui_set_screen(ui, ui.screen);
}

// The focused button is highlighted by LVGL's own focus state, so nothing else
// is needed to make keyboard navigation visible; this helper only exists for
// hosts that want to focus the button matching the current mode after a switch.
inline void ui_focus_mode_button(Ui& ui, lv_group_t* group) {
  if (group == nullptr) { return; }
  for (uint8_t i = 0; i < kDisplayModeCount; i += 1) {
    if (displayModeAt(i) == ui.mode) { lv_group_focus_obj(ui.nav_button[i]); }
  }
}

// Value/state updates: text and bar/arc values only — no object churn.
inline void ui_update(Ui& ui, const PresentationModel& model, const char* status_text) {
  const MetricPresentation* metrics[3] = {&model.temperature, &model.humidity, &model.pressure};
  const DisplayLayout layout = computeLayout(ui.mode, model, ui.geometry);
  const Locale locale = (ui.locale == Locale::kNone) ? Locale::kEn : ui.locale;
  for (int i = 0; i < 3; i += 1) {
    const MetricPresentation& metric = *metrics[i];
    MetricWidgets& widgets = ui.metrics[i];
    const GlassStyle style = glass_for(metric.state, metric.color_token);
    const lv_color_t accent = token_color(metric.color_token);

    apply_glass(widgets.card, style);

    /* Value text: tweened between updates (LV_ANIM_OFF semantics when the host
     * disables animations), always ending on formatMetricText()'s output. */
    set_value_text(ui, widgets, metric);
    char text[24];
    formatMetricText(metric, text, sizeof(text));
    lv_obj_set_style_text_color(widgets.value, accent, LV_PART_MAIN);
    /* Pick the largest font that still fits the value rect: a long pressure
     * reading must not overflow a narrow card, and a short box must not clip the
     * digits vertically. */
    const int value_width = lv_obj_get_width(widgets.value);
    const int value_height = lv_obj_get_height(widgets.value);
    lv_obj_set_style_text_font(widgets.value,
                               font_for_text(text, value_width, value_height, font_large()),
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(widgets.value,
                                (ui.mode == DisplayMode::kGauges) ? LV_TEXT_ALIGN_CENTER
                                                                  : LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    /* In gauge mode the value sits inside the ring, so its unit must be centred
     * under it as well; every other mode keeps the left-aligned column. */
    lv_obj_set_style_text_align(widgets.unit,
                                (ui.mode == DisplayMode::kGauges) ? LV_TEXT_ALIGN_CENTER
                                                                  : LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);

    /* Card label: localized full name when it fits, compact form otherwise. The
     * choice is made from the measurement of the layout rectangle, so a longer
     * translation can never clip. */
    const TextKey full_keys[3] = {TextKey::kTemperature, TextKey::kHumidity, TextKey::kPressure};
    const TextKey short_keys[3] = {TextKey::kTemperatureShort, TextKey::kHumidityShort,
                                   TextKey::kPressureShort};
    lv_label_set_text(widgets.label,
                      localized_metric_label(full_keys[i], short_keys[i], locale, font_small(),
                                             lv_obj_get_width(widgets.label)));
    lv_label_set_text(widgets.unit, metric.unit);
    /* State badge: localized word, semantic role stays in dataStateName(). */
    const TextKey state_keys[3] = {TextKey::kFresh, TextKey::kStale, TextKey::kError};
    const TextKey state_key = (metric.state == DataState::kError)
                                  ? state_keys[2]
                                  : ((metric.state == DataState::kStale) ? state_keys[1] : state_keys[0]);
    lv_label_set_text(widgets.state, tr(state_key, locale));
    lv_obj_set_style_text_color(widgets.state, token_color(colorTokenForState(metric.state)), LV_PART_MAIN);

    if (ui.mode != DisplayMode::kHistory) {
      const LayoutRect& icon_rect = layout.metrics[i].icon;
      ensure_icon(widgets, icon_id_for_index(i),
                  (icon_rect.w < icon_rect.h ? icon_rect.w : icon_rect.h), metric.color_token);
    }

    if (widgets.bar) {
      const int value = metric.has_value ? (int)(metric.fraction * 1000.0f + 0.5f) : 0;
      /* LV_ANIM_ON lets LVGL glide the indicator to the new value using the
       * style anim duration set at build time (works on the ESP32 too). */
      lv_bar_set_value(widgets.bar, value, ui.animations ? LV_ANIM_ON : LV_ANIM_OFF);
      lv_obj_set_style_bg_color(widgets.bar, accent, LV_PART_INDICATOR);
    }
    if (widgets.gauge_bg) {
      const int sweep = metric.has_value ? (int)(gaugeSweepFor(metric.fraction) * 1000.0f / 270.0f + 0.5f) : 0;
      lv_arc_set_value(widgets.gauge_bg, sweep);
      lv_obj_set_style_arc_color(widgets.gauge_bg, accent, LV_PART_INDICATOR);
    }
  }
  if (status_text != nullptr) {
    lv_label_set_text(ui.status, status_text);
  }
}

// Header badge + diagnostics overlay. The host supplies the *values* (and the
// connection state, which is protocol data), the UI owns the words: every label
// here is looked up by key in the active locale.
inline void ui_update_status(Ui& ui, const StatusView& status) {
  const Locale locale = (ui.locale == Locale::kNone) ? Locale::kEn : ui.locale;
  const char* source = (status.source != nullptr) ? status.source : "";
  const char* connection = (status.connection != nullptr) ? status.connection : "";
  /* The connection word is localized; the source word (LIVE/DEMO) is an
     identifier and reads the same in both locales. */
  const char* connection_text = connection;
  if (strcmp(connection, "ONLINE") == 0) { connection_text = tr(TextKey::kOnline, locale); }
  else if (strcmp(connection, "OFFLINE") == 0) { connection_text = tr(TextKey::kOffline, locale); }
  else if (strcmp(connection, "CONNECTING") == 0) { connection_text = tr(TextKey::kConnecting, locale); }
  else if (strcmp(connection, "ERROR") == 0) { connection_text = tr(TextKey::kError, locale); }

  char badge[64];
  if (connection_text[0] != '\0') {
    snprintf(badge, sizeof(badge), "%s %s", source, connection_text);
  } else {
    snprintf(badge, sizeof(badge), "%s", source);
  }
  lv_label_set_text(ui.status, badge);

  DataState state = DataState::kFresh;
  if (strcmp(connection, "OFFLINE") == 0) { state = DataState::kStale; }
  else if (strcmp(connection, "ERROR") == 0) { state = DataState::kError; }
  else if (strcmp(connection, "CONNECTING") == 0) { state = DataState::kStale; }
  lv_obj_set_style_text_color(ui.status, token_color(colorTokenForState(state)), LV_PART_MAIN);

  if (ui.info_text == nullptr) { return; }
  const uint32_t age_s = status.age_ms / 1000u;
  char http_text[24];
  if (status.http_status > 0) {
    snprintf(http_text, sizeof(http_text), "%d", status.http_status);
  } else {
    snprintf(http_text, sizeof(http_text), "%s", tr(TextKey::kNoResponse, locale));
  }
  char text[640];
  snprintf(text, sizeof(text),
           "%s: %s\n"
           "%s: %s\n"
           "%s: %s\n"
           "%s: %.1f ms\n"
           "%s: %s\n"
           "%s: %u.%u %s\n"
           "%s: %u / %u | %s %d/%d\n"
           "%s: %s",
           tr(TextKey::kSource, locale), source[0] != '\0' ? source : "-",
           tr(TextKey::kEndpoint, locale),
           (status.endpoint != nullptr && status.endpoint[0] != '\0') ? status.endpoint : "-",
           tr(TextKey::kHttp, locale), http_text,
           tr(TextKey::kLatency, locale), (double)status.latency_ms,
           tr(TextKey::kLastUpdate, locale),
           (status.last_update != nullptr) ? status.last_update : "-",
           tr(TextKey::kDataAge, locale), (unsigned)(age_s / 10), (unsigned)(age_s % 10),
           tr(TextKey::kSeconds, locale),
           tr(TextKey::kPolls, locale), (unsigned)status.success_count,
           (unsigned)status.error_count, tr(TextKey::kFields, locale), status.fields_seen,
           status.fields_expected,
           tr(TextKey::kNote, locale),
           (status.note != nullptr && status.note[0] != '\0') ? status.note : "-");
  lv_label_set_text(ui.info_text, text);
}

// History: three thin lines inside a glass card, one per metric, each already
// normalised to its OWN range by the host (pressure keeps its own scale).
// Lane rectangles come from the shared layout, never from local arithmetic.
inline void ui_update_history(Ui& ui, const PresentationModel& model, const HistoryView& view) {
  const DisplayLayout layout = computeLayout(DisplayMode::kHistory, model, ui.geometry);
  const Locale locale = (ui.locale == Locale::kNone) ? Locale::kEn : ui.locale;

  if (!view.available || view.count < 2) {
    lv_label_set_text(ui.history_empty, tr(TextKey::kCollectingHistory, locale));
    lv_obj_remove_flag(ui.history_empty, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i += 1) {
      lv_obj_add_flag(ui.history_lines[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui.history_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }

  char info[96];
  /* ASCII separator only: a middle dot is not in every font subset. */
  snprintf(info, sizeof(info), "%s: %d %s | %s %s", tr(TextKey::kHistory, locale),
           (int)view.count, tr(TextKey::kPoints, locale), view.window_label,
           tr(TextKey::kWindow, locale));
  lv_label_set_text(ui.history_empty, info);
  lv_obj_remove_flag(ui.history_empty, LV_OBJ_FLAG_HIDDEN);
  if (view.window_label != nullptr && view.window_label[0] != '\0') {
    lv_label_set_text_fmt(ui.history_axis_from, "-%s", view.window_label);
  } else {
    lv_label_set_text(ui.history_axis_from, "-");
  }
  lv_label_set_text(ui.history_axis_to, tr(TextKey::kNow, locale));

  static lv_point_precise_t points[3][192];
  const ColorToken tokens[3] = {ColorToken::kTempComfort, ColorToken::kHumidityComfort,
                                ColorToken::kPressureNormal};
  const char* labels[3] = {"Temperature", "Humidity", "Pressure"};

  for (int m = 0; m < 3; m += 1) {
    const float* series = view.series[m];
    const int32_t count = view.series_count[m];
    const LayoutRect lane = layout.history_lane[m];
    int used = 0;
    if (series != nullptr && count > 1 && lane.w > 0 && lane.h > 0) {
      for (int32_t i = 0; i < count && used < 192; i += 1) {
        const float v = series[i];
        if (!(v == v)) { continue; }  /* NaN = gap: the line is broken */
        /* The lane is the data area only: the full lane height carries the
           series, so a wobble is always visible. */
        const float clamped = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
        points[m][used].x = lane.x + (int32_t)((float)lane.w * (float)i / (float)(count - 1));
        points[m][used].y = lane.y + lane.h - (int32_t)((float)lane.h * clamped);
        used += 1;
      }
    }
    if (used >= 2) {
      lv_line_set_points(ui.history_lines[m], points[m], used);
      lv_obj_set_style_line_color(ui.history_lines[m], token_color(tokens[m]), LV_PART_MAIN);
      lv_obj_remove_flag(ui.history_lines[m], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui.history_lines[m], LV_OBJ_FLAG_HIDDEN);
    }
    const TextKey full_keys[3] = {TextKey::kTemperature, TextKey::kHumidity, TextKey::kPressure};
    const TextKey short_keys[3] = {TextKey::kTemperatureShort, TextKey::kHumidityShort,
                                   TextKey::kPressureShort};
    lv_label_set_text(ui.history_labels[m],
                      localized_metric_label(full_keys[m], short_keys[m], locale, font_small(),
                                             layout.history_label[m].w));
    lv_obj_remove_flag(ui.history_labels[m], LV_OBJ_FLAG_HIDDEN);
  }
}

// Rough object accounting for the performance report (walk the tree).
inline uint32_t ui_count_objects(lv_obj_t* obj) {
  uint32_t total = 1;
  const uint32_t children = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < children; i += 1) {
    total += ui_count_objects(lv_obj_get_child(obj, (int32_t)i));
  }
  return total;
}

}  // namespace ws_ui
