#pragma once

// =============================================================================
// i18n.h — shared presentation-layer localization (RU / EN).
//
// APPLICATION STATE, shared by every renderer:
//
//     Locale (kNone | kRu | kEn)          "no language chosen yet" is a real state
//        │
//        ├── Web UI    (web/index.html, JS catalog with the same key names)
//        └── LVGL UI   (include/lvgl_ui.h, tr() from the table below)
//
// Rules this file implements:
//   * locale ids are exactly "ru" and "en" — one spelling everywhere
//     (no "rus"/"eng"/"russian"/"english" variants);
//   * on every start the locale is kNone and the visible screen is the language
//     selection; the dashboard is not shown before a choice is made;
//   * the choice is NOT persisted in this iteration (no NVS/EEPROM/localStorage);
//   * translations are looked up BY KEY, never by `if (ru) "…" else "…"` at the
//     call site;
//   * numbers and units are data, not text: "25.4", "°C", "%", "hPa" are never
//     translated, and semantic role ids (temp-comfort, …) never appear as labels.
//
// The header is dependency-free (no LVGL, no Arduino), so the firmware, the PC
// simulator and any future renderer share exactly one vocabulary.
// =============================================================================

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ locales --
enum class Locale : uint8_t { kNone = 0, kRu, kEn };

constexpr uint8_t kLocaleCount = 2;  // selectable locales (kNone is not one)

inline const char* localeId(Locale locale) {
  switch (locale) {
    case Locale::kRu: return "ru";
    case Locale::kEn: return "en";
    default: return "";
  }
}

/* Parses a locale id ("ru"/"en", case insensitive). Anything else → kNone. */
inline Locale localeFromId(const char* id) {
  if (id == nullptr) { return Locale::kNone; }
  if ((id[0] == 'r' || id[0] == 'R') && (id[1] == 'u' || id[1] == 'U')) { return Locale::kRu; }
  if ((id[0] == 'e' || id[0] == 'E') && (id[1] == 'n' || id[1] == 'N')) { return Locale::kEn; }
  return Locale::kNone;
}

/* The two selectable locales in display order (used by both renderers). */
inline Locale localeAt(uint8_t index) { return (index == 0) ? Locale::kRu : Locale::kEn; }

// -------------------------------------------------------------- app screens --
/* What the application shows. `kLanguageSelection` is the first screen on every
   start; the dashboard becomes reachable only after a locale was chosen. */
enum class AppScreen : uint8_t { kLanguageSelection = 0, kDashboard };

// --------------------------------------------------------- translation keys --
enum class TextKey : uint8_t {
  // language selection screen
  kAppTitle = 0,
  kAppTitleShort,       // compact header form (320x240 header is one line)
  kChooseLanguage,
  kLanguageRu,          // shown in its own language: РУССКИЙ
  kLanguageEn,          // shown in its own language: ENGLISH
  kLanguageHint,

  // display modes (full + compact panel form)
  kTabNumeric,
  kTabHorizontal,
  kTabBars,
  kTabGauges,
  kTabHistory,
  kTabNumericShort,
  kTabHorizontalShort,
  kTabBarsShort,
  kTabGaugesShort,
  kTabHistoryShort,

  // metrics (full + compact panel form)
  kTemperature,
  kTemperatureShort,
  kHumidity,
  kHumidityShort,
  kPressure,
  kPressureShort,

  // data states
  kFresh,
  kStale,
  kError,

  // connection states
  kOnline,
  kOffline,
  kConnecting,

  // data source
  kLive,
  kDemo,

  // status / diagnostics
  kSource,
  kEndpoint,
  kHttp,
  kLatency,
  kLastUpdate,
  kDataAge,
  kPolls,
  kNote,
  kFields,
  kNoResponse,
  kSpan,

  // history
  kHistory,
  kPoints,
  kWindow,
  kNow,
  kCollectingHistory,
  kRetention,
  kClearHistory,
  kSeconds,
  kMinutes,
  kHours,

  kUpdated,
  kAge,
  kScale,

  kTextKeyCount
};

// ------------------------------------------------------------- the catalog ---
struct Translation {
  const char* ru;
  const char* en;
};

/* One table, two locales. `inline` keeps it header-only (C++17) so both targets
   link the same strings — no per-renderer translation copy. */
inline const Translation* catalog() {
  static const Translation table[(size_t)TextKey::kTextKeyCount] = {
      /* kAppTitle            */ {"Погодная станция", "Weather Station"},
      /* kAppTitleShort       */ {"Погода", "Weather"},
      /* kChooseLanguage      */ {"Выберите язык", "Choose a language"},
      /* kLanguageRu          */ {"РУССКИЙ", "РУССКИЙ"},
      /* kLanguageEn          */ {"ENGLISH", "ENGLISH"},
      /* kLanguageHint        */ {"Язык интерфейса можно выбрать заново при следующем запуске",
                                  "The interface language is asked again on every start"},

      /* kTabNumeric          */ {"ЧИСЛА", "NUMERIC"},
      /* kTabHorizontal       */ {"ГОРИЗОНТАЛЬНЫЕ", "HORIZONTAL"},
      /* kTabBars             */ {"СТОЛБЦЫ", "BARS"},
      /* kTabGauges           */ {"ШКАЛЫ", "GAUGES"},
      /* kTabHistory          */ {"ИСТОРИЯ", "HISTORY"},
      /* kTabNumericShort     */ {"ЧИСЛА", "NUM"},
      /* kTabHorizontalShort  */ {"ГОРИЗ", "HOR"},
      /* kTabBarsShort        */ {"СТОЛБ", "BARS"},
      /* kTabGaugesShort      */ {"ШКАЛЫ", "GAUGE"},
      /* kTabHistoryShort     */ {"ИСТОР", "HIST"},

      /* kTemperature         */ {"Температура", "Temperature"},
      /* kTemperatureShort    */ {"Темп", "Temp"},
      /* kHumidity            */ {"Влажность", "Humidity"},
      /* kHumidityShort       */ {"Влажн", "Humid"},
      /* kPressure            */ {"Давление", "Pressure"},
      /* kPressureShort       */ {"Давл", "Press"},

      /* kFresh               */ {"СВЕЖО", "FRESH"},
      /* kStale               */ {"СТАРО", "STALE"},
      /* kError               */ {"ОШИБКА", "ERROR"},

      /* kOnline              */ {"ОНЛАЙН", "ONLINE"},
      /* kOffline             */ {"ОФЛАЙН", "OFFLINE"},
      /* kConnecting          */ {"ПОДКЛЮЧЕНИЕ", "CONNECTING"},

      /* kLive                */ {"LIVE", "LIVE"},
      /* kDemo                */ {"DEMO", "DEMO"},

      /* kSource              */ {"Источник", "Source"},
      /* kEndpoint            */ {"Адрес", "Endpoint"},
      /* kHttp                */ {"HTTP", "HTTP"},
      /* kLatency             */ {"Задержка", "Latency"},
      /* kLastUpdate          */ {"Обновлено", "Last update"},
      /* kDataAge             */ {"Возраст данных", "Data age"},
      /* kPolls               */ {"Опросы", "Polls"},
      /* kNote                */ {"Примечание", "Note"},
      /* kFields              */ {"Поля", "Fields"},
      /* kNoResponse          */ {"нет ответа", "no response"},
      /* kSpan                */ {"с", "s"},

      /* kHistory             */ {"История", "History"},
      /* kPoints              */ {"точек", "points"},
      /* kWindow              */ {"окно", "window"},
      /* kNow                 */ {"сейчас", "now"},
      /* kCollectingHistory   */ {"Собираем историю…", "Collecting history…"},
      /* kRetention           */ {"Хранение", "Retention"},
      /* kClearHistory        */ {"Очистить историю", "Clear history"},
      /* kSeconds             */ {"с", "s"},
      /* kMinutes             */ {"мин", "min"},
      /* kHours               */ {"ч", "h"},

      /* kUpdated             */ {"обновлено", "updated"},
      /* kAge                 */ {"возраст", "age"},
      /* kScale               */ {"шкала", "scale"},
  };
  return table;
}

/* Locale-aware lookup. kNone falls back to EN so a half-initialised UI never
   shows empty labels; the language screen itself uses the native names above. */
inline const char* tr(TextKey key, Locale locale) {
  const size_t index = (size_t)key;
  if (index >= (size_t)TextKey::kTextKeyCount) { return ""; }
  const Translation& entry = catalog()[index];
  return (locale == Locale::kRu) ? entry.ru : entry.en;
}

/* Human-readable retention window for a duration in seconds, e.g.
   RU: "10 с", "1 мин", "24 ч"   EN: "10 s", "1 min", "24 h".
   Uses the catalog's unit words, so it is localized without new keys. */
inline void formatRetention(uint32_t seconds, Locale locale, char* out, size_t size) {
  if (seconds < 60) {
    snprintf(out, size, "%u %s", (unsigned)seconds, tr(TextKey::kSeconds, locale));
  } else if (seconds < 3600) {
    snprintf(out, size, "%u %s", (unsigned)(seconds / 60), tr(TextKey::kMinutes, locale));
  } else {
    snprintf(out, size, "%u %s", (unsigned)(seconds / 3600), tr(TextKey::kHours, locale));
  }
}

/* "10.5 s" / "3 мин" style data age, used by the status overlay and the card
   footers. Localized unit words, numeric value untouched. */
inline void formatAge(uint32_t age_ms, Locale locale, char* out, size_t size) {
  if (age_ms < 60000u) {
    snprintf(out, size, "%.1f %s", (double)age_ms / 1000.0, tr(TextKey::kSeconds, locale));
  } else {
    snprintf(out, size, "%.1f %s", (double)age_ms / 60000.0, tr(TextKey::kMinutes, locale));
  }
}
