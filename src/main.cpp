// ESP32 Weather Station — firmware.
//
// Iterations implemented so far:
//   01 sensors + Wi-Fi smoke test · 05 HTTP API · 06 autonomous Web UI served
//   from flash · 07 network reachability diagnostics · 08 four Web UI modes ·
//   09 stabilisation: logging modes, shared WeatherModel, display-agnostic
//   presentation boundary, API contract v1.
//
// ---------------------------------------------------------------------------
// ARCHITECTURE BOUNDARY (display-agnostic — the LVGL/ILI9341 seam)
// ---------------------------------------------------------------------------
//   Hardware
//     ├─ BMP280 (I²C, addr 0x76)          DHT11 (GPIO4)
//          ↓                                     ↓
//   Sensor layer      Bmp280Reading            Dht11Reading      (raw, per sensor)
//          └──────────────┬──────────────────────┘
//                         ↓
//   WeatherModel      temperature_c (BMP280) · humidity_pct (DHT11) ·
//                     pressure_pa (BMP280) · valid · age_ms · uptime_ms
//                     One quantity per metric, sources never mixed.
//                         ↓
//   PresentationModel semantic role ids + FRESH/STALE/ERROR + display ranges
//                     (thresholds and ranges live in one config block)
//                    ┌────┴─────────────────────┐
//                    ↓                          ↓
//   WebRenderer                 LVGLRenderer (future)
//     JSON v1 over HTTP /          consumes PresentationModel directly;
//     browser-side presentation    no CSS, no HTML, no sensor access
//
// Rule for every renderer: read the model, never the sensors. This is what makes
// the same data usable by the browser, by LVGL and by an ILI9341 panel without
// duplicated business logic.
//
// ---------------------------------------------------------------------------
// LOGGING MODES (WS_DEBUG_LOGS in platformio.ini)
// ---------------------------------------------------------------------------
//   NORMAL (0, default): boot/init, Wi-Fi connect & disconnect, contract
//     verification, gateway probe, HTTP start/stop, self-test failures, every
//     [ERROR], and the periodic line ONLY when something is wrong.
//   DEBUG (1): additionally every [*][DEBUG] line, the 5 s Wi-Fi/HTTP snapshots
//     and the per-request HTTP/API logs.
//   Tags are unchanged either way:
//     [BOOT] [SENSOR] [SENSOR][BMP280] [SENSOR][DHT11] [WIFI] [WIFI][DEBUG]
//     [HTTP] [HTTP][DEBUG] [API] [ERROR]
//
// Fixed hardware contract (do not change without an explicit requirement):
//   BMP280: VCC 3.3V, GND GND, SCL GPIO22, SDA GPIO21, CSB 3.3V, SDO GND -> addr 0x76
//   DHT11 : + 3.3V, OUT GPIO4, - GND
//
// Network contract:
//   SSID Beeline_2G_F121F9, static IPv4 192.168.1.111/24, gw/DNS1 192.168.1.1, DNS2 1.1.1.1
//   Credentials live only in the untracked include/secrets.h.

#include <Arduino.h>
#include <DHTesp.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>
#include <stdarg.h>

#include <Adafruit_BMP280.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h — copy include/secrets.example.h and fill in the real values."
#endif

// Shared, display-agnostic core: models, semantics, presentation, geometry and
// the layout engine. The very same header is compiled by the PC simulator.
#include "weather_core.h"

#ifndef WIFI_SSID
#error "include/secrets.h must define WIFI_SSID."
#endif
#ifndef WIFI_PASSWORD
#error "include/secrets.h must define WIFI_PASSWORD."
#endif
#if !defined(WIFI_STATIC_IP) || !defined(WIFI_GATEWAY) || !defined(WIFI_SUBNET) || \
    !defined(WIFI_DNS1) || !defined(WIFI_DNS2)
#error "include/secrets.h must define the static IPv4 contract (IP/gateway/subnet/DNS1/DNS2)."
#endif

// ---------------------------------------------------------------------------
// Pin map — single source of truth (README section 1).
// GPIO21/22 are the project I2C bus, GPIO4 is reserved for DHT11.
// ---------------------------------------------------------------------------
namespace Pins {
constexpr uint8_t kI2cSda = 21;    // BMP280 SDA
constexpr uint8_t kI2cScl = 22;    // BMP280 SCL
constexpr uint8_t kDht11Data = 4;  // DHT11 OUT
}  // namespace Pins

// ---------------------------------------------------------------------------
// Configuration (thresholds/intervals stay configurable, not hardcoded in logic)
// ---------------------------------------------------------------------------
namespace Config {
constexpr uint32_t kSerialBaud = 115200;
constexpr uint8_t kBmp280Address = 0x76;  // SDO tied to GND selects 0x76
constexpr uint32_t kI2cClockHz = 100000;
constexpr uint32_t kBmp280PollMs = 5000;
constexpr uint32_t kDht11PollMs = 2500;  // DHT11 datasheet: >= 1 s between reads
constexpr uint32_t kWifiConnectTimeoutMs = 15000;  // never wait forever
constexpr uint32_t kWifiRetryMs = 10000;
constexpr uint32_t kDiagReportMs = 5000;
constexpr uint16_t kHttpPort = 80;
// Iteration 07 — network reachability diagnostics. Every probe is bounded so the
// sensor schedule is never blocked for long.
constexpr uint32_t kGatewayProbeTimeoutMs = 400;  // per TCP probe to the gateway
constexpr uint32_t kHttpSelfTestTimeoutMs = 700;  // end-to-end check of the listening socket
// Display refresh cadence for the (optional) panel / headless renderer. It is
// independent of the sensor schedule and of the Web UI polling.
constexpr uint32_t kDisplayUpdateMs = 5000;
}  // namespace Config

// ---------------------------------------------------------------------------
// Logging modes (see the block at the top). WS_DEBUG_LOGS comes from
// platformio.ini build_flags; 0 = NORMAL, 1 = DEBUG.
// ---------------------------------------------------------------------------
#ifndef WS_DEBUG_LOGS
#define WS_DEBUG_LOGS 0
#endif

#if WS_DEBUG_LOGS
#define WS_LOG_DEBUG(...) Serial.printf(__VA_ARGS__)
#else
#define WS_LOG_DEBUG(...) ((void)0)
#endif

// True in DEBUG builds. Used for blocks that must keep running (state updates)
// but should only print in DEBUG; the compiler folds the branch away in NORMAL.
constexpr bool kDebugLogs = (WS_DEBUG_LOGS != 0);

// ---------------------------------------------------------------------------
// Shared semantic vocabulary and display configuration.
//
// These values are the single firmware-side source of truth for classification
// and for scale geometry. web/index.html mirrors them (SEMANTIC_THRESHOLDS,
// METRIC_CONFIG, APP_CONFIG.freshnessMaxAgeMs) — keep both sides in sync; the
// consistency check lives in the iteration report.
//
// Role -> future LVGL colour token (no CSS in firmware; tokens are the contract
// the LVGL renderer will consume):
//   temp-cold          -> WS_COLOR_TEMP_COLD           #4C9BFF
//   temp-comfort       -> WS_COLOR_TEMP_COMFORT        #43D19E
//   temp-hot           -> WS_COLOR_TEMP_HOT            #FF6B5E
//   humidity-low       -> WS_COLOR_HUMIDITY_LOW        #FFC14D
//   humidity-comfort   -> WS_COLOR_HUMIDITY_COMFORT    #2FD3C0
//   humidity-high      -> WS_COLOR_HUMIDITY_HIGH       #9B7BFF
//   pressure-low       -> WS_COLOR_PRESSURE_LOW        #7F8CFF
//   pressure-normal    -> WS_COLOR_PRESSURE_NORMAL     #43D19E
//   pressure-high      -> WS_COLOR_PRESSURE_HIGH       #FFA04D
//   data state FRESH   -> WS_COLOR_STATE_FRESH         #43D19E
//   data state STALE   -> WS_COLOR_STATE_STALE         #FFC14D
//   data state ERROR   -> WS_COLOR_STATE_ERROR         #FF6B5E
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Text helpers shared by the diagnostics of every layer.
// ---------------------------------------------------------------------------
void formatIpv4(const IPAddress& address, char* out, size_t size) {
  snprintf(out, size, "%u.%u.%u.%u", static_cast<unsigned>(address[0]),
           static_cast<unsigned>(address[1]), static_cast<unsigned>(address[2]),
           static_cast<unsigned>(address[3]));
}

// Formats into the caller's buffer and returns it, so one printf can use it.
// Use a separate buffer per argument inside a single printf call.
const char* ipText(const IPAddress& address, char* buffer, size_t size) {
  formatIpv4(address, buffer, size);
  return buffer;
}

// True when both addresses live in the subnet described by `mask`.
bool ipInSubnet(const IPAddress& address, const IPAddress& mask, const IPAddress& other) {
  for (uint8_t i = 0; i < 4; ++i) {
    if ((address[i] & mask[i]) != (other[i] & mask[i])) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Weather data model (shared by every future consumer: HTTP API, Web UI, LVGL)
// ---------------------------------------------------------------------------
struct Bmp280Reading {
  bool valid = false;
  float temperature_c = NAN;
  float pressure_pa = NAN;  // Pa kept inside the sensor layer; hPa is derived
  uint32_t updated_at_ms = 0;
};

struct Dht11Reading {
  bool valid = false;
  float temperature_c = NAN;
  float humidity_pct = NAN;
  uint32_t updated_at_ms = 0;
};

Adafruit_BMP280 g_bmp;
DHTesp g_dht;

bool g_bmp_ready = false;
Bmp280Reading g_bmp_reading;
Dht11Reading g_dht_reading;

uint32_t g_bmp_last_poll_ms = 0;
uint32_t g_dht_last_poll_ms = 0;
uint32_t g_last_report_ms = 0;

// ---------------------------------------------------------------------------
// Application-level WeatherModel — display-agnostic.
//
// One measured quantity per metric, with validity, age and source. Sources are
// never mixed: temperature_c and pressure_pa come from BMP280 only,
// humidity_pct comes from DHT11 only. This is the ONLY weather data the
// presentation layers may read (HTTP today, LVGL/ILI9341 later).
// ---------------------------------------------------------------------------
WeatherModel weatherModel() {
  const uint32_t now = millis();
  WeatherModel model;
  model.uptime_ms = now;

  const bool bmp_available = g_bmp_reading.updated_at_ms != 0;
  const bool bmp_valid = g_bmp_ready && g_bmp_reading.valid;
  const uint32_t bmp_age_ms = bmp_available ? (now - g_bmp_reading.updated_at_ms) : 0;

  model.temperature_c.source = "BMP280";
  model.temperature_c.available = bmp_available;
  model.temperature_c.valid = bmp_valid;
  model.temperature_c.value = g_bmp_reading.temperature_c;
  model.temperature_c.age_ms = bmp_age_ms;

  model.pressure_hpa.source = "BMP280";
  model.pressure_hpa.available = bmp_available;
  model.pressure_hpa.valid = bmp_valid;
  model.pressure_hpa.value = g_bmp_reading.pressure_pa / 100.0f;
  model.pressure_hpa.age_ms = bmp_age_ms;

  const bool dht_available = g_dht_reading.updated_at_ms != 0;
  model.humidity_pct.source = "DHT11";
  model.humidity_pct.available = dht_available;
  model.humidity_pct.valid = g_dht_reading.valid;
  model.humidity_pct.value = g_dht_reading.humidity_pct;
  model.humidity_pct.age_ms = dht_available ? (now - g_dht_reading.updated_at_ms) : 0;

  return model;
}

// ---------------------------------------------------------------------------
// PresentationModel — semantic classification for any renderer.
//
// Turns the WeatherModel into role ids + data states + scale positions. It has
// no idea whether it feeds HTML, LVGL or an ILI9341 panel, and it never touches
// a sensor driver. NOTE: the firmware is the data source itself, so there is no
// "offline" state here; a browser that cannot reach the device degrades to STALE
// on its own side.
// ---------------------------------------------------------------------------
// ===========================================================================
// DISPLAY SUBSYSTEM — optional hardware; the station is fully functional
// without any panel.
//
// Chain (each layer knows only the layer above it):
//   WeatherModel -> PresentationModel -> DisplayMode -> DisplayRenderer
//                                                          -> DisplayBackend
//
//   DisplayRenderer : layout + draw primitives only. It never touches Wire,
//                     Adafruit_BMP280, DHTesp, WiFi or WebServer, never calls
//                     the API and never re-derives thresholds — it uses the
//                     role/state/fraction/range/unit/icon already computed in
//                     PresentationModel.
//   DisplayBackend  : the only place that knows how to paint. NONE = no-op that
//                     counts primitives so the whole chain stays testable;
//                     ILI9341 will be another implementation of the same calls.
//
// ILI9341 IS DELIBERATELY NOT IMPLEMENTED YET: the concrete module (breakout
// wiring, SPI pins, rotation, backlight) is unknown, and guessing GPIO from the
// controller name alone is unsafe. See README section 12 for the exact steps.
// ===========================================================================

// ---- backend selection (compile-time; NONE is the shipped default) --------
#define DISPLAY_BACKEND_NONE 0
#define DISPLAY_BACKEND_ILI9341 1

#ifndef DISPLAY_BACKEND
#define DISPLAY_BACKEND DISPLAY_BACKEND_NONE
#endif

#if DISPLAY_BACKEND != DISPLAY_BACKEND_NONE && DISPLAY_BACKEND != DISPLAY_BACKEND_ILI9341
#error "Unknown DISPLAY_BACKEND value (expected DISPLAY_BACKEND_NONE or DISPLAY_BACKEND_ILI9341)."
#endif

#if DISPLAY_BACKEND == DISPLAY_BACKEND_ILI9341
#error "DISPLAY_BACKEND_ILI9341 is not implemented yet: add the panel driver (SPI + rotation + backlight for the actual module), an LVGL display driver with a flush callback, lib_deps += lvgl/lvgl and the module's GPIO map. See README section 12."
#endif

constexpr bool kDisplayPanelAttached = (DISPLAY_BACKEND != DISPLAY_BACKEND_NONE);

const char* displayBackendName() { return kDisplayPanelAttached ? "ILI9341" : "NONE"; }

// ---- display mode is APPLICATION state, not Web/panel state ---------------
DisplayMode g_display_mode = DisplayMode::kNumeric;
uint32_t g_display_last_ms = 0;

// Backend/configuration in one place: the renderers never hardcode a size.
DisplayGeometry displayGeometry() { return kGeometry320x240; }  // headless reference profile

// ---- layout engine --------------------------------------------------------
enum class TrackOrientation : uint8_t { kHorizontal, kVertical };

namespace DisplayBackend {
uint16_t g_primitives = 0;  // draw calls issued by the current frame

void beginFrame() { g_primitives = 0; }
uint16_t primitiveCount() { return g_primitives; }

// Headless implementation: the calls are counted but nothing is painted. An
// ILI9341 backend replaces these bodies (LVGL object updates + flush) without
// touching the renderers above.
void drawCard(const LayoutRect& rect, ColorToken accent) {
  (void)rect;
  (void)accent;
  ++g_primitives;
}

void drawIcon(const LayoutRect& rect, IconId icon, ColorToken token) {
  (void)rect;
  (void)icon;
  (void)token;
  ++g_primitives;
}

void drawText(const LayoutRect& rect, const char* text, ColorToken token) {
  (void)rect;
  (void)text;
  (void)token;
  ++g_primitives;
}

void drawTrack(const LayoutRect& rect, float fraction, ColorToken token,
               TrackOrientation orientation) {
  (void)rect;
  (void)fraction;
  (void)token;
  (void)orientation;
  ++g_primitives;
}

void drawGauge(const LayoutRect& rect, float sweep_deg, ColorToken token) {
  (void)rect;
  (void)sweep_deg;
  (void)token;
  ++g_primitives;
}
}  // namespace DisplayBackend

// ---- renderers: PresentationModel in, draw calls out ----------------------
void renderNumeric(const PresentationModel& model, const DisplayGeometry& geometry) {
  WS_LOG_DEBUG("[DISPLAY][DEBUG] render mode=%s\n", displayModeId(DisplayMode::kNumeric));
  const DisplayLayout layout = computeLayout(DisplayMode::kNumeric, model, geometry);
  char text[24];
  DisplayBackend::beginFrame();
  DisplayBackend::drawText(layout.header, "ESP32 Weather Station",
                           colorTokenForState(model.worst_state));
  for (uint8_t i = 0; i < layout.metric_count; ++i) {
    const MetricPresentation& metric = *presentationMetricAt(model, i);
    const MetricLayout& cell = layout.metrics[i];
    DisplayBackend::drawCard(cell.card, colorTokenForState(metric.state));
    DisplayBackend::drawIcon(cell.icon, metric.icon, colorTokenForState(metric.state));
    DisplayBackend::drawText(cell.label, metric.label, colorTokenForState(metric.state));
    formatMetricText(metric, text, sizeof(text));
    DisplayBackend::drawText(cell.value, text, metric.color_token);
    DisplayBackend::drawText(cell.state, dataStateName(metric.state), colorTokenForState(metric.state));
  }
}

void renderHorizontal(const PresentationModel& model, const DisplayGeometry& geometry) {
  WS_LOG_DEBUG("[DISPLAY][DEBUG] render mode=%s\n", displayModeId(DisplayMode::kHorizontal));
  const DisplayLayout layout = computeLayout(DisplayMode::kHorizontal, model, geometry);
  char text[24];
  DisplayBackend::beginFrame();
  for (uint8_t i = 0; i < layout.metric_count; ++i) {
    const MetricPresentation& metric = *presentationMetricAt(model, i);
    const MetricLayout& cell = layout.metrics[i];
    DisplayBackend::drawCard(cell.card, colorTokenForState(metric.state));
    DisplayBackend::drawIcon(cell.icon, metric.icon, colorTokenForState(metric.state));
    DisplayBackend::drawText(cell.label, metric.label, colorTokenForState(metric.state));
    formatMetricText(metric, text, sizeof(text));
    DisplayBackend::drawText(cell.value, text, metric.color_token);
    DisplayBackend::drawText(cell.state, dataStateName(metric.state), colorTokenForState(metric.state));
    DisplayBackend::drawTrack(cell.track, metric.fraction, metric.color_token,
                              TrackOrientation::kHorizontal);
  }
}

void renderBars(const PresentationModel& model, const DisplayGeometry& geometry) {
  WS_LOG_DEBUG("[DISPLAY][DEBUG] render mode=%s\n", displayModeId(DisplayMode::kBars));
  const DisplayLayout layout = computeLayout(DisplayMode::kBars, model, geometry);
  char text[24];
  DisplayBackend::beginFrame();
  for (uint8_t i = 0; i < layout.metric_count; ++i) {
    const MetricPresentation& metric = *presentationMetricAt(model, i);
    const MetricLayout& cell = layout.metrics[i];
    DisplayBackend::drawCard(cell.card, colorTokenForState(metric.state));
    DisplayBackend::drawIcon(cell.icon, metric.icon, colorTokenForState(metric.state));
    DisplayBackend::drawText(cell.label, metric.label, colorTokenForState(metric.state));
    DisplayBackend::drawTrack(cell.track, metric.fraction, metric.color_token,
                              TrackOrientation::kVertical);
    formatMetricText(metric, text, sizeof(text));
    DisplayBackend::drawText(cell.value, text, metric.color_token);
    DisplayBackend::drawText(cell.state, dataStateName(metric.state), colorTokenForState(metric.state));
  }
}

void renderGauges(const PresentationModel& model, const DisplayGeometry& geometry) {
  WS_LOG_DEBUG("[DISPLAY][DEBUG] render mode=%s\n", displayModeId(DisplayMode::kGauges));
  const DisplayLayout layout = computeLayout(DisplayMode::kGauges, model, geometry);
  char text[24];
  DisplayBackend::beginFrame();
  for (uint8_t i = 0; i < layout.metric_count; ++i) {
    const MetricPresentation& metric = *presentationMetricAt(model, i);
    const MetricLayout& cell = layout.metrics[i];
    DisplayBackend::drawCard(cell.card, colorTokenForState(metric.state));
    DisplayBackend::drawGauge(cell.gauge, cell.sweep_deg, metric.color_token);
    DisplayBackend::drawIcon(cell.icon, metric.icon, colorTokenForState(metric.state));
    formatMetricText(metric, text, sizeof(text));
    DisplayBackend::drawText(cell.value, text, metric.color_token);
    DisplayBackend::drawText(cell.label, metric.label, colorTokenForState(metric.state));
    DisplayBackend::drawText(cell.state, dataStateName(metric.state), colorTokenForState(metric.state));
  }
}

// Renders the ACTIVE mode. Mode selection is application state -> renderer.
void renderDisplay(const PresentationModel& model, DisplayMode mode,
                   const DisplayGeometry& geometry) {
  switch (mode) {
    case DisplayMode::kHorizontal: renderHorizontal(model, geometry); break;
    case DisplayMode::kBars: renderBars(model, geometry); break;
    case DisplayMode::kGauges: renderGauges(model, geometry); break;
    default: renderNumeric(model, geometry); break;
  }
}

void logDisplayFrame(DisplayMode mode, const PresentationModel& model) {
  char temperature[16];
  char humidity[16];
  char pressure[16];
  const MetricPresentation& t = model.temperature;
  const MetricPresentation& h = model.humidity;
  const MetricPresentation& p = model.pressure;
  formatMetricText(t, temperature, sizeof(temperature));
  formatMetricText(h, humidity, sizeof(humidity));
  formatMetricText(p, pressure, sizeof(pressure));
  WS_LOG_DEBUG("[DISPLAY][DEBUG] mode=%s temp=%s%s humidity=%s%s pressure=%s%s\n",
               displayModeId(mode), temperature, compactUnitFor(t.icon), humidity,
               compactUnitFor(h.icon), pressure, compactUnitFor(p.icon));
}

void setDisplayMode(DisplayMode mode) {
  if (mode == g_display_mode) {
    return;
  }
  WS_LOG_DEBUG("[DISPLAY][DEBUG] mode switch: %s -> %s\n", displayModeId(g_display_mode),
               displayModeId(mode));
  g_display_mode = mode;
}

// Headless self-test: every mode is laid out for every supported geometry and
// the layout invariant is checked. In DEBUG it also runs all four renderers once
// (the "[DISPLAY][DEBUG] render mode=..." lines) — with no panel attached.
bool displaySelfTest(const PresentationModel& model) {
  const DisplayGeometry profiles[] = {kGeometry320x240, kGeometry480x320, kGeometry800x480,
                                      kGeometryDesktop};
  const uint8_t profile_count = sizeof(profiles) / sizeof(profiles[0]);
  uint16_t frames = 0;
  uint16_t out_of_bounds = 0;

  for (uint8_t p = 0; p < profile_count; ++p) {
    for (uint8_t m = 0; m < kDisplayModeCount; ++m) {
      const DisplayLayout layout = computeLayout(displayModeAt(m), model, profiles[p]);
      ++frames;
      if (!layoutFitsScreen(layout)) {
        ++out_of_bounds;
        Serial.printf("[ERROR][DISPLAY] layout out of bounds: profile=%s mode=%s\n",
                      profiles[p].profile, displayModeId(displayModeAt(m)));
      }
    }
  }

#if WS_DEBUG_LOGS
  for (uint8_t m = 0; m < kDisplayModeCount; ++m) {
    renderDisplay(model, displayModeAt(m), displayGeometry());
  }
#endif

  Serial.printf("[DISPLAY] self-test: %u modes x %u profiles = %u layouts, out-of-bounds=%u\n",
                static_cast<unsigned>(kDisplayModeCount), static_cast<unsigned>(profile_count),
                static_cast<unsigned>(frames), static_cast<unsigned>(out_of_bounds));
  return out_of_bounds == 0;
}

void displayBegin() {
  const DisplayGeometry geometry = displayGeometry();
  Serial.printf("[DISPLAY] backend=%s\n", displayBackendName());
  Serial.println(
      "[DISPLAY] LVGL/headless renderer initialized (headless: LVGL not linked — this is a "
      "supported configuration, see README section 12)");
  Serial.printf("[DISPLAY] geometry=%ux%u profile=%s mode=%s\n",
                static_cast<unsigned>(geometry.width), static_cast<unsigned>(geometry.height),
                geometry.profile, displayModeId(g_display_mode));
  displaySelfTest(buildPresentationModel(weatherModel()));
}

// Periodic display refresh. Quiet in NORMAL: a missing panel is a normal
// configuration, not an error.
void displayService() {
  const uint32_t now = millis();
  if (now - g_display_last_ms < Config::kDisplayUpdateMs) {
    return;
  }
  g_display_last_ms = now;
  const PresentationModel model = buildPresentationModel(weatherModel());
  renderDisplay(model, g_display_mode, displayGeometry());
  logDisplayFrame(g_display_mode, model);
}

// ---------------------------------------------------------------------------
// Sensor layer: shared I2C bus diagnostics + BMP280
// ---------------------------------------------------------------------------
void logI2cScan() {
  uint8_t device_count = 0;
  for (uint8_t address = 0x08; address <= 0x77; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      ++device_count;
      Serial.printf("[BOOT][I2C] device at 0x%02X%s\n", static_cast<unsigned>(address),
                    address == Config::kBmp280Address ? " (expected BMP280 address)" : "");
    }
  }
  if (device_count == 0) {
    Serial.println(
        "[ERROR][I2C] no device on SDA=GPIO21/SCL=GPIO22 — check 3.3V, GND, CSB=3.3V, SDO=GND");
  }
}

bool bmp280Begin() {
  Wire.begin(Pins::kI2cSda, Pins::kI2cScl, Config::kI2cClockHz);
  Serial.printf("[BOOT][I2C] bus started: sda=GPIO%u scl=GPIO%u clock=%u Hz\n",
                static_cast<unsigned>(Pins::kI2cSda), static_cast<unsigned>(Pins::kI2cScl),
                static_cast<unsigned>(Config::kI2cClockHz));
  logI2cScan();

  if (!g_bmp.begin(Config::kBmp280Address)) {
    Serial.printf(
        "[ERROR][SENSOR][BMP280] begin() failed at 0x%02X — check CSB=3.3V, SDO=GND, 3.3V/GND "
        "wiring (a BME280 clone would report chip id 0x60)\n",
        static_cast<unsigned>(Config::kBmp280Address));
    return false;
  }

  g_bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* mode */
                    Adafruit_BMP280::SAMPLING_X2,     /* temperature oversampling */
                    Adafruit_BMP280::SAMPLING_X16,    /* pressure oversampling */
                    Adafruit_BMP280::FILTER_X16,      /* IIR filter */
                    Adafruit_BMP280::STANDBY_MS_500); /* standby */
  Serial.printf("[SENSOR][BMP280] init ok: addr=0x%02X chip_id=0x%02X\n",
                static_cast<unsigned>(Config::kBmp280Address),
                static_cast<unsigned>(g_bmp.sensorID()));
  return true;
}

// Reads the sensor. Read errors are reported explicitly and never replaced by 0.
void bmp280Poll() {
  const float temperature_c = g_bmp.readTemperature();
  const float pressure_pa = g_bmp.readPressure();

  if (isnan(temperature_c) || isnan(pressure_pa) || pressure_pa <= 0.0f) {
    g_bmp_reading.valid = false;  // last good values stay visible as stale
    Serial.printf(
        "[ERROR][SENSOR][BMP280] read failed: t=%.2f C p=%.2f Pa — reading marked invalid/stale\n",
        temperature_c, pressure_pa);
    return;
  }

  g_bmp_reading.valid = true;
  g_bmp_reading.temperature_c = temperature_c;
  g_bmp_reading.pressure_pa = pressure_pa;
  g_bmp_reading.updated_at_ms = millis();
}

// ---------------------------------------------------------------------------
// Sensor layer: DHT11 (one-wire on GPIO4)
// ---------------------------------------------------------------------------
void dht11Begin() {
  g_dht.setup(Pins::kDht11Data, DHTesp::DHT11);
  Serial.printf("[SENSOR][DHT11] init ok: data=GPIO%u model=DHT11 min_period=%d ms\n",
                static_cast<unsigned>(Pins::kDht11Data), g_dht.getMinimumSamplingPeriod());
}

// One blocking DHT read takes ~25 ms worst case; the poll interval keeps it out
// of the Wi-Fi/UI path and the scheduler stays in loop().
void dht11Poll() {
  const TempAndHumidity sample = g_dht.getTempAndHumidity();
  const DHTesp::DHT_ERROR_t status = g_dht.getStatus();

  if (status != DHTesp::ERROR_NONE || isnan(sample.temperature) || isnan(sample.humidity)) {
    g_dht_reading.valid = false;
    Serial.printf("[ERROR][SENSOR][DHT11] read failed: status=%s (error not masked)\n",
                  g_dht.getStatusString());
    return;
  }

  if (sample.humidity < 0.0f || sample.humidity > 100.0f) {
    g_dht_reading.valid = false;
    Serial.printf("[ERROR][SENSOR][DHT11] humidity %.1f %% outside 0..100 — rejected\n",
                  static_cast<double>(sample.humidity));
    return;
  }

  g_dht_reading.valid = true;
  g_dht_reading.temperature_c = sample.temperature;
  g_dht_reading.humidity_pct = sample.humidity;
  g_dht_reading.updated_at_ms = millis();
}

// ---------------------------------------------------------------------------
// Wi-Fi layer: static IPv4 only, non-blocking connect with retry.
// Log tags: [WIFI], [WIFI][DEBUG], [ERROR][WIFI].
// ---------------------------------------------------------------------------
enum class WifiPhase { kDisconnected, kConnecting, kConnected };

WifiPhase g_wifi_phase = WifiPhase::kDisconnected;
uint32_t g_wifi_phase_since_ms = 0;
uint32_t g_wifi_attempts = 0;
uint32_t g_wifi_connect_events = 0;
uint32_t g_wifi_disconnect_events = 0;

// The status is sampled every loop; these counters make "connected and then
// silently loses Wi-Fi" visible instead of silent.
wl_status_t g_wifi_last_status = WL_IDLE_STATUS;
uint32_t g_wifi_status_changes = 0;
bool g_wifi_contract_ok = false;  // result of the last verifyWifiContract() call

// Filled by the Wi-Fi event handler (event task) and printed from loop(), so all
// Serial writes stay in the loop context.
volatile bool g_wifi_ev_connected = false;
volatile bool g_wifi_ev_disconnected = false;
volatile bool g_wifi_ev_got_ip = false;
volatile bool g_wifi_ev_lost_ip = false;
volatile uint32_t g_wifi_ev_count = 0;
volatile uint8_t g_wifi_last_disconnect_reason = 0;

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "no_ssid_avail";
    case WL_SCAN_COMPLETED: return "scan_completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

// Wi-Fi events carry the disconnect reason, which a status poll alone cannot
// expose: this core version has no WiFi.disconnectReason() getter, only
// disconnectReasonName() plus the event payload.
void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  ++g_wifi_ev_count;
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      g_wifi_ev_connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      g_wifi_ev_got_ip = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      g_wifi_last_disconnect_reason = info.wifi_sta_disconnected.reason;
      g_wifi_ev_disconnected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      g_wifi_ev_lost_ip = true;
      break;
    default:
      break;
  }
}

// Prints the Wi-Fi events collected since the last call, plus every status edge.
// The bookkeeping always runs; only the printing depends on the log mode.
void logPendingWifiEvents() {
  const wl_status_t status = WiFi.status();
  if (status != g_wifi_last_status) {
    ++g_wifi_status_changes;
    WS_LOG_DEBUG("[WIFI][DEBUG] WiFi.status() %s (%d) -> %s (%d) at %u ms\n",
                 wifiStatusName(g_wifi_last_status), static_cast<int>(g_wifi_last_status),
                 wifiStatusName(status), static_cast<int>(status),
                 static_cast<unsigned>(millis()));
    g_wifi_last_status = status;
  }

  if (g_wifi_ev_connected) {
    g_wifi_ev_connected = false;
    ++g_wifi_connect_events;
    WS_LOG_DEBUG("[WIFI][DEBUG] event: STA_CONNECTED (association/auth ok)\n");
  }
  if (g_wifi_ev_got_ip) {
    g_wifi_ev_got_ip = false;
    WS_LOG_DEBUG("[WIFI][DEBUG] event: STA_GOT_IP\n");
  }
  if (g_wifi_ev_lost_ip) {
    g_wifi_ev_lost_ip = false;
    WS_LOG_DEBUG("[WIFI][DEBUG] event: STA_LOST_IP\n");
  }
  if (g_wifi_ev_disconnected) {
    g_wifi_ev_disconnected = false;
    ++g_wifi_disconnect_events;
    WS_LOG_DEBUG("[WIFI][DEBUG] event: STA_DISCONNECTED reason=%u (%s) — total disconnects=%u\n",
                 static_cast<unsigned>(g_wifi_last_disconnect_reason),
                 WiFi.disconnectReasonName(
                     static_cast<wifi_err_reason_t>(g_wifi_last_disconnect_reason)),
                 static_cast<unsigned>(g_wifi_disconnect_events));
  }
}

// Reads back what the station actually uses and compares it with the contract.
// A wrong subnet cannot be detected from WiFi.status() alone (it stays
// WL_CONNECTED), so this comparison is the important part.
bool verifyWifiContract() {
  IPAddress expected_ip;
  IPAddress expected_gateway;
  IPAddress expected_subnet;
  IPAddress expected_dns1;
  IPAddress expected_dns2;
  expected_ip.fromString(WIFI_STATIC_IP);
  expected_gateway.fromString(WIFI_GATEWAY);
  expected_subnet.fromString(WIFI_SUBNET);
  expected_dns1.fromString(WIFI_DNS1);
  expected_dns2.fromString(WIFI_DNS2);

  const IPAddress ip = WiFi.localIP();
  const IPAddress gateway = WiFi.gatewayIP();
  const IPAddress mask = WiFi.subnetMask();
  const IPAddress dns1 = WiFi.dnsIP(0);
  const IPAddress dns2 = WiFi.dnsIP(1);

  char ip_buf[16];
  char gw_buf[16];
  char mask_buf[16];
  char dns1_buf[16];
  char dns2_buf[16];
  bool ok = true;

  if (ip == IPAddress(0, 0, 0, 0)) {
    ok = false;
    Serial.println(
        "[ERROR][WIFI] localIP()=0.0.0.0 — адрес не назначен, статический конфиг не применился");
  } else if (ip != expected_ip) {
    ok = false;
    Serial.printf("[ERROR][WIFI] localIP()=%s, по контракту %s — браузер по %s не попадёт на ESP32\n",
                  ipText(ip, ip_buf, sizeof(ip_buf)), WIFI_STATIC_IP, WIFI_STATIC_IP);
  }
  if (gateway != expected_gateway) {
    ok = false;
    Serial.printf("[ERROR][WIFI] gatewayIP()=%s, по контракту %s\n",
                  ipText(gateway, gw_buf, sizeof(gw_buf)), WIFI_GATEWAY);
  }
  if (mask != expected_subnet) {
    ok = false;
    Serial.printf("[ERROR][WIFI] subnetMask()=%s, по контракту %s\n",
                  ipText(mask, mask_buf, sizeof(mask_buf)), WIFI_SUBNET);
  }
  if (dns1 != expected_dns1) {
    ok = false;
    Serial.printf("[ERROR][WIFI] dnsIP(0)=%s, по контракту %s\n",
                  ipText(dns1, dns1_buf, sizeof(dns1_buf)), WIFI_DNS1);
  }
  if (dns2 != expected_dns2) {
    ok = false;
    Serial.printf("[ERROR][WIFI] dnsIP(1)=%s, по контракту %s\n",
                  ipText(dns2, dns2_buf, sizeof(dns2_buf)), WIFI_DNS2);
  }

  // The station and its gateway must sit in the same subnet, otherwise the ESP32
  // is attached to a different network/VLAN even though the status says connected.
  if (!ipInSubnet(ip, mask, gateway)) {
    ok = false;
    Serial.printf(
        "[ERROR][WIFI] gateway %s вне подсети %s/%s — ESP32 в другой сети/VLAN: из локальной сети "
        "адрес %s недостижим\n",
        ipText(gateway, gw_buf, sizeof(gw_buf)), WIFI_STATIC_IP, WIFI_SUBNET, WIFI_STATIC_IP);
  }

  if (ok) {
    Serial.printf("[WIFI] static contract verified: ip=%s mask=%s gw=%s dns1=%s dns2=%s\n",
                  ipText(ip, ip_buf, sizeof(ip_buf)), ipText(mask, mask_buf, sizeof(mask_buf)),
                  ipText(gateway, gw_buf, sizeof(gw_buf)), ipText(dns1, dns1_buf, sizeof(dns1_buf)),
                  ipText(dns2, dns2_buf, sizeof(dns2_buf)));
  }
  g_wifi_contract_ok = ok;
  return ok;
}

// Bounded TCP probe of the gateway. A successful probe proves the L3 path to the
// configured gateway exists; a failed one does NOT prove the network is broken
// (routers often do not listen on 80/443), but together with the contract check
// it separates "wrong subnet/VLAN" from "HTTP problem".
void probeGateway() {
  const IPAddress gateway = WiFi.gatewayIP();
  char gw_buf[16];
  if (gateway == IPAddress(0, 0, 0, 0)) {
    Serial.println("[ERROR][WIFI] gateway=0.0.0.0 — шлюза нет, локальная сеть недостижима");
    return;
  }

  const uint16_t ports[] = {80, 443};
  for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); ++i) {
    WiFiClient probe;
    const uint32_t started_ms = millis();
    const bool connected = probe.connect(gateway, ports[i], Config::kGatewayProbeTimeoutMs) == 1;
    const uint32_t elapsed_ms = millis() - started_ms;
    if (connected) {
      probe.stop();
      // One line in every mode: this is the proof that the ESP32 shares the
      // gateway's subnet. The per-port detail stays in DEBUG.
      Serial.printf("[WIFI] gateway reachable: %s:%u (%u ms)\n",
                    ipText(gateway, gw_buf, sizeof(gw_buf)), static_cast<unsigned>(ports[i]),
                    static_cast<unsigned>(elapsed_ms));
      WS_LOG_DEBUG("[WIFI][DEBUG] gateway tcp probe: путь до шлюза есть, сеть/подсеть совпадают "
                   "с контрактом\n");
      return;
    }
    WS_LOG_DEBUG("[WIFI][DEBUG] gateway tcp probe %s:%u -> нет ответа (%u ms)\n",
                 ipText(gateway, gw_buf, sizeof(gw_buf)), static_cast<unsigned>(ports[i]),
                 static_cast<unsigned>(elapsed_ms));
  }
  Serial.println(
      "[ERROR][WIFI] шлюз не ответил на tcp 80/443: чаще всего это неверный шлюз/подсеть/VLAN "
      "(роутер может и просто не слушать эти порты — проверьте пингом с компьютера)");
}

// Full Wi-Fi snapshot. DEBUG only; in NORMAL the connect path prints the compact
// per-field lines from logWifiConnectDetails() plus the contract verification.
void logWifiSnapshot(const char* event) {
  const wl_status_t status = WiFi.status();
  const bool connected = (status == WL_CONNECTED);

#if WS_DEBUG_LOGS
  Serial.printf("[WIFI] %s: ssid=\"%s\" status=%s (%d) isConnected=%s\n", event,
                WiFi.SSID().c_str(), wifiStatusName(status), static_cast<int>(status),
                WiFi.isConnected() ? "yes" : "no");

  char ip_buf[16];
  char gw_buf[16];
  char mask_buf[16];
  char dns1_buf[16];
  char dns2_buf[16];
  Serial.printf(
      "[WIFI][DEBUG] ip=%s gw=%s mask=%s dns1=%s dns2=%s rssi=%d dBm channel=%d bssid=%s mac=%s "
      "mode=%d hostname=%s\n",
      ipText(WiFi.localIP(), ip_buf, sizeof(ip_buf)),
      ipText(WiFi.gatewayIP(), gw_buf, sizeof(gw_buf)),
      ipText(WiFi.subnetMask(), mask_buf, sizeof(mask_buf)),
      ipText(WiFi.dnsIP(0), dns1_buf, sizeof(dns1_buf)),
      ipText(WiFi.dnsIP(1), dns2_buf, sizeof(dns2_buf)), static_cast<int>(WiFi.RSSI()),
      static_cast<int>(WiFi.channel()), WiFi.BSSIDstr().c_str(), WiFi.macAddress().c_str(),
      static_cast<int>(WiFi.getMode()), WiFi.getHostname());
  Serial.printf(
      "[WIFI][DEBUG] counters: attempts=%u connect_events=%u disconnect_events=%u "
      "status_changes=%u wifi_events=%u\n",
      static_cast<unsigned>(g_wifi_attempts), static_cast<unsigned>(g_wifi_connect_events),
      static_cast<unsigned>(g_wifi_disconnect_events), static_cast<unsigned>(g_wifi_status_changes),
      static_cast<unsigned>(g_wifi_ev_count));
  if (!connected) {
    Serial.println(
        "[WIFI][DEBUG] нет WL_CONNECTED — HTTP-сервер в этом состоянии не поднимается, "
        "адрес 192.168.1.111 недостижим по определению");
  }
#else
  (void)event;
#endif

  if (connected) {
    verifyWifiContract();
  }
}

// One field per line, printed only on the connect transition: makes the required
// values greppable in the Serial Monitor without spamming every heartbeat.
void logWifiConnectDetails() {
  const wl_status_t status = WiFi.status();
  char ip_buf[16];
  char gw_buf[16];
  char mask_buf[16];
  char dns1_buf[16];
  char dns2_buf[16];

  Serial.printf("[WIFI] ssid=%s\n", WiFi.SSID().c_str());
  Serial.printf("[WIFI] status=%s (%d)\n", wifiStatusName(status), static_cast<int>(status));
  Serial.printf("[WIFI] ip=%s\n", ipText(WiFi.localIP(), ip_buf, sizeof(ip_buf)));
  Serial.printf("[WIFI] gateway=%s\n", ipText(WiFi.gatewayIP(), gw_buf, sizeof(gw_buf)));
  Serial.printf("[WIFI] mask=%s\n", ipText(WiFi.subnetMask(), mask_buf, sizeof(mask_buf)));
  Serial.printf("[WIFI] dns1=%s\n", ipText(WiFi.dnsIP(0), dns1_buf, sizeof(dns1_buf)));
  Serial.printf("[WIFI] dns2=%s\n", ipText(WiFi.dnsIP(1), dns2_buf, sizeof(dns2_buf)));
  Serial.printf("[WIFI] rssi=%d dBm channel=%d bssid=%s mac=%s\n", static_cast<int>(WiFi.RSSI()),
                static_cast<int>(WiFi.channel()), WiFi.BSSIDstr().c_str(),
                WiFi.macAddress().c_str());
  IPAddress expected_ip;
  expected_ip.fromString(WIFI_STATIC_IP);
  Serial.printf("[WIFI] проверка: localIP()==%s -> %s\n", WIFI_STATIC_IP,
                WiFi.localIP() == expected_ip ? "MATCH" : "MISMATCH");
  WS_LOG_DEBUG(
      "[WIFI][DEBUG] напоминание: компьютер/телефон должен быть в той же сети 192.168.1.0/24 "
      "(не guest Wi-Fi, не VPN, не другой VLAN)\n");
}

// Applies the project's fixed static IPv4 contract before every connection attempt.
bool wifiApplyStaticConfig() {
#if defined(WIFI_USE_STATIC_IP) && (WIFI_USE_STATIC_IP)
  IPAddress local_ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns1;
  IPAddress dns2;
  if (!local_ip.fromString(WIFI_STATIC_IP) || !gateway.fromString(WIFI_GATEWAY) ||
      !subnet.fromString(WIFI_SUBNET) || !dns1.fromString(WIFI_DNS1) ||
      !dns2.fromString(WIFI_DNS2)) {
    Serial.println("[ERROR][WIFI] invalid IPv4 literal in include/secrets.h — static config skipped");
    return false;
  }
  if (!WiFi.config(local_ip, gateway, subnet, dns1, dns2)) {
    Serial.println("[ERROR][WIFI] WiFi.config() rejected the static IPv4 configuration");
    return false;
  }
  WS_LOG_DEBUG(
      "[WIFI][DEBUG] WiFi.config() applied: ip=%s mask=%s gw=%s dns1=%s dns2=%s (DHCP не "
      "используется)\n",
      WIFI_STATIC_IP, WIFI_SUBNET, WIFI_GATEWAY, WIFI_DNS1, WIFI_DNS2);
  return true;
#else
  Serial.println("[ERROR][WIFI] WIFI_USE_STATIC_IP is disabled — the project contract requires the "
                 "static IPv4 address; DHCP is not a valid working mode");
  return false;
#endif
}

void wifiStartConnect() {
  ++g_wifi_attempts;
  Serial.printf("[WIFI] attempt #%u: ssid=\"%s\" static=%s gw=%s dns=%s/%s\n",
                static_cast<unsigned>(g_wifi_attempts), WIFI_SSID, WIFI_STATIC_IP, WIFI_GATEWAY,
                WIFI_DNS1, WIFI_DNS2);

  WS_LOG_DEBUG("[WIFI][DEBUG] step 2/5 WiFi.config(...) — статический IPv4 до begin()\n");
  const bool config_ok = wifiApplyStaticConfig();
  if (!config_ok) {
    Serial.println(
        "[ERROR][WIFI] статический конфиг не применился: возможен адрес от DHCP вместо "
        "192.168.1.111");
  }

  Serial.println("[WIFI] connecting...");
  WS_LOG_DEBUG("[WIFI][DEBUG] step 3/5 WiFi.begin(ssid, password)\n");
  const wl_status_t begin_status = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WS_LOG_DEBUG(
      "[WIFI][DEBUG] step 4/5 ждём WL_CONNECTED (timeout %u ms); begin() вернул %s (%d) — "
      "подключение асинхронное\n",
      static_cast<unsigned>(Config::kWifiConnectTimeoutMs), wifiStatusName(begin_status),
      static_cast<int>(begin_status));

  g_wifi_phase = WifiPhase::kConnecting;
  g_wifi_phase_since_ms = millis();
}

void wifiBegin() {
  WS_LOG_DEBUG("[WIFI][DEBUG] step 1/5 WiFi.mode(WIFI_STA) — сеть: ssid=\"%s\"\n", WIFI_SSID);
  WiFi.persistent(false);  // do not rewrite Wi-Fi config into NVS on every connect
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(onWifiEvent);  // needed for disconnect reasons (see onWifiEvent)
  WS_LOG_DEBUG("[WIFI][DEBUG] mode=%d hostname=%s mac=%s persistent=off sleep=off\n",
               static_cast<int>(WiFi.getMode()), WiFi.getHostname(), WiFi.macAddress().c_str());
  WS_LOG_DEBUG(
      "[WIFI][DEBUG] контракт (из include/secrets.h): ip=%s mask=%s gw=%s dns1=%s dns2=%s\n",
      WIFI_STATIC_IP, WIFI_SUBNET, WIFI_GATEWAY, WIFI_DNS1, WIFI_DNS2);
}

// The HTTP server follows the Wi-Fi link: started once the link is up, stopped
// when the link drops, so recovery needs no power cycle.
void httpStart();
void httpStop();

// Non-blocking state machine; called from loop().
void wifiService() {
  const uint32_t now = millis();

  logPendingWifiEvents();

  switch (g_wifi_phase) {
    case WifiPhase::kDisconnected:
      if (g_wifi_attempts == 0 || now - g_wifi_phase_since_ms >= Config::kWifiRetryMs) {
        wifiStartConnect();
      }
      break;

    case WifiPhase::kConnecting:
      if (WiFi.status() == WL_CONNECTED) {
        const uint32_t connect_ms = now - g_wifi_phase_since_ms;
        g_wifi_phase = WifiPhase::kConnected;
        g_wifi_phase_since_ms = now;

        Serial.printf("[WIFI] connected (WL_CONNECTED) after %u ms, attempt #%u\n",
                      static_cast<unsigned>(connect_ms), static_cast<unsigned>(g_wifi_attempts));

        // Order matters: verify the address first, then start listening, so the
        // HTTP server is never reported as running on an unusable address.
        WS_LOG_DEBUG("[WIFI][DEBUG] step 5/5 проверка localIP()/gw/mask/dns и доступности шлюза\n");
        logWifiSnapshot("connected");  // DEBUG: snapshot · NORMAL: contract verification
        logWifiConnectDetails();
        probeGateway();
        if (!g_wifi_contract_ok) {
          Serial.println(
              "[ERROR][WIFI] адрес/маска/шлюз не совпали с контрактом — HTTP поднимается, но "
              "браузер по 192.168.1.111 может быть недостижим");
        }
        httpStart();
      } else if (now - g_wifi_phase_since_ms >= Config::kWifiConnectTimeoutMs) {
        Serial.printf(
            "[WIFI] attempt #%u timed out after %u ms: status=%s (%d) — SSID не найден, неверный "
            "пароль или вне зоны; повтор через %u ms\n",
            static_cast<unsigned>(g_wifi_attempts),
            static_cast<unsigned>(Config::kWifiConnectTimeoutMs), wifiStatusName(WiFi.status()),
            static_cast<int>(WiFi.status()), static_cast<unsigned>(Config::kWifiRetryMs));
        WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
        g_wifi_phase = WifiPhase::kDisconnected;
        g_wifi_phase_since_ms = now;
      }
      break;

    case WifiPhase::kConnected:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf(
            "[ERROR][WIFI] link lost at %u ms: status=%s (%d) — HTTP останавливается, дальше "
            "повторное подключение с тем же статическим %s\n",
            static_cast<unsigned>(now), wifiStatusName(WiFi.status()),
            static_cast<int>(WiFi.status()), WIFI_STATIC_IP);
        logWifiSnapshot("link lost");
        httpStop();
        g_wifi_phase = WifiPhase::kDisconnected;
        g_wifi_phase_since_ms = now;
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// HTTP layer: embedded Web UI + JSON API + diagnostics.
// Lifecycle is driven by the Wi-Fi state machine (start on link up, stop on
// link down). handleClient() is non-blocking, so the sensor schedule in loop()
// keeps running; this is also the foundation for a later SSE/WebSocket stream.
// Log tags: [HTTP], [HTTP][DEBUG], [ERROR][HTTP].
// ---------------------------------------------------------------------------
WebServer g_http(Config::kHttpPort);
bool g_http_running = false;

// Request counters: if the browser cannot reach the device, these stay at zero —
// that is the difference between "HTTP problem" and "network problem".
uint32_t g_http_requests = 0;
uint32_t g_http_health_requests = 0;
uint32_t g_http_weather_requests = 0;
uint32_t g_http_not_found_requests = 0;

// Static buffers: no per-request allocation and no large stack temporaries.
constexpr size_t kJsonBufferSize = 384;
char g_json_buffer[kJsonBufferSize];

// web/index.html is embedded into the firmware at build time
// (board_build.embed_files in platformio.ini) and served straight from flash:
// one source of truth for the page, no filesystem, no RAM copy of the asset.
extern const uint8_t web_index_html_start[] asm("_binary_web_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_web_index_html_end");

// ---------------------------------------------------------------------------
// WEATHER API CONTRACT — version 1 (FROZEN).
//
//   GET /api/weather -> 200 application/json, Cache-Control: no-store
//
//   "temperature_c"  number|null   °C    source BMP280
//   "humidity_pct"   number|null   %     source DHT11
//   "pressure_hpa"   number|null   hPa   source BMP280  (pressure_pa / 100)
//   "pressure_pa"    number|null   Pa    source BMP280  (pressure_hpa * 100)
//   "bmp280_valid"   boolean             last BMP280 read succeeded
//   "dht11_valid"    boolean             last DHT11 read succeeded
//   "bmp280_age_ms"  number|null         age of the last successful BMP280 read
//   "dht11_age_ms"   number|null         age of the last successful DHT11 read
//   "uptime_ms"      number              millis() since boot
//
// Field names and order are part of the contract; both sensor blocks follow the
// same rule: null means "never measured" and an error is never masked with 0.
// A stale sensor keeps its last known value together with valid=false.
// Version 2 (semantic roles / data states, e.g. over SSE) is a separate,
// additive decision — v1 stays byte-compatible.
// ---------------------------------------------------------------------------
constexpr uint16_t kWeatherApiVersion = 1;

const char* validityName(bool valid) { return valid ? "ok" : "stale"; }

// Minimal append-only writer over a fixed buffer.
struct JsonWriter {
  JsonWriter(char* target, size_t target_capacity) : buffer(target), capacity(target_capacity) {}

  char* buffer = nullptr;
  size_t capacity = 0;
  size_t length = 0;
  bool complete = true;
};

void jsonAppend(JsonWriter& writer, const char* format, ...) {
  if (writer.length >= writer.capacity) {
    writer.complete = false;
    return;
  }
  va_list args;
  va_start(args, format);
  const int written =
      vsnprintf(writer.buffer + writer.length, writer.capacity - writer.length, format, args);
  va_end(args);
  if (written < 0 || static_cast<size_t>(written) >= writer.capacity - writer.length) {
    writer.complete = false;
    writer.length = writer.capacity - 1;  // keep the buffer NUL-terminated
    return;
  }
  writer.length += static_cast<size_t>(written);
}

// A value that was never measured is emitted as null — read errors are never
// masked with 0. A stale reading keeps its last known value and valid=false.
void jsonAppendNumberOrNull(JsonWriter& writer, float value, bool available, const char* format) {
  if (!available || isnan(value)) {
    jsonAppend(writer, "null");
    return;
  }
  jsonAppend(writer, format, static_cast<double>(value));
}

size_t buildWeatherJson(char* buffer, size_t capacity, const WeatherModel& model) {
  JsonWriter writer{buffer, capacity};

  jsonAppend(writer, "{");
  jsonAppend(writer, "\"temperature_c\":");
  jsonAppendNumberOrNull(writer, model.temperature_c.value, model.temperature_c.available, "%.2f");
  jsonAppend(writer, ",\"humidity_pct\":");
  jsonAppendNumberOrNull(writer, model.humidity_pct.value, model.humidity_pct.available, "%.0f");
  jsonAppend(writer, ",\"pressure_hpa\":");
  jsonAppendNumberOrNull(writer, model.pressure_hpa.value, model.pressure_hpa.available, "%.2f");
  jsonAppend(writer, ",\"pressure_pa\":");
  jsonAppendNumberOrNull(writer, model.pressure_hpa.value * 100.0f, model.pressure_hpa.available,
                         "%.0f");
  jsonAppend(writer, ",\"bmp280_valid\":%s", model.temperature_c.valid ? "true" : "false");
  jsonAppend(writer, ",\"dht11_valid\":%s", model.humidity_pct.valid ? "true" : "false");
  jsonAppend(writer, ",\"bmp280_age_ms\":");
  if (model.temperature_c.available) {
    jsonAppend(writer, "%u", static_cast<unsigned>(model.temperature_c.age_ms));
  } else {
    jsonAppend(writer, "null");
  }
  jsonAppend(writer, ",\"dht11_age_ms\":");
  if (model.humidity_pct.available) {
    jsonAppend(writer, "%u", static_cast<unsigned>(model.humidity_pct.age_ms));
  } else {
    jsonAppend(writer, "null");
  }
  jsonAppend(writer, ",\"uptime_ms\":%u}", static_cast<unsigned>(model.uptime_ms));

  if (!writer.complete) {
    Serial.printf("[ERROR][HTTP] JSON buffer too small (%u bytes) — response truncated\n",
                  static_cast<unsigned>(capacity));
  }
  return writer.length;
}

// Formats an IPv4 address without allocating a String on the request path.
// (formatIpv4 itself lives in the shared helper section above.)

// Serves the autonomous Web UI (web/index.html) directly from flash.
// The page then polls /api/weather on its own.
void handleRoot() {
  const size_t length = static_cast<size_t>(web_index_html_end - web_index_html_start);
  ++g_http_requests;

#if WS_DEBUG_LOGS
  char client_ip[16];
  formatIpv4(g_http.client().remoteIP(), client_ip, sizeof(client_ip));
  Serial.printf("[HTTP] GET %s from %s -> 200 (%u bytes, text/html); requests=%u\n",
                g_http.uri().c_str(), client_ip, static_cast<unsigned>(length),
                static_cast<unsigned>(g_http_requests));
#endif

  g_http.sendHeader("Cache-Control", "no-store");
  g_http.send_P(200, "text/html; charset=utf-8", reinterpret_cast<PGM_P>(web_index_html_start),
                length);
}

// Minimal liveness endpoint: 200 text/plain "OK". Does not touch sensors.
void handleHealth() {
  ++g_http_requests;
  ++g_http_health_requests;

#if WS_DEBUG_LOGS
  char client_ip[16];
  formatIpv4(g_http.client().remoteIP(), client_ip, sizeof(client_ip));
  Serial.printf("[HTTP] GET /health from %s -> 200 text/plain OK\n", client_ip);
#endif

  g_http.sendHeader("Cache-Control", "no-store");
  g_http.send(200, "text/plain", "OK");
}

void handleWeatherApi() {
  const WeatherModel model = weatherModel();
  const size_t length = buildWeatherJson(g_json_buffer, sizeof(g_json_buffer), model);
  ++g_http_requests;
  ++g_http_weather_requests;

#if WS_DEBUG_LOGS
  char client_ip[16];
  char bmp_age[16];
  char dht_age[16];
  formatIpv4(g_http.client().remoteIP(), client_ip, sizeof(client_ip));
  snprintf(bmp_age, sizeof(bmp_age), "%u", static_cast<unsigned>(model.temperature_c.age_ms));
  snprintf(dht_age, sizeof(dht_age), "%u", static_cast<unsigned>(model.humidity_pct.age_ms));

  Serial.printf(
      "[API] GET /api/weather from %s -> 200 (%u bytes, bmp280=%s age=%s ms, dht11=%s age=%s ms, "
      "uptime=%u ms, requests=%u)\n",
      client_ip, static_cast<unsigned>(length), validityName(model.temperature_c.valid),
      model.temperature_c.available ? bmp_age : "n/a", validityName(model.humidity_pct.valid),
      model.humidity_pct.available ? dht_age : "n/a", static_cast<unsigned>(model.uptime_ms),
      static_cast<unsigned>(g_http_requests));
#endif

  g_http.sendHeader("Cache-Control", "no-store");
  g_http.send(200, "application/json", g_json_buffer);
}

void handleNotFound() {
  ++g_http_requests;
  ++g_http_not_found_requests;

  char client_ip[16];
  formatIpv4(g_http.client().remoteIP(), client_ip, sizeof(client_ip));
  Serial.printf("[ERROR][HTTP] 404 uri=%s from %s (запрос ДОШЁЛ до ESP32 — сеть работает)\n",
                g_http.uri().c_str(), client_ip);

  g_http.sendHeader("Cache-Control", "no-store");
  g_http.send(404, "application/json", "{\"error\":\"not_found\"}");
}

// Route registration happens once in setup(); start/stop only toggles listening.
void httpSetup() {
  g_http.on("/", HTTP_GET, handleRoot);
  g_http.on("/index.html", HTTP_GET, handleRoot);  // same page, explicit path
  g_http.on("/health", HTTP_GET, handleHealth);
  g_http.on("/api/weather", HTTP_GET, handleWeatherApi);
  g_http.onNotFound(handleNotFound);
  g_http.enableDelay(false);  // loop() already yields; avoids an extra delay(1)
  Serial.printf(
      "[HTTP] routes registered: GET / (embedded index.html), GET /health, GET /api/weather "
      "(port %u)\n",
      static_cast<unsigned>(Config::kHttpPort));
}

// Verifies that the listening socket really accepts and dispatches a request, so
// "running" is not just an assumption after begin().
// How it works: connect to our own static IP:80 and request /health. lwIP
// loopback is enabled in this SDK (CONFIG_LWIP_NETIF_LOOPBACK=y), so this
// exercises TCP + WebServer + the handler without any external client.
// Limitation: this proves the ESP32 side only. It cannot prove that the AP
// forwards traffic from other devices — that needs a request from the LAN, which
// shows up in the request counter below.
void httpSelfTest() {
  IPAddress self_ip;
  if (!self_ip.fromString(WIFI_STATIC_IP)) {
    Serial.println("[ERROR][HTTP] self-test skipped: WIFI_STATIC_IP is not a valid IPv4 literal");
    return;
  }

  WiFiClient client;
  if (client.connect(self_ip, Config::kHttpPort, Config::kHttpSelfTestTimeoutMs) != 1) {
    Serial.printf(
        "[ERROR][HTTP] self-test: tcp connect к %s:%u не удался за %u ms — подтвердить приём "
        "соединений изнутри не удалось, нужна внешняя проверка (curl/ping с компьютера)\n",
        WIFI_STATIC_IP, static_cast<unsigned>(Config::kHttpPort),
        static_cast<unsigned>(Config::kHttpSelfTestTimeoutMs));
    // Drop a possibly half-open accept state so real clients are not stuck.
    g_http.stop();
    g_http.begin();
    return;
  }

  client.printf("GET /health HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", WIFI_STATIC_IP);

  const uint32_t deadline_ms = millis() + Config::kHttpSelfTestTimeoutMs;
  char status_line[48] = {0};
  size_t filled = 0;
  bool got_200 = false;

  while (millis() < deadline_ms) {
    g_http.handleClient();  // drive the server so it accepts our own request
    while (client.available() && filled < sizeof(status_line) - 1) {
      status_line[filled++] = static_cast<char>(client.read());
    }
    status_line[filled] = '\0';
    if (strstr(status_line, "200") != nullptr) {
      got_200 = true;
      break;
    }
    delay(1);
  }
  client.stop();

  if (got_200) {
    // Compact proof in every mode; the raw status line only in DEBUG.
    Serial.println("[HTTP] self-test ok (GET /health -> 200)");
    WS_LOG_DEBUG("[HTTP][DEBUG] self-test response: %s", status_line);
    WS_LOG_DEBUG("\n");
  } else {
    Serial.printf(
        "[ERROR][HTTP] self-test: ответа с кодом 200 нет за %u ms (получено \"%s\") — приём "
        "соединений не подтверждён\n",
        static_cast<unsigned>(Config::kHttpSelfTestTimeoutMs), status_line);
    g_http.stop();
    g_http.begin();  // clear a possibly half-open accept state, keep listening
  }
}

// Periodic HTTP state line (DEBUG only): proves the server is alive and whether
// any external client has ever reached it.
void logHttpStatus(const char* event) {
  WS_LOG_DEBUG(
      "[HTTP][DEBUG] %s: running=%d port=%u api=v%u url=http://%s/ requests=%u (weather=%u, "
      "health=%u, 404=%u) heap=%u\n",
      event, g_http_running ? 1 : 0, static_cast<unsigned>(Config::kHttpPort),
      static_cast<unsigned>(kWeatherApiVersion), WIFI_STATIC_IP,
      static_cast<unsigned>(g_http_requests), static_cast<unsigned>(g_http_weather_requests),
      static_cast<unsigned>(g_http_health_requests),
      static_cast<unsigned>(g_http_not_found_requests),
      static_cast<unsigned>(ESP.getFreeHeap()));
}

void httpStart() {
  if (g_http_running) {
    WS_LOG_DEBUG("[HTTP][DEBUG] httpStart() пропущен: сервер уже запущен\n");
    return;
  }

  g_http.begin();
  g_http_running = true;

  Serial.println("[HTTP] server started");
  Serial.printf("[HTTP] bind port=%u\n", static_cast<unsigned>(Config::kHttpPort));
  Serial.printf("[HTTP] url=http://%s/\n", WIFI_STATIC_IP);
  WS_LOG_DEBUG(
      "[HTTP][DEBUG] WebServer API не отдаёт состояние listen-сокета после begin() — проверяем "
      "приём соединений сквозным self-test\n");

  httpSelfTest();
  logHttpStatus("after start");
}

void httpStop() {
  if (!g_http_running) {
    return;
  }
  g_http.stop();
  g_http_running = false;
  Serial.println("[HTTP] server stopped (Wi-Fi link down) — restarts with the next link up");
}

void httpService() {
  if (!g_http_running) {
    return;
  }
  g_http.handleClient();
}

// ---------------------------------------------------------------------------
// Diagnostics
//
// NORMAL: prints only when a sensor is NOT healthy (invalid / never read /
//         not initialised), so a healthy station stays silent between events.
// DEBUG : prints the full block every kDiagReportMs plus the semantic role and
//         data state of every metric (the values an LVGL renderer will use).
// ---------------------------------------------------------------------------
void reportSensors() {
  const uint32_t now = millis();

  const bool bmp_healthy = g_bmp_ready && g_bmp_reading.updated_at_ms != 0 && g_bmp_reading.valid;
  const bool dht_healthy = g_dht_reading.updated_at_ms != 0 && g_dht_reading.valid;

  if (!kDebugLogs && bmp_healthy && dht_healthy) {
    return;  // NORMAL: nothing to report while both sensors are fine
  }

  if (!g_bmp_ready) {
    Serial.println("[SENSOR][BMP280] status=error: sensor not initialized");
  } else if (g_bmp_reading.updated_at_ms == 0) {
    Serial.println("[SENSOR][BMP280] status=stale: no successful read yet");
  } else {
    Serial.printf(
        "[SENSOR][BMP280] t=%.2f C p=%.2f hPa (%.0f Pa) status=%s age=%u ms\n",
        static_cast<double>(g_bmp_reading.temperature_c),
        static_cast<double>(g_bmp_reading.pressure_pa / 100.0f),
        static_cast<double>(g_bmp_reading.pressure_pa), g_bmp_reading.valid ? "ok" : "stale",
        static_cast<unsigned>(now - g_bmp_reading.updated_at_ms));
  }

  if (g_dht_reading.updated_at_ms == 0) {
    Serial.println("[SENSOR][DHT11] status=stale: no successful read yet");
  } else {
    Serial.printf("[SENSOR][DHT11] t=%.1f C rh=%.1f %% status=%s age=%u ms\n",
                  static_cast<double>(g_dht_reading.temperature_c),
                  static_cast<double>(g_dht_reading.humidity_pct),
                  g_dht_reading.valid ? "ok" : "stale",
                  static_cast<unsigned>(now - g_dht_reading.updated_at_ms));
  }

  Serial.printf("[SENSOR] free_heap=%u bytes\n", static_cast<unsigned>(ESP.getFreeHeap()));

#if WS_DEBUG_LOGS
  // Same vocabulary the Web UI and the future LVGL renderer use.
  const PresentationModel presentation = buildPresentationModel(weatherModel());
  Serial.printf(
      "[SENSOR][DEBUG] presentation: temp role=%s state=%s fraction=%.3f | humidity role=%s "
      "state=%s fraction=%.3f | pressure role=%s state=%s fraction=%.3f | worst=%s\n",
      presentation.temperature.role ? presentation.temperature.role : "none",
      dataStateName(presentation.temperature.state), presentation.temperature.fraction,
      presentation.humidity.role ? presentation.humidity.role : "none",
      dataStateName(presentation.humidity.state), presentation.humidity.fraction,
      presentation.pressure.role ? presentation.pressure.role : "none",
      dataStateName(presentation.pressure.state), presentation.pressure.fraction,
      dataStateName(presentation.worst_state));
#endif
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(Config::kSerialBaud);
  delay(200);  // short settle so the first boot banner is not lost over USB serial

  Serial.println();
  Serial.println("[BOOT] ESP32 Weather Station — iteration 09: stabilisation + display boundary");
  Serial.printf("[BOOT] weather API v%u at /api/weather (contract frozen, 9 fields)\n",
                static_cast<unsigned>(kWeatherApiVersion));
  Serial.printf("[BOOT] logging: %s (WS_DEBUG_LOGS=%d)\n",
                kDebugLogs ? "DEBUG — полный диагностический поток"
                           : "NORMAL — только события, ошибки и аномалии",
                static_cast<int>(WS_DEBUG_LOGS));
  Serial.printf("[BOOT] reset_reason=%d free_heap=%u bytes chip=%s cores=%d\n",
                static_cast<int>(esp_reset_reason()), static_cast<unsigned>(ESP.getFreeHeap()),
                ESP.getChipModel(), ESP.getChipCores());
  Serial.println("[BOOT] pin map: BMP280 SDA=GPIO21 SCL=GPIO22 addr=0x76 | DHT11 OUT=GPIO4");
  Serial.printf("[BOOT] ожидаемая сеть: ssid=\"%s\" static ip=%s mask=%s gw=%s dns1=%s dns2=%s\n",
                WIFI_SSID, WIFI_STATIC_IP, WIFI_SUBNET, WIFI_GATEWAY, WIFI_DNS1, WIFI_DNS2);
  WS_LOG_DEBUG(
      "[BOOT][DEBUG] порядок: WiFi.mode(STA) -> WiFi.config(static) -> WiFi.begin -> "
      "WL_CONNECTED -> проверка localIP -> httpStart()\n");

  g_bmp_ready = bmp280Begin();
  dht11Begin();
  displayBegin();  // optional panel / headless renderer self-test
  httpSetup();     // routes registered once; listening starts with the Wi-Fi link
  wifiBegin();

  g_last_report_ms = millis();
  Serial.println("[BOOT] setup complete — Wi-Fi и HTTP продвигаются из loop()");
}

void loop() {
  const uint32_t now = millis();

  wifiService();
  httpService();
  displayService();  // headless/panel refresh, independent of Wi-Fi and HTTP

  if (g_bmp_ready && now - g_bmp_last_poll_ms >= Config::kBmp280PollMs) {
    g_bmp_last_poll_ms = now;
    bmp280Poll();
  }

  if (now - g_dht_last_poll_ms >= Config::kDht11PollMs) {
    g_dht_last_poll_ms = now;
    dht11Poll();
  }

  if (now - g_last_report_ms >= Config::kDiagReportMs) {
    g_last_report_ms = now;
    reportSensors();  // NORMAL: only anomalies

#if WS_DEBUG_LOGS
    if (g_wifi_phase == WifiPhase::kConnected) {
      logWifiSnapshot("link ok");
      logHttpStatus("heartbeat");
    } else {
      Serial.printf("[WIFI][DEBUG] heartbeat: phase=%s status=%s (%d) — HTTP %s\n",
                    g_wifi_phase == WifiPhase::kConnecting ? "connecting" : "disconnected",
                    wifiStatusName(WiFi.status()), static_cast<int>(WiFi.status()),
                    g_http_running ? "running" : "not running");
    }
#else
    // NORMAL: stay silent while Wi-Fi and HTTP are healthy; speak up otherwise.
    if (g_wifi_phase != WifiPhase::kConnected || !g_http_running) {
      Serial.printf(
          "[WIFI] heartbeat: phase=%s status=%s (%d) — HTTP %s — повторы подключения каждые %u ms\n",
          g_wifi_phase == WifiPhase::kConnecting ? "connecting" : "disconnected",
          wifiStatusName(WiFi.status()), static_cast<int>(WiFi.status()),
          g_http_running ? "running" : "not running",
          static_cast<unsigned>(Config::kWifiRetryMs));
    }
#endif
  }

  delay(10);  // short yield only; no long blocking delays in the main loop
}
