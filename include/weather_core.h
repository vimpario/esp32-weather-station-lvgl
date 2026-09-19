#pragma once

// ===========================================================================
// weather_core.h — SHARED display-agnostic core.
//
// Used by BOTH targets, unchanged:
//   * ESP32 firmware  (src/main.cpp: sensors -> WeatherModel -> presentation ->
//     Web API v1 + the optional LVGL UI + layout self-test)
//   * PC simulator    (esp32-weather-station-sim: SDL2 + LVGL, demo data source)
//
// Contains no Arduino, Wi-Fi, HTTP, SDL or LVGL dependency: only C++ types and
// pure functions. This is the single source of truth for
//   * metric semantics: thresholds -> semantic colour ROLES
//   * DATA STATES (FRESH / STALE / ERROR)
//   * presentation model (value / valid / age / role / range / fraction / unit / icon)
//   * display geometry profiles and the adaptive layout engine
//   * display-independent colour tokens and the reference RGB theme
//   * display mode ids (numeric / horizontal / bars / gauges / history)
//
// The Web UI mirrors the thresholds and ranges in JavaScript; the LVGL UI
// (include/lvgl_ui.h) consumes this header directly, so geometry and semantics
// are literally the same code on the PC and on the device.
// ===========================================================================

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"

namespace Roles {
constexpr const char* kTempCold = "temp-cold";
constexpr const char* kTempComfort = "temp-comfort";
constexpr const char* kTempHot = "temp-hot";
constexpr const char* kHumidityLow = "humidity-low";
constexpr const char* kHumidityComfort = "humidity-comfort";
constexpr const char* kHumidityHigh = "humidity-high";
constexpr const char* kPressureLow = "pressure-low";
constexpr const char* kPressureNormal = "pressure-normal";
constexpr const char* kPressureHigh = "pressure-high";
}  // namespace Roles

namespace Semantics {
// Classification thresholds (identical to SEMANTIC_THRESHOLDS in the Web UI).
constexpr float kTempColdBelowC = 18.0f;
constexpr float kTempHotAboveC = 26.0f;
constexpr float kHumidityLowBelowPct = 30.0f;
constexpr float kHumidityHighAbovePct = 60.0f;
constexpr float kPressureLowBelowHpa = 1000.0f;
constexpr float kPressureHighAboveHpa = 1020.0f;

// Display ranges (identical to METRIC_CONFIG.metric.range in the Web UI).
// Pressure keeps a 100 hPa span so 1 hPa is ~1 % of the scale.
constexpr float kTempMinC = -10.0f;
constexpr float kTempMaxC = 40.0f;
constexpr float kHumidityMinPct = 0.0f;
constexpr float kHumidityMaxPct = 100.0f;
constexpr float kPressureMinHpa = 950.0f;
constexpr float kPressureMaxHpa = 1050.0f;

// Freshness windows (identical to APP_CONFIG.freshnessMaxAgeMs in the Web UI):
// BMP280 polls every 5 s, DHT11 every 2.5 s.
constexpr uint32_t kTempMaxAgeMs = 15000;
constexpr uint32_t kHumidityMaxAgeMs = 7500;
constexpr uint32_t kPressureMaxAgeMs = 15000;
}  // namespace Semantics

enum class DataState { kFresh, kStale, kError };

inline const char* dataStateName(DataState state) {
  switch (state) {
    case DataState::kFresh: return "FRESH";
    case DataState::kStale: return "STALE";
    default: return "ERROR";
  }
}

// ===========================================================================
// DISPLAY VOCABULARY — shared by every renderer (Web today, LVGL/ILI9341 next)
// ===========================================================================

// Presentation modes. These ids are the SAME strings the Web UI uses
// (MODES = ['numeric','horizontal','bars','gauges','history']), so a mode is
// application state, not a Web- or panel-specific concept.
enum class DisplayMode : uint8_t { kNumeric, kHorizontal, kBars, kGauges, kHistory };
constexpr uint8_t kDisplayModeCount = 5;

inline const char* displayModeId(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::kHorizontal: return "horizontal";
    case DisplayMode::kBars: return "bars";
    case DisplayMode::kGauges: return "gauges";
    case DisplayMode::kHistory: return "history";
    default: return "numeric";
  }
}

inline DisplayMode displayModeAt(uint8_t index) {
  switch (index % kDisplayModeCount) {
    case 1: return DisplayMode::kHorizontal;
    case 2: return DisplayMode::kBars;
    case 3: return DisplayMode::kGauges;
    case 4: return DisplayMode::kHistory;
    default: return DisplayMode::kNumeric;
  }
}

// Short panel label for the on-screen mode buttons. Latin only, because the
// built-in LVGL fonts carry no Cyrillic glyphs (same reason as label_latin).
inline const char* displayModeShortId(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::kHorizontal: return "HOR";
    case DisplayMode::kBars: return "BARS";
    case DisplayMode::kGauges: return "GAUGE";
    case DisplayMode::kHistory: return "HIST";
    default: return "NUM";
  }
}

// Abstract icon identities. Renderers never see SVG or image data: the Web maps
// them to its inline SVG symbols (i-temp / i-humidity / i-pressure / i-network),
// the LVGL backend will map them to an LVGL image or vector asset.
enum class IconId : uint8_t { kTemperature, kHumidity, kPressure, kNetwork };

inline const char* iconIdName(IconId icon) {
  switch (icon) {
    case IconId::kHumidity: return "humidity";
    case IconId::kPressure: return "pressure";
    case IconId::kNetwork: return "wifi";
    default: return "temperature";
  }
}

// Display-independent semantic colour tokens. No CSS, no RGB values here: the
// theme layer below (and the Web stylesheet) decide how a token actually looks.
enum class ColorToken : uint8_t {
  kTempCold, kTempComfort, kTempHot,
  kHumidityLow, kHumidityComfort, kHumidityHigh,
  kPressureLow, kPressureNormal, kPressureHigh,
  kDataFresh, kDataStale, kDataError
};

inline const char* colorTokenName(ColorToken token) {
  switch (token) {
    case ColorToken::kTempCold: return "TEMP_COLD";
    case ColorToken::kTempComfort: return "TEMP_COMFORT";
    case ColorToken::kTempHot: return "TEMP_HOT";
    case ColorToken::kHumidityLow: return "HUMIDITY_LOW";
    case ColorToken::kHumidityComfort: return "HUMIDITY_COMFORT";
    case ColorToken::kHumidityHigh: return "HUMIDITY_HIGH";
    case ColorToken::kPressureLow: return "PRESSURE_LOW";
    case ColorToken::kPressureNormal: return "PRESSURE_NORMAL";
    case ColorToken::kPressureHigh: return "PRESSURE_HIGH";
    case ColorToken::kDataFresh: return "DATA_FRESH";
    case ColorToken::kDataStale: return "DATA_STALE";
    default: return "DATA_ERROR";
  }
}

inline ColorToken colorTokenForRole(const char* role) {
  if (role == nullptr) { return ColorToken::kDataError; }
  if (strcmp(role, Roles::kTempCold) == 0) { return ColorToken::kTempCold; }
  if (strcmp(role, Roles::kTempComfort) == 0) { return ColorToken::kTempComfort; }
  if (strcmp(role, Roles::kTempHot) == 0) { return ColorToken::kTempHot; }
  if (strcmp(role, Roles::kHumidityLow) == 0) { return ColorToken::kHumidityLow; }
  if (strcmp(role, Roles::kHumidityComfort) == 0) { return ColorToken::kHumidityComfort; }
  if (strcmp(role, Roles::kHumidityHigh) == 0) { return ColorToken::kHumidityHigh; }
  if (strcmp(role, Roles::kPressureLow) == 0) { return ColorToken::kPressureLow; }
  if (strcmp(role, Roles::kPressureNormal) == 0) { return ColorToken::kPressureNormal; }
  if (strcmp(role, Roles::kPressureHigh) == 0) { return ColorToken::kPressureHigh; }
  return ColorToken::kDataError;
}

inline ColorToken colorTokenForState(DataState state) {
  switch (state) {
    case DataState::kFresh: return ColorToken::kDataFresh;
    case DataState::kStale: return ColorToken::kDataStale;
    default: return ColorToken::kDataError;
  }
}

// Theme layer: token -> RGB. This is the ONLY place a pixel colour is decided;
// the values mirror the Web stylesheet's --role-* / --state-* variables so both
// renderers show the same semantics. The Web keeps its own CSS mapping.
enum class ThemeId : uint8_t { kReferenceDark };

struct ThemeEntry {
  ColorToken token;
  uint32_t rgb;
};

constexpr ThemeEntry kReferenceTheme[] = {
    {ColorToken::kTempCold, 0x4C9BFF},          {ColorToken::kTempComfort, 0x43D19E},
    {ColorToken::kTempHot, 0xFF6B5E},           {ColorToken::kHumidityLow, 0xFFC14D},
    {ColorToken::kHumidityComfort, 0x2FD3C0},   {ColorToken::kHumidityHigh, 0x9B7BFF},
    {ColorToken::kPressureLow, 0x7F8CFF},       {ColorToken::kPressureNormal, 0x43D19E},
    {ColorToken::kPressureHigh, 0xFFA04D},      {ColorToken::kDataFresh, 0x43D19E},
    {ColorToken::kDataStale, 0xFFC14D},         {ColorToken::kDataError, 0xFF6B5E},
};

inline uint32_t themeRgb(ThemeId theme, ColorToken token) {
  (void)theme;  // one theme today; a light theme would be a second table
  for (size_t i = 0; i < sizeof(kReferenceTheme) / sizeof(kReferenceTheme[0]); ++i) {
    if (kReferenceTheme[i].token == token) { return kReferenceTheme[i].rgb; }
  }
  return 0xFFFFFFFF;
}

// Display geometry profiles. Renderers receive the geometry and derive every
// rectangle from it, so nothing is designed for one panel size only.
struct DisplayGeometry {
  uint16_t width;
  uint16_t height;
  const char* profile;
};

constexpr DisplayGeometry kGeometry320x240{320, 240, "320x240"};
constexpr DisplayGeometry kGeometry480x320{480, 320, "480x320"};
constexpr DisplayGeometry kGeometry800x480{800, 480, "800x480"};
constexpr DisplayGeometry kGeometryDesktop{1280, 800, "desktop"};

// Adaptive layout constants (fractions of the geometry, never absolute pixels).
namespace Layout {
constexpr float kMarginOfShortSide = 0.035f;
constexpr float kGutterOfShortSide = 0.02f;
constexpr float kHeaderOfHeight = 0.13f;
constexpr float kNavOfHeight = 0.095f;
constexpr int kMinMarginPx = 4;
constexpr int kMinGutterPx = 4;
constexpr int kMinHeaderPx = 18;
constexpr int kMinNavPx = 18;
constexpr int kMaxNavPx = 40;
constexpr int kMinCardPx = 24;
// Circular scales sweep 270° (gap at the bottom) — gauge geometry, not a screen size.
constexpr float kGaugeSweepDeg = 270.0f;
}  // namespace Layout

struct MetricValue {
  const char* source = "";  // "BMP280" / "DHT11"
  bool available = false;   // at least one successful read since boot
  bool valid = false;       // the most recent read succeeded
  float value = NAN;        // display unit: °C / % / hPa
  uint32_t age_ms = 0;      // age of the last successful read
};

struct WeatherModel {
  MetricValue temperature_c;  // BMP280
  MetricValue humidity_pct;   // DHT11
  MetricValue pressure_hpa;   // BMP280 (stored in hPa; the raw Pa value is derived)
  uint32_t uptime_ms = 0;
};

struct MetricPresentation {
  const char* role;      // semantic role id, nullptr when there is no value
  DataState state;
  ColorToken color_token;
  const char* label;     // display label (the Web UI uses localized labels)
  /* Latin label for panel renderers: LVGL's built-in Montserrat fonts carry no
   * Cyrillic glyphs, so the panel uses Latin until a Cyrillic font asset is
   * added (see README "Typography"). */
  const char* label_latin;
  const char* unit;
  const char* source;
  IconId icon;
  uint8_t decimals;
  bool available;        // at least one successful read since boot
  bool valid;            // the most recent read succeeded
  bool has_value;
  float value;           // display unit (NAN when !has_value)
  uint32_t age_ms;
  float range_min;
  float range_max;
  float fraction;        // 0..1 position inside the range (NAN when !has_value)
};

struct PresentationModel {
  MetricPresentation temperature;
  MetricPresentation humidity;
  MetricPresentation pressure;
  DataState worst_state;
  uint32_t uptime_ms;
};

inline DataState classifyMetric(const MetricValue& metric, uint32_t max_age_ms) {
  if (!metric.available || isnan(metric.value)) {
    return DataState::kError;  // never measured — never reported as 0
  }
  if (!metric.valid) {
    return DataState::kStale;  // last known value, unconfirmed
  }
  if (metric.age_ms > max_age_ms) {
    return DataState::kStale;  // valid flag, but too old to trust
  }
  return DataState::kFresh;
}

inline float rangeFraction(float value, float range_min, float range_max) {
  if (isnan(value) || range_max <= range_min) {
    return NAN;
  }
  const float fraction = (value - range_min) / (range_max - range_min);
  return fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
}

inline PresentationModel buildPresentationModel(const WeatherModel& model) {
  PresentationModel presentation;
  presentation.uptime_ms = model.uptime_ms;

  // --- temperature -------------------------------------------------------
  presentation.temperature.state =
      classifyMetric(model.temperature_c, Semantics::kTempMaxAgeMs);
  presentation.temperature.range_min = Semantics::kTempMinC;
  presentation.temperature.range_max = Semantics::kTempMaxC;
  presentation.temperature.label = "Температура";
  presentation.temperature.label_latin = "Temperature";
  presentation.temperature.unit = "\xC2\xB0""C";  // °C
  presentation.temperature.source = model.temperature_c.source;
  presentation.temperature.icon = IconId::kTemperature;
  presentation.temperature.decimals = 1;
  presentation.temperature.available = model.temperature_c.available;
  presentation.temperature.valid = model.temperature_c.valid;
  presentation.temperature.has_value = (presentation.temperature.state != DataState::kError);
  presentation.temperature.value =
      presentation.temperature.has_value ? model.temperature_c.value : NAN;
  presentation.temperature.age_ms = model.temperature_c.age_ms;
  presentation.temperature.fraction =
      rangeFraction(presentation.temperature.value, Semantics::kTempMinC, Semantics::kTempMaxC);
  presentation.temperature.role = nullptr;
  if (presentation.temperature.has_value) {
    presentation.temperature.role =
        model.temperature_c.value < Semantics::kTempColdBelowC ? Roles::kTempCold
        : model.temperature_c.value > Semantics::kTempHotAboveC ? Roles::kTempHot
                                                                : Roles::kTempComfort;
  }
  presentation.temperature.color_token =
      colorTokenForRole(presentation.temperature.role);

  // --- humidity ----------------------------------------------------------
  presentation.humidity.state =
      classifyMetric(model.humidity_pct, Semantics::kHumidityMaxAgeMs);
  presentation.humidity.range_min = Semantics::kHumidityMinPct;
  presentation.humidity.range_max = Semantics::kHumidityMaxPct;
  presentation.humidity.label = "Влажность";
  presentation.humidity.label_latin = "Humidity";
  presentation.humidity.unit = "%";
  presentation.humidity.source = model.humidity_pct.source;
  presentation.humidity.icon = IconId::kHumidity;
  presentation.humidity.decimals = 0;
  presentation.humidity.available = model.humidity_pct.available;
  presentation.humidity.valid = model.humidity_pct.valid;
  presentation.humidity.has_value = (presentation.humidity.state != DataState::kError);
  presentation.humidity.value = presentation.humidity.has_value ? model.humidity_pct.value : NAN;
  presentation.humidity.age_ms = model.humidity_pct.age_ms;
  presentation.humidity.fraction = rangeFraction(presentation.humidity.value,
                                                 Semantics::kHumidityMinPct,
                                                 Semantics::kHumidityMaxPct);
  presentation.humidity.role = nullptr;
  if (presentation.humidity.has_value) {
    presentation.humidity.role =
        model.humidity_pct.value < Semantics::kHumidityLowBelowPct ? Roles::kHumidityLow
        : model.humidity_pct.value > Semantics::kHumidityHighAbovePct ? Roles::kHumidityHigh
                                                                     : Roles::kHumidityComfort;
  }
  presentation.humidity.color_token = colorTokenForRole(presentation.humidity.role);

  // --- pressure ----------------------------------------------------------
  presentation.pressure.state =
      classifyMetric(model.pressure_hpa, Semantics::kPressureMaxAgeMs);
  presentation.pressure.range_min = Semantics::kPressureMinHpa;
  presentation.pressure.range_max = Semantics::kPressureMaxHpa;
  presentation.pressure.label = "Давление";
  presentation.pressure.label_latin = "Pressure";
  presentation.pressure.unit = "hPa";
  presentation.pressure.source = model.pressure_hpa.source;
  presentation.pressure.icon = IconId::kPressure;
  presentation.pressure.decimals = 1;
  presentation.pressure.available = model.pressure_hpa.available;
  presentation.pressure.valid = model.pressure_hpa.valid;
  presentation.pressure.has_value = (presentation.pressure.state != DataState::kError);
  presentation.pressure.value = presentation.pressure.has_value ? model.pressure_hpa.value : NAN;
  presentation.pressure.age_ms = model.pressure_hpa.age_ms;
  presentation.pressure.fraction = rangeFraction(presentation.pressure.value,
                                                 Semantics::kPressureMinHpa,
                                                 Semantics::kPressureMaxHpa);
  presentation.pressure.role = nullptr;
  if (presentation.pressure.has_value) {
    presentation.pressure.role =
        model.pressure_hpa.value < Semantics::kPressureLowBelowHpa ? Roles::kPressureLow
        : model.pressure_hpa.value > Semantics::kPressureHighAboveHpa ? Roles::kPressureHigh
                                                                     : Roles::kPressureNormal;
  }
  presentation.pressure.color_token = colorTokenForRole(presentation.pressure.role);

  // Worst state of the three drives the aggregate indicator.
  presentation.worst_state = DataState::kFresh;
  const MetricPresentation* metrics[] = {&presentation.temperature, &presentation.humidity,
                                         &presentation.pressure};
  for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
    if (metrics[i]->state == DataState::kError) {
      presentation.worst_state = DataState::kError;
      break;
    }
    if (metrics[i]->state == DataState::kStale) {
      presentation.worst_state = DataState::kStale;
    }
  }

  return presentation;
}

struct LayoutRect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

inline int scaledPx(float fraction, int total, int minimum) {
  const int value = static_cast<int>(fraction * static_cast<float>(total) + 0.5f);
  return value < minimum ? minimum : value;
}

inline LayoutRect makeRect(int x, int y, int w, int h) {
  return LayoutRect{static_cast<int16_t>(x), static_cast<int16_t>(y),
                    static_cast<int16_t>(w < 0 ? 0 : w), static_cast<int16_t>(h < 0 ? 0 : h)};
}

struct MetricLayout {
  LayoutRect card;
  LayoutRect icon;
  LayoutRect label;
  LayoutRect value;
  LayoutRect unit;
  LayoutRect state;
  LayoutRect track;
  LayoutRect gauge;
  float fill;       // NAN when there is no value
  float sweep_deg;  // gauge sweep for this metric
};

struct DisplayLayout {
  DisplayMode mode;
  DisplayGeometry geometry;
  LayoutRect header;
  /* Mode selector strip under the header. It is part of every mode's layout so
     the LVGL UI can render it once and the panel/PC geometry never diverges. */
  LayoutRect nav;
  LayoutRect nav_button[kDisplayModeCount];
  MetricLayout metrics[3];
  uint8_t metric_count;
  // History mode (shared by the LVGL panel and any future renderer): one panel,
  // a title strip, one lane per metric and an X-axis band. Geometry only — the
  // renderer never invents coordinates of its own.
  LayoutRect history_panel;
  LayoutRect history_title;
  /* One label column per lane plus the lane's data area, so a series line never
     runs through its own label. */
  LayoutRect history_label[3];
  LayoutRect history_lane[3];
  LayoutRect history_axis;
  /* Diagnostics overlay ("status/debug panel"): a centred glass panel over the
     content area. Layout-owned like everything else, so both targets place it
     identically. */
  LayoutRect info;
  LayoutRect info_title;
  LayoutRect info_text;
  /* Language-selection screen (the first screen on every start): a centred glass
     panel with a title, an instruction, one button per locale and a hint. */
  LayoutRect lang_panel;
  LayoutRect lang_title;
  LayoutRect lang_instruction;
  LayoutRect lang_button[kLocaleCount];
  LayoutRect lang_hint;
};

inline const MetricPresentation* presentationMetricAt(const PresentationModel& model, uint8_t index) {
  switch (index) {
    case 0: return &model.temperature;
    case 1: return &model.humidity;
    default: return &model.pressure;
  }
}

inline float gaugeSweepFor(float fraction) {
  return isnan(fraction) ? 0.0f : fraction * Layout::kGaugeSweepDeg;
}

// Every rectangle is derived from the geometry: no panel size is hardcoded.
inline DisplayLayout computeLayout(DisplayMode mode, const PresentationModel& model,
                            const DisplayGeometry& geometry) {
  DisplayLayout layout;
  layout.mode = mode;
  layout.geometry = geometry;
  layout.metric_count = 3;

  const int width = static_cast<int>(geometry.width);
  const int height = static_cast<int>(geometry.height);
  const int short_side = width < height ? width : height;
  const int margin = scaledPx(Layout::kMarginOfShortSide, short_side, Layout::kMinMarginPx);
  const int gutter = scaledPx(Layout::kGutterOfShortSide, short_side, Layout::kMinGutterPx);
  const int header_h = scaledPx(Layout::kHeaderOfHeight, height, Layout::kMinHeaderPx);
  int nav_h = scaledPx(Layout::kNavOfHeight, height, Layout::kMinNavPx);
  if (nav_h > Layout::kMaxNavPx) { nav_h = Layout::kMaxNavPx; }

  const int content_x = margin;
  const int nav_y = margin + header_h + gutter;
  const int content_y = nav_y + nav_h + gutter;
  const int content_w = width - 2 * margin;
  const int content_h = height - content_y - margin;

  layout.header = makeRect(content_x, margin, content_w, header_h);
  {
    /* Language-selection screen: a centred panel that owns the whole screen; the
       dashboard is not visible while it is up. Two stacked buttons, each at
       least a comfortable touch target, sized from the geometry. */
    const int panel_w = content_w - 2 * (content_w / 10);
    const int panel_x = content_x + (content_w - panel_w) / 2;
    const int panel_h = height - 2 * (height / 8);
    const int panel_y = (height - panel_h) / 2;
    layout.lang_panel = makeRect(panel_x, panel_y, panel_w, panel_h);
    layout.lang_title = makeRect(panel_x + gutter, panel_y + gutter, panel_w - 2 * gutter,
                                 panel_h / 6);
    layout.lang_instruction = makeRect(panel_x + gutter, panel_y + (panel_h * 22) / 100,
                                       panel_w - 2 * gutter, panel_h / 9);
    layout.lang_hint = makeRect(panel_x + gutter, panel_y + (panel_h * 34) / 100,
                                panel_w - 2 * gutter, panel_h / 6);
    const int button_gap = gutter;
    const int button_h = scaledPx(0.15f, height, 26);
    const int buttons_top = panel_y + (panel_h * 52) / 100;
    layout.lang_button[0] = makeRect(panel_x + gutter * 2, buttons_top, panel_w - 4 * gutter,
                                     button_h);
    layout.lang_button[1] = makeRect(panel_x + gutter * 2, buttons_top + button_h + button_gap,
                                     panel_w - 4 * gutter, button_h);
  }
  layout.nav = makeRect(content_x, nav_y, content_w, nav_h);
  {
    /* Equal segments: the last one absorbs the rounding remainder so the strip
       always ends exactly at the content's right edge. */
    const int segment = content_w / (int)kDisplayModeCount;
    for (uint8_t i = 0; i < kDisplayModeCount; ++i) {
      const int x = content_x + (int)i * segment;
      const int w = (i + 1 == kDisplayModeCount) ? (content_x + content_w - x) : (segment - gutter / 2);
      layout.nav_button[i] = makeRect(x, nav_y, w, nav_h);
    }
  }
  layout.history_panel = makeRect(content_x, content_y, content_w, content_h);
  {
    /* Diagnostics overlay: starts right below the mode strip (so switching modes
       stays possible while it is open) and covers the rest of the screen, which
       is what lets the eight status lines fit at 320x240 without shrinking the
       font. */
    const int info_x = content_x + content_w / 24;
    const int info_w = content_w - 2 * (content_w / 24);
    const int info_y = nav_y + nav_h;
    const int info_h = height - info_y - margin / 2;
    layout.info = makeRect(info_x, info_y, info_w, info_h);
    layout.info_title = makeRect(info_x + gutter, info_y + 2, info_w - 2 * gutter, info_h / 8);
    layout.info_text = makeRect(info_x + gutter, info_y + info_h / 8 + 2, info_w - 2 * gutter,
                                info_h - info_h / 8 - 4);
  }
  layout.history_title = makeRect(content_x + gutter, content_y + gutter, content_w - 2 * gutter,
                                  content_h / 8);
  layout.history_axis = makeRect(content_x + gutter, content_y + content_h - content_h / 8,
                                 content_w - 2 * gutter, content_h / 8 - gutter);
  {
    const int lanes_top = content_y + content_h / 8 + gutter;
    const int lanes_h = (content_h - content_h / 8 - gutter) - (content_h / 8);
    const int lane_h = lanes_h / 3;
    const int label_w = content_w / 4;
    for (uint8_t i = 0; i < 3; ++i) {
      layout.history_label[i] = makeRect(content_x + gutter, lanes_top + i * lane_h, label_w,
                                         lane_h - 2);
      layout.history_lane[i] = makeRect(content_x + gutter + label_w + 4,
                                        lanes_top + i * lane_h + 4,
                                        content_w - 2 * gutter - label_w - 4, lane_h - 2 - 8);
    }
  }

  for (uint8_t i = 0; i < 3; ++i) {
    const MetricPresentation& metric = *presentationMetricAt(model, i);
    layout.metrics[i].fill = metric.fraction;
    layout.metrics[i].sweep_deg = gaugeSweepFor(metric.fraction);
    layout.metrics[i].card = makeRect(content_x, content_y, 0, 0);
    layout.metrics[i].icon = makeRect(content_x, content_y, 0, 0);
    layout.metrics[i].label = makeRect(content_x, content_y, 0, 0);
    layout.metrics[i].value = makeRect(content_x, content_y, 0, 0);
    layout.metrics[i].unit = makeRect(content_x, content_y, 0, 0);
    layout.metrics[i].state = makeRect(content_x, content_y, 0, 0);
    layout.metrics[i].track = makeRect(content_x, content_y, 0, 0);
    layout.metrics[i].gauge = makeRect(content_x, content_y, 0, 0);
  }

  const bool wide_enough_for_columns = (content_w >= content_h);

  if (mode == DisplayMode::kNumeric) {
    // Large value cards: a row of three on landscape screens, a single stacked
    // column on portrait/small ones — decided from the geometry, not from a size.
    // Rows are vertical fractions of the card so nothing overlaps on any screen.
    const int columns = wide_enough_for_columns ? 3 : 1;
    const int rows = 3 / columns;
    const int cell_w = (content_w - (columns - 1) * gutter) / columns;
    const int cell_h = (content_h - (rows - 1) * gutter) / rows;
    for (uint8_t i = 0; i < 3; ++i) {
      const int column = (columns == 3) ? i : 0;
      const int row = (columns == 3) ? 0 : static_cast<int>(i);
      const int x = content_x + column * (cell_w + gutter);
      const int y = content_y + row * (cell_h + gutter);
      MetricLayout& cell = layout.metrics[i];
      cell.card = makeRect(x, y, cell_w, cell_h);
      cell.icon = makeRect(x + (cell_w * 62) / 100, y + cell_h / 20, (cell_w * 32) / 100,
                           cell_h / 6);
      cell.label = makeRect(x + gutter, y + cell_h / 20, (cell_w * 58) / 100, cell_h / 7);
      cell.value = makeRect(x + gutter, y + (cell_h * 26) / 100, cell_w - 2 * gutter,
                            (cell_h * 26) / 100);
      cell.unit = makeRect(x + gutter, y + (cell_h * 54) / 100, cell_w - 2 * gutter, cell_h / 9);
      cell.state = makeRect(x + gutter, y + (cell_h * 82) / 100, cell_w - 2 * gutter, cell_h / 9);
    }
  } else if (mode == DisplayMode::kHorizontal) {
    // Three horizontal scales, one per metric: header row + track below.
    const int row_h = (content_h - 2 * gutter) / 3;
    for (uint8_t i = 0; i < 3; ++i) {
      const int y = content_y + static_cast<int>(i) * (row_h + gutter);
      MetricLayout& cell = layout.metrics[i];
      cell.card = makeRect(content_x, y, content_w, row_h);
      /* The header band must be tall enough for the largest value font
         (Montserrat 28 renders 32 px): a shorter box clips the digits. */
      const int head_h = (row_h * 58) / 100;
      cell.icon = makeRect(content_x + gutter, y + gutter, head_h / 2, head_h / 2);
      cell.label = makeRect(cell.icon.x + cell.icon.w + gutter, y + gutter, (content_w * 26) / 100,
                            head_h / 2);
      cell.value = makeRect(content_x + (content_w * 42) / 100, y + gutter / 2, (content_w * 30) / 100,
                            head_h);
      cell.unit = makeRect(content_x + (content_w * 73) / 100, y + (head_h * 52) / 100,
                           (content_w * 25) / 100, (head_h * 46) / 100);
      cell.state = makeRect(content_x + gutter, y + (row_h * 56) / 100, (content_w * 22) / 100,
                            (row_h * 30) / 100);
      cell.track = makeRect(content_x + (content_w * 26) / 100, y + (row_h * 60) / 100,
                            (content_w * 71) / 100, (row_h * 26) / 100);
    }
  } else if (mode == DisplayMode::kBars) {
    // Three columns, each with its own vertical scale. The text rows are anchored
    // to the BOTTOM of the card and sized from the card height, so a short card
    // (320x240 with the mode strip) can never clip the value or the state badge.
    const int cell_w = (content_w - 2 * gutter) / 3;
    const int state_h = content_h / 8;
    const int unit_h = content_h / 9;
    const int value_h = content_h / 6;
    for (uint8_t i = 0; i < 3; ++i) {
      const int x = content_x + static_cast<int>(i) * (cell_w + gutter);
      MetricLayout& cell = layout.metrics[i];
      cell.card = makeRect(x, content_y, cell_w, content_h);
      cell.icon = makeRect(x + gutter, content_y + gutter, content_h / 10, content_h / 10);
      cell.label = makeRect(cell.icon.x + cell.icon.w + 2, content_y + gutter,
                            cell_w - 2 * gutter - cell.icon.w - 2, content_h / 10);
      const int state_y = content_y + content_h - state_h;
      const int unit_y = state_y - unit_h;
      const int value_y = unit_y - value_h;
      cell.value = makeRect(x + gutter, value_y, cell_w - 2 * gutter, value_h - 2);
      cell.unit = makeRect(x + gutter, unit_y, cell_w - 2 * gutter, unit_h - 2);
      cell.state = makeRect(x + gutter, state_y, cell_w - 2 * gutter, state_h - 2);
      const int track_top = content_y + (content_h * 26) / 100;
      const int track_bottom = value_y - gutter;
      cell.track = makeRect(x + (cell_w * 18) / 100, track_top, (cell_w * 64) / 100,
                            track_bottom - track_top);
    }
  } else {
    // Three circular scales: the value lives inside the ring, labels below it.
    const int cell_w = (content_w - 2 * gutter) / 3;
    const int cell_h = content_h;
    /* Keep the ring clear of the card border and its rounded corners: the ring
       stroke is centred on the arc, so a zero inset would be clipped. */
    const int inset = cell_w / 16;
    const int square = (cell_w < cell_h ? cell_w : cell_h) - 2 * inset;
    for (uint8_t i = 0; i < 3; ++i) {
      const int x = content_x + static_cast<int>(i) * (cell_w + gutter);
      MetricLayout& cell = layout.metrics[i];
      cell.card = makeRect(x, content_y, cell_w, cell_h);
      cell.gauge = makeRect(x + (cell_w - square) / 2, content_y + inset, square, square);
      cell.icon = makeRect(cell.gauge.x + (square * 39) / 100, cell.gauge.y + (square * 17) / 100,
                           (square * 22) / 100, (square * 22) / 100);
      cell.value = makeRect(cell.gauge.x, cell.gauge.y + (square * 40) / 100, square,
                            (square * 32) / 100);
      cell.unit = makeRect(cell.gauge.x, cell.gauge.y + (square * 72) / 100, square,
                           (square * 18) / 100);
      cell.label = makeRect(x + gutter, cell.gauge.y + square + 2, cell_w - 2 * gutter,
                            (content_h * 9) / 100);
      cell.state = makeRect(x + gutter, cell.gauge.y + square + 4 + (content_h * 9) / 100,
                            cell_w - 2 * gutter, (content_h * 8) / 100);
      cell.track = makeRect(x, content_y, 0, 0);  // not used in gauge mode
    }
  }

  return layout;
}

// Invariant used by the headless self-test: nothing may fall outside the screen.
inline bool layoutFitsScreen(const DisplayLayout& layout) {
  const int width = static_cast<int>(layout.geometry.width);
  const int height = static_cast<int>(layout.geometry.height);
  const LayoutRect rects[] = {
      layout.header,
      layout.nav,
      layout.nav_button[0], layout.nav_button[1], layout.nav_button[2],
      layout.nav_button[3], layout.nav_button[4],
      layout.metrics[0].card,  layout.metrics[0].icon,  layout.metrics[0].label,
      layout.metrics[0].value, layout.metrics[0].unit,  layout.metrics[0].state,
      layout.metrics[0].track, layout.metrics[0].gauge, layout.metrics[1].card,
      layout.metrics[1].icon,  layout.metrics[1].label, layout.metrics[1].value,
      layout.metrics[1].unit,  layout.metrics[1].state, layout.metrics[1].track,
      layout.metrics[1].gauge, layout.metrics[2].card,  layout.metrics[2].icon,
      layout.metrics[2].label, layout.metrics[2].value, layout.metrics[2].unit,
      layout.metrics[2].state, layout.metrics[2].track, layout.metrics[2].gauge,
      layout.history_panel,   layout.history_title,
      layout.history_label[0], layout.history_label[1], layout.history_label[2],
      layout.history_lane[0], layout.history_lane[1],   layout.history_lane[2],
      layout.info,            layout.info_title,        layout.info_text,
      layout.lang_panel,      layout.lang_title,        layout.lang_instruction,
      layout.lang_button[0],  layout.lang_button[1],    layout.lang_hint,
      layout.history_axis,
  };
  for (size_t i = 0; i < sizeof(rects) / sizeof(rects[0]); ++i) {
    const LayoutRect& rect = rects[i];
    if (rect.x < 0 || rect.y < 0 || rect.w < 0 || rect.h < 0) { return false; }
    if (static_cast<int>(rect.x) + static_cast<int>(rect.w) > width) { return false; }
    if (static_cast<int>(rect.y) + static_cast<int>(rect.h) > height) { return false; }
  }
  return true;
}

// ---- display backend: the only layer that paints --------------------------

inline void formatMetricText(const MetricPresentation& metric, char* buffer, size_t size) {
  if (!metric.has_value || isnan(metric.value)) {
    snprintf(buffer, size, "--");  // missing data is never rendered as 0
    return;
  }
  snprintf(buffer, size, "%.*f", static_cast<int>(metric.decimals),
           static_cast<double>(metric.value));
}

inline const char* compactUnitFor(IconId icon) {
  switch (icon) {
    case IconId::kHumidity: return "%";
    case IconId::kPressure: return "hPa";
    default: return "C";
  }
}
