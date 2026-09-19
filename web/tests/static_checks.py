"""Static checks for the ESP32 Weather Station stabilisation iteration.

Verifies the frozen contracts (API, pinout, Wi-Fi, polling) and that the
firmware-side semantic vocabulary matches web/index.html exactly — the property
that makes both renderers (and a future LVGL one) display-agnostic.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1])
SRC_MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
# The display vocabulary, the semantics and the presentation model moved into the
# shared header (used by both the firmware and the PC simulator). Checks that
# look for them search the firmware sources as a whole.
CORE = (ROOT / "include" / "weather_core.h").read_text(encoding="utf-8")
SRC = SRC_MAIN + "\n" + CORE
HTML = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
INI = (ROOT / "platformio.ini").read_text(encoding="utf-8")
EXAMPLE = (ROOT / "include" / "secrets.example.h").read_text(encoding="utf-8")
GITIGNORE = (ROOT / ".gitignore").read_text(encoding="utf-8")

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


def num(source, pattern):
    match = re.search(pattern, source)
    return float(match.group(1)) if match else None


# ---------------------------------------------------------------- 1. security
check("example header has placeholder password", 'WIFI_PASSWORD "CHANGE_ME"' in EXAMPLE)
check("example header keeps the project SSID", 'WIFI_SSID "Beeline_2G_F121F9"' in EXAMPLE)
check("no real password in the example header", REAL_WIFI_PASSWORD not in EXAMPLE)
check("real password absent from firmware sources",
      REAL_WIFI_PASSWORD not in SRC and REAL_WIFI_PASSWORD not in HTML and REAL_WIFI_PASSWORD not in INI)
check(".gitignore excludes include/secrets.h", "include/secrets.h" in GITIGNORE)

# ------------------------------------------------------------- 2. API contract
api_keys = re.findall(r'jsonAppend\(writer, ",?\\"([a-z0-9_]+)\\":', SRC)
expected_keys = ["temperature_c", "humidity_pct", "pressure_hpa", "pressure_pa",
                 "bmp280_valid", "dht11_valid", "bmp280_age_ms", "dht11_age_ms", "uptime_ms"]
check("API v1 field names and order unchanged", api_keys == expected_keys, str(api_keys))
check("API version constant present (v1)", "kWeatherApiVersion = 1" in SRC)
check("API handler uses WeatherModel", "const WeatherModel model = weatherModel();" in SRC)
check("routes unchanged", all(route in SRC for route in
      ['g_http.on("/", HTTP_GET, handleRoot)', 'g_http.on("/health", HTTP_GET, handleHealth)',
       'g_http.on("/api/weather", HTTP_GET, handleWeatherApi)']))
check("no new endpoints / no SSE / no WebSocket server",
      "/api/stream" not in SRC and "EventSource" not in SRC and
      len(re.findall(r'g_http\.on\(', SRC)) == 4)

# -------------------------------------------------- 3. hardware + network frozen
check("pinout SDA=21 SCL=22 DHT11=4", all(p in SRC for p in
      ["kI2cSda = 21", "kI2cScl = 22", "kDht11Data = 4"]))
check("BMP280 address 0x76 on I2C", "kBmp280Address = 0x76" in SRC and "I2C" in SRC)
check("static IP contract untouched",
      all(x in SRC for x in ["WIFI_STATIC_IP", "WIFI_USE_STATIC_IP", "WIFI_GATEWAY", "WIFI_SUBNET",
                             "WIFI_DNS1", "WIFI_DNS2"]))
check("no DHCP fallback", "WiFi.config(local_ip, gateway, subnet, dns1, dns2)" in SRC)
check("poll intervals unchanged", "kBmp280PollMs = 5000" in SRC and "kDht11PollMs = 2500" in SRC)
check("gateway probe + self-test timeouts bounded",
      "kGatewayProbeTimeoutMs = 400" in SRC and "kHttpSelfTestTimeoutMs = 700" in SRC)

# ------------------------------------------------- 4. model / boundary structure
check("WeatherModel with per-metric MetricValue",
      "struct WeatherModel" in SRC and SRC.count("MetricValue ") >= 3)
check("model carries source/valid/age/uptime",
      all(field in SRC for field in ["const char* source", "bool valid", "uint32_t age_ms",
                                     "uint32_t uptime_ms"]))
check("temperature+pressure from BMP280, humidity from DHT11",
      'model.temperature_c.source = "BMP280"' in SRC and
      'model.pressure_hpa.source = "BMP280"' in SRC and
      'model.humidity_pct.source = "DHT11"' in SRC)
check("PresentationModel + builder exist",
      "struct PresentationModel" in SRC and "buildPresentationModel(const WeatherModel&" in SRC)
check("renderers cannot see sensors (no sensor access in HTTP layer)",
      "g_bmp." not in SRC_MAIN[SRC_MAIN.index("void handleWeatherApi()"):SRC_MAIN.index("void handleNotFound()")])

# ------------------------------------------- 5. vocabulary shared with the Web UI
fw_roles = set(re.findall(r'constexpr const char\* k\w+ = "([a-z-]+)";', SRC))
web_roles = set(re.findall(r": '([a-z-]+)'", HTML))
expected_roles = {"temp-cold", "temp-comfort", "temp-hot", "humidity-low", "humidity-comfort",
                  "humidity-high", "pressure-low", "pressure-normal", "pressure-high"}
check("firmware defines exactly the 9 role ids", fw_roles == expected_roles,
      "missing=" + str(expected_roles - fw_roles))
check("web UI uses the same 9 role ids", expected_roles <= web_roles,
      "missing=" + str(expected_roles - web_roles))
check("data states identical", all(state in SRC for state in ["kFresh", "kStale", "kError"]) and
      all(f"'{s}'" in HTML for s in ["FRESH", "STALE", "ERROR"]))

pairs = [
    ("temperature cold threshold", num(SRC, r"kTempColdBelowC = ([\d.]+)f"),
     num(HTML, r"coldBelowC: ([\d.]+)")),
    ("temperature hot threshold", num(SRC, r"kTempHotAboveC = ([\d.]+)f"),
     num(HTML, r"hotAboveC: ([\d.]+)")),
    ("humidity low threshold", num(SRC, r"kHumidityLowBelowPct = ([\d.]+)f"),
     num(HTML, r"lowBelowPct: ([\d.]+)")),
    ("humidity high threshold", num(SRC, r"kHumidityHighAbovePct = ([\d.]+)f"),
     num(HTML, r"highAbovePct: ([\d.]+)")),
    ("pressure low threshold", num(SRC, r"kPressureLowBelowHpa = ([\d.]+)f"),
     num(HTML, r"lowBelowHpa: ([\d.]+)")),
    ("pressure high threshold", num(SRC, r"kPressureHighAboveHpa = ([\d.]+)f"),
     num(HTML, r"highAboveHpa: ([\d.]+)")),
    ("temperature range min", num(SRC, r"kTempMinC = (-?[\d.]+)f"), num(HTML, r"min: (-?[\d.]+), max: 40")),
    ("temperature range max", num(SRC, r"kTempMaxC = ([\d.]+)f"), num(HTML, r"min: -?[\d.]+, max: ([\d.]+) \}, ticks: \[-10")),
    ("humidity range max", num(SRC, r"kHumidityMaxPct = ([\d.]+)f"), num(HTML, r"max: ([\d.]+) \}, ticks: \[0, 25")),
    ("pressure range min", num(SRC, r"kPressureMinHpa = ([\d.]+)f"), num(HTML, r"min: ([\d.]+), max: 1050")),
    ("pressure range max", num(SRC, r"kPressureMaxHpa = ([\d.]+)f"), num(HTML, r"max: ([\d.]+) \}, ticks: \[950")),
    ("temperature freshness window", num(SRC, r"kTempMaxAgeMs = (\d+)"),
     num(HTML, r"temperature: (\d+)")),
    ("humidity freshness window", num(SRC, r"kHumidityMaxAgeMs = (\d+)"),
     num(HTML, r"humidity: (\d+)")),
    ("pressure freshness window", num(SRC, r"kPressureMaxAgeMs = (\d+)"),
     num(HTML, r"pressure: (\d+)")),
]
for name, firmware, web in pairs:
    check(name + " matches (firmware = web)", firmware is not None and firmware == web,
          f"firmware={firmware} web={web}")

# --------------------------------------------------- 6. logging mode + Web regression
check("WS_DEBUG_LOGS default 0 in platformio.ini", "-DWS_DEBUG_LOGS=0" in INI)
check("log level plumbed through the firmware", "#if WS_DEBUG_LOGS" in SRC and
      "#define WS_LOG_DEBUG" in SRC)
check("five Web modes defined",
      "const MODES = ['numeric', 'horizontal', 'bars', 'gauges', 'history']" in HTML)
check("web default mode numeric", "const DEFAULT_MODE = 'numeric'" in HTML)
check("web poll interval unchanged (1500 ms / 5000 hidden)",
      "pollIntervalMs: 1500" in HTML and "pollIntervalHiddenMs: 5000" in HTML)
# The SVG namespace URI is a namespace identifier, not a network resource.
html_without_ns = HTML.replace("http://www.w3.org/2000/svg", "").replace("http://www.w3.org/1999/xlink", "")
check("no external assets in the page", not re.search(r'https?://|//cdn|@import', html_without_ns))

# ------------------------------------------------------------------- reporting
failed = [r for r in results if not r[1]]
width = max(len(r[0]) for r in results)
for name, ok, detail in results:
    print(f"  [{'OK' if ok else 'FAIL'}] {name:<{width}} {'' if ok else detail}")
print()
print(f"static checks: {len(results) - len(failed)} passed, {len(failed)} failed")
sys.exit(1 if failed else 0)
