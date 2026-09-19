"""Static checks for the simulator / shared-LVGL-UI iteration.

Usage:  python display_checks.py <firmware-root> [<simulator-root>]

The first argument is the firmware repository (the one PlatformIO builds); the
optional second argument is the external PC-simulator workspace. When the
simulator root is given, its wiring and artefacts are checked as well.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1])
SIM = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None

SRC = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
HTML = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
INI = (ROOT / "platformio.ini").read_text(encoding="utf-8")
CORE = (ROOT / "include" / "weather_core.h").read_text(encoding="utf-8")
UI = (ROOT / "include" / "lvgl_ui.h").read_text(encoding="utf-8")
CONF = (ROOT / "include" / "lv_conf.h").read_text(encoding="utf-8")

# The real Wi-Fi password is read from the untracked include/secrets.h at runtime:
# the checkers must verify that it never appears in tracked files, which means the
# value itself must not be part of this (tracked) script either.
def _real_wifi_password(root):
    try:
        text = (pathlib.Path(root) / "include" / "secrets.h").read_text(encoding="utf-8")
    except OSError:
        return None
    match = re.search(r'WIFI_PASSWORD\s+"([^"]+)"', text)
    return match.group(1) if match else None


REAL_WIFI_PASSWORD = _real_wifi_password(ROOT)

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))


def between(text, start_marker, end_marker):
    start = text.index(start_marker)
    return text[start:text.index(end_marker, start)]


def strip_comments(text):
    """Remove // and /* */ comments so checks only look at real code."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


CORE_CODE = strip_comments(CORE)
UI_CODE = strip_comments(UI)
SRC_CODE = strip_comments(SRC)

# ------------------------------------------------------ shared / target split
for forbidden in ["Arduino.h", "Wire.", "WiFi.", "WebServer", "Adafruit_BMP280", "DHTesp",
                  "buildWeatherJson", "lvgl.h", "SDL"]:
    check(f"shared core is free of {forbidden!r}", forbidden not in CORE_CODE)
STD_HEADERS = {"stdint.h", "stddef.h", "string.h", "stdio.h", "math.h", "stdbool.h"}
core_headers = set(re.findall(r"#include\s+<([^>]+)>", CORE))
check("shared core only needs the C standard library", core_headers <= STD_HEADERS,
      str(sorted(core_headers)))
check("shared UI includes LVGL and the shared core",
      '#include "lvgl.h"' in UI and '#include "weather_core.h"' in UI)
for forbidden in ["SDL_", "#include <SDL", "Windows.h", "iostream", "vector<", "string>"]:
    check(f"shared UI is free of {forbidden!r} (PC/desktop only)", forbidden not in UI_CODE)
check("firmware main.cpp consumes the shared core", '#include "weather_core.h"' in SRC)
check("firmware does not compile the LVGL UI yet",
      '#include "lvgl_ui.h"' not in SRC and "#include <lvgl.h>" not in SRC)
check("LVGL configuration is shared and gated by WS_PC_SIMULATOR",
      "WS_PC_SIMULATOR" in CONF and "LV_USE_SDL" in CONF)
sdl_block = between(CONF, "#if defined(WS_PC_SIMULATOR)", "#endif") if \
    "#if defined(WS_PC_SIMULATOR)" in CONF else ""
check("SDL driver is enabled only for the simulator build",
      "LV_USE_SDL 1" in sdl_block and "LV_SDL_ACCELERATED 0" in sdl_block, sdl_block[:60])
for absent in ["src/sim_main.cpp", "src/demo_weather.cpp", "src/demo_weather.h",
               "include/demo_weather.h", "simulator", "display", "drivers", "desktop"]:
    path = ROOT / absent
    check(f"firmware tree has no {absent}", not path.exists())

lib_deps_lines = []
collecting = False
for line in INI.splitlines():
    if line.strip().startswith("lib_deps"):
        collecting = True
        continue
    if collecting:
        if not line.strip() or not line.startswith((" ", "\t")) or line.strip().startswith(";"):
            break
        lib_deps_lines.append(line.strip())
check("LVGL is not in the firmware lib_deps",
      not any("lvgl" in l.lower() for l in lib_deps_lines), str(lib_deps_lines))
check("firmware library set unchanged (2 entries)", len(lib_deps_lines) == 2, str(lib_deps_lines))
check("firmware keeps DISPLAY_BACKEND=0 default", "-DDISPLAY_BACKEND=0" in INI)

# ------------------------------------------------------------------ the modes
mode_fn = between(CORE, "const char* displayModeId(", "DisplayMode displayModeAt(")
fw_mode_ids = sorted(re.findall(r'return "([a-z]+)";', mode_fn))
web_mode_ids = sorted(re.findall(r"'([a-z]+)'", re.search(r"const MODES = \[([^\]]+)\]", HTML).group(1)))
check("five display modes in the shared core",
      fw_mode_ids == ["bars", "gauges", "history", "horizontal", "numeric"], str(fw_mode_ids))
check("firmware/panel mode ids are byte-identical with the Web modes",
      fw_mode_ids == web_mode_ids, f"shared={fw_mode_ids} web={web_mode_ids}")
check("mode enumeration exposes a count and an index accessor",
      "kDisplayModeCount" in CORE and "DisplayMode displayModeAt(" in CORE)

# ------------------------------------------------------------------- icons
icon_fn = between(CORE, "const char* iconIdName(", "// Display-independent semantic colour")
icon_ids = sorted(re.findall(r'return "([a-z]+)";', icon_fn))
web_symbols = sorted(re.findall(r'<symbol id="i-([a-z]+)"', HTML))
check("four abstract icon ids", icon_ids == ["humidity", "pressure", "temperature", "wifi"],
      str(icon_ids))
# The Web page names its own SVG assets; the shared identity is the semantic
# IconId. The documented alias table (README "Design-token identity map") is
# what makes the two sets 1:1.
ICON_ALIASES = {"temp": "temperature", "humidity": "humidity", "pressure": "pressure",
                "network": "wifi"}
check("each shared icon id maps onto exactly one Web SVG symbol",
      sorted(ICON_ALIASES.get(s, s) for s in web_symbols) == icon_ids,
      f"shared={icon_ids} web={web_symbols}")
check("the identity map is documented",
      all(f"`i-{web}`" in (SIM / "README.md").read_text(encoding="utf-8")
          for web in web_symbols) if SIM is not None else True)

# ------------------------------------------------------------ colour tokens
token_fn = between(CORE, "const char* colorTokenName(", "ColorToken colorTokenForRole(")
token_names = sorted(re.findall(r'return "([A-Z_]+)";', token_fn))
check("12 tokens with the frozen vocabulary", token_names == sorted([
    "TEMP_COLD", "TEMP_COMFORT", "TEMP_HOT", "HUMIDITY_LOW", "HUMIDITY_COMFORT", "HUMIDITY_HIGH",
    "PRESSURE_LOW", "PRESSURE_NORMAL", "PRESSURE_HIGH", "DATA_FRESH", "DATA_STALE", "DATA_ERROR"]),
    str(token_names))

theme = between(CORE, "constexpr ThemeEntry kReferenceTheme[]", "uint32_t themeRgb(")
theme_entries = re.findall(r'\{ColorToken::(k\w+),\s*0x([0-9A-Fa-f]{6})\}', theme)
web_vars = dict((m[0], m[1].lstrip("#").upper()) for m in
                re.findall(r'--(role-[a-z-]+|state-[a-z]+):\s*#([0-9A-Fa-f]{6})', HTML))
expected = {
    "kTempCold": "role-temp-cold", "kTempComfort": "role-temp-comfort", "kTempHot": "role-temp-hot",
    "kHumidityLow": "role-humidity-low", "kHumidityComfort": "role-humidity-comfort",
    "kHumidityHigh": "role-humidity-high", "kPressureLow": "role-pressure-low",
    "kPressureNormal": "role-pressure-normal", "kPressureHigh": "role-pressure-high",
    "kDataFresh": "state-fresh", "kDataStale": "state-stale", "kDataError": "state-error",
}
mismatch = [f"{tok}: theme=#{rgb} web={web_vars.get(expected[tok], '?')}"
            for tok, rgb in theme_entries if web_vars.get(expected.get(tok, "")) != rgb.upper()]
check("12 theme entries", len(theme_entries) == 12, str(len(theme_entries)))
check("theme RGB matches the Web stylesheet exactly", not mismatch, "; ".join(mismatch))
check("LVGL maps tokens through the shared theme, not literals",
      "themeRgb(ThemeId::kReferenceDark, token)" in UI_CODE)
check("the LVGL layer introduces no new colour literals",
      len(re.findall(r"0x[0-9A-Fa-f]{6}", between(UI, "inline lv_color_t token_color(",
                                                  "inline void apply_glass("))) == 0)

# --------------------------------------------------------------- geometry
check("four geometry profiles",
      all(p in CORE for p in ['"320x240"', '"480x320"', '"800x480"', '"desktop"']))
layout_body = between(CORE, "DisplayLayout computeLayout(", "bool layoutFitsScreen(")
pixel_literals = re.findall(r'(?<![\w.])(320|240|480|800|1280)(?![\w.])', layout_body)
check("layout has no panel-size literals", not pixel_literals, str(pixel_literals))
check("layout uses adaptive fractions",
      "scaledPx(" in layout_body and "kMarginOfShortSide" in layout_body)
check("layout invariant checker exists", "bool layoutFitsScreen(" in CORE)
check("gauge sweep is geometry, not screen size", "kGaugeSweepDeg" in CORE)
check("history lanes have their own label column",
      "history_label[3]" in CORE and "history_lane[3]" in CORE)
check("layout invariant covers the history rects",
      all(f"history_label[{i}]" in CORE and f"history_lane[{i}]" in CORE for i in range(3)))
check("every metric has a unit rectangle",
      "LayoutRect unit;" in CORE and "cell.unit =" in layout_body)

# ------------------------------------------------------- one UI implementation
for name in ["ui_build(", "ui_set_mode(", "ui_update(", "ui_update_history("]:
    check(f"shared UI exposes {name}", name in UI)
build_fn = between(UI, "inline void ui_build(", "inline void ui_set_mode(")
draw_fn = between(UI, "inline void draw_icon(", "struct MetricWidgets")
check("widget objects are created only while building the tree or drawing an icon",
      UI_CODE.count("lv_obj_create(") == build_fn.count("lv_obj_create(") + draw_fn.count("lv_obj_create("),
      f"total={UI_CODE.count('lv_obj_create(')} build={build_fn.count('lv_obj_create(')} "
      f"draw_icon={draw_fn.count('lv_obj_create(')}")
check("ui_build creates the fixed tree in one pass",
      build_fn.count("lv_obj_create(") == 9 and "make_text(" in build_fn and
      "lv_button_create(" in build_fn)
check("no per-update object creation in the update path",
      "lv_obj_create(" not in between(UI, "inline void ui_update(Ui& ui, const PresentationModel& model,",
                                     "ui_count_objects("))
check("icons come from shared drawing code (primitives + a font glyph)",
      "inline void draw_icon(" in UI and "LV_SYMBOL_TINT" in UI and "icon_points_pool" in UI)
check("labels are looked up by translation key in the shared catalog",
      "localized_metric_label(" in UI and "TextKey::kTemperature" in UI)
check("the language screen exists in the shared UI",
      "lang_button[kLocaleCount]" in UI and "language_button_event_cb" in UI and
      "ui_apply_locale(" in UI and "ui_apply_text_locale(" in UI)
check("no per-call if/else translation inside the renderers",
      '"ru"' not in UI and '"en"' not in UI)
check("shared fonts are generated, guarded and Cyrillic-capable",
      (ROOT / "lib" / "ws_fonts" / "ws_font_14.c").exists() and
      "#if defined(WS_LVGL_FONTS)" in (ROOT / "lib" / "ws_fonts" / "ws_font_14.c").read_text(encoding="utf-8") and
      "range_start = 1040" in (ROOT / "lib" / "ws_fonts" / "ws_font_14.c").read_text(encoding="utf-8"))
check("the firmware does not compile the fonts without LVGL",
      "ws_fonts" not in (ROOT / "platformio.ini").read_text(encoding="utf-8"))
check("history series buffer matches the demo provider size", "points[3][192]" in UI)

# ------------------------------------------------- display subsystem (firmware)
display = between(SRC, "// DISPLAY SUBSYSTEM", "// Sensor layer: shared I2C bus diagnostics")
display_code = strip_comments(display)
for forbidden in ["Wire.", "g_bmp.", "g_dht.", "Adafruit_BMP280", "DHTesp", "WiFi.", "g_http.",
                  "WebServer", "buildWeatherJson"]:
    check(f"display layer free of {forbidden!r} (code)", forbidden not in display_code)
renderers = strip_comments(between(display, "void renderNumeric(", "void logDisplayFrame("))
for threshold in ["kTempColdBelowC", "kTempHotAboveC", "kHumidityLowBelowPct",
                  "kHumidityHighAbovePct", "kPressureLowBelowHpa", "kPressureHighAboveHpa"]:
    check(f"renderers do not recompute {threshold}", threshold not in renderers)
check("NONE / ILI9341 macros exist",
      "#define DISPLAY_BACKEND_NONE 0" in SRC and "#define DISPLAY_BACKEND_ILI9341 1" in SRC)
check("default backend is NONE", "#define DISPLAY_BACKEND DISPLAY_BACKEND_NONE" in SRC)
check("ILI9341 reserved with a compile-time stop",
      "DISPLAY_BACKEND_ILI9341 is not implemented yet" in SRC)
check("backend exposes the paint primitives",
      all(p in display for p in ["void drawCard(", "void drawIcon(", "void drawText(",
                                 "void drawTrack(", "void drawGauge("]))
check("renderer functions have the required names",
      all(f"void {n}(" in display for n in
          ["renderNumeric", "renderHorizontal", "renderBars", "renderGauges", "renderDisplay"]))
check("mode switching is application state",
      "g_display_mode" in SRC and "void setDisplayMode(" in display)
check("display refresh decoupled from sensors and HTTP",
      "kDisplayUpdateMs" in SRC and "void displayService()" in display and "displayService();" in SRC)
check("headless self-test covers every mode and every profile",
      "displayModeAt(m)" in display and "kDisplayModeCount" in display and
      "layoutFitsScreen(" in display)
check("no panel GPIO / no TFT library referenced", "TFT_" not in SRC and "SPI.begin" not in SRC)

# --------------------------------------------------------- frozen contracts
api_keys = re.findall(r'jsonAppend\(writer, ",?\\"([a-z0-9_]+)\\":', SRC)
check("API v1 unchanged", api_keys == ["temperature_c", "humidity_pct", "pressure_hpa", "pressure_pa",
                                      "bmp280_valid", "dht11_valid", "bmp280_age_ms",
                                      "dht11_age_ms", "uptime_ms"])
check("pinout unchanged", all(p in SRC for p in
      ["kI2cSda = 21", "kI2cScl = 22", "kDht11Data = 4", "kBmp280Address = 0x76"]))
check("polling intervals unchanged", "kBmp280PollMs = 5000" in SRC and "kDht11PollMs = 2500" in SRC)
check("Wi-Fi contract unchanged", "WIFI_STATIC_IP" in SRC and "WIFI_USE_STATIC_IP" in SRC)
check("Web page is still embedded", "board_build.embed_files" in INI and
      "web/index.html" in INI)

# ------------------------------------------------------------- simulator side
if SIM is not None:
    sim_main = (SIM / "src" / "sim_main.cpp").read_text(encoding="utf-8")
    smoke = (SIM / "src" / "smoke_main.cpp").read_text(encoding="utf-8")
    demo = (SIM / "src" / "demo_weather.cpp").read_text(encoding="utf-8")
    live = (SIM / "src" / "live_source.cpp").read_text(encoding="utf-8")
    api = (SIM / "src" / "api_weather.cpp").read_text(encoding="utf-8")
    http = (SIM / "src" / "http_client.cpp").read_text(encoding="utf-8")
    history = (SIM / "src" / "history_buffer.h").read_text(encoding="utf-8")
    demo_header = (SIM / "src" / "demo_weather.h").read_text(encoding="utf-8")
    live_header = (SIM / "src" / "live_source.h").read_text(encoding="utf-8")
    cmake = (SIM / "CMakeLists.txt").read_text(encoding="utf-8")
    readme = (SIM / "README.md").read_text(encoding="utf-8") if (SIM / "README.md").exists() else ""
    check("simulator reads the shared headers from the firmware tree",
          "FIRMWARE_DIR" in cmake and "LV_BUILD_CONF_DIR" in cmake)
    check("simulator pins LVGL 9.5.0", "v9.5.0" in cmake)
    check("simulator defines WS_PC_SIMULATOR for the LVGL target",
          "WS_PC_SIMULATOR" in cmake)
    check("simulator runs the shared UI, not a copy",
          "ws_ui::ui_build(" in sim_main and "ws_ui::ui_update(" in sim_main)
    check("simulator builds the UI on its own display's screen",
          "lv_display_get_screen_active(" in sim_main)
    check("screenshots are read from LVGL's draw buffer",
          "lv_display_get_buf_active(" in sim_main and "SDL_RenderReadPixels" not in sim_main)
    demo_scenarios = re.findall(r"case Scenario::(k\w+): return \"([A-Z_]+)\";", demo)
    check("eleven demo scenarios (10 interactive + the fixed GOLDEN dataset)",
          len(demo_scenarios) == 11, str(len(demo_scenarios)))
    check("demo provider is deterministic (no rand)",
          "rand(" not in demo and "sinf(" in demo)
    check("history is seeded so the view is meaningful immediately",
          "seed_history" in demo and "history_.configure(" in demo and
          "capacity_for_window" in demo)
    check("history retention spans 10 s to 24 h",
          all(name in demo for name in ['"10 s"', '"1 min"', '"5 min"', '"1 h"', '"24 h"']))
    check("simulator CLI offers matrix/golden/compare/screenshot/performance",
          all(f'"{option}"' in sim_main for option in
              ["--matrix", "--golden-dir", "--compare-dir", "--screenshot", "--resolution", "--scale",
               "--retention", "--performance", "--no-animations"]))
    golden_names = ("320x240_language_ru", "320x240_language_en", "320x240_numeric_ru",
                    "320x240_numeric_en", "320x240_horizontal_ru", "320x240_horizontal_en",
                    "320x240_bars_ru", "320x240_bars_en", "320x240_gauges_ru",
                    "320x240_gauges_en", "320x240_history_ru", "320x240_history_en")
    check("golden references cover RU/EN x (language + 5 modes) as committed PNGs",
          all((SIM / "golden" / f"{name}.png").exists() for name in golden_names),
          str([n for n in golden_names if not (SIM / "golden" / f"{n}.png").exists()]))
    check("multi-resolution geometry set is still present (3 x 5 frames)",
          len([p for p in (SIM / "golden").glob("*.png") if p.name[0].isdigit() and "x" in p.name and "_" not in p.name]) == 15)
    check("golden tooling is committed in the simulator workspace",
          (SIM / "golden.cmd").exists() and (SIM / "tools" / "golden.py").exists())
    check("the golden command compares against references instead of regenerating",
          "compare" in (SIM / "golden.cmd").read_text(encoding="utf-8") and
          "--golden-shots" in (SIM / "golden.cmd").read_text(encoding="utf-8"))
    check("minimal smoke test is a separate executable",
          "weather_smoke" in cmake and "src/smoke_main.cpp" in cmake)
    check("smoke test proves rendering, animation, mouse and keyboard",
          all(marker in smoke for marker in
              ["rendering:", "animation:", "mouse:", "keyboard:", "--self-test"]))
    check("smoke mouse events carry the LVGL window id",
          "SDL_GetWindowID(lv_sdl_window_get_window(" in smoke)
    check("keyboard input is bound to an LVGL group",
          "lv_indev_set_group(" in smoke and "ui_attach_input_group" in sim_main)
    check("shared UI exposes the nav strip and an input-group hook",
          "nav_button[kDisplayModeCount]" in UI and "ui_attach_input_group" in UI)
    check("mode buttons are placed relative to the nav container",
          "place_in(button, layout.nav_button[i], layout.nav)" in UI)
    check("animations are LVGL-native (no desktop library)",
          all(marker in UI for marker in ["lv_anim_init(", "lv_anim_start(", "anim_value_text"]) and
          "LV_ANIM_ON" in UI)
    check("animations can be disabled for deterministic captures",
          "ui.animations" in UI and "--no-animations" in sim_main and
          "session.ui.animations = false" in sim_main)
    check("performance report measures objects and frame time",
          "run_performance" in sim_main and "draw_buffer_bytes" in sim_main)

    # --------------------------------------------------------- mouse input path
    check("polled SDL events are forwarded to LVGL's SDL driver",
          "lv_sdl_mouse_handler(&forwarded)" in sim_main and
          "lv_sdl_keyboard_handler(&forwarded)" in sim_main)
    check("polled events are never dropped before LVGL sees them",
          "process_sdl_event(" in sim_main and "pump_sdl_events(" in sim_main and
          "SDL_PollEvent(&event)" in sim_main)
    check("pointer indev is not hijacked (the driver finds it by read_cb identity)",
          "lv_indev_set_read_cb" not in sim_main)
    check("decorative containers are passive in the shared UI",
          "inline void make_passive(" in UI and
          "lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);" in UI and
          "make_passive(ui.content);" in UI and "make_passive(ui.nav);" in UI and
          "make_passive(obj);" in UI)
    check("mode buttons stay clickable (the only interactive objects)",
          "lv_button_create(" in UI and "LV_EVENT_CLICKED, &ui" in UI)
    check("input test injects real SDL mouse events and checks the mode",
          "SDL_MOUSEBUTTONDOWN" in sim_main and "SDL_MOUSEBUTTONUP" in sim_main and
          "SDL_MOUSEMOTION" in sim_main and "run_input_selftest" in sim_main)
    check("input test covers zoom, overlay, data sources and keyboard",
          all(marker in sim_main for marker in
              ["mouse click at zoom switches mode", "mouse click switches mode with the overlay",
               "mouse click switches mode in every data source", "keyboard switches mode"]))
    check("input debug tracing is opt-in only",
          "g_input_debug" in sim_main and '"--input-debug"' in sim_main and
          "WSIM_INPUT_DEBUG" in sim_main and "INPUT_DEBUG" in sim_main)
    check("no separate mouse-only mode state",
          "session.ui.mode" in sim_main and "ui_set_mode(" in sim_main and
          "g_mouse_mode" not in sim_main)
    check("button rectangles are printed for the hit-test diagnostics",
          "nav_button[kDisplayModeCount]" in UI and "lv_indev_search_obj(" in sim_main)
    check("input regression is part of the headless regression script",
          "--input-selftest" in (SIM / "regression.cmd").read_text(encoding="utf-8"))

    # ------------------------------------------------------------- LIVE mode
    check("LIVE is the default data source",
          "DataSource source = DataSource::kLive;" in sim_main and
          '"--live"' in sim_main and '"--demo"' in sim_main)
    check("the API endpoint is configurable with the required default",
          "http://192.168.1.111/api/weather" in sim_main and '"--url"' in sim_main)
    check("Windows-native HTTP client (WinHTTP, no curl/Node/browser)",
          "winhttp.h" in http and "WinHttpOpen" in http and
          not any(token in http for token in ["libcurl", "node", "chromium", "WinInet"]))
    check("WinHTTP is linked only into the simulator target",
          "winhttp" in cmake and "target_link_libraries(weather_station_simulator PRIVATE winhttp)"
          in cmake)
    check("no proxy for direct LAN access",
          "WINHTTP_ACCESS_TYPE_NO_PROXY" in http)
    check("network work runs on a worker thread, not the UI thread",
          "std::thread" in live and "worker_loop" in live and
          "std::thread(&LiveSource::worker_loop, this)" in live)
    check("thread-safe handoff through a sequence-numbered snapshot",
          "published_sequence_" in live and "take_snapshot" in live and "std::mutex" in live)
    check("polling interval defaults to ~1.5 s",
          "kPollIntervalMs = 1500" in sim_main)
    check("one request in flight (sequential worker loop, no queue)",
          "client_.get(request_timeout_ms_)" in live and "sleep_for" in live)
    check("API v1 contract validated field by field (9 keys, frozen order)",
          all(f'"{key}"' in api for key in
              ["temperature_c", "humidity_pct", "pressure_hpa", "pressure_pa", "bmp280_valid",
               "dht11_valid", "bmp280_age_ms", "dht11_age_ms", "uptime_ms"]) and
          "kApiV1KeyCount = 9" in (SIM / "src" / "api_weather.h").read_text(encoding="utf-8"))
    check("null/absent values become NAN, never 0",
          "NAN" in api and "is_null" in api and
          "model.temperature_c.value = model.temperature_c.available ? (float)raw.temperature : NAN;"
          in api)
    check("valid=false is carried through to the shared classifier",
          "model.temperature_c.valid = model.temperature_c.available && raw.bmp_valid;" in api)
    check("the network layer is separate from the presentation layer",
          "buildPresentationModel" not in live and "buildPresentationModel" not in api and
          "lv_" not in live and "lv_" not in api and "lv_" not in http)
    check("LIVE and DEMO share one history buffer implementation",
          (SIM / "src" / "history_buffer.h").exists() and "class HistoryBuffer" in history and
          "pc::HistoryBuffer history_" in demo_header and
          "HistoryBuffer history_" in live_header)
    check("history is fed from the same response (no extra request)",
          "history().push_model(snapshot.model" in sim_main)
    check("offline keeps the last snapshot and ages it",
          "aged_model(" in sim_main and "have_live_model" in sim_main)
    check("LIVE failure/recovery test exists",
          "--live-selftest" in sim_main and "reconnect(" in live)
    check("live probe prints status/latency/values",
          "--live-probe" in sim_main and "latency=%.1f ms" in sim_main)
    check("status overlay shows source/endpoint/http/latency/age through the catalog",
          all(marker in UI for marker in
              ["kSource", "kEndpoint", "kHttp", "kLatency", "kLastUpdate", "kDataAge", "kPolls"]))
    check("header badge shows the source and the localized connection word",
          'snprintf(badge, sizeof(badge), "%s %s", source, connection_text)' in UI and
          "tr(TextKey::kOnline" in UI)
    check("no Wi-Fi password or secret is printed by the simulator",
          REAL_WIFI_PASSWORD not in sim_main and REAL_WIFI_PASSWORD not in readme and "WIFI_PASSWORD" not in readme)

    # Wrapped prose and block quotes: compare with whitespace normalised.
    readme_flat = re.sub(r"\s+", " ", readme.replace(">", " "))
    check("simulator documents the hardware caveat",
          "Simulator validates application/UI behavior and LVGL rendering, but final hardware "
          "validation is required for SPI timing, colors, orientation, backlight, and "
          "panel-specific behavior." in readme_flat)
    check("simulator README lists the reproducible toolchain with versions",
          all(marker in readme_flat for marker in
              ["PREREQUISITES", "INSTALL", "CONFIGURE", "BUILD", "RUN", "SCREENSHOT",
               "2.32.10", "9.5.0"]))
    check("simulator README separates validated from non-guaranteed behaviour",
          "cannot guarantee" in readme_flat and "panel colours" in readme_flat)
    check("simulator README documents LIVE mode and WinHTTP",
          "LIVE mode (real ESP32" in readme_flat and "WinHTTP" in readme_flat)
    check("simulator README documents the manual and automated mouse tests",
          "MANUAL mouse test" in readme_flat and "Automated mouse test" in readme_flat)
    check("helper scripts exist",
          all((SIM / name).exists() for name in
              ["check_env.cmd", "build.cmd", "run.cmd", "smoke.cmd", "live.cmd", "regression.cmd"]))

failed = [r for r in results if not r[1]]
width = max(len(r[0]) for r in results)
for name, ok, detail in results:
    print(f"  [{'OK' if ok else 'FAIL'}] {name:<{width}} {'' if ok else detail}")
print()
print(f"display/static checks: {len(results) - len(failed)} passed, {len(failed)} failed")
sys.exit(1 if failed else 0)
