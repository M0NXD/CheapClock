#include <LovyanGFX.hpp>
#include <bb_captouch.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <SD.h>
#include <FS.h>
#include <SPI.h>

// ── Firmware version ────────────────────────────────────────
#define FW_VERSION "1.0.9"
#define OTA_VERSION_URL "https://arc.ntwk.co.uk/CheapClock/version.txt"
#define OTA_FIRMWARE_URL "https://arc.ntwk.co.uk/CheapClock/CheapClock.ino.bin"

// ── WiFi ─────────────────────────────────────────────────────
const char* SSID     = "";
const char* PASSWORD = "";
const char* DATA_URL = "https://www.hamqsl.com/solarxml.php";
const unsigned long REFRESH_MS = 60000UL;

// ── LDR (onboard photoresistor) ──────────────────────────────
#define LDR_PIN 34   // ADC1_CH6, input-only

// ── SD Card (VSPI: CS=5, MOSI=23, MISO=19, SCK=18) ─────────
#define SD_CS   5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK  18
SPIClass sdSPI(HSPI);
bool sdAvailable = false;

// ── Persistent settings ──────────────────────────────────────
char myCallsign[16] = "";
char myGrid[8]      = "";
uint8_t brightness  = 255;
char sdSSID[33]     = "";
char sdPassword[65] = "";
bool    autoCycle      = false;
bool    autoBrightness = false;
int8_t  tzOffset       = 0;      // UTC offset in whole hours (-12 to +14)
uint8_t cycleSpeed     = 15;     // auto-cycle interval in seconds (5-120)
uint8_t screenTimeout  = 0;      // minutes, 0 = disabled
char    owmKey[40]     = "";     // OpenWeatherMap API key (set in config.txt)
bool    screenAsleep   = false;
unsigned long lastActivity = 0;
bool    rotateDisplay  = false;  // rotate screen 180 degrees

// ── Colour palette (RGB565) ───────────────────────────────────
#define C_BG    0x000000
#define C_HDR   0x002530
#define C_CYAN  0x00FFFF
#define C_GRAY  0xC618
#define C_WHITE 0xFFFFFF
#define C_GREEN 0x00FF00
#define C_AMBER 0xFFDD44
#define C_RED   0xFFAA44
#define C_DIV   0x202830
#define C_DIM   0xB0B0B0
#define C_MENU_BG   0x101820
#define C_MENU_HDR  0x003050
#define C_KEY_BG    0x1A2535
#define C_KEY_PRESS 0x0050A0
#define C_KEY_SPL   0x003060
#define C_PASS_BG   0x0A1020
#define C_NET_HL    0x004080

// ── Display class (ST7789 SPI) ────────────────────────────────
class LGFX_JustDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX_JustDisplay(void) {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = VSPI_HOST;  cfg.spi_mode    = 0;
      cfg.freq_write  = 27000000;   cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;      cfg.use_lock    = true;
      cfg.dma_channel = 1;
      cfg.pin_sclk    = 14;  cfg.pin_mosi = 13;
      cfg.pin_miso    = -1;  cfg.pin_dc   = 2;
      _bus.config(cfg);  _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 15;   cfg.pin_rst          = -1;
      cfg.pin_busy         = -1;   cfg.panel_width      = 240;
      cfg.panel_height     = 320;  cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;    cfg.dummy_read_bits  = 1;
      cfg.readable  = false;  cfg.invert     = false;
      cfg.rgb_order = false;  cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX_JustDisplay tft;
BBCapTouch touch;

// ── Solar data ───────────────────────────────────────────────
struct SolarData {
  char solarflux[8]    = "---";
  char sunspots[8]     = "---";
  char kindex[6]       = "--";
  char aindex[6]       = "--";
  char xray[8]         = "--";
  char solarwind[10]   = "---";
  char geomagfield[16] = "---";
  char signalnoise[12] = "---";
  char updated[36]     = "";
  char bands[4][2][8];
  SolarData() {
    for (int b = 0; b < 4; b++) {
      strcpy(bands[b][0], "---");
      strcpy(bands[b][1], "---");
    }
  }
};

SolarData solar;
unsigned long lastFetch = 0;

// ── DX Cluster data ───────────────────────────────────────────
#define DX_URL "http://dxlite.g7vjr.org/?json=1"
#define MAX_SPOTS 10
struct DXSpot {
  char freq[12]     = "---";
  char callsign[16] = "---";
};
DXSpot   dxSpots[MAX_SPOTS];
int      dxCount        = 0;
unsigned long lastDXFetch = 0;

// ── Contest Calendar data ─────────────────────────────────────
#define CONTEST_URL "https://www.contestcalendar.com/calendar.rss"
#define MAX_CONTESTS 14
#define PARSE_CONTESTS 50   // parse up to 50, keep only active/upcoming
#define CONTEST_REFRESH_MS 3600000UL   // 1 hour

struct Contest {
  char name[32];
  char dates[20];
  char mode[8];
  int  endMonth;  // 1-12, 0=unknown
  int  endDay;    // 1-31
  int  endHour;   // 0-23, -1=end of day
};
Contest       contests[MAX_CONTESTS];
int           contestCount     = 0;
unsigned long lastContestFetch = 0;

// ── POTA data ────────────────────────────────────────────────
#define POTA_URL "https://api.pota.app/spot/activator"
#define MAX_POTA 10
struct POTASpot {
  char parkRef[12]   = "---";
  char callsign[16]  = "---";
  char freq[12]      = "---";
  char mode[6]       = "---";
};
POTASpot   potaSpots[MAX_POTA];
int        potaCount       = 0;
unsigned long lastPOTAFetch = 0;

// ── SOTA data ────────────────────────────────────────────────
#define SOTA_URL    "https://api2.sota.org.uk/api/spots/30"
#define MAX_SOTA    10
struct SOTASpot {
  char summitCode[12] = "---";
  char callsign[16]   = "---";
  char freq[12]       = "---";
  char mode[6]        = "---";
};
SOTASpot     sotaSpots[MAX_SOTA];
int          sotaCount      = 0;
unsigned long lastSOTAFetch = 0;

// ── WSPR band activity ───────────────────────────────────────
#define WSPR_URL "https://db1.wspr.live/?query=SELECT%20band%2Ccount()%20FROM%20wspr.rx%20WHERE%20time%3E(NOW()-INTERVAL%201%20HOUR)%20GROUP%20BY%20band%20FORMAT%20TSV"
#define WSPR_REFRESH_MS 300000UL  // 5 minutes
const int    WSPR_BAND_NUMS[11]  = {1, 3, 5, 7, 10, 14, 18, 21, 24, 28, 50};
const char*  WSPR_BAND_NAMES[11] = {"160","80","60","40","30","20","17","15","12","10","6m"};
int          wsprCounts[11]      = {0};
unsigned long lastWsprFetch      = 0;

// ── Space weather alerts (NOAA SWPC) ─────────────────────────
#define SWPC_URL "https://services.swpc.noaa.gov/products/alerts.json"
#define SWPC_REFRESH_MS 600000UL  // 10 minutes
int  swpcAlertCount    = 0;
char swpcAlertCode[10] = "";
unsigned long lastSWPCFetch = 0;

// ── OpenWeatherMap ────────────────────────────────────────────
#define WX_REFRESH_MS 1800000UL   // 30 minutes
struct WeatherData {
  char  desc[32]  = "";
  char  city[24]  = "";
  float tempC     = 0;
  int   humidity  = 0;
  float windMs    = 0;
  bool  valid     = false;
};
WeatherData   wxData;
unsigned long lastWxFetch = 0;

// ── VOACAP-style HF propagation prediction ───────────────────
#define VOACAP_REFRESH_MS 300000UL  // recalc every 5 minutes
#define VOACAP_NUM_BANDS   6
#define VOACAP_NUM_REGIONS 6
const float VOACAP_BAND_FREQS[VOACAP_NUM_BANDS] = {3.5f, 7.0f, 14.0f, 21.0f, 24.9f, 28.0f};
const char* VOACAP_BAND_NAMES[VOACAP_NUM_BANDS] = {"80m","40m","20m","15m","12m","10m"};
const char* VOACAP_REGION_NAMES[VOACAP_NUM_REGIONS] = {"NA","SA","EU","AF","AS","OC"};
// Representative target lat/lon for each region
const float VOACAP_REGION_LAT[VOACAP_NUM_REGIONS] = { 40.0f, -15.0f,  50.0f,   0.0f,  35.0f, -25.0f};
const float VOACAP_REGION_LON[VOACAP_NUM_REGIONS] = {-100.0f, -50.0f,  15.0f,  25.0f, 105.0f, 145.0f};
// Prediction results: 0=closed, 1=poor, 2=marginal, 3=good
uint8_t voacapGrid[VOACAP_NUM_BANDS][VOACAP_NUM_REGIONS];
unsigned long lastVoacapCalc = 0;

// ── PSKReporter data ─────────────────────────────────────────
#define PSKR_REFRESH_MS 300000UL  // 5 minutes (PSKReporter cache interval)
#define MAX_PSKR 40
struct PSKRSpot {
  char  callsign[12];  // receiver callsign
  char  grid[7];       // receiver grid
  float lat, lon;      // precomputed from grid
  float freqMHz;       // frequency in MHz
  char  mode[6];       // FT8, CW, etc.
  int   snr;           // signal-to-noise ratio
};
PSKRSpot      pskrSpots[MAX_PSKR];
int           pskrCount      = 0;
unsigned long lastPSKRFetch  = 0;

// ── DX cluster bearing ───────────────────────────────────────
int selectedDXSpot = -1;  // -1 = none selected

// ── Display mode ─────────────────────────────────────────────
enum DisplayMode { DISP_SOLAR, DISP_DX, DISP_MAP, DISP_CONTEST, DISP_CLOCK, DISP_POTA, DISP_SOTA, DISP_WSPR, DISP_VOACAP, DISP_PSKR };
#define NUM_SCREENS 10
bool screenEnabled[NUM_SCREENS] = {true,true,true,true,true,true,true,true,true,true};
int  screenOrder[NUM_SCREENS]   = {0,1,2,3,4,5,6,7,8,9};  // display order → enum index
const char* screenNames[NUM_SCREENS] = {
  "Solar","DX","Greyline","Contests","Clock",
  "POTA","SOTA","WSPR","VOACAP","PSKRptr"
};
DisplayMode dispMode      = DISP_SOLAR;
DisplayMode prevDispMode  = DISP_SOLAR;  // screen active before entering settings
unsigned long lastSwitch  = 0;

// ── UI State ──────────────────────────────────────────────────
enum UIState { STATE_MAIN, STATE_MENU, STATE_WIFI_LIST, STATE_WIFI_PASS,
               STATE_CALLSIGN_ENTRY, STATE_GRID_ENTRY };
UIState uiState = STATE_MAIN;
int settingsPage = 0;

// ── Refresh indicator ────────────────────────────────────────
bool     fetchInProgress = false;
unsigned long lastFetchTime = 0;  // millis() when last fetch completed

// WiFi scan state
#define MAX_NETS 12
String   wifiSSIDs[MAX_NETS];
int32_t  wifiRSSI[MAX_NETS];
int      wifiCount      = 0;
int      selectedNet    = -1;
String   newPassword    = "";
bool     shiftOn        = false;
bool     numMode        = false;
bool     passVisible    = false;
bool     ledOn          = false;  // RGB LED (active LOW on GPIO 4)
unsigned long lastScan  = 0;
bool     scanPending    = false;

// Touch debounce
bool     wasTouched     = false;
unsigned long lastTouch = 0;
const unsigned long TOUCH_DEBOUNCE = 70;

// ── XML helpers ───────────────────────────────────────────────
String tagValue(const String& xml, const String& tag) {
  String o = "<" + tag + ">";
  String c = "</" + tag + ">";
  int s = xml.indexOf(o);
  if (s < 0) return "";
  s += o.length();
  int e = xml.indexOf(c, s);
  if (e < 0) return "";
  String v = xml.substring(s, e);
  v.trim();
  return v;
}

void parseBands(const String& xml, SolarData& d) {
  int pos = 0;
  for (int i = 0; i < 8; i++) {
    int ts = xml.indexOf("<band ", pos);
    if (ts < 0) break;
    int cs = xml.indexOf('>', ts) + 1;
    int ce = xml.indexOf("</band>", cs);
    if (ce < 0) break;
    String v = xml.substring(cs, ce);
    v.trim();
    v.toCharArray(d.bands[i % 4][i / 4], 8);
    pos = ce + 7;
  }
}

void setField(char* dst, size_t n, const String& src) {
  strncpy(dst, src.c_str(), n - 1);
  dst[n - 1] = '\0';
}

// ── SD card settings ─────────────────────────────────────────
void loadSettingsSD() {
  if (!sdAvailable) return;
  File f = SD.open("/config.txt", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim(); val.trim();
    if (key == "callsign") val.toCharArray(myCallsign, sizeof(myCallsign));
    else if (key == "grid") val.toCharArray(myGrid, sizeof(myGrid));
    else if (key == "brightness") brightness = constrain(val.toInt(), 0, 255);
    else if (key == "ssid") val.toCharArray(sdSSID, sizeof(sdSSID));
    else if (key == "password") val.toCharArray(sdPassword, sizeof(sdPassword));
    else if (key == "autocycle") autoCycle = (val.toInt() != 0);
    else if (key == "autobrightness") autoBrightness = (val.toInt() != 0);
    else if (key == "tzoffset") tzOffset = (int8_t)constrain(val.toInt(), -12, 14);
    else if (key == "cyclespeed") cycleSpeed = constrain(val.toInt(), 5, 120);
    else if (key == "screentimeout") screenTimeout = constrain(val.toInt(), 0, 30);
    else if (key == "owmkey") val.toCharArray(owmKey, sizeof(owmKey));
    else if (key == "rotate") rotateDisplay = (val.toInt() != 0);
    else if (key == "screens") {
      for (int i = 0; i < NUM_SCREENS && i < (int)val.length(); i++)
        screenEnabled[i] = (val[i] != '0');
    }
    else if (key == "screenorder") {
      if ((int)val.length() == NUM_SCREENS) {
        bool seen[NUM_SCREENS] = {};
        bool valid = true;
        for (int i = 0; i < NUM_SCREENS; i++) {
          int v = val[i] - '0';
          if (v < 0 || v >= NUM_SCREENS || seen[v]) { valid = false; break; }
          seen[v] = true;
        }
        if (valid)
          for (int i = 0; i < NUM_SCREENS; i++) screenOrder[i] = val[i] - '0';
      }
    }
  }
  f.close();
}

void saveSettingsSD() {
  if (!sdAvailable) return;
  File f = SD.open("/config.txt", FILE_WRITE);
  if (!f) return;
  f.printf("callsign=%s\n", myCallsign);
  f.printf("grid=%s\n", myGrid);
  f.printf("brightness=%d\n", brightness);
  f.printf("autocycle=%d\n", autoCycle ? 1 : 0);
  f.printf("autobrightness=%d\n", autoBrightness ? 1 : 0);
  f.printf("tzoffset=%d\n", (int)tzOffset);
  f.printf("cyclespeed=%d\n", cycleSpeed);
  f.printf("screentimeout=%d\n", screenTimeout);
  f.print("screens=");
  for (int i = 0; i < NUM_SCREENS; i++) f.print(screenEnabled[i] ? '1' : '0');
  f.print('\n');
  f.print("screenorder=");
  for (int i = 0; i < NUM_SCREENS; i++) f.print((char)('0' + screenOrder[i]));
  f.print('\n');
  f.printf("rotate=%d\n", rotateDisplay ? 1 : 0);
  if (WiFi.status() == WL_CONNECTED) {
    f.printf("ssid=%s\n", WiFi.SSID().c_str());
    // Only save password if we have it from the last manual entry
    if (strlen(sdPassword) > 0)
      f.printf("password=%s\n", sdPassword);
  } else if (strlen(sdSSID) > 0) {
    f.printf("ssid=%s\n", sdSSID);
    if (strlen(sdPassword) > 0)
      f.printf("password=%s\n", sdPassword);
  }
  f.close();
}

// ── Brightness control (LEDC PWM on GPIO 27) ────────────────
void applyBrightness() {
  ledcWrite(27, brightness);
}

// ── Auto-brightness via LDR on GPIO 34 ───────────────────────
void updateAutoBrightness() {
  static int samples[8] = {2048,2048,2048,2048,2048,2048,2048,2048};
  static uint8_t idx = 0;
  samples[idx] = analogRead(LDR_PIN);
  idx = (idx + 1) & 7;
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += samples[i];
  int avg = sum / 8;
  // Low ADC value = bright ambient light, high = dark
  int b = map(avg, 300, 2000, 255, 30);
  ledcWrite(27, constrain(b, 30, 255));
}

// ── Refresh indicator + WiFi RSSI ────────────────────────────
void drawRefreshIndicator() {
  // Refresh dot — top-right corner
  uint32_t col;
  if (fetchInProgress) {
    col = C_AMBER;
  } else if (millis() - lastFetchTime < 10000) {
    col = C_GREEN;
  } else {
    col = C_DIM;
  }
  tft.fillCircle(316, 3, 2, col);

  // WiFi RSSI bars (4 ascending bars to the left of the dot)
  int baseX = 296;
  int baseY = 16;
  int bars = 0;
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  }
  for (int b = 0; b < 4; b++) {
    int barH = 4 + b * 3;  // 4, 7, 10, 13
    uint32_t bc = (b < bars) ? C_GREEN : 0x202020;
    tft.fillRect(baseX + b * 5, baseY - barH, 3, barH, bc);
  }
}

// ── POTA spots fetch ─────────────────────────────────────────
bool fetchPOTASpots() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, POTA_URL);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  String json = http.getString();
  http.end();

  potaCount = 0;
  int pos = 0;
  while (potaCount < MAX_POTA) {
    int objStart = json.indexOf('{', pos);
    if (objStart < 0) break;
    int objEnd = json.indexOf('}', objStart);
    if (objEnd < 0) break;
    String obj = json.substring(objStart, objEnd + 1);
    pos = objEnd + 1;

    // Extract string fields from JSON object (handles optional whitespace)
    auto jsonVal = [&](const String& key) -> String {
      String search = "\"" + key + "\"";
      int idx = obj.indexOf(search);
      if (idx < 0) return "";
      int colonIdx = obj.indexOf(':', idx + search.length());
      if (colonIdx < 0) return "";
      // Skip whitespace after colon
      int vStart = colonIdx + 1;
      while (vStart < (int)obj.length() && obj[vStart] == ' ') vStart++;
      if (vStart >= (int)obj.length() || obj[vStart] != '"') return "";
      vStart++; // skip opening quote
      int vEnd = obj.indexOf('"', vStart);
      if (vEnd < 0) return "";
      return obj.substring(vStart, vEnd);
    };

    String ref  = jsonVal("reference");
    String call = jsonVal("activator");
    String freq = jsonVal("frequency");
    String mode = jsonVal("mode");

    if (ref.length() == 0 && call.length() == 0) continue;

    setField(potaSpots[potaCount].parkRef,  sizeof(potaSpots[potaCount].parkRef),  ref);
    setField(potaSpots[potaCount].callsign, sizeof(potaSpots[potaCount].callsign), call);
    setField(potaSpots[potaCount].freq,     sizeof(potaSpots[potaCount].freq),     freq);
    setField(potaSpots[potaCount].mode,     sizeof(potaSpots[potaCount].mode),     mode);
    potaCount++;
  }
  lastPOTAFetch = millis();
  return potaCount > 0;
}

// ── POTA spots display ───────────────────────────────────────
#define POTA_ROW_H   41
#define POTA_ROW_Y0  35
#define POTA_COL_MID 160

void drawPOTASpots() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  // Header
  tft.fillRect(0, 0, 320, 22, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3);
  tft.print("POTA SPOTS");
  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  int tw = tft.textWidth("pota.app");
  tft.setCursor(296 - tw - 4, 7);
  tft.print("pota.app");
  drawRefreshIndicator();

  // Sub-headers
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 25);
  tft.print("PARK / CALLSIGN");
  tft.setCursor(POTA_COL_MID + 4, 25);
  tft.print("PARK / CALLSIGN");
  tft.drawFastHLine(0, POTA_ROW_Y0 - 1, 320, C_DIV);
  tft.drawFastVLine(POTA_COL_MID, 22, 218, C_DIV);

  if (potaCount == 0) {
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(80, 120);
    tft.print("No POTA spots available");
    return;
  }

  for (int i = 0; i < potaCount; i++) {
    bool rightCol = (i >= 5);
    int  slot     = rightCol ? (i - 5) : i;
    int  xBase    = rightCol ? (POTA_COL_MID + 4) : 4;
    int  xRight   = rightCol ? 316 : (POTA_COL_MID - 4);
    int  rowY     = POTA_ROW_Y0 + slot * POTA_ROW_H;

    // Park reference — green
    tft.setTextSize(1);
    tft.setTextColor(C_GREEN);
    tft.setCursor(xBase, rowY + 3);
    tft.print(potaSpots[i].parkRef);

    // Frequency — yellow, right-aligned on same line
    tft.setTextColor(0xFFFF44);
    String fStr = String(potaSpots[i].freq);
    if (fStr.length() > 0) {
      int fw = tft.textWidth(fStr.c_str());
      tft.setCursor(xRight - fw, rowY + 3);
      tft.print(fStr);
    }

    // Callsign — white, larger, below
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(xBase, rowY + 16);
    tft.print(potaSpots[i].callsign);

    // Mode badge — light grey, right side
    tft.setTextSize(1);
    tft.setTextColor(C_GRAY);
    int mw = tft.textWidth(potaSpots[i].mode);
    tft.setCursor(xRight - mw, rowY + 28);
    tft.print(potaSpots[i].mode);

    if (slot < 4)
      tft.drawFastHLine(xBase, rowY + POTA_ROW_H - 1, POTA_COL_MID - 8, C_DIV);
  }
}

// ── SOTA spots fetch ─────────────────────────────────────────
bool fetchSOTASpots() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, SOTA_URL);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  String json = http.getString();
  http.end();

  sotaCount = 0;
  int pos = 0;
  while (sotaCount < MAX_SOTA) {
    int objStart = json.indexOf('{', pos);
    if (objStart < 0) break;
    int objEnd = json.indexOf('}', objStart);
    if (objEnd < 0) break;
    String obj = json.substring(objStart, objEnd + 1);
    pos = objEnd + 1;

    auto jsonVal = [&](const String& key) -> String {
      String search = "\"" + key + "\"";
      int idx = obj.indexOf(search);
      if (idx < 0) return "";
      int colonIdx = obj.indexOf(':', idx + search.length());
      if (colonIdx < 0) return "";
      int vStart = colonIdx + 1;
      while (vStart < (int)obj.length() && obj[vStart] == ' ') vStart++;
      if (vStart >= (int)obj.length() || obj[vStart] != '"') return "";
      vStart++;
      int vEnd = obj.indexOf('"', vStart);
      if (vEnd < 0) return "";
      return obj.substring(vStart, vEnd);
    };

    String assoc  = jsonVal("associationCode");
    String summit = jsonVal("summitCode");
    String call   = jsonVal("activatorCallsign");
    String freq   = jsonVal("frequency");
    String mode   = jsonVal("mode");

    if (call.length() == 0) continue;

    String ref = (assoc.length() > 0) ? (assoc + "/" + summit) : summit;
    if (ref.length() > 11) ref = ref.substring(0, 11);

    setField(sotaSpots[sotaCount].summitCode, sizeof(sotaSpots[sotaCount].summitCode), ref);
    setField(sotaSpots[sotaCount].callsign,   sizeof(sotaSpots[sotaCount].callsign),   call);
    setField(sotaSpots[sotaCount].freq,       sizeof(sotaSpots[sotaCount].freq),       freq);
    setField(sotaSpots[sotaCount].mode,       sizeof(sotaSpots[sotaCount].mode),       mode);
    sotaCount++;
  }
  lastSOTAFetch = millis();
  return sotaCount > 0;
}

// ── SOTA spots display ───────────────────────────────────────
void drawSOTASpots() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  tft.fillRect(0, 0, 320, 22, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3);
  tft.print("SOTA SPOTS");
  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  int tw = tft.textWidth("sota.org.uk");
  tft.setCursor(296 - tw - 4, 7);
  tft.print("sota.org.uk");
  drawRefreshIndicator();

  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 25);
  tft.print("SUMMIT / CALLSIGN");
  tft.setCursor(POTA_COL_MID + 4, 25);
  tft.print("SUMMIT / CALLSIGN");
  tft.drawFastHLine(0, POTA_ROW_Y0 - 1, 320, C_DIV);
  tft.drawFastVLine(POTA_COL_MID, 22, 218, C_DIV);

  if (sotaCount == 0) {
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(80, 120);
    tft.print("No SOTA spots available");
    return;
  }

  for (int i = 0; i < sotaCount; i++) {
    bool rightCol = (i >= 5);
    int  slot     = rightCol ? (i - 5) : i;
    int  xBase    = rightCol ? (POTA_COL_MID + 4) : 4;
    int  xRight   = rightCol ? 316 : (POTA_COL_MID - 4);
    int  rowY     = POTA_ROW_Y0 + slot * POTA_ROW_H;

    tft.setTextSize(1);
    tft.setTextColor(C_GREEN);
    tft.setCursor(xBase, rowY + 3);
    tft.print(sotaSpots[i].summitCode);

    tft.setTextColor(0xFFFF44);
    String fStr = String(sotaSpots[i].freq);
    if (fStr.length() > 0) {
      int fw = tft.textWidth(fStr.c_str());
      tft.setCursor(xRight - fw, rowY + 3);
      tft.print(fStr);
    }

    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(xBase, rowY + 16);
    tft.print(sotaSpots[i].callsign);

    tft.setTextSize(1);
    tft.setTextColor(C_GRAY);
    int mw = tft.textWidth(sotaSpots[i].mode);
    tft.setCursor(xRight - mw, rowY + 28);
    tft.print(sotaSpots[i].mode);

    if (slot < 4)
      tft.drawFastHLine(xBase, rowY + POTA_ROW_H - 1, POTA_COL_MID - 8, C_DIV);
  }
}

// ── Clock helpers: Maidenhead, sunrise/sunset, moon phase ─────

// Decode Maidenhead grid to lat/lon centre (degrees)
void gridToLatLon(const char* grid, float* lat, float* lon) {
  *lat = 0; *lon = 0;
  if (!grid || strlen(grid) < 2) return;
  char g[7]; strncpy(g, grid, 6); g[6] = '\0';
  for (int i = 0; i < (int)strlen(g); i++) g[i] = toupper(g[i]);
  if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return;
  *lon = (g[0] - 'A') * 20.0f - 180.0f;
  *lat = (g[1] - 'A') * 10.0f - 90.0f;
  if (strlen(g) >= 4 && isDigit(g[2]) && isDigit(g[3])) {
    *lon += (g[2] - '0') * 2.0f + 1.0f;
    *lat += (g[3] - '0') * 1.0f + 0.5f;
  } else {
    *lon += 10.0f;
    *lat += 5.0f;
  }
}

// Returns sunrise and sunset as minutes past UTC midnight.
// Returns false for polar day/night.
bool calcSunriseSunset(float lat_deg, float lon_deg, time_t t,
                       int* riseMin, int* setMin) {
  struct tm* tm_utc = gmtime(&t);
  int doy = tm_utc->tm_yday + 1;
  float B    = (360.0f / 365.0f) * (doy - 81) * (float)M_PI / 180.0f;
  float decl = 23.45f * sin(B) * (float)M_PI / 180.0f;
  float EoT  = 9.87f * sin(2 * B) - 7.53f * cos(B) - 1.5f * sin(B);
  float noon = 720.0f - 4.0f * lon_deg - EoT;
  float lat  = lat_deg * (float)M_PI / 180.0f;
  float denom = cos(lat) * cos(decl);
  if (fabsf(denom) < 0.001f) return false;
  float cosHA = (sin(-0.833f * (float)M_PI / 180.0f) - sin(lat) * sin(decl)) / denom;
  if (cosHA < -1.0f || cosHA > 1.0f) return false;
  float HA = acos(cosHA) * 180.0f / (float)M_PI;
  *riseMin = (int)(noon - 4.0f * HA + 0.5f);
  *setMin  = (int)(noon + 4.0f * HA + 0.5f);
  *riseMin = ((*riseMin % 1440) + 1440) % 1440;
  *setMin  = ((*setMin  % 1440) + 1440) % 1440;
  return true;
}

// Moon age in days (0 = new moon, ~14.77 = full moon, 29.53 = next new)
float moonAge(time_t t) {
  const time_t KNOWN_NEW = 947182440UL;  // 2000-01-06 18:14 UTC
  const float CYCLE = 29.53058867f;
  float age = fmod((float)(t - KNOWN_NEW) / 86400.0f, CYCLE);
  if (age < 0) age += CYCLE;
  return age;
}

const char* moonPhaseName(float age) {
  if      (age <  1.85f) return "New Moon";
  else if (age <  7.38f) return "Wax Crescent";
  else if (age <  9.22f) return "First Quarter";
  else if (age < 14.77f) return "Wax Gibbous";
  else if (age < 16.61f) return "Full Moon";
  else if (age < 22.15f) return "Wan Gibbous";
  else if (age < 23.99f) return "Last Quarter";
  else                   return "Wan Crescent";
}

// ── Clock display ────────────────────────────────────────────
void drawClockDisplay() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  // Header
  tft.fillRect(0, 0, 320, 22, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3);
  tft.print("CLOCK");
  drawRefreshIndicator();

  time_t now = time(nullptr);
  bool synced = (now > 1000000000UL);
  struct tm* t = gmtime(&now);

  if (!synced) {
    tft.setTextSize(2);
    tft.setTextColor(C_RED);
    tft.setCursor(60, 100);
    tft.print("NTP not synced");
    return;
  }

  // UTC time — large
  char timeBuf[10];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02dZ", t->tm_hour, t->tm_min);
  tft.setTextSize(4);
  int tw = tft.textWidth(timeBuf);
  tft.setTextColor(C_WHITE);
  tft.setCursor((320 - tw) / 2, 28);
  tft.print(timeBuf);

  // Date
  char dateBuf[20];
  const char* dayNames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  const char* monNames[] = {"Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
  snprintf(dateBuf, sizeof(dateBuf), "%s %d %s %d",
           dayNames[t->tm_wday], t->tm_mday, monNames[t->tm_mon], 1900 + t->tm_year);
  tft.setTextSize(2);
  tft.setTextColor(C_GRAY);
  tw = tft.textWidth(dateBuf);
  tft.setCursor((320 - tw) / 2, 64);
  tft.print(dateBuf);

  int nextY = 84;

  // Local time (only if tzOffset != 0)
  if (tzOffset != 0) {
    time_t localNow = now + (int32_t)tzOffset * 3600L;
    struct tm* lt = gmtime(&localNow);
    char localBuf[16];
    snprintf(localBuf, sizeof(localBuf), "LOCAL %02d:%02d UTC%+d",
             lt->tm_hour, lt->tm_min, (int)tzOffset);
    tft.setTextSize(1);
    tft.setTextColor(C_AMBER);
    tw = tft.textWidth(localBuf);
    tft.setCursor((320 - tw) / 2, nextY);
    tft.print(localBuf);
    nextY += 14;
  }

  // Callsign
  if (strlen(myCallsign) > 0) {
    tft.setTextSize(3);
    tft.setTextColor(C_CYAN);
    tw = tft.textWidth(myCallsign);
    tft.setCursor((320 - tw) / 2, nextY + 4);
    tft.print(myCallsign);
    nextY += 32;
  }

  // Grid square
  if (strlen(myGrid) > 0) {
    tft.setTextSize(2);
    tft.setTextColor(C_AMBER);
    tw = tft.textWidth(myGrid);
    tft.setCursor((320 - tw) / 2, nextY + 2);
    tft.print(myGrid);
    nextY += 22;
  }

  // Divider
  nextY += 6;
  tft.drawFastHLine(20, nextY, 280, C_DIV);
  nextY += 8;

  // Sunrise / Sunset (requires grid)
  if (strlen(myGrid) >= 2) {
    float lat, lon;
    gridToLatLon(myGrid, &lat, &lon);
    int riseMin, setMin;
    if (calcSunriseSunset(lat, lon, now, &riseMin, &setMin)) {
      char sunBuf[34];
      snprintf(sunBuf, sizeof(sunBuf), "Rise %02d:%02dZ   Set %02d:%02dZ",
               riseMin / 60, riseMin % 60, setMin / 60, setMin % 60);
      tft.setTextSize(1);
      tft.setTextColor(0xFFDD44);
      tw = tft.textWidth(sunBuf);
      tft.setCursor((320 - tw) / 2, nextY);
      tft.print(sunBuf);
      nextY += 13;
    }
  }

  // Moon phase (always shown when synced)
  float age = moonAge(now);
  char moonBuf[28];
  snprintf(moonBuf, sizeof(moonBuf), "Moon: %s (%.1fd)", moonPhaseName(age), age);
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tw = tft.textWidth(moonBuf);
  tft.setCursor((320 - tw) / 2, nextY);
  tft.print(moonBuf);
  nextY += 13;

  // Weather (only if OWM key is set and data is valid)
  if (wxData.valid && strlen(owmKey) > 0) {
    tft.drawFastHLine(20, nextY, 280, C_DIV);
    nextY += 6;
    // Temp + description
    char wxLine1[40];
    snprintf(wxLine1, sizeof(wxLine1), "%.1f°C  %s", wxData.tempC, wxData.desc);
    // Capitalise first letter
    if (wxLine1[0] >= 'a' && wxLine1[0] <= 'z') wxLine1[0] -= 32;
    tft.setTextSize(1);
    tft.setTextColor(0x44DDFF);
    tw = tft.textWidth(wxLine1);
    tft.setCursor((320 - tw) / 2, nextY);
    tft.print(wxLine1);
    nextY += 11;
    // Humidity + wind + city
    char wxLine2[40];
    snprintf(wxLine2, sizeof(wxLine2), "%d%% RH  %.1fm/s  %s",
             wxData.humidity, wxData.windMs, wxData.city);
    tft.setTextColor(C_GRAY);
    tw = tft.textWidth(wxLine2);
    tft.setCursor((320 - tw) / 2, nextY);
    tft.print(wxLine2);
  }
}

// ── DX Cluster fetch ──────────────────────────────────────────
// Parses pipe-delimited HTML table from dxlite.g7vjr.org.
// Each row has 5 <td> cells: spotter | freq | callsign | comment | time
bool fetchDXSpots() {
  HTTPClient http;
  http.begin(DX_URL);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  String html = http.getString();
  http.end();

  dxCount = 0;
  int pos = 0;
  int col = 0;   // 0=spotter, 1=freq, 2=callsign, 3=comment, 4=time

  while (dxCount < MAX_SPOTS) {
    // Find next <td>
    int tdStart = html.indexOf("<td", pos);
    if (tdStart < 0) break;
    int tdClose = html.indexOf('>', tdStart);
    if (tdClose < 0) break;
    int tdEnd = html.indexOf("</td>", tdClose);
    if (tdEnd < 0) break;

    // Extract raw cell text, strip any inner tags
    String cell = html.substring(tdClose + 1, tdEnd);
    // Strip inner HTML tags
    String text = "";
    bool inTag = false;
    for (int i = 0; i < (int)cell.length(); i++) {
      char c = cell[i];
      if (c == '<') { inTag = true; continue; }
      if (c == '>') { inTag = false; continue; }
      if (!inTag) text += c;
    }
    text.trim();

    if (col == 1) {
      // Frequency
      setField(dxSpots[dxCount].freq, sizeof(dxSpots[dxCount].freq), text);
    } else if (col == 2) {
      // Callsign
      setField(dxSpots[dxCount].callsign, sizeof(dxSpots[dxCount].callsign), text);
    } else if (col == 4) {
      // Time — end of this row, advance to next spot
      dxCount++;
      col = 0;
      pos = tdEnd + 5;
      continue;
    }

    col++;
    if (col > 4) col = 0;
    pos = tdEnd + 5;
  }

  lastDXFetch = millis();
  return dxCount > 0;
}

// ── DX Cluster display ────────────────────────────────────────
// Two columns of 5 spots side by side, 5 rows × 41px each
#define DX_FREQ_COL  0xFFEE00   // bright yellow-amber for frequency
#define DX_ROW_H     41
#define DX_ROW_Y0    35
#define DX_COL_MID   160        // x divider between left/right panels

void drawDXCluster() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  // Header
  tft.fillRect(0, 0, 320, 22, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3);
  tft.print("DX CLUSTER");
  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  int tw = tft.textWidth("dxlite.g7vjr.org");
  tft.setCursor(296 - tw - 4, 7);
  tft.print("dxlite.g7vjr.org");
  drawRefreshIndicator();

  // Sub-headers for each panel
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 25);
  tft.print("FREQ (kHz)");
  tft.setCursor(DX_COL_MID + 4, 25);
  tft.print("FREQ (kHz)");

  tft.drawFastHLine(0, DX_ROW_Y0 - 1, 320, C_DIV);

  // Vertical divider between panels
  tft.drawFastVLine(DX_COL_MID, 22, 218, C_DIV);

  if (dxCount == 0) {
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(80, 120);
    tft.print("No spots available");
    return;
  }

  for (int i = 0; i < dxCount; i++) {
    // Left panel: spots 0-4  |  Right panel: spots 5-9
    bool rightCol = (i >= 5);
    int  slot     = rightCol ? (i - 5) : i;
    int  xBase    = rightCol ? (DX_COL_MID + 4) : 4;
    int  xRight   = rightCol ? 316 : (DX_COL_MID - 4);
    int  rowY     = DX_ROW_Y0 + slot * DX_ROW_H;

    // Frequency — bright yellow-amber, top of row
    tft.setTextSize(2);
    tft.setTextColor(DX_FREQ_COL);
    tft.setCursor(xBase, rowY + 3);
    tft.print(dxSpots[i].freq);

    // Callsign — white, below freq
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    int cw = tft.textWidth(dxSpots[i].callsign);
    tft.setCursor(xRight - cw, rowY + 22);
    tft.print(dxSpots[i].callsign);

    // Row separator (within each column)
    if (slot < 4)
      tft.drawFastHLine(xBase, rowY + DX_ROW_H - 1, DX_COL_MID - 8, C_DIV);
  }
}

// ── DX bearing modal overlay ──────────────────────────────────
void drawDXBearingModal(int idx) {
  if (idx < 0 || idx >= dxCount) return;
  const char* call = dxSpots[idx].callsign;

  // Overlay panel
  tft.fillRoundRect(30, 60, 260, 120, 10, C_MENU_HDR);
  tft.drawRoundRect(30, 60, 260, 120, 10, C_CYAN);

  tft.setTextSize(1);
  tft.setTextColor(C_CYAN);
  const char* hdr = "DX BEARING";
  tft.setCursor(30 + (260 - tft.textWidth(hdr)) / 2, 70);
  tft.print(hdr);
  tft.drawFastHLine(40, 82, 240, C_DIV);

  // Callsign
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(30 + (260 - tft.textWidth(call)) / 2, 88);
  tft.print(call);

  // Bearing calculation
  float dxLat, dxLon;
  if (strlen(myGrid) >= 4 && getDXCCLatLon(call, &dxLat, &dxLon)) {
    float myLat, myLon;
    float ml = (float)(myGrid[1] - 'A') * 10.0f + (myGrid[3] - '0') + 0.5f - 90.0f;
    float mn = (float)(myGrid[0] - 'A') * 20.0f + (myGrid[2] - '0') * 2.0f + 1.0f - 180.0f;
    myLat = ml; myLon = mn;

    float bear, distKm;
    calcBearingDist(myLat, myLon, dxLat, dxLon, &bear, &distKm);

    char line[40];
    snprintf(line, sizeof(line), "%.0f deg  %.0f km", bear, distKm);
    tft.setTextSize(1);
    tft.setTextColor(C_AMBER);
    tft.setCursor(30 + (260 - tft.textWidth(line)) / 2, 118);
    tft.print(line);

    char freq[20];
    snprintf(freq, sizeof(freq), "%s kHz", dxSpots[idx].freq);
    tft.setTextColor(DX_FREQ_COL);
    tft.setCursor(30 + (260 - tft.textWidth(freq)) / 2, 134);
    tft.print(freq);
  } else {
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    const char* nm = "No bearing (set grid)";
    tft.setCursor(30 + (260 - tft.textWidth(nm)) / 2, 120);
    tft.print(nm);
  }

  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  const char* dismiss = "Tap to dismiss";
  tft.setCursor(30 + (260 - tft.textWidth(dismiss)) / 2, 160);
  tft.print(dismiss);
}

// ── Contest Calendar fetch & display ─────────────────────────
static String stripHtmlTags(const String& s) {
  String out = "";
  bool inTag = false;
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c == '<') { inTag = true;  continue; }
    if (c == '>') { inTag = false; continue; }
    if (!inTag && c != '\n' && c != '\r' && c != '\t') out += c;
  }
  out.trim();
  return out;
}

// Extract "Mon DD" from strings like "Feb 15, 0000Z" or "0000Z Feb 15"
static String extractDatePart(const String& s) {
  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                           "Jul","Aug","Sep","Oct","Nov","Dec"};
  for (int m = 0; m < 12; m++) {
    int idx = s.indexOf(months[m]);
    if (idx >= 0) {
      int end = min((int)s.length(), idx + 9);
      String part = s.substring(idx, end);
      for (int i = 0; i < (int)part.length(); i++) {
        if (part[i] == ',' || part[i] == 'Z') { part = part.substring(0, i); break; }
      }
      part.trim();
      return part;
    }
  }
  String r = s;
  if (r.length() > 10) r = r.substring(0, 10);
  r.trim();
  return r;
}

// Parse "HHMMz" from a string, returning hour (0-23) or -1
static int parseZuluHour(const String& s) {
  // Find a 4-digit number followed by 'Z'
  for (int i = 0; i < (int)s.length() - 4; i++) {
    if (isDigit(s[i]) && isDigit(s[i+1]) && isDigit(s[i+2]) && isDigit(s[i+3])
        && (s[i+4] == 'Z' || s[i+4] == 'z')) {
      return (s[i] - '0') * 10 + (s[i+1] - '0');
    }
  }
  return -1;
}

// Parse month number (1-12) from a string containing "Jan","Feb",etc. Returns 0 if not found.
static int parseMonthNum(const String& s) {
  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                           "Jul","Aug","Sep","Oct","Nov","Dec"};
  for (int m = 0; m < 12; m++) {
    if (s.indexOf(months[m]) >= 0) return m + 1;
  }
  return 0;
}

// Parse day number after month name. Returns 0 if not found.
static int parseDayNum(const String& s) {
  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                           "Jul","Aug","Sep","Oct","Nov","Dec"};
  for (int m = 0; m < 12; m++) {
    int idx = s.indexOf(months[m]);
    if (idx >= 0) {
      int dStart = idx + 3;
      while (dStart < (int)s.length() && !isDigit(s[dStart])) dStart++;
      if (dStart < (int)s.length()) {
        int day = 0;
        while (dStart < (int)s.length() && isDigit(s[dStart])) {
          day = day * 10 + (s[dStart] - '0');
          dStart++;
        }
        return day;
      }
    }
  }
  return 0;
}

bool fetchContests() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, CONTEST_URL);
  http.setTimeout(12000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); Serial.printf("[CONTEST] HTTP %d\n", code); return false; }

  String xml = http.getString();
  http.end();
  Serial.printf("[CONTEST] RSS %d bytes\n", xml.length());

  // Parse RSS <item> entries: <title>Name</title> <description>dates</description>
  static Contest temp[PARSE_CONTESTS];
  int tempCount = 0;
  int pos = 0;

  while (tempCount < PARSE_CONTESTS) {
    int itemS = xml.indexOf("<item>", pos);
    if (itemS < 0) break;
    int itemE = xml.indexOf("</item>", itemS);
    if (itemE < 0) break;
    pos = itemE + 7;

    String item = xml.substring(itemS, itemE);

    // <title>Contest Name</title>
    int tS = item.indexOf("<title>");
    int tE = item.indexOf("</title>");
    if (tS < 0 || tE < 0) continue;
    String name = item.substring(tS + 7, tE);
    name.trim();
    if (name.length() == 0) continue;
    if (name.length() > 31) name = name.substring(0, 29) + "~";
    name.toCharArray(temp[tempCount].name, 32);

    // <description>0000Z, Mar 7 to 2359Z, Mar 15</description>
    int dS = item.indexOf("<description>");
    int dE = item.indexOf("</description>");
    String datePart = "";
    if (dS >= 0 && dE >= 0) datePart = item.substring(dS + 13, dE);
    datePart.trim();

    // --- Display date ---
    String d1 = extractDatePart(datePart);
    String d2 = "";
    int toIdx = datePart.indexOf(" to ");
    if (toIdx >= 0) d2 = extractDatePart(datePart.substring(toIdx + 4));

    // For multi-segment ("and"), use last segment for end time
    String lastSeg = datePart;
    int andIdx = datePart.lastIndexOf(" and ");
    if (andIdx >= 0) lastSeg = datePart.substring(andIdx + 5);

    String endSeg = (toIdx >= 0) ? datePart.substring(toIdx + 4) : datePart;
    if (andIdx >= 0) endSeg = lastSeg;
    int toInLast = lastSeg.indexOf(" to ");
    if (toInLast >= 0) endSeg = lastSeg.substring(toInLast + 4);

    String dispDate;
    if (d2.length() >= 3) {
      if (d1.length() >= 3 && d1.substring(0, 3) == d2.substring(0, 3)) {
        int sp = d2.indexOf(' ');
        dispDate = d1 + (sp >= 0 ? "-" + d2.substring(sp + 1) : "");
      } else {
        dispDate = d1 + "-" + (d2.length() > 6 ? d2.substring(0, 6) : d2);
      }
    } else {
      dispDate = d1;
    }
    if (dispDate.length() > 19) dispDate = dispDate.substring(0, 17) + "..";
    dispDate.toCharArray(temp[tempCount].dates, 20);

    // --- END TIME (for filtering) ---
    temp[tempCount].endMonth = parseMonthNum(endSeg);
    temp[tempCount].endDay   = parseDayNum(endSeg);
    int dashZ = endSeg.indexOf("Z-");
    if (dashZ >= 0) {
      temp[tempCount].endHour = parseZuluHour(endSeg.substring(dashZ + 2));
    } else {
      temp[tempCount].endHour = parseZuluHour(endSeg);
    }
    if (temp[tempCount].endHour < 0) temp[tempCount].endHour = 23;

    // --- MODE (infer from contest name) ---
    String nu = name;
    nu.toUpperCase();
    String mode;
    if      (nu.indexOf("RTTY") >= 0)                             mode = "RTTY";
    else if (nu.indexOf("FT4") >= 0 || nu.indexOf("FT8") >= 0)   mode = "DIG";
    else if (nu.indexOf("PSK") >= 0 || nu.indexOf("DIGI") >= 0)  mode = "DIG";
    else if (nu.indexOf(" CW") >= 0 || nu.endsWith("CW")
          || nu.indexOf("CWT") >= 0 || nu.indexOf("SPRINT") >= 0
          || nu.indexOf("TOPBAND") >= 0)                           mode = "CW";
    else if (nu.indexOf("SSB") >= 0 || nu.indexOf("PHONE") >= 0
          || nu.indexOf("SIDEBAND") >= 0)                          mode = "SSB";
    else                                                           mode = "MIX";
    mode.toCharArray(temp[tempCount].mode, 8);

    tempCount++;
  }

  // Filter: keep only contests that haven't ended yet
  struct tm now;
  bool haveTime = getLocalTime(&now, 100);
  int curMonth = haveTime ? (now.tm_mon + 1) : 0;
  int curDay   = haveTime ? now.tm_mday : 0;
  int curHour  = haveTime ? now.tm_hour : 0;

  Serial.printf("[CONTEST] parsed %d, haveTime=%d, cur=%d/%d %d:00\n",
                tempCount, haveTime, curMonth, curDay, curHour);

  contestCount = 0;
  for (int i = 0; i < tempCount && contestCount < MAX_CONTESTS; i++) {
    if (haveTime && temp[i].endMonth > 0 && temp[i].endDay > 0) {
      if (temp[i].endMonth < curMonth) continue;
      if (temp[i].endMonth == curMonth) {
        if (temp[i].endDay < curDay) continue;
        if (temp[i].endDay == curDay && temp[i].endHour < curHour) continue;
      }
    }
    contests[contestCount++] = temp[i];
  }

  lastContestFetch = millis();
  return contestCount > 0;
}

#define CON_COL_MID 160
#define CON_ROW_H   31
#define CON_ROW_Y0  23
#define CON_ROWS    7

void drawContestCalendar() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  // Header
  tft.fillRect(0, 0, 320, 22, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3);
  tft.print("CONTESTS");
  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  int tw = tft.textWidth("contestcalendar.com");
  tft.setCursor(296 - tw - 4, 7);
  tft.print("contestcalendar.com");
  drawRefreshIndicator();

  if (contestCount == 0) {
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(72, 120);
    tft.print("No contest data available");
    return;
  }

  // Column divider
  tft.drawFastVLine(CON_COL_MID, CON_ROW_Y0, 240 - CON_ROW_Y0, C_DIV);

  for (int i = 0; i < contestCount; i++) {
    bool rightCol = (i >= CON_ROWS);
    int  slot     = rightCol ? (i - CON_ROWS) : i;
    int  xBase    = rightCol ? (CON_COL_MID + 2) : 0;
    int  colW     = CON_COL_MID - 2;
    int  rowY     = CON_ROW_Y0 + slot * CON_ROW_H;

    // Row divider (skip first in each column)
    if (slot > 0)
      tft.drawFastHLine(xBase, rowY, colW, C_DIV);

    // Mode badge colour
    String m = String(contests[i].mode);
    uint32_t modeCol;
    if      (m == "CW")   modeCol = C_GREEN;
    else if (m == "SSB")  modeCol = 0x44BBFF;
    else if (m == "MIX")  modeCol = C_AMBER;
    else if (m == "RTTY") modeCol = C_CYAN;
    else                  modeCol = 0xFFDD44;

    // Mode badge
    tft.fillRoundRect(xBase + 2, rowY + 2, 28, 12, 2, modeCol);
    tft.setTextSize(1);
    tft.setTextColor(C_WHITE);
    int mw = tft.textWidth(contests[i].mode);
    tft.setCursor(xBase + 2 + (28 - mw) / 2, rowY + 4);
    tft.print(contests[i].mode);

    // Contest name — truncated to fit column
    int nameX = xBase + 33;
    int nameMaxW = colW - 35;
    tft.setTextColor(C_WHITE);
    String name = String(contests[i].name);
    while (name.length() > 0 && tft.textWidth(name.c_str()) > nameMaxW)
      name.remove(name.length() - 1);
    tft.setCursor(nameX, rowY + 4);
    tft.print(name);

    // Date — below name, light grey
    tft.setTextColor(C_GRAY);
    tft.setCursor(xBase + 4, rowY + 18);
    tft.print(contests[i].dates);
  }
}

// ── Colour helpers ────────────────────────────────────────────
uint32_t condColor(const char* s) {
  if (strcmp(s, "Good") == 0) return C_GREEN;
  if (strcmp(s, "Fair") == 0) return C_AMBER;
  if (strcmp(s, "Poor") == 0) return C_RED;
  return C_GRAY;
}
uint32_t kColor(const char* s) {
  int k = atoi(s);
  return k <= 2 ? C_GREEN : k <= 4 ? C_AMBER : C_RED;
}
uint32_t aColor(const char* s) {
  int a = atoi(s);
  return a <= 7 ? C_GREEN : a <= 29 ? C_AMBER : C_RED;
}
uint32_t geoColor(const char* s) {
  if (strncmp(s, "QUIET",     5) == 0) return C_GREEN;
  if (strncmp(s, "UNSETTLED", 9) == 0) return C_AMBER;
  if (strncmp(s, "ACTIVE",    6) == 0) return C_AMBER;
  return C_RED;
}

// ── World map / Greyline ──────────────────────────────────────
#include <math.h>
#include <time.h>

#define MAP_Y0     18    // y start of map (after header)
#define MAP_H      220   // map height in pixels (Mercator 2:1 ratio)
#define MAP_W      320

#define C_OCEAN_DAY   0x0044BB
#define C_OCEAN_NIGHT 0x000E2A
#define C_OCEAN_TWIL  0x002266   // civil twilight ocean
#define C_LAND_DAY    0x2D6A2D
#define C_LAND_NIGHT  0x0C220C
#define C_LAND_TWIL   0x1A4A1A   // civil twilight land
#define C_TERMINATOR  0xFFFF00
#define C_GRID        0x182838   // subtle grid lines
#define C_SUN_MARKER  0xFFDD00   // subsolar point

// Grid line Y positions (precomputed)
// y = round((90 - lat) * 220 / 180)
#define GRID_ARCTIC     29   // 66.56°N
#define GRID_TROPIC_N   81   // 23.44°N
#define GRID_EQUATOR   110   // 0°
#define GRID_TROPIC_S  139   // 23.44°S
#define GRID_ANTARCTIC 191   // 66.56°S

// Land rectangles: { x0, x1, y0, y1 } in map pixels
// x: 0-319  (lon: -180 to +180)   x = (lon+180)/360*319
// y: 0-219  (lat:  +90 to  -90)   y = (90-lat)*220/180
struct MapRect { uint16_t x0, x1; uint8_t y0, y1; };

// x = round((lon+180)*319/360)   y = round((90-lat)*220/180)
// Ref: lon -180→x0  -90→x80  0→x160  90→x240  180→x319
//      lat  90→y0   45→y55   0→y110  -45→y165  -90→y220
// KEY: continental Europe must NOT overlap the UK latitude/longitude band,
//      so the British Isles read as islands surrounded by ocean.
// KEY: Hudson Bay, Caspian Sea, Gulf of Carpentaria carved out as ocean.
const MapRect mapRects[] = {
  // ── ALASKA ──────────────────────────────────────────────────
  {   0,  12,  41,  45 },  // Aleutian chain W
  {  12,  18,  40,  46 },  // Alaska Peninsula
  {  11,  28,  23,  31 },  // Alaska N (lat 65-71)
  {  14,  35,  31,  37 },  // Alaska C (lat 60-65)
  {  22,  44,  37,  43 },  // SE Alaska / Panhandle (lat 54-60)

  // ── CANADIAN ARCTIC ISLANDS ───────────────────────────────
  {  58,  95,   8,  18 },  // High Arctic (Ellesmere, Devon, Axel Heiberg)
  {  49,  62,  13,  24 },  // Baffin Island W
  {  82, 106,  18,  24 },  // Baffin Island E + Resolution Island
  {  62,  82,  18,  22 },  // Southampton Island / Coats Island

  // ── CANADA (Hudson Bay carved out as ocean) ───────────────
  // Hudson Bay ≈ x75-90, y33-46  (lon -95 to -79, lat 52-63)
  {  44,  74,  24,  34 },  // W Canada N — Yukon/NWT (lat 62-70, W of Bay)
  {  75,  90,  24,  33 },  // Keewatin — above Hudson Bay (lat 63-70)
  {  91, 106,  24,  34 },  // E Canada N — Ungava/Baffin S (lat 62-70)
  {  44,  74,  34,  43 },  // W Canada C — BC/Alberta N (lat 55-62)
  {  91, 106,  34,  43 },  // E Canada C — Quebec N/Labrador (lat 55-62)
  {  44,  74,  43,  55 },  // W Canada S — BC/Alberta/Sask (lat 45-55)
  {  75,  90,  46,  55 },  // Ontario — south of Hudson Bay (lat 45-52)
  {  91, 115,  43,  55 },  // E Canada S — Quebec S/Maritimes (lat 45-55)
  { 106, 111,  37,  43 },  // Newfoundland

  // ── CONTINENTAL USA ─────────────────────────────────────────
  // Great Lakes gap ≈ x78-92, y50-57  (thin strips avoid filling them)
  {  49,  77,  55,  61 },  // USA NW (lat 40-45, W of Great Lakes)
  {  93, 108,  55,  61 },  // USA NE (lat 40-45, E of Great Lakes)
  {  78,  92,  55,  57 },  // Great Lakes N shore strip (lat 43-45)
  {  49, 108,  61,  67 },  // USA mid belt (lat 35-40)
  {  49,  73,  67,  73 },  // USA SW — Texas/NM/AZ (lat 30-35)
  {  73,  85,  67,  72 },  // USA S-C — LA/MS/AL (lat 31-35)
  {  93, 108,  67,  73 },  // USA SE — Carolinas/GA (lat 30-35)
  {  86,  93,  72,  82 },  // Florida peninsula
  {  93,  98,  72,  76 },  // Georgia/SC coast

  // ── MEXICO / CENTRAL AMERICA ────────────────────────────────
  {  49,  55,  73,  82 },  // Baja California
  {  55,  78,  73,  82 },  // N Mexico (lat 23-30)
  {  59,  78,  82,  88 },  // C Mexico (lat 17-23)
  {  62,  75,  88,  93 },  // S Mexico — Oaxaca/Chiapas (lat 12-17)
  {  75,  80,  88,  93 },  // Yucatan Peninsula
  {  78,  86,  93,  98 },  // Guatemala / Honduras
  {  83,  91,  98, 102 },  // Nicaragua / Costa Rica / Panama

  // ── CARIBBEAN ───────────────────────────────────────────────
  {  84, 100,  80,  84 },  // Cuba
  {  97, 103,  83,  87 },  // Hispaniola
  { 102, 106,  84,  87 },  // Puerto Rico
  {  96,  99,  87,  90 },  // Jamaica

  // ── SOUTH AMERICA ───────────────────────────────────────────
  // Shape tapers from wide Amazon to narrow Patagonia
  {  89, 103,  93,  98 },  // Colombia W (lat 8-12)
  {  93, 115,  98, 102 },  // Venezuela (lat 5-8)
  { 105, 120,  93,  98 },  // Venezuela N / Guyana (lat 8-12)
  {  84,  93, 100, 110 },  // Ecuador / Peru coast (lat 0-8)
  {  93, 120, 102, 110 },  // Amazon N / Guyana (lat 0-5)
  { 120, 127, 102, 108 },  // NE Brazil bulge (lat 2-8S)
  {  84,  95, 110, 122 },  // Peru / Bolivia W (lat -10 to 0)
  {  95, 130, 110, 118 },  // Amazon C + NE Brazil (lat -5 to 0)
  {  93, 132, 118, 126 },  // C Brazil / Mato Grosso (lat -13 to -5)
  {  93, 128, 126, 134 },  // Brazil S / Paraguay (lat -20 to -13)
  { 115, 130, 134, 140 },  // SE Brazil coast (lat -24 to -20)
  {  93, 115, 134, 141 },  // Argentina NE / Bolivia S (lat -25 to -20)
  {  93, 111, 141, 147 },  // Argentina C / Uruguay (lat -28 to -25)
  {  93, 106, 147, 153 },  // Argentina (lat -35 to -28)
  {  93, 102, 153, 159 },  // Argentina S (lat -40 to -35)
  {  93, 100, 159, 171 },  // Patagonia (lat -50 to -40)
  {  93,  98, 171, 177 },  // Tierra del Fuego (lat -55 to -50)

  // ── GREENLAND ───────────────────────────────────────────────
  { 118, 142,   6,  12 },  // N Greenland
  { 112, 145,  12,  20 },  // C Greenland
  { 106, 143,  20,  28 },  // S-C Greenland
  { 110, 138,  28,  35 },  // S Greenland tip

  // ── ICELAND ─────────────────────────────────────────────────
  { 137, 149,  29,  33 },

  // ── SVALBARD ────────────────────────────────────────────────
  { 169, 177,  14,  19 },

  // ── BRITISH ISLES ───────────────────────────────────────────
  { 151, 154,  43,  48 },  // Ireland      (lat 51-55, lon -10 to -6)
  { 154, 159,  38,  43 },  // Scotland     (lat 55-59, lon  -6 to -1)
  { 155, 161,  43,  48 },  // Eng + Wales  (lat 51-55, lon  -5 to  2)

  // ── SCANDINAVIA ─────────────────────────────────────────────
  { 163, 178,  23,  30 },  // N Norway / N Sweden (lat 66-70)
  { 163, 186,  30,  37 },  // Norway + Sweden / Finland N (lat 60-66)
  { 164, 184,  37,  43 },  // S Scandinavia + Finland S (lat 55-60)
  { 165, 170,  43,  46 },  // Denmark (lat 52-55)

  // ── CONTINENTAL EUROPE ──────────────────────────────────────
  // N Europe belt (lat 48-55) — starts at x=162 east of UK
  { 162, 195,  43,  51 },  // Benelux/Germany/Poland (lat 48-55)
  { 195, 202,  43,  49 },  // Baltic states (lat 50-55)

  // W Europe
  { 155, 168,  49,  57 },  // France (lat 43-50, lon -5 to 10)
  { 148, 162,  57,  63 },  // Iberian N — N Spain (lat 39-43)
  { 148, 155,  63,  67 },  // Portugal (lat 35-39)
  { 155, 162,  63,  67 },  // S Spain (lat 35-39)

  // Italy (boot shape — narrow rects follow the peninsula)
  { 168, 177,  51,  56 },  // N Italy / Po Valley (lat 44-48)
  { 169, 174,  56,  60 },  // Italy C-W — Tuscany/Lazio (lat 40-44)
  { 174, 178,  56,  60 },  // Italy C-E — Adriatic coast (lat 40-44)
  { 171, 176,  60,  64 },  // Italy S — Campania/Puglia (lat 38-40)
  { 173, 176,  64,  67 },  // Calabria (lat 35-38)
  { 170, 174,  67,  69 },  // Sicily
  { 167, 170,  61,  64 },  // Sardinia
  { 168, 170,  58,  60 },  // Corsica

  // Balkans / E Europe
  { 178, 195,  51,  57 },  // Romania / Hungary (lat 43-48)
  { 178, 188,  57,  61 },  // Balkans N — Serbia/Bosnia (lat 40-43)
  { 188, 195,  57,  61 },  // Bulgaria (lat 40-43)
  { 178, 186,  61,  65 },  // Greece N / Albania (lat 37-40)
  { 181, 185,  65,  68 },  // Greece S / Peloponnese (lat 35-37)
  { 182, 186,  68,  70 },  // Crete

  // ── TURKEY / ANATOLIA ───────────────────────────────────────
  { 186, 204,  58,  62 },  // Turkey N coast (lat 40-42)
  { 183, 204,  62,  66 },  // Turkey C + S (lat 36-40)
  { 188, 197,  66,  68 },  // Turkey SE — Hatay (lat 34-36)

  // ── CAUCASUS ────────────────────────────────────────────────
  { 196, 206,  52,  57 },  // Georgia / Armenia / Azerbaijan

  // ── RUSSIA / N ASIA ─────────────────────────────────────────
  // Caspian Sea carved out: ≈ x202-208, y53-65 (lon 48-54, lat 37-47)
  { 186, 319,  24,  34 },  // Russia N tundra (lat 62-70)
  { 186, 319,  34,  43 },  // Russia / Siberia N (lat 55-62)
  { 186, 201,  43,  53 },  // Russia W of Caspian (lat 47-55)
  { 209, 319,  43,  53 },  // Russia/Siberia E of Caspian (lat 47-55)
  { 186, 201,  53,  55 },  // Russia SW — Volga delta (lat 45-47)
  { 209, 319,  53,  55 },  // Siberia S / Kazakhstan (lat 45-47)
  { 295, 306,  35,  43 },  // Kamchatka Peninsula
  { 284, 289,  42,  55 },  // Sakhalin Island
  { 196, 210,   8,  20 },  // Novaya Zemlya

  // ── MIDDLE EAST / ARABIA ────────────────────────────────────
  // Red Sea gap ≈ x188-195, y73-90  (lon 32-40, lat 14-30)
  { 191, 202,  66,  73 },  // Levant / Syria / Iraq (lat 30-36)
  { 202, 213,  66,  73 },  // Iran W / Kurdistan (lat 30-36)
  { 196, 202,  73,  78 },  // Hejaz — W Arabia coast (lat 25-30)
  { 202, 213,  73,  79 },  // Arabia NE / Kuwait (lat 25-30)
  { 196, 210,  78,  85 },  // Arabia C (lat 20-25)
  { 198, 207,  85,  90 },  // Yemen / S Arabia (lat 14-20)
  { 210, 215,  73,  78 },  // UAE / Oman N
  { 211, 217,  78,  82 },  // Oman S

  // ── AFRICA ──────────────────────────────────────────────────
  // Mediterranean coast — ocean gap preserved between S Europe & N Africa
  { 142, 188,  67,  73 },  // N Africa W — Morocco to Libya
  { 188, 196,  67,  73 },  // Egypt (W of Red Sea gap)

  // Sahara belt (lat 20-30)
  { 142, 195,  73,  86 },  // Sahara + N-C Africa (lon -10 to 40)

  // W Africa — the distinctive western bulge
  { 142, 155,  86,  93 },  // Senegal/Guinea coast (lat 12-20)
  { 155, 170,  86,  96 },  // Mali/Niger S (lat 10-20)
  { 142, 148,  93, 100 },  // Sierra Leone/Liberia (lat 8-12)
  { 148, 155,  96, 103 },  // Ghana/Ivory Coast (lat 5-10)
  { 155, 165, 100, 105 },  // Nigeria S / Gulf of Guinea (lat 4-8)

  // C/E Africa
  { 170, 204,  86,  96 },  // C Africa / Chad / Sudan (lat 10-20)
  { 155, 204,  96, 110 },  // C Africa / Congo (lat 0-10)
  { 155, 204, 110, 122 },  // C-S Africa / Congo S (lat -10 to 0)

  // Horn of Africa
  { 196, 208,  86,  92 },  // Ethiopia N / Eritrea (lat 15-20)
  { 196, 215,  92,  98 },  // Ethiopia S / Somalia N (lat 8-15)
  { 204, 215,  98, 105 },  // Somalia S (lat 4-8)
  { 208, 212, 105, 110 },  // Kenya coast / Somalia tip

  // Southern Africa — tapers southward
  { 163, 200, 122, 130 },  // Zambia/Tanzania/Malawi (lat -15 to -10)
  { 168, 200, 130, 138 },  // Zimbabwe/Mozambique (lat -22 to -15)
  { 172, 196, 138, 144 },  // Botswana/SA N (lat -28 to -22)
  { 175, 193, 144, 150 },  // S Africa (lat -33 to -28)
  { 178, 190, 150, 153 },  // Cape region (lat -35 to -33)

  // ── MADAGASCAR ──────────────────────────────────────────────
  { 201, 207, 125, 132 },  // Madagascar N
  { 199, 206, 132, 142 },  // Madagascar S

  // ── CENTRAL / SOUTH ASIA ────────────────────────────────────
  // Iran/Afghanistan — E of Caspian gap
  { 209, 226,  55,  61 },  // Turkmenistan / N Iran (lat 40-45)
  { 202, 226,  61,  67 },  // Iran / Afghanistan (lat 35-40)
  { 228, 245,  45,  55 },  // Central Asia / Kazakhstan S

  // India — triangular taper narrowing southward
  { 214, 226,  67,  73 },  // Pakistan N / Kashmir (lat 30-35)
  { 214, 231,  73,  79 },  // Pakistan S / NW India / Rajasthan (lat 25-30)
  { 222, 240,  79,  86 },  // N India / Nepal / Ganges plain (lat 20-25)
  { 226, 240,  86,  92 },  // India C (lat 15-20)
  { 228, 238,  92,  98 },  // India S-C (lat 8-15)
  { 230, 235,  98, 102 },  // India S (lat 5-8)
  { 231, 234, 102, 105 },  // India tip — Kerala/TN (lat 3-5)
  { 234, 238, 100, 106 },  // Sri Lanka

  // ── EAST ASIA ───────────────────────────────────────────────
  { 224, 260,  45,  55 },  // Mongolia / China N (lat 45-55)
  { 224, 268,  55,  61 },  // China NC (lat 40-45)
  { 224, 275,  61,  67 },  // China C (lat 35-40)
  { 240, 252,  67,  79 },  // Sichuan / Yunnan (lat 25-35)
  { 252, 275,  67,  73 },  // China E coast (lat 30-35)
  { 252, 270,  73,  79 },  // China SE (lat 25-30)
  { 252, 268,  79,  82 },  // S China / Guangxi (lat 23-25)

  // Korea
  { 270, 276,  61,  66 },  // Korea N
  { 270, 276,  66,  69 },  // Korea S

  // Japan (detailed island chain)
  { 277, 286,  55,  59 },  // Hokkaido
  { 277, 284,  59,  62 },  // N Honshu — Tohoku
  { 277, 291,  62,  66 },  // C Honshu — Kanto/Chubu
  { 278, 287,  66,  70 },  // S Honshu / Shikoku
  { 275, 280,  70,  73 },  // Kyushu
  { 279, 283,  73,  77 },  // Okinawa / Ryukyu

  // Taiwan
  { 273, 277,  78,  82 },

  // ── SOUTHEAST ASIA ──────────────────────────────────────────
  // Indochina — tapers south along Malay Peninsula
  { 247, 260,  79,  86 },  // Myanmar / Indochina N (lat 20-25)
  { 248, 258,  86,  92 },  // Thailand / Laos (lat 15-20)
  { 249, 256,  92,  98 },  // Thailand S / Cambodia (lat 8-15)
  { 249, 253,  98, 105 },  // Malay Peninsula N (lat 4-8)
  { 250, 253, 105, 110 },  // Malay Peninsula S / Singapore (lat 0-4)

  // Indonesian archipelago
  { 244, 253, 103, 117 },  // Sumatra
  { 256, 266, 100, 108 },  // Borneo N
  { 255, 265, 108, 114 },  // Borneo S
  { 271, 275, 109, 115 },  // Sulawesi
  { 253, 261, 115, 121 },  // Java
  { 261, 266, 117, 121 },  // Bali / Lombok / Sumbawa
  { 266, 275, 117, 121 },  // Flores / Timor

  // Philippines
  { 264, 271,  86,  93 },  // Luzon
  { 264, 270,  93,  97 },  // Visayas
  { 264, 268,  97, 103 },  // Mindanao

  // New Guinea
  { 275, 290, 100, 109 },  // New Guinea W / Papua
  { 280, 298, 109, 116 },  // New Guinea E / PNG

  // ── AUSTRALIA ───────────────────────────────────────────────
  // Gulf of Carpentaria carved out: ≈ x279-286, y124-131
  { 261, 278, 122, 130 },  // Australia NW (lat -15 to -10)
  { 287, 296, 122, 130 },  // Australia NE — Queensland (lat -15 to -10)
  { 261, 296, 130, 138 },  // Australia C-N (lat -22 to -15)
  { 264, 296, 138, 146 },  // Australia C (lat -28 to -22)
  { 268, 293, 146, 152 },  // Australia S-C (lat -33 to -28)
  { 271, 290, 152, 158 },  // Australia S (lat -38 to -33)
  { 283, 289, 158, 163 },  // Tasmania

  // ── NEW ZEALAND ─────────────────────────────────────────────
  { 309, 315, 151, 157 },  // North Island N
  { 307, 314, 157, 161 },  // North Island S / Cook Strait
  { 307, 313, 161, 167 },  // South Island

  // ── PACIFIC ISLANDS ─────────────────────────────────────────
  {   1,   5,  82,  86 },  // Hawaii
  { 310, 314, 105, 108 },  // Fiji

  // ── ANTARCTICA ──────────────────────────────────────────────
  {  84,  92, 181, 190 },  // Antarctic Peninsula
  {   0, 319, 190, 219 },  // Main ice sheet
  {  30, 120, 186, 190 },  // W Antarctica extension
  { 200, 290, 186, 190 },  // E Antarctica extension
};
#define N_RECTS (int)(sizeof(mapRects)/sizeof(mapRects[0]))

// Draws the world map body (greyline, terminator, grid, home highlight).
// Call after clearing the screen and drawing a header.
void drawWorldMapBody() {
  // Get current UTC time (NTP synced)
  time_t now = time(nullptr);
  bool timeSynced = (now > 1000000000UL);
  struct tm* t = gmtime(&now);

  // Solar position
  float sin_decl = 0, cos_decl = 1, sub_lon_deg = 0, sub_lat_deg = 0;
  if (timeSynced) {
    int doy = t->tm_yday + 1;
    float utc_h  = t->tm_hour + t->tm_min / 60.0f;
    float decl   = 23.45f * sin((2.0f * M_PI / 365.0f) * (doy - 80)) * M_PI / 180.0f;
    sin_decl     = sin(decl);
    cos_decl     = cos(decl);
    sub_lon_deg  = (12.0f - utc_h) * 15.0f;
    sub_lat_deg  = decl * 180.0f / M_PI;
  }

  // Precompute cos(dlon) for each x column
  float cos_dlon[MAP_W];
  for (int x = 0; x < MAP_W; x++) {
    float lon     = (x / (float)MAP_W) * 360.0f - 180.0f;
    float dlon    = (lon - sub_lon_deg) * M_PI / 180.0f;
    while (dlon >  M_PI) dlon -= 2.0f * M_PI;
    while (dlon < -M_PI) dlon += 2.0f * M_PI;
    cos_dlon[x] = cos(dlon);
  }

  // Twilight threshold: cos(96°) ≈ -0.1045 (civil twilight = 6° below horizon)
  const float TWIL_THRESH = -0.1045f;

  // Draw map row by row, grouping consecutive same-colour pixels
  for (int y = 0; y < MAP_H; y++) {
    float lat     = 90.0f - (y / (float)MAP_H) * 180.0f;
    float lat_r   = lat * M_PI / 180.0f;
    float sinLat  = sin(lat_r);
    float cosLat  = cos(lat_r);
    float A = sin_decl * sinLat;
    float B = cos_decl * cosLat;

    // Check if this row is a grid line (dotted every 4px)
    bool isGridRow = (y == GRID_EQUATOR || y == GRID_TROPIC_N ||
                      y == GRID_TROPIC_S || y == GRID_ARCTIC ||
                      y == GRID_ANTARCTIC);

    int   spanX   = 0;
    uint32_t spanCol = 0;

    for (int x = 0; x <= MAP_W; x++) {
      uint32_t col = 0;
      if (x < MAP_W) {
        // Sun elevation angle (sin of altitude)
        float sinAlt = A + B * cos_dlon[x];
        // Day / twilight / night classification
        bool isDay  = !timeSynced || (sinAlt >= 0);
        bool isTwil = timeSynced && !isDay && (sinAlt >= TWIL_THRESH);
        // Land check
        bool land = false;
        for (int i = 0; i < N_RECTS && !land; i++)
          land = (x >= mapRects[i].x0 && x <= mapRects[i].x1 &&
                  y >= mapRects[i].y0 && y <= mapRects[i].y1);
        if (isDay)
          col = land ? C_LAND_DAY : C_OCEAN_DAY;
        else if (isTwil)
          col = land ? C_LAND_TWIL : C_OCEAN_TWIL;
        else
          col = land ? C_LAND_NIGHT : C_OCEAN_NIGHT;

        // Grid lines (dotted: draw every other 4px group)
        if (isGridRow && ((x / 4) & 1) == 0)
          col = C_GRID;
      }
      if (x == MAP_W || col != spanCol) {
        if (x > spanX && spanCol)
          tft.drawFastHLine(spanX, MAP_Y0 + y, x - spanX, spanCol);
        spanX   = x;
        spanCol = col;
      }
    }
  }

  // Draw terminator line in yellow (2px wide for visibility)
  if (timeSynced && fabs(sin_decl) > 0.01f) {
    float tan_decl = sin_decl / cos_decl;
    int prevTY = -999;
    for (int x = 0; x < MAP_W; x++) {
      float term_lat = atan(-cos_dlon[x] / tan_decl) * 180.0f / M_PI;
      int tY = (int)((90.0f - term_lat) / 180.0f * MAP_H);
      tY = constrain(tY, 0, MAP_H - 1);
      // Fill gap between columns for smooth line
      int y0 = min(tY, prevTY == -999 ? tY : prevTY);
      int y1 = max(tY, prevTY == -999 ? tY : prevTY);
      // Draw 2px wide terminator for better visibility
      int draw_y0 = max(0, y0 - 1);
      int draw_y1 = min(MAP_H - 1, y1 + 1);
      tft.drawFastVLine(x, MAP_Y0 + draw_y0, draw_y1 - draw_y0 + 1, C_TERMINATOR);
      prevTY = tY;
    }
  }

  // Draw subsolar point marker (sun position)
  if (timeSynced) {
    float sunLon = sub_lon_deg;
    while (sunLon >  180.0f) sunLon -= 360.0f;
    while (sunLon < -180.0f) sunLon += 360.0f;
    int sunX = (int)((sunLon + 180.0f) / 360.0f * MAP_W);
    int sunY = (int)((90.0f - sub_lat_deg) / 180.0f * MAP_H);
    sunX = constrain(sunX, 2, MAP_W - 3);
    sunY = constrain(sunY, 2, MAP_H - 3);
    // Small 5px sun symbol: filled circle with rays
    tft.fillCircle(sunX, MAP_Y0 + sunY, 2, C_SUN_MARKER);
    tft.drawPixel(sunX,     MAP_Y0 + sunY - 4, C_SUN_MARKER);
    tft.drawPixel(sunX,     MAP_Y0 + sunY + 4, C_SUN_MARKER);
    tft.drawPixel(sunX - 4, MAP_Y0 + sunY,     C_SUN_MARKER);
    tft.drawPixel(sunX + 4, MAP_Y0 + sunY,     C_SUN_MARKER);
    tft.drawPixel(sunX - 3, MAP_Y0 + sunY - 3, C_SUN_MARKER);
    tft.drawPixel(sunX + 3, MAP_Y0 + sunY - 3, C_SUN_MARKER);
    tft.drawPixel(sunX - 3, MAP_Y0 + sunY + 3, C_SUN_MARKER);
    tft.drawPixel(sunX + 3, MAP_Y0 + sunY + 3, C_SUN_MARKER);
  }

  // ── Maidenhead grid overlay ──────────────────────────────────
  // 18 fields across (A-R), each 20° lon; 18 fields down (A-R), each 10° lat
  #define MH_GRID_COL 0x304050
  // Vertical lines: every 20° of longitude
  for (int i = 1; i < 18; i++) {
    float lon = -180.0f + i * 20.0f;
    int gx = (int)((lon + 180.0f) / 360.0f * MAP_W);
    if (gx >= 0 && gx < MAP_W) {
      for (int gy = 0; gy < MAP_H; gy += 4)
        tft.drawPixel(gx, MAP_Y0 + gy, MH_GRID_COL);
    }
  }
  // Horizontal lines: every 10° of latitude
  for (int j = 1; j < 18; j++) {
    float lat = 90.0f - j * 10.0f;
    int gy = (int)((90.0f - lat) / 180.0f * MAP_H);
    if (gy >= 0 && gy < MAP_H) {
      for (int gx = 0; gx < MAP_W; gx += 4)
        tft.drawPixel(gx, MAP_Y0 + gy, MH_GRID_COL);
    }
  }
  // Label major grid fields (every other field to avoid clutter)
  tft.setTextSize(1);
  tft.setTextColor(MH_GRID_COL);
  for (int i = 0; i < 18; i += 2) {
    for (int j = 0; j < 18; j += 3) {
      float cenLon = -180.0f + i * 20.0f + 10.0f;
      float cenLat = 90.0f - j * 10.0f - 5.0f;
      int lx = (int)((cenLon + 180.0f) / 360.0f * MAP_W);
      int ly = (int)((90.0f - cenLat) / 180.0f * MAP_H);
      if (lx >= 0 && lx < MAP_W - 12 && ly >= 0 && ly < MAP_H - 8) {
        char label[3];
        label[0] = 'A' + i;
        label[1] = 'A' + (17 - j);  // j=0 is north (lat~85°N = 'R'), invert to Maidenhead
        label[2] = '\0';
        tft.setCursor(lx, MAP_Y0 + ly);
        tft.print(label);
      }
    }
  }

  // Highlight user's home grid if set
  if (strlen(myGrid) >= 2) {
    int gi = myGrid[0] - 'A';
    int gj = myGrid[1] - 'A';
    if (gi < 0 || gi >= 18) gi = toupper(myGrid[0]) - 'A';
    if (gj < 0 || gj >= 18) gj = toupper(myGrid[1]) - 'A';
    if (gi >= 0 && gi < 18 && gj >= 0 && gj < 18) {
      float lon0 = -180.0f + gi * 20.0f;
      float lat0 = -90.0f + gj * 10.0f;  // southern edge: Maidenhead A=-90°, O=50°, etc.
      int x0 = (int)((lon0 + 180.0f) / 360.0f * MAP_W);
      int y0 = (int)((90.0f - (lat0 + 10.0f)) / 180.0f * MAP_H);  // northern edge (smaller y)
      int x1 = (int)((lon0 + 20.0f + 180.0f) / 360.0f * MAP_W);
      int y1 = (int)((90.0f - lat0) / 180.0f * MAP_H);             // southern edge (larger y)
      tft.drawRect(x0, MAP_Y0 + y0, x1 - x0, y1 - y0, C_CYAN);
    }
  }

  // NTP warning overlaid at bottom of map if not synced
  if (!timeSynced) {
    tft.setTextSize(1);
    tft.setTextColor(C_RED);
    tft.setCursor(4, MAP_Y0 + MAP_H - 12);
    tft.print("NTP not synced - no greyline");
  }
}

void drawGreylineMap() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  // Header
  tft.fillRect(0, 0, MAP_W, MAP_Y0, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 1);
  tft.print("WORLD / GREYLINE");

  // UTC clock in header
  time_t now = time(nullptr);
  if (now > 1000000000UL) {
    struct tm* t = gmtime(&now);
    char tbuf[10];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02dZ", t->tm_hour, t->tm_min);
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    int tw = tft.textWidth(tbuf);
    tft.setCursor(296 - tw - 4, 7);
    tft.print(tbuf);
  }

  drawWorldMapBody();
  drawRefreshIndicator();
}

// ── Draw main display ─────────────────────────────────────────
void drawDisplay(const SolarData& d) {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  tft.fillRect(0, 0, 320, 22, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3);
  tft.print("SOLAR CONDITIONS");

  // Show just the time from the updated field (last 8+ chars typically "HHMMz" or "HHMM UTC")
  {
    String upd = String(d.updated);
    upd.trim();
    // Extract just the time portion — find last space-separated token(s) with digits
    int lastSpace = upd.lastIndexOf(' ');
    String timePart = upd;
    if (lastSpace > 0) {
      // Check if last token is "UTC" — if so grab the token before it too
      String tail = upd.substring(lastSpace + 1);
      if (tail == "UTC" || tail == "UT" || tail == "GMT") {
        int prevSpace = upd.lastIndexOf(' ', lastSpace - 1);
        timePart = (prevSpace > 0) ? upd.substring(prevSpace + 1) : upd;
      } else {
        timePart = tail;
      }
    }
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    int tw = tft.textWidth(timePart.c_str());
    tft.setCursor(290 - tw, 7);
    tft.print(timePart);
  }

  drawRefreshIndicator();

  // Space weather alert badge (top-left of header, if active)
  if (swpcAlertCount > 0) {
    tft.fillRoundRect(4, 4, 44, 14, 2, C_RED);
    tft.setTextSize(1); tft.setTextColor(C_WHITE);
    char abuf[10]; snprintf(abuf, sizeof(abuf), "ALT:%d", swpcAlertCount);
    int aw = tft.textWidth(abuf);
    tft.setCursor(4 + (44 - aw) / 2, 7);
    tft.print(abuf);
  }

  tft.drawFastVLine(157, 23, 217, C_DIV);

  char geoShort[9];
  strncpy(geoShort, d.geomagfield, 8);
  geoShort[8] = '\0';

  struct Row { const char* lbl; const char* val; uint32_t col; };
  Row rows[] = {
    { "SFI",   d.solarflux,   C_WHITE               },
    { "SN",    d.sunspots,    C_WHITE               },
    { "K-IDX", d.kindex,      kColor(d.kindex)      },
    { "A-IDX", d.aindex,      aColor(d.aindex)      },
    { "X-RAY", d.xray,        C_WHITE               },
    { "WIND",  d.solarwind,   C_WHITE               },
    { "GEO",   geoShort,      geoColor(d.geomagfield) },
    { "S/N",   d.signalnoise, C_WHITE               },
  };

  tft.setTextSize(2);
  int ly = 29;
  for (auto& r : rows) {
    tft.setTextColor(C_GRAY);
    tft.setCursor(6, ly);
    tft.print(r.lbl);
    tft.setTextColor(r.col);
    tft.setCursor(152 - tft.textWidth(r.val), ly);
    tft.print(r.val);
    ly += 27;
  }

  const int RX = 163;
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(RX, 27);
  tft.print("BAND CONDITIONS");

  tft.setCursor(218, 43);
  tft.print("DAY");
  tft.setCursor(270, 43);
  tft.print("NIGHT");

  tft.drawFastHLine(RX, 53, 155, C_DIV);

  const char* bName[] = { "80-40m", "30-20m", "17-15m", "12-10m" };
  int by = 58;
  for (int i = 0; i < 4; i++) {
    tft.setTextSize(1);
    tft.setTextColor(C_GRAY);
    tft.setCursor(RX, by + 4);
    tft.print(bName[i]);

    tft.setTextSize(2);
    tft.setTextColor(condColor(d.bands[i][0]));
    tft.setCursor(216, by);
    tft.print(d.bands[i][0]);

    tft.setTextColor(condColor(d.bands[i][1]));
    tft.setCursor(270, by);
    tft.print(d.bands[i][1]);

    by += 44;
  }

}

// ── DXCC prefix table + bearing ──────────────────────────────
struct DXCCEntry { const char* pfx; int8_t lat; int16_t lon; };
static const DXCCEntry dxccList[] = {
  {"UA9",56,60},{"UA0",56,105},{"VE",56,-96},{"VK",-27,133},
  {"ZL",-41,174},{"JA",35,136},{"BY",35,105},{"HL",37,128},
  {"ZS",-30,25},{"PY",-15,-50},{"LU",-35,-65},{"CE",-30,-71},
  {"XE",19,-99},{"TF",64,-20},{"EI",53,-8},{"GI",55,-6},
  {"GW",52,-3},{"GM",57,-4},{"G",52,-2},{"M",52,-2},
  {"F",47,2},{"DL",51,10},{"PA",52,5},{"ON",50,4},
  {"OZ",56,10},{"SM",60,15},{"OH",62,26},{"LA",60,10},
  {"SP",52,20},{"OM",49,19},{"OK",50,15},{"OE",47,14},
  {"HB",47,8},{"I",43,12},{"EA",40,-4},{"CT",39,-8},
  {"HA",47,19},{"YO",46,25},{"LZ",43,25},{"SV",38,24},
  {"UA",56,37},{"UR",50,31},{"9A",45,16},{"YU",44,21},
  {"4X",32,35},{"5B",35,33},{"A6",24,54},{"VU",20,77},
  {"HS",15,100},{"YB",-5,120},{"DU",12,122},{"BV",25,121},
  {"K",40,-100},{"W",40,-100},{"N",40,-100},
};
#define DXCC_N (int)(sizeof(dxccList)/sizeof(dxccList[0]))

bool getDXCCLatLon(const char* call, float* lat, float* lon) {
  int bestLen = 0, bestIdx = -1;
  for (int i = 0; i < DXCC_N; i++) {
    int plen = strlen(dxccList[i].pfx);
    if (plen > bestLen && strncasecmp(call, dxccList[i].pfx, plen) == 0) {
      bestLen = plen; bestIdx = i;
    }
  }
  if (bestIdx < 0) return false;
  *lat = (float)dxccList[bestIdx].lat;
  *lon = (float)dxccList[bestIdx].lon;
  return true;
}

void calcBearingDist(float la1d, float lo1d, float la2d, float lo2d,
                     float* bear, float* distKm) {
  float la1 = la1d*(float)M_PI/180, la2 = la2d*(float)M_PI/180;
  float lo1 = lo1d*(float)M_PI/180, lo2 = lo2d*(float)M_PI/180;
  float dlo = lo2 - lo1;
  *bear = fmod(atan2(sin(dlo)*cos(la2),
               cos(la1)*sin(la2)-sin(la1)*cos(la2)*cos(dlo))
               *180/(float)M_PI + 360, 360);
  float dla = la2 - la1;
  float a = sin(dla/2)*sin(dla/2)+cos(la1)*cos(la2)*sin(dlo/2)*sin(dlo/2);
  *distKm = 2*6371.0f*atan2(sqrt(a), sqrt(1-a));
}

// ── NOAA space weather alerts ─────────────────────────────────
bool fetchSWPCAlerts() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, SWPC_URL);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  String json = http.getString();
  http.end();
  swpcAlertCount = 0; swpcAlertCode[0] = '\0';
  int pos = 0;
  while ((pos = json.indexOf("\"product_id\"", pos)) >= 0) {
    swpcAlertCount++;
    if (swpcAlertCount == 1) {
      int q1 = json.indexOf('"', json.indexOf(':', pos + 12) + 1);
      int q2 = (q1 >= 0) ? json.indexOf('"', q1 + 1) : -1;
      if (q1 >= 0 && q2 > q1) {
        String c = json.substring(q1 + 1, q2);
        c.toCharArray(swpcAlertCode, sizeof(swpcAlertCode));
      }
    }
    pos += 12;
  }
  lastSWPCFetch = millis();
  return true;
}

// ── WSPR band activity ───────────────────────────────────────
bool fetchWSPR() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, WSPR_URL);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  String tsv = http.getString();
  http.end();
  for (int i = 0; i < 11; i++) wsprCounts[i] = 0;
  int pos = 0;
  while (pos < (int)tsv.length()) {
    int nl = tsv.indexOf('\n', pos);
    if (nl < 0) nl = tsv.length();
    String line = tsv.substring(pos, nl); line.trim();
    int tab = line.indexOf('\t');
    if (tab > 0) {
      int bandNum = line.substring(0, tab).toInt();
      int cnt     = line.substring(tab + 1).toInt();
      for (int i = 0; i < 11; i++) {
        if (WSPR_BAND_NUMS[i] == bandNum) { wsprCounts[i] = cnt; break; }
      }
    }
    pos = nl + 1;
  }
  lastWsprFetch = millis();
  return true;
}

void drawWSPR() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);
  tft.fillRect(0, 0, 320, 22, C_HDR);
  tft.setTextSize(2); tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3); tft.print("WSPR ACTIVITY");
  tft.setTextSize(1); tft.setTextColor(C_DIM);
  int tw = tft.textWidth("wspr.live");
  tft.setCursor(296-tw-4, 7); tft.print("wspr.live");
  drawRefreshIndicator();
  tft.drawFastVLine(160, 22, 218, C_DIV);

  // Two columns: bands 0-4 left, 5-10 right
  for (int i = 0; i < 11; i++) {
    bool right = (i >= 5);
    int slot   = right ? (i - 5) : i;
    int xBase  = right ? 164 : 4;
    int colW   = 156;
    int rowH   = right ? 36 : 44;
    int rowY   = 24 + slot * rowH;

    int cnt = wsprCounts[i];
    uint32_t col = cnt >= 100 ? C_GREEN : cnt >= 10 ? C_AMBER : cnt >= 1 ? C_RED : C_DIM;

    // Band name
    tft.setTextSize(2); tft.setTextColor(col);
    tft.setCursor(xBase, rowY + 2);
    tft.print(WSPR_BAND_NAMES[i]);

    // Count right-aligned
    char cbuf[8]; snprintf(cbuf, sizeof(cbuf), "%d", cnt);
    int cw = tft.textWidth(cbuf);
    tft.setCursor(xBase + colW - cw - 2, rowY + 2);
    tft.print(cbuf);

    // Bar
    int maxW = colW - 4;
    int barW = (cnt > 0) ? max(2, (int)(log10((float)cnt+1) / log10(2001.0f) * maxW)) : 0;
    int barY = rowY + 19;
    tft.fillRect(xBase, barY, barW, 5, col);
    tft.fillRect(xBase + barW, barY, maxW - barW, 5, 0x0A1020);

    if (slot > 0)
      tft.drawFastHLine(xBase, rowY, colW, C_DIV);
  }
}

// ── VOACAP-style HF propagation prediction engine ────────────
// Simplified MUF model based on ITU-R approach:
//   foF2 ~ f(SSN, time-of-day, season, latitude)
//   MUF  ~ foF2 * M(3000) factor scaled by path distance
//   Reliability based on how far below MUF the operating freq is

// Estimate foF2 (MHz) at a midpoint given SSN, local solar hour, month, lat
static float estimateFoF2(float ssn, float solarHour, int month, float latDeg) {
  // Base foF2 from SSN: sqrt relationship matches ionisation physics
  // SSN ~10 (solar min) → ~5.8 MHz, SSN ~150 (solar max) → ~13.8 MHz
  float base = 3.0f + 0.88f * sqrtf(max(0.0f, ssn));

  // Diurnal variation: peaks ~13 local solar time, minimum ~04
  float hrAngle = (solarHour - 13.0f) * (float)M_PI / 12.0f;
  float diurnal = 0.35f + 0.65f * max(0.0f, cosf(hrAngle * 0.85f));

  // Night floor
  if (diurnal < 0.35f) diurnal = 0.35f;

  // Seasonal variation — winter anomaly: foF2 peaks in local winter at mid-lats
  // monthAngle centred on January (month=1) for NH winter peak
  float monthAngle = (month - 1.0f) * (float)M_PI / 6.0f;
  float seasonal;
  if (fabsf(latDeg) < 25.0f) {
    // Tropical: weak seasonal, slight equinox peaks
    seasonal = 1.0f + 0.05f * cosf(2.0f * monthAngle);
  } else {
    // Mid/high lat: winter anomaly — foF2 higher in local winter
    float latSign = (latDeg > 0) ? 1.0f : -1.0f;
    float winterBoost = 0.15f * latSign * cosf(monthAngle);
    seasonal = 1.0f + winterBoost;
  }

  // Latitude factor: equatorial anomaly peak ~15-20 deg
  float absLat = fabsf(latDeg);
  float latFactor;
  if (absLat < 20.0f)
    latFactor = 1.1f + 0.1f * cosf(absLat * (float)M_PI / 40.0f);
  else if (absLat < 60.0f)
    latFactor = 1.0f;
  else
    latFactor = 0.7f + 0.3f * cosf((absLat - 60.0f) * (float)M_PI / 60.0f);

  return base * diurnal * seasonal * latFactor;
}

// Calculate MUF for a given path distance (km) from foF2
static float mufFromFoF2(float foF2, float distKm) {
  float d = constrain(distKm, 500.0f, 10000.0f);
  float M;
  if (d <= 3000.0f)
    M = 1.0f + 2.0f * (d / 3000.0f);
  else
    M = 3.0f + 0.5f * ((d - 3000.0f) / 7000.0f);

  // Multi-hop loss for long paths
  int hops = max(1, (int)(d / 3500.0f + 0.5f));
  float hopLoss = 1.0f - 0.03f * (hops - 1);

  return foF2 * M * hopLoss;
}

void calcVOACAP() {
  float txLat, txLon;
  gridToLatLon(myGrid, &txLat, &txLon);
  if (fabsf(txLat) < 0.01f && fabsf(txLon) < 0.01f) {
    memset(voacapGrid, 0, sizeof(voacapGrid));
    lastVoacapCalc = millis();
    return;
  }

  // Get current SSN from solar data (fallback to SFI-derived estimate)
  float ssn = atof(solar.sunspots);
  if (ssn <= 0) {
    float sfi = atof(solar.solarflux);
    if (sfi > 0) ssn = max(0.0f, (sfi - 63.7f) / 0.727f);
    else ssn = 50.0f;
  }

  time_t now = time(nullptr);
  struct tm* utc = gmtime(&now);
  float utcHour = utc->tm_hour + utc->tm_min / 60.0f;
  int month = utc->tm_mon + 1;

  for (int r = 0; r < VOACAP_NUM_REGIONS; r++) {
    float rxLat = VOACAP_REGION_LAT[r];
    float rxLon = VOACAP_REGION_LON[r];

    // Midpoint of path
    float midLat = (txLat + rxLat) / 2.0f;
    float midLon = (txLon + rxLon) / 2.0f;

    // Local solar time at midpoint
    float solarHour = fmodf(utcHour + midLon / 15.0f + 24.0f, 24.0f);

    // Path distance
    float bear, distKm;
    calcBearingDist(txLat, txLon, rxLat, rxLon, &bear, &distKm);

    // Estimate foF2 at path midpoint
    float foF2 = estimateFoF2(ssn, solarHour, month, midLat);

    // Calculate MUF for this path
    float muf = mufFromFoF2(foF2, distKm);

    // D-layer absorption: stronger during daytime, worse at lower freqs
    float zenithFactor = max(0.0f, cosf((solarHour - 12.0f) * (float)M_PI / 12.0f));

    for (int b = 0; b < VOACAP_NUM_BANDS; b++) {
      float freq = VOACAP_BAND_FREQS[b];

      if (freq > muf * 1.1f) {
        voacapGrid[b][r] = 0;  // above MUF — closed
      } else {
        float ratio = freq / muf;
        float absorption = zenithFactor * (20.0f / (freq * freq)) * ssn / 100.0f;
        float absLoss = min(1.0f, absorption * 0.3f);
        float reliability = (1.0f - ratio * 0.7f) * (1.0f - absLoss * 0.6f);

        if (distKm > 8000.0f) reliability *= 0.7f;
        if (freq < 10.0f && zenithFactor < 0.2f) reliability *= 1.2f;
        if (freq > 14.0f && zenithFactor > 0.5f) reliability = min(1.0f, reliability * 1.15f);

        if (reliability > 0.6f)       voacapGrid[b][r] = 3;
        else if (reliability > 0.35f) voacapGrid[b][r] = 2;
        else if (reliability > 0.15f) voacapGrid[b][r] = 1;
        else                          voacapGrid[b][r] = 0;
      }
    }
  }
  lastVoacapCalc = millis();
}

void drawVOACAP() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  // Header bar
  tft.fillRect(0, 0, 320, 22, C_HDR);
  tft.setTextSize(2); tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3); tft.print("HF PROPAGATION");
  tft.setTextSize(1); tft.setTextColor(C_DIM);
  int tw = tft.textWidth("VOACAP");
  tft.setCursor(296 - tw - 4, 7); tft.print("VOACAP");
  drawRefreshIndicator();

  // Check if grid is set
  if (strlen(myGrid) < 4) {
    tft.setTextSize(1); tft.setTextColor(C_DIM);
    const char* msg = "Set grid square in settings";
    tft.setCursor((320 - tft.textWidth(msg)) / 2, 120);
    tft.print(msg);
    return;
  }

  // QTH + SSN info line
  tft.setTextSize(1); tft.setTextColor(C_DIM);
  char qthBuf[32];
  snprintf(qthBuf, sizeof(qthBuf), "QTH: %s  SSN: %s", myGrid, solar.sunspots);
  tft.setCursor(6, 26); tft.print(qthBuf);

  // Current UTC hour
  time_t now = time(nullptr);
  struct tm* utc = gmtime(&now);
  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02dZ", utc->tm_hour);
  int ttw = tft.textWidth(timeBuf);
  tft.setCursor(314 - ttw, 26); tft.print(timeBuf);

  // ── Grid layout ──────────────────────────────────────
  int tableX = 6;
  int tableY = 40;
  int bandColW = 36;
  int cellW = 44;
  int cellH = 30;
  int hdrH = 16;

  // Region headers
  tft.setTextSize(1); tft.setTextColor(C_CYAN);
  for (int r = 0; r < VOACAP_NUM_REGIONS; r++) {
    int cx = tableX + bandColW + r * cellW + cellW / 2;
    int tw2 = tft.textWidth(VOACAP_REGION_NAMES[r]);
    tft.setCursor(cx - tw2 / 2, tableY + 2);
    tft.print(VOACAP_REGION_NAMES[r]);
  }
  tft.drawFastHLine(tableX, tableY + hdrH, bandColW + VOACAP_NUM_REGIONS * cellW, C_DIV);

  // Band rows
  for (int b = 0; b < VOACAP_NUM_BANDS; b++) {
    int rowY = tableY + hdrH + 2 + b * cellH;

    // Band name
    tft.setTextSize(2); tft.setTextColor(C_WHITE);
    tft.setCursor(tableX, rowY + 6);
    tft.print(VOACAP_BAND_NAMES[b]);

    // Cells
    for (int r = 0; r < VOACAP_NUM_REGIONS; r++) {
      int cx = tableX + bandColW + r * cellW + 4;
      int cy = rowY + 3;
      int bw = cellW - 8;
      int bh = cellH - 8;

      uint8_t val = voacapGrid[b][r];
      uint32_t fillCol, textCol;
      const char* label;

      switch (val) {
        case 3: fillCol = 0x004000; textCol = C_GREEN; label = "GOOD"; break;
        case 2: fillCol = 0x303000; textCol = C_AMBER; label = "FAIR"; break;
        case 1: fillCol = 0x300800; textCol = C_RED;   label = "POOR"; break;
        default: fillCol = 0x0A0A10; textCol = 0x404050; label = "--";  break;
      }

      tft.fillRoundRect(cx, cy, bw, bh, 3, fillCol);
      tft.setTextSize(1); tft.setTextColor(textCol);
      int lw = tft.textWidth(label);
      tft.setCursor(cx + (bw - lw) / 2, cy + (bh - 8) / 2);
      tft.print(label);
    }

    if (b < VOACAP_NUM_BANDS - 1)
      tft.drawFastHLine(tableX, rowY + cellH - 1, bandColW + VOACAP_NUM_REGIONS * cellW, C_DIV);
  }
}

// ── OpenWeatherMap ────────────────────────────────────────────
void fetchWeather(float lat, float lon) {
  if (strlen(owmKey) == 0) return;
  char url[160];
  snprintf(url, sizeof(url),
    "https://api.openweathermap.org/data/2.5/weather?lat=%.4f&lon=%.4f&appid=%s&units=metric",
    lat, lon, owmKey);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return; }
  String json = http.getString();
  http.end();

  wxData.valid = false;
  // desc — inside "weather":[{"description":"..."}]
  {
    int idx = json.indexOf("\"description\"");
    if (idx >= 0) {
      int q1 = json.indexOf('"', json.indexOf(':', idx + 13) + 1);
      int q2 = (q1 >= 0) ? json.indexOf('"', q1 + 1) : -1;
      if (q2 > q1) json.substring(q1+1, q2).toCharArray(wxData.desc, sizeof(wxData.desc));
    }
  }
  // name
  {
    int idx = json.indexOf("\"name\"");
    if (idx >= 0) {
      int q1 = json.indexOf('"', json.indexOf(':', idx + 6) + 1);
      int q2 = (q1 >= 0) ? json.indexOf('"', q1 + 1) : -1;
      if (q2 > q1) json.substring(q1+1, q2).toCharArray(wxData.city, sizeof(wxData.city));
    }
  }
  // temp
  {
    int idx = json.indexOf("\"temp\"");
    if (idx >= 0) {
      int colon = json.indexOf(':', idx + 6);
      wxData.tempC = json.substring(colon + 1).toFloat();
    }
  }
  // humidity
  {
    int idx = json.indexOf("\"humidity\"");
    if (idx >= 0) {
      int colon = json.indexOf(':', idx + 10);
      wxData.humidity = json.substring(colon + 1).toInt();
    }
  }
  // wind speed
  {
    int idx = json.indexOf("\"speed\"");
    if (idx >= 0) {
      int colon = json.indexOf(':', idx + 7);
      wxData.windMs = json.substring(colon + 1).toFloat();
    }
  }
  wxData.valid = true;
  lastWxFetch  = millis();
}

// ── PSKReporter fetch ─────────────────────────────────────────
bool fetchPSKReporter() {
  if (strlen(myCallsign) == 0) return false;

  String url = "https://retrieve.pskreporter.info/query?senderCallsign=";
  url += myCallsign;
  url += "&flowStartSeconds=-900&rronly=1";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); lastPSKRFetch = millis(); return false; }
  String xml = http.getString();
  http.end();

  pskrCount = 0;
  int pos = 0;
  while (pskrCount < MAX_PSKR) {
    int tagStart = xml.indexOf("<receptionReport ", pos);
    if (tagStart < 0) break;
    int tagEnd = xml.indexOf("/>", tagStart);
    if (tagEnd < 0) tagEnd = xml.indexOf(">", tagStart);
    if (tagEnd < 0) break;
    String tag = xml.substring(tagStart, tagEnd);
    pos = tagEnd + 2;

    // Extract XML attribute value by name
    auto getAttr = [&](const String& name) -> String {
      String search = name + "=\"";
      int idx = tag.indexOf(search);
      if (idx < 0) return "";
      int vStart = idx + search.length();
      int vEnd = tag.indexOf('"', vStart);
      if (vEnd < 0) return "";
      return tag.substring(vStart, vEnd);
    };

    String rcvCall = getAttr("receiverCallsign");
    String rcvGrid = getAttr("receiverLocator");
    String freq    = getAttr("frequency");
    String mode    = getAttr("mode");
    String snr     = getAttr("sNR");

    if (rcvGrid.length() < 2) continue;

    setField(pskrSpots[pskrCount].callsign, sizeof(pskrSpots[pskrCount].callsign), rcvCall);
    setField(pskrSpots[pskrCount].grid,     sizeof(pskrSpots[pskrCount].grid),     rcvGrid);
    setField(pskrSpots[pskrCount].mode,     sizeof(pskrSpots[pskrCount].mode),     mode);
    pskrSpots[pskrCount].freqMHz = freq.toFloat() / 1000000.0f;
    pskrSpots[pskrCount].snr     = snr.toInt();
    gridToLatLon(pskrSpots[pskrCount].grid,
                 &pskrSpots[pskrCount].lat, &pskrSpots[pskrCount].lon);
    pskrCount++;
  }
  lastPSKRFetch = millis();
  return pskrCount > 0;
}

// ── PSKReporter map display ──────────────────────────────────
void drawPSKMap() {
  tft.setTextWrap(false);

  // Header (fills header area; map body overwrites the rest — no fillScreen needed)
  tft.fillRect(0, 0, 320, MAP_Y0, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 1);
  tft.print("PSKREPORTER");

  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  char cbuf[16];
  snprintf(cbuf, sizeof(cbuf), "%d spots", pskrCount);
  int tw = tft.textWidth(cbuf);
  tft.setCursor(296 - tw - 4, 7);
  tft.print(cbuf);

  drawWorldMapBody();
  drawRefreshIndicator();

  if (pskrCount == 0) {
    tft.setTextSize(1);
    if (strlen(myCallsign) == 0) {
      tft.setTextColor(C_RED);
      tft.setCursor(60, MAP_Y0 + MAP_H / 2);
      tft.print("Set callsign in settings");
    } else {
      tft.setTextColor(C_DIM);
      tft.setCursor(70, MAP_Y0 + MAP_H / 2);
      tft.print("No spots in last 15 min");
    }
    return;
  }

  // Plot spots — colour by band
  for (int i = 0; i < pskrCount; i++) {
    int sx = (int)((pskrSpots[i].lon + 180.0f) / 360.0f * MAP_W);
    int sy = (int)((90.0f - pskrSpots[i].lat) / 180.0f * MAP_H);
    sx = constrain(sx, 2, MAP_W - 3);
    sy = constrain(sy, 2, MAP_H - 3);

    uint32_t col;
    float f = pskrSpots[i].freqMHz;
    if      (f < 3)  col = 0xFF4444;   // 160m red
    else if (f < 5)  col = 0xFF8844;   // 80m  orange
    else if (f < 8)  col = 0xFFCC00;   // 40m  amber
    else if (f < 12) col = 0xFFFF00;   // 30m  yellow
    else if (f < 16) col = 0x00FF00;   // 20m  green
    else if (f < 20) col = 0x00FFBB;   // 17m  teal
    else if (f < 23) col = 0x00BBFF;   // 15m  light blue
    else if (f < 26) col = 0x8888FF;   // 12m  purple
    else if (f < 30) col = 0xFF44FF;   // 10m  magenta
    else             col = 0xFFFFFF;   // 6m+  white

    tft.fillCircle(sx, MAP_Y0 + sy, 2, col);
  }

  // Draw home marker on top (cyan diamond) if grid set
  if (strlen(myGrid) >= 2) {
    float hlat, hlon;
    gridToLatLon(myGrid, &hlat, &hlon);
    int hx = (int)((hlon + 180.0f) / 360.0f * MAP_W);
    int hy = (int)((90.0f - hlat) / 180.0f * MAP_H);
    hx = constrain(hx, 3, MAP_W - 4);
    hy = constrain(hy, 3, MAP_H - 4);
    tft.fillCircle(hx, MAP_Y0 + hy, 3, C_CYAN);
    tft.drawCircle(hx, MAP_Y0 + hy, 3, C_WHITE);
  }
}

// ── Advance display mode (used by auto-timer and tap-to-skip) ─
void advanceDisplayMode() {
  lastSwitch = millis();

  // Find current position in screenOrder
  int curPos = 0;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (screenOrder[i] == (int)dispMode) { curPos = i; break; }
  }
  // Advance to next enabled screen in display order
  int nextPos = curPos;
  for (int i = 0; i < NUM_SCREENS; i++) {
    nextPos = (nextPos + 1) % NUM_SCREENS;
    if (screenEnabled[screenOrder[nextPos]]) break;
  }
  if (!screenEnabled[screenOrder[nextPos]]) return;  // no screens enabled
  dispMode = (DisplayMode)screenOrder[nextPos];

  // Fetch data and draw the new screen
  switch (dispMode) {
    case DISP_SOLAR:
      drawDisplay(solar);
      break;
    case DISP_DX:
      fetchInProgress = true;
      fetchDXSpots();
      fetchInProgress = false;
      drawDXCluster();
      break;
    case DISP_MAP:
      drawGreylineMap();
      break;
    case DISP_CONTEST:
      if (contestCount == 0 || millis() - lastContestFetch >= CONTEST_REFRESH_MS) {
        fetchInProgress = true;
        fetchContests();
        fetchInProgress = false;
      }
      drawContestCalendar();
      break;
    case DISP_CLOCK:
      drawClockDisplay();
      break;
    case DISP_POTA:
      fetchInProgress = true;
      fetchPOTASpots();
      fetchInProgress = false;
      drawPOTASpots();
      break;
    case DISP_SOTA:
      fetchInProgress = true;
      fetchSOTASpots();
      fetchInProgress = false;
      drawSOTASpots();
      break;
    case DISP_WSPR:
      fetchInProgress = true;
      fetchWSPR();
      fetchInProgress = false;
      drawWSPR();
      break;
    case DISP_VOACAP:
      calcVOACAP();
      drawVOACAP();
      break;
    case DISP_PSKR:
      fetchInProgress = true;
      fetchPSKReporter();
      fetchInProgress = false;
      drawPSKMap();
      break;
  }
}

// ── Settings Menu (multi-page) ────────────────────────────────
// ── OTA Update ──────────────────────────────────────────────
void drawOTAProgress(int percent, const char* status) {
  // Progress area: clear and redraw
  tft.fillRect(10, 100, 300, 100, C_MENU_BG);

  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(22, 105);
  tft.print(status);

  // Progress bar background
  tft.fillRoundRect(20, 140, 280, 20, 4, C_KEY_BG);
  // Progress bar fill
  int fillW = (280 * percent) / 100;
  if (fillW > 0)
    tft.fillRoundRect(20, 140, fillW, 20, 4, C_GREEN);

  // Percentage text
  char pctBuf[8];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", percent);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  tft.setCursor(20 + (280 - tft.textWidth(pctBuf)) / 2, 145);
  tft.print(pctBuf);
}

void performOTACheck() {
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillRect(10, 100, 300, 100, C_MENU_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_RED);
    tft.setCursor(22, 120);
    tft.print("WiFi not connected");
    return;
  }

  // Show checking status
  tft.fillRect(10, 100, 300, 100, C_MENU_BG);
  tft.setTextSize(2);
  tft.setTextColor(C_AMBER);
  tft.setCursor(22, 120);
  tft.print("Checking for update...");

  // Fetch version file
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, OTA_VERSION_URL);
  http.setTimeout(8000);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    http.end();
    tft.fillRect(10, 100, 300, 100, C_MENU_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_RED);
    tft.setCursor(22, 120);
    tft.print("Check failed");
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(22, 145);
    tft.printf("HTTP error: %d", code);
    return;
  }

  String remoteVer = http.getString();
  http.end();
  remoteVer.trim();

  if (remoteVer == FW_VERSION) {
    tft.fillRect(10, 100, 300, 100, C_MENU_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_GREEN);
    tft.setCursor(22, 110);
    tft.print("Up to date!");
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(22, 135);
    tft.printf("Version: %s", FW_VERSION);
    return;
  }

  // New version available — show info then install
  tft.fillRect(10, 100, 300, 100, C_MENU_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_CYAN);
  tft.setCursor(22, 90);
  tft.printf("New: v%s (current: v%s)", remoteVer.c_str(), FW_VERSION);

  drawOTAProgress(0, "Downloading...");

  // Set up progress callback
  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) {
      int pct = (cur * 100) / total;
      drawOTAProgress(pct, "Installing...");
    }
  });

  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(client, OTA_FIRMWARE_URL);

  // If we get here, the update failed (success would reboot)
  tft.fillRect(10, 100, 300, 100, C_MENU_BG);
  tft.setTextSize(2);
  tft.setTextColor(C_RED);
  tft.setCursor(22, 110);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      tft.print("Update failed");
      tft.setTextSize(1);
      tft.setTextColor(C_DIM);
      tft.setCursor(22, 135);
      tft.print(httpUpdate.getLastErrorString());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      tft.print("No update found");
      break;
    default:
      tft.print("Unknown error");
      break;
  }
}

void drawSettingsNavBar() {
  // Navigation bar at bottom: < [dots] >
  int navY = 210;
  tft.fillRect(0, navY, 320, 30, C_MENU_BG);

  // Left arrow
  tft.fillRoundRect(10, navY + 2, 40, 26, 4, settingsPage > 0 ? C_KEY_SPL : C_KEY_BG);
  tft.setTextSize(2);
  tft.setTextColor(settingsPage > 0 ? C_WHITE : C_DIM);
  tft.setCursor(22, navY + 7);
  tft.print("<");

  // Right arrow
  tft.fillRoundRect(270, navY + 2, 40, 26, 4, settingsPage < 6 ? C_KEY_SPL : C_KEY_BG);
  tft.setTextColor(settingsPage < 6 ? C_WHITE : C_DIM);
  tft.setCursor(282, navY + 7);
  tft.print(">");

  // Page dots (7 pages, centered)
  for (int i = 0; i < 7; i++) {
    uint32_t dc = (i == settingsPage) ? C_CYAN : C_DIM;
    tft.fillCircle(121 + i * 13, navY + 15, 4, dc);
  }
}

void drawSettingsMenu() {
  tft.fillScreen(C_MENU_BG);

  // Header
  tft.fillRect(0, 0, 320, 36, C_MENU_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(10, 9);
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "SETTINGS (%d/7)", settingsPage + 1);
  tft.print(hdr);

  // Close button (top-right)
  tft.fillRoundRect(278, 6, 36, 24, 4, C_RED);
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(287, 10);
  tft.print("X");

  if (settingsPage == 0) {
    // ── Page 1: WiFi / Network + LED toggle ──────────────────
    tft.fillRoundRect(10, 52, 300, 48, 6, C_KEY_SPL);
    tft.drawRoundRect(10, 52, 300, 48, 6, C_CYAN);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 64);
    tft.print("WiFi / Network");

    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(22, 88);
    if (WiFi.status() == WL_CONNECTED) {
      tft.print("Connected: ");
      tft.print(WiFi.SSID());
    } else {
      tft.setTextColor(C_RED);
      tft.print("Not connected");
    }

    tft.fillRoundRect(10, 112, 300, 40, 6, C_KEY_BG);
    tft.drawRoundRect(10, 112, 300, 40, 6, C_DIM);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 124);
    tft.print("Red LED:");
    uint32_t pillCol = ledOn ? C_GREEN : 0xFFAA44;
    tft.fillRoundRect(220, 118, 70, 28, 6, pillCol);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(228, 124);
    tft.print(ledOn ? " ON" : "OFF");

    // UTC Offset: [-] [value] [+]
    tft.fillRoundRect(10, 162, 300, 38, 6, C_KEY_BG);
    tft.drawRoundRect(10, 162, 300, 38, 6, C_DIM);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 172);
    tft.print("UTC Offset:");

    tft.fillRoundRect(170, 168, 36, 28, 4, C_KEY_SPL);
    tft.setTextColor(C_WHITE);
    tft.setCursor(182, 174);
    tft.print("-");

    char tzBuf[8];
    snprintf(tzBuf, sizeof(tzBuf), "%+d", (int)tzOffset);
    tft.fillRoundRect(210, 168, 52, 28, 4, C_KEY_BG);
    tft.setTextColor(C_CYAN);
    int tzv = tft.textWidth(tzBuf);
    tft.setCursor(210 + (52 - tzv) / 2, 174);
    tft.print(tzBuf);

    tft.fillRoundRect(266, 168, 36, 28, 4, C_KEY_SPL);
    tft.setTextColor(C_WHITE);
    tft.setCursor(278, 174);
    tft.print("+");

  } else if (settingsPage == 1) {
    // ── Page 2: Callsign, Grid, Brightness ───────────────────
    // Callsign button
    tft.fillRoundRect(10, 52, 300, 40, 6, C_KEY_SPL);
    tft.drawRoundRect(10, 52, 300, 40, 6, C_CYAN);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 62);
    tft.print("Callsign: ");
    tft.setTextColor(C_AMBER);
    tft.print(strlen(myCallsign) > 0 ? myCallsign : "(not set)");

    // Grid square button
    tft.fillRoundRect(10, 102, 300, 40, 6, C_KEY_SPL);
    tft.drawRoundRect(10, 102, 300, 40, 6, C_CYAN);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 112);
    tft.print("Grid: ");
    tft.setTextColor(C_AMBER);
    tft.print(strlen(myGrid) > 0 ? myGrid : "(not set)");

    // Brightness slider: [-] [value] [+]
    tft.setTextSize(2);
    tft.setTextColor(autoBrightness ? C_DIM : C_WHITE);
    tft.setCursor(22, 160);
    tft.print("Brightness:");

    uint32_t btnCol = autoBrightness ? C_DIM : C_KEY_SPL;

    // Minus button
    tft.fillRoundRect(170, 152, 36, 30, 4, btnCol);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(182, 158);
    tft.print("-");

    // Value display
    char bval[8];
    snprintf(bval, sizeof(bval), "%d", brightness);
    tft.fillRoundRect(210, 152, 52, 30, 4, C_KEY_BG);
    tft.setTextColor(autoBrightness ? C_DIM : C_CYAN);
    int bw = tft.textWidth(bval);
    tft.setCursor(210 + (52 - bw) / 2, 158);
    tft.print(bval);

    // Plus button
    tft.fillRoundRect(266, 152, 36, 30, 4, btnCol);
    tft.setTextColor(C_WHITE);
    tft.setCursor(278, 158);
    tft.print("+");

  } else if (settingsPage == 2) {
    // ── Page 3: Auto Cycle, Sleep, Auto Brightness, Rotate ───

    // Auto-cycle toggle (y=44-78)
    tft.fillRoundRect(10, 44, 300, 34, 6, C_KEY_BG);
    tft.drawRoundRect(10, 44, 300, 34, 6, C_DIM);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 54);
    tft.print("Auto Cycle:");
    uint32_t acCol = autoCycle ? C_GREEN : 0xFFAA44;
    tft.fillRoundRect(220, 50, 70, 24, 6, acCol);
    tft.setTextColor(C_WHITE);
    tft.setCursor(232, 56);
    tft.print(autoCycle ? "ON" : "OFF");

    // Screen timeout: [-] [value] [+] (y=86-120)
    tft.fillRoundRect(10, 86, 300, 34, 6, C_KEY_BG);
    tft.drawRoundRect(10, 86, 300, 34, 6, C_DIM);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 96);
    tft.print("Sleep:");

    // Minus
    tft.fillRoundRect(150, 92, 36, 24, 4, C_KEY_SPL);
    tft.setTextColor(C_WHITE);
    tft.setCursor(162, 98);
    tft.print("-");

    // Value
    char stBuf[8];
    if (screenTimeout == 0) snprintf(stBuf, sizeof(stBuf), "OFF");
    else snprintf(stBuf, sizeof(stBuf), "%dm", screenTimeout);
    tft.fillRoundRect(190, 92, 52, 24, 4, C_KEY_BG);
    tft.setTextColor(C_CYAN);
    int stw = tft.textWidth(stBuf);
    tft.setCursor(190 + (52 - stw) / 2, 98);
    tft.print(stBuf);

    // Plus
    tft.fillRoundRect(246, 92, 36, 24, 4, C_KEY_SPL);
    tft.setTextColor(C_WHITE);
    tft.setCursor(258, 98);
    tft.print("+");

    // Auto Brightness toggle (y=128-162)
    tft.fillRoundRect(10, 128, 300, 34, 6, C_KEY_BG);
    tft.drawRoundRect(10, 128, 300, 34, 6, C_DIM);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 138);
    tft.print("Auto Bright:");
    uint32_t abCol = autoBrightness ? C_GREEN : 0xFFAA44;
    tft.fillRoundRect(220, 134, 70, 24, 6, abCol);
    tft.setTextColor(C_WHITE);
    tft.setCursor(232, 140);
    tft.print(autoBrightness ? " ON" : "OFF");

    // Rotate 180° toggle (y=170-204)
    tft.fillRoundRect(10, 170, 300, 34, 6, C_KEY_BG);
    tft.drawRoundRect(10, 170, 300, 34, 6, C_DIM);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 180);
    tft.print("Rotate 180\xF8:");
    uint32_t rdCol = rotateDisplay ? C_GREEN : 0xFFAA44;
    tft.fillRoundRect(220, 176, 70, 24, 6, rdCol);
    tft.setTextColor(C_WHITE);
    tft.setCursor(232, 182);
    tft.print(rotateDisplay ? " ON" : "OFF");

  } else if (settingsPage == 3) {
    // ── Page 4: Cycle Speed ───────────────────────────────────
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 60);
    tft.print("Cycle Speed:");

    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(22, 80);
    tft.print("Auto-cycle interval (5-120 seconds)");

    // Minus button
    tft.fillRoundRect(50, 108, 50, 36, 4, C_KEY_SPL);
    tft.setTextSize(3);
    tft.setTextColor(C_WHITE);
    tft.setCursor(66, 115);
    tft.print("-");

    // Value display
    char csBuf[8];
    snprintf(csBuf, sizeof(csBuf), "%ds", cycleSpeed);
    tft.fillRoundRect(110, 108, 100, 36, 4, C_KEY_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_CYAN);
    int csw = tft.textWidth(csBuf);
    tft.setCursor(110 + (100 - csw) / 2, 118);
    tft.print(csBuf);

    // Plus button
    tft.fillRoundRect(220, 108, 50, 36, 4, C_KEY_SPL);
    tft.setTextSize(3);
    tft.setTextColor(C_WHITE);
    tft.setCursor(234, 115);
    tft.print("+");

    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(22, 160);
    tft.print("Steps of 5 seconds");

  } else if (settingsPage == 4) {
    // ── Page 5: About ────────────────────────────────────────
    tft.fillRoundRect(30, 60, 260, 120, 10, C_MENU_HDR);
    tft.drawRoundRect(30, 60, 260, 120, 10, C_CYAN);

    tft.setTextSize(1);
    tft.setTextColor(C_CYAN);
    tft.setCursor(30 + (260 - tft.textWidth("ABOUT")) / 2, 72);
    tft.print("ABOUT");
    tft.drawFastHLine(40, 83, 240, C_DIV);

    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    const char* line1 = "CheapClock";
    tft.setCursor(30 + (260 - tft.textWidth(line1)) / 2, 92);
    tft.print(line1);

    char verLine[24];
    snprintf(verLine, sizeof(verLine), "v%s", FW_VERSION);
    tft.setTextColor(C_AMBER);
    tft.setCursor(30 + (260 - tft.textWidth(verLine)) / 2, 118);
    tft.print(verLine);

    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    const char* sdMsg = sdAvailable ? "SD card: OK" : "SD card: not found";
    tft.setCursor(30 + (260 - tft.textWidth(sdMsg)) / 2, 148);
    tft.print(sdMsg);

  } else if (settingsPage == 5) {
    // ── Page 6: OTA Update ───────────────────────────────────
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, 56);
    tft.print("Firmware Update");

    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(22, 78);
    tft.print("Current version: ");
    tft.setTextColor(C_CYAN);
    tft.print(FW_VERSION);

    // Check for Update button
    tft.fillRoundRect(30, 110, 260, 44, 6, C_KEY_SPL);
    tft.drawRoundRect(30, 110, 260, 44, 6, C_CYAN);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    const char* btnTxt = "Check for Update";
    tft.setCursor(30 + (260 - tft.textWidth(btnTxt)) / 2, 122);
    tft.print(btnTxt);

    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(22, 170);
    tft.print("Updates from arc.ntwk.co.uk");

  } else if (settingsPage == 6) {
    // ── Page 7: Screen Toggles & Order ──────────────────────
    for (int pos = 0; pos < NUM_SCREENS; pos++) {
      int i   = screenOrder[pos];   // actual screen enum index
      int col = pos / 5;            // 0=left, 1=right
      int row = pos % 5;
      int ax  = col == 0 ?  4 : 164;  // arrow area x (16px wide)
      int bx  = col == 0 ? 22 : 182;  // button x (132px wide)
      int cy  = 42 + row * 33;
      bool on = screenEnabled[i];

      // Arrow area background
      tft.fillRect(ax, cy, 16, 28, C_MENU_BG);
      tft.setTextSize(1);
      // ▲ (up) — skip on first row of each column
      if (row > 0) {
        tft.setTextColor(C_DIM);
        tft.setCursor(ax + 4, cy + 3);
        tft.print("^");
      }
      // ▼ (down) — skip on last row of each column
      if (row < 4) {
        tft.setTextColor(C_DIM);
        tft.setCursor(ax + 4, cy + 18);
        tft.print("v");
      }

      // Toggle button
      tft.fillRoundRect(bx, cy, 132, 28, 4, C_KEY_BG);
      tft.drawRoundRect(bx, cy, 132, 28, 4, on ? C_GREEN : C_DIM);
      tft.setTextSize(2);
      tft.setTextColor(on ? C_WHITE : C_DIM);
      tft.setCursor(bx + 5, cy + 6);
      tft.print(screenNames[i]);

      // ON/OFF pill
      uint32_t pillCol = on ? C_GREEN : 0x404040;
      tft.fillRoundRect(bx + 92, cy + 4, 36, 20, 4, pillCol);
      tft.setTextSize(1);
      tft.setTextColor(C_WHITE);
      tft.setCursor(bx + 97, cy + 10);
      tft.print(on ? "ON" : "OFF");
    }
  }

  drawSettingsNavBar();
}

// ── WiFi list screen ──────────────────────────────────────────
void triggerScan() {
  WiFi.scanNetworks(true); // async
  scanPending = true;
  lastScan = millis();
}

void drawWifiList() {
  tft.fillScreen(C_MENU_BG);

  // Header
  tft.fillRect(0, 0, 320, 36, C_MENU_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(10, 9);
  tft.print("WIFI NETWORKS");

  // Back button
  tft.fillRoundRect(4, 5, 50, 26, 4, C_KEY_SPL);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  tft.setCursor(14, 12);
  tft.print("< BACK");

  // Scan status
  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  tft.setCursor(200, 14);
  if (scanPending) {
    tft.print("Scanning...");
  } else {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d found", wifiCount);
    tft.print(buf);
  }

  // Network list (up to 6 visible)
  int maxVisible = 6;
  for (int i = 0; i < maxVisible; i++) {
    int ni = i; // no scroll for now, show first 6
    if (ni >= wifiCount) break;

    int rowY = 40 + i * 33;
    bool isCurrent = (WiFi.status() == WL_CONNECTED && wifiSSIDs[ni] == WiFi.SSID());

    uint32_t bg = isCurrent ? C_NET_HL : C_KEY_BG;
    tft.fillRoundRect(6, rowY, 308, 30, 4, bg);

    // SSID
    tft.setTextSize(1);
    tft.setTextColor(C_WHITE);
    // Truncate SSID to fit
    String ssid = wifiSSIDs[ni];
    if (ssid.length() > 26) ssid = ssid.substring(0, 24) + "..";
    tft.setCursor(12, rowY + 5);
    tft.print(ssid);

    // RSSI bar + dBm
    int rssi = wifiRSSI[ni];
    uint32_t rssiCol = rssi > -60 ? C_GREEN : rssi > -75 ? C_AMBER : C_RED;
    tft.setTextColor(rssiCol);
    char rBuf[12];
    snprintf(rBuf, sizeof(rBuf), "%ddBm", rssi);
    tft.setCursor(262, rowY + 5);
    tft.print(rBuf);

    // Signal bars (4 bars)
    int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
      uint32_t bc = b < bars ? rssiCol : C_DIM;
      tft.fillRect(242 + b * 4, rowY + 18 - b * 3, 3, 4 + b * 3, bc);
    }

    // Connected tick
    if (isCurrent) {
      tft.setTextColor(C_GREEN);
      tft.setCursor(292, rowY + 5);
      tft.print("\x7E"); // ~
    }

    // Lock icon (all are assumed secured for now)
    tft.setTextColor(C_DIM);
    tft.setCursor(12, rowY + 18);
    tft.print("WPA");
  }

  if (wifiCount == 0 && !scanPending) {
    tft.setTextSize(2);
    tft.setTextColor(C_DIM);
    tft.setCursor(60, 120);
    tft.print("No networks found");
  }
}

// ── On-screen keyboard ────────────────────────────────────────
// Layout: 4 rows on lower ~155px of screen (y=85 to y=239)
// Row heights: 38px each

#define KB_Y0  88   // top of keyboard
#define KB_KH  36   // key height
#define KB_KW  30   // standard key width
#define KB_GAP  2   // gap between keys

const char* rowsLower[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
const char* rowsUpper[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
const char* rowsNum[3]   = { "1234567890", "-/:;()$@\"", ".,?!'[]{}%" };
// Row 3 is special: SHIFT/NUM | SPACE | BACKSPACE | OK

void drawKey(int x, int y, int w, int h, const char* label, uint32_t bg) {
  tft.fillRoundRect(x, y, w, h, 4, bg);
  tft.drawRoundRect(x, y, w, h, 4, 0x334455);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  int lw = tft.textWidth(label);
  tft.setCursor(x + (w - lw) / 2, y + (h - 8) / 2);
  tft.print(label);
}

// Redraws only the password input field (top 84px) — fast, no keyboard redraw
void drawPassField() {
  tft.fillRect(0, 0, 320, 84, C_PASS_BG);

  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  tft.setCursor(6, 6);
  tft.print("Network:");
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 18);
  String s = wifiSSIDs[selectedNet];
  if (s.length() > 30) s = s.substring(0, 28) + "..";
  tft.print(s);

  tft.setTextColor(C_DIM);
  tft.setCursor(6, 38);
  tft.print("Password:");

  tft.fillRoundRect(6, 50, 262, 26, 4, 0x0D1525);
  tft.drawRoundRect(6, 50, 262, 26, 4, C_CYAN);

  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  String masked = "";
  for (unsigned int i = 0; i < newPassword.length(); i++) masked += '*';
  String toShow = passVisible ? newPassword : masked;
  while (tft.textWidth(toShow.c_str()) > 250 && toShow.length() > 0) toShow = toShow.substring(1);
  tft.setCursor(10, 59);
  tft.print(toShow);
  tft.fillRect(10 + tft.textWidth(toShow.c_str()), 58, 2, 12, C_CYAN);

  tft.fillRoundRect(272, 50, 42, 26, 4, C_KEY_SPL);
  tft.setTextColor(C_WHITE);
  tft.setCursor(276, 59);
  tft.print(passVisible ? "HIDE" : "SHOW");

  tft.drawFastHLine(0, 84, 320, C_DIV);
}

// Redraws only the keyboard area (y>=85)
void drawPassKeyboard() {
  tft.fillRect(0, 85, 320, 155, C_PASS_BG);

  const char** rows = numMode ? rowsNum : (shiftOn ? rowsUpper : rowsLower);

  // Row 0
  {
    int n = strlen(rows[0]);
    int startX = (320 - n * (KB_KW + KB_GAP) + KB_GAP) / 2;
    for (int i = 0; i < n; i++) {
      char lbl[2] = { rows[0][i], 0 };
      drawKey(startX + i * (KB_KW + KB_GAP), KB_Y0, KB_KW, KB_KH, lbl, C_KEY_BG);
    }
  }

  // Row 1
  {
    int n = strlen(rows[1]);
    int startX = (320 - n * (KB_KW + KB_GAP) + KB_GAP) / 2;
    for (int i = 0; i < n; i++) {
      char lbl[2] = { rows[1][i], 0 };
      drawKey(startX + i * (KB_KW + KB_GAP), KB_Y0 + KB_KH + KB_GAP, KB_KW, KB_KH, lbl, C_KEY_BG);
    }
  }

  // Row 2: SHIFT/NUM + letters + DEL
  {
    int rowY = KB_Y0 + 2 * (KB_KH + KB_GAP);
    int n = strlen(rows[2]);
    drawKey(2, rowY, 40, KB_KH, numMode ? "ABC" : (shiftOn ? "shf" : "SHF"), C_KEY_SPL);
    int available = 320 - 4 - 40 - 42;
    int kw2 = (available - (n - 1) * KB_GAP) / n;
    for (int i = 0; i < n; i++) {
      char lbl[2] = { rows[2][i], 0 };
      drawKey(44 + i * (kw2 + KB_GAP), rowY, kw2, KB_KH, lbl, C_KEY_BG);
    }
    drawKey(278, rowY, 40, KB_KH, "DEL", 0xFFAA44);
  }

  // Row 3: mode toggle | @ | SPACE | . | - | CONN
  {
    int rowY = KB_Y0 + 3 * (KB_KH + KB_GAP);
    drawKey(2,   rowY, 44, KB_KH, numMode ? "ABC" : "123", C_KEY_SPL);
    drawKey(48,  rowY, 28, KB_KH, "@",    C_KEY_BG);
    drawKey(78,  rowY, 120, KB_KH, "SPACE", C_KEY_BG);
    drawKey(200, rowY, 28, KB_KH, ".",    C_KEY_BG);
    drawKey(230, rowY, 28, KB_KH, "-",    C_KEY_BG);
    drawKey(260, rowY, 58, KB_KH, "CONN", 0x00AA55);
  }
}

// Full initial draw (field + keyboard)
void drawPasswordEntry() {
  tft.fillScreen(C_PASS_BG);
  drawPassField();
  drawPassKeyboard();
}

// ── Callsign / Grid entry field ──────────────────────────────
String entryBuffer = "";

void drawEntryField() {
  tft.fillRect(0, 0, 320, 84, C_PASS_BG);

  bool isCallsign = (uiState == STATE_CALLSIGN_ENTRY);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 6);
  tft.print(isCallsign ? "ENTER CALLSIGN" : "ENTER GRID SQUARE");

  tft.fillRoundRect(6, 36, 280, 32, 4, 0x0D1525);
  tft.drawRoundRect(6, 36, 280, 32, 4, C_CYAN);
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(12, 42);
  tft.print(entryBuffer);
  // Cursor
  tft.fillRect(12 + tft.textWidth(entryBuffer.c_str()), 42, 2, 16, C_CYAN);

  // OK button
  tft.fillRoundRect(290, 36, 26, 32, 4, 0x00AA55);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  tft.setCursor(295, 48);
  tft.print("OK");

  tft.drawFastHLine(0, 84, 320, C_DIV);
}

void drawEntryKeyboard() {
  tft.fillRect(0, 85, 320, 155, C_PASS_BG);
  // Force uppercase keyboard for callsign, allow mixed for grid
  const char** rows = numMode ? rowsNum : rowsUpper;

  // Row 0
  {
    int n = strlen(rows[0]);
    int startX = (320 - n * (KB_KW + KB_GAP) + KB_GAP) / 2;
    for (int i = 0; i < n; i++) {
      char lbl[2] = { rows[0][i], 0 };
      drawKey(startX + i * (KB_KW + KB_GAP), KB_Y0, KB_KW, KB_KH, lbl, C_KEY_BG);
    }
  }

  // Row 1
  {
    int n = strlen(rows[1]);
    int startX = (320 - n * (KB_KW + KB_GAP) + KB_GAP) / 2;
    for (int i = 0; i < n; i++) {
      char lbl[2] = { rows[1][i], 0 };
      drawKey(startX + i * (KB_KW + KB_GAP), KB_Y0 + KB_KH + KB_GAP, KB_KW, KB_KH, lbl, C_KEY_BG);
    }
  }

  // Row 2: NUM toggle + letters + DEL
  {
    int rowY = KB_Y0 + 2 * (KB_KH + KB_GAP);
    int n = strlen(rows[2]);
    drawKey(2, rowY, 40, KB_KH, numMode ? "ABC" : "123", C_KEY_SPL);
    int available = 320 - 4 - 40 - 42;
    int kw2 = (available - (n - 1) * KB_GAP) / n;
    for (int i = 0; i < n; i++) {
      char lbl[2] = { rows[2][i], 0 };
      drawKey(44 + i * (kw2 + KB_GAP), rowY, kw2, KB_KH, lbl, C_KEY_BG);
    }
    drawKey(278, rowY, 40, KB_KH, "DEL", 0xFFAA44);
  }

  // Row 3: mode toggle | / | SPACE | CANCEL | SAVE
  {
    int rowY = KB_Y0 + 3 * (KB_KH + KB_GAP);
    drawKey(2,   rowY, 44, KB_KH, numMode ? "ABC" : "123", C_KEY_SPL);
    drawKey(48,  rowY, 28, KB_KH, "/",     C_KEY_BG);
    drawKey(78,  rowY, 80, KB_KH, "SPACE", C_KEY_BG);
    drawKey(160, rowY, 70, KB_KH, "CANCEL", 0xFFAA44);
    drawKey(232, rowY, 86, KB_KH, "SAVE",   0x00AA55);
  }
}

void drawEntryScreen() {
  tft.fillScreen(C_PASS_BG);
  drawEntryField();
  drawEntryKeyboard();
}

bool handleEntryTouch(uint16_t tx, uint16_t ty) {
  bool isCallsign = (uiState == STATE_CALLSIGN_ENTRY);

  // OK button
  if (tx >= 290 && tx <= 316 && ty >= 36 && ty <= 68) {
    goto saveEntry;
  }

  {
    const char** rows = numMode ? rowsNum : rowsUpper;

    // Row 0
    if (ty >= KB_Y0 && ty < KB_Y0 + KB_KH) {
      int n = strlen(rows[0]);
      int startX = (320 - n * (KB_KW + KB_GAP) + KB_GAP) / 2;
      int idx = (tx - startX) / (KB_KW + KB_GAP);
      if (idx >= 0 && idx < n && tx >= (uint16_t)startX) {
        char c = rows[0][idx];
        if (isCallsign) c = toupper(c);
        entryBuffer += c;
        drawEntryField();
        return false;
      }
    }

    // Row 1
    if (ty >= KB_Y0 + KB_KH + KB_GAP && ty < KB_Y0 + 2*(KB_KH + KB_GAP)) {
      int n = strlen(rows[1]);
      int startX = (320 - n * (KB_KW + KB_GAP) + KB_GAP) / 2;
      int idx = (tx - startX) / (KB_KW + KB_GAP);
      if (idx >= 0 && idx < n && tx >= (uint16_t)startX) {
        char c = rows[1][idx];
        if (isCallsign) c = toupper(c);
        entryBuffer += c;
        drawEntryField();
        return false;
      }
    }

    // Row 2
    {
      int rowY = KB_Y0 + 2*(KB_KH + KB_GAP);
      if (ty >= rowY && ty < rowY + KB_KH) {
        if (tx >= 2 && tx < 42) {
          numMode = !numMode;
          drawEntryKeyboard(); return false;
        }
        if (tx >= 278 && tx <= 318) {
          if (entryBuffer.length() > 0) entryBuffer.remove(entryBuffer.length() - 1);
          drawEntryField(); return false;
        }
        int n = strlen(rows[2]);
        int available = 320 - 4 - 40 - 42;
        int kw2 = (available - (n - 1) * KB_GAP) / n;
        int idx = (tx - 44) / (kw2 + KB_GAP);
        if (idx >= 0 && idx < n && tx >= 44 && tx < 278) {
          char c = rows[2][idx];
          if (isCallsign) c = toupper(c);
          entryBuffer += c;
          drawEntryField();
          return false;
        }
      }
    }

    // Row 3
    {
      int rowY = KB_Y0 + 3*(KB_KH + KB_GAP);
      if (ty >= rowY && ty < rowY + KB_KH) {
        if (tx >= 2 && tx < 46) {
          numMode = !numMode;
          drawEntryKeyboard(); return false;
        }
        if (tx >= 48 && tx < 76) {
          entryBuffer += '/';
          drawEntryField(); return false;
        }
        if (tx >= 78 && tx < 158) {
          entryBuffer += ' ';
          drawEntryField(); return false;
        }
        if (tx >= 160 && tx < 230) {
          // CANCEL
          uiState = STATE_MENU;
          settingsPage = 1;
          drawSettingsMenu();
          return false;
        }
        if (tx >= 232 && tx <= 318) {
          goto saveEntry;
        }
      }
    }
  }
  return false;

saveEntry:
  entryBuffer.trim();
  if (isCallsign) {
    entryBuffer.toUpperCase();
    entryBuffer.toCharArray(myCallsign, sizeof(myCallsign));
  } else {
    entryBuffer.toCharArray(myGrid, sizeof(myGrid));
  }
  saveSettingsSD();
  uiState = STATE_MENU;
  settingsPage = 1;
  drawSettingsMenu();
  return false;
}

// ── Handle touches in password screen ────────────────────────
bool handlePassTouch(uint16_t tx, uint16_t ty) {
  // Show/hide toggle — only field changes
  if (tx >= 272 && tx <= 314 && ty >= 50 && ty <= 76) {
    passVisible = !passVisible;
    drawPassField();
    return false;
  }

  const char** rows = numMode ? rowsNum : (shiftOn ? rowsUpper : rowsLower);

  // Row 0
  if (ty >= KB_Y0 && ty < KB_Y0 + KB_KH) {
    int n = strlen(rows[0]);
    int startX = (320 - n * (KB_KW + KB_GAP) + KB_GAP) / 2;
    int idx = (tx - startX) / (KB_KW + KB_GAP);
    if (idx >= 0 && idx < n && tx >= startX) {
      newPassword += rows[0][idx];
      bool wasShift = shiftOn;
      if (shiftOn && !numMode) shiftOn = false;
      drawPassField();
      if (wasShift) drawPassKeyboard(); // update SHF key highlight
      return false;
    }
  }

  // Row 1
  if (ty >= KB_Y0 + KB_KH + KB_GAP && ty < KB_Y0 + 2*(KB_KH + KB_GAP)) {
    int n = strlen(rows[1]);
    int startX = (320 - n * (KB_KW + KB_GAP) + KB_GAP) / 2;
    int idx = (tx - startX) / (KB_KW + KB_GAP);
    if (idx >= 0 && idx < n && tx >= startX) {
      newPassword += rows[1][idx];
      bool wasShift = shiftOn;
      if (shiftOn && !numMode) shiftOn = false;
      drawPassField();
      if (wasShift) drawPassKeyboard();
      return false;
    }
  }

  // Row 2
  {
    int rowY = KB_Y0 + 2*(KB_KH + KB_GAP);
    if (ty >= rowY && ty < rowY + KB_KH) {
      if (tx >= 2 && tx < 42) {
        // Shift/Num key — keyboard layout changes
        if (numMode) { numMode = false; }
        else         { shiftOn = !shiftOn; }
        drawPassKeyboard(); return false;
      }
      if (tx >= 278 && tx <= 318) {
        // Backspace — only field changes
        if (newPassword.length() > 0) newPassword.remove(newPassword.length() - 1);
        drawPassField(); return false;
      }
      int n = strlen(rows[2]);
      int available = 320 - 4 - 40 - 42;
      int kw2 = (available - (n - 1) * KB_GAP) / n;
      int idx = (tx - 44) / (kw2 + KB_GAP);
      if (idx >= 0 && idx < n && tx >= 44 && tx < 278) {
        newPassword += rows[2][idx];
        bool wasShift = shiftOn;
        if (shiftOn && !numMode) shiftOn = false;
        drawPassField();
        if (wasShift) drawPassKeyboard();
        return false;
      }
    }
  }

  // Row 3
  {
    int rowY = KB_Y0 + 3*(KB_KH + KB_GAP);
    if (ty >= rowY && ty < rowY + KB_KH) {
      if (tx >= 2 && tx < 46) {          // 123/ABC toggle — layout changes
        numMode = !numMode; shiftOn = false;
        drawPassKeyboard(); return false;
      }
      if (tx >= 48 && tx < 76) {         // @
        newPassword += '@';
        drawPassField(); return false;
      }
      if (tx >= 78 && tx < 198) {        // SPACE
        newPassword += ' ';
        drawPassField(); return false;
      }
      if (tx >= 200 && tx < 228) {       // .
        newPassword += '.';
        drawPassField(); return false;
      }
      if (tx >= 230 && tx < 258) {       // -
        newPassword += '-';
        drawPassField(); return false;
      }
      if (tx >= 260 && tx <= 318) {      // CONNECT
        // Attempt WiFi connection
        tft.fillRect(0, 0, 320, 84, 0x001020);
        tft.setTextSize(1);
        tft.setTextColor(C_AMBER);
        tft.setCursor(6, 30);
        tft.print("Connecting...");

        WiFi.disconnect(true);
        delay(300);
        WiFi.begin(wifiSSIDs[selectedNet].c_str(), newPassword.c_str());

        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
          delay(500);
          tft.fillRect(200, 28, 10, 10, (millis() / 500) % 2 ? C_AMBER : C_PASS_BG);
        }

        if (WiFi.status() == WL_CONNECTED) {
          // Save credentials to SD
          strncpy(sdSSID, wifiSSIDs[selectedNet].c_str(), sizeof(sdSSID) - 1);
          strncpy(sdPassword, newPassword.c_str(), sizeof(sdPassword) - 1);
          saveSettingsSD();

          tft.fillRect(0, 0, 320, 84, 0x001020);
          tft.setTextSize(1);
          tft.setTextColor(C_GREEN);
          tft.setCursor(6, 30);
          tft.print("Connected! Returning...");
          // Start NTP
          configTime(0, 0, "pool.ntp.org", "time.nist.gov");
          delay(1500);
          uiState = STATE_MAIN;
          lastFetch = 0; // force refresh
          switch (dispMode) {
            case DISP_SOLAR:   drawDisplay(solar);           break;
            case DISP_DX:      drawDXCluster();              break;
            case DISP_MAP:     drawGreylineMap();             break;
            case DISP_CONTEST: drawContestCalendar();         break;
            case DISP_CLOCK:   drawClockDisplay();            break;
            case DISP_POTA:    drawPOTASpots();               break;
            case DISP_SOTA:    drawSOTASpots();               break;
            case DISP_WSPR:    drawWSPR();                    break;
            case DISP_VOACAP:  calcVOACAP(); drawVOACAP();   break;
            case DISP_PSKR:    drawPSKMap();                  break;
          }
        } else {
          tft.fillRect(0, 0, 320, 84, 0x001020);
          tft.setTextSize(1);
          tft.setTextColor(C_RED);
          tft.setCursor(6, 20);
          tft.print("Failed to connect.");
          tft.setTextColor(C_DIM);
          tft.setCursor(6, 35);
          tft.print("Check password and try again.");
          tft.fillRoundRect(90, 54, 140, 24, 4, C_KEY_SPL);
          tft.setTextColor(C_WHITE);
          tft.setCursor(118, 61);
          tft.print("Try Again");
          // Wait for tap then redraw keyboard
          delay(2000);
          drawPasswordEntry();
        }
        return false;
      }
    }
  }

  return false;
}

// ── Handle touches in wifi list ───────────────────────────────
bool handleWifiListTouch(uint16_t tx, uint16_t ty) {
  // Back button
  if (ty < 36 && tx < 60) {
    uiState = STATE_MENU;
    WiFi.scanDelete();
    drawSettingsMenu();
    return false;
  }

  // Network rows (y=40, each 33px tall)
  int maxVisible = 6;
  for (int i = 0; i < maxVisible && i < wifiCount; i++) {
    int rowY = 40 + i * 33;
    if (ty >= rowY && ty < rowY + 30) {
      selectedNet = i;
      newPassword = "";
      shiftOn = false;
      numMode = false;
      passVisible = false;
      uiState = STATE_WIFI_PASS;
      drawPasswordEntry();
      return false;
    }
  }
  return false;
}

// ── Handle touches in settings menu ──────────────────────────
bool handleMenuTouch(uint16_t tx, uint16_t ty) {
  // Close button
  if (tx >= 278 && ty <= 36) {
    uiState = STATE_MAIN;
    // Restore previous screen; if it was disabled, find the next enabled one in order
    dispMode = prevDispMode;
    if (!screenEnabled[(int)dispMode]) {
      int curPos = 0;
      for (int i = 0; i < NUM_SCREENS; i++) {
        if (screenOrder[i] == (int)dispMode) { curPos = i; break; }
      }
      for (int i = 0; i < NUM_SCREENS; i++) {
        curPos = (curPos + 1) % NUM_SCREENS;
        if (screenEnabled[screenOrder[curPos]]) {
          dispMode = (DisplayMode)screenOrder[curPos];
          break;
        }
      }
    }
    lastSwitch = millis();
    // Redraw whichever screen we landed on
    switch (dispMode) {
      case DISP_SOLAR:   drawDisplay(solar);  break;
      case DISP_DX:      drawDXCluster();     break;
      case DISP_MAP:     drawGreylineMap();   break;
      case DISP_CONTEST: drawContestCalendar(); break;
      case DISP_CLOCK:   drawClockDisplay();  break;
      case DISP_POTA:    drawPOTASpots();     break;
      case DISP_SOTA:    drawSOTASpots();     break;
      case DISP_WSPR:    drawWSPR();          break;
      case DISP_VOACAP:  drawVOACAP();        break;
      case DISP_PSKR:    drawPSKMap();        break;
    }
    return false;
  }

  // Navigation bar (y >= 210)
  if (ty >= 210) {
    if (tx < 60 && settingsPage > 0) {
      settingsPage--;
      drawSettingsMenu();
      return false;
    }
    if (tx > 260 && settingsPage < 6) {
      settingsPage++;
      drawSettingsMenu();
      return false;
    }
    return false;
  }

  if (settingsPage == 0) {
    // Page 1: WiFi option (y=52 to y=100)
    if (ty >= 52 && ty <= 100) {
      uiState = STATE_WIFI_LIST;
      wifiCount = 0;
      triggerScan();
      drawWifiList();
      return false;
    }
    // LED toggle (y=112 to y=152)
    if (ty >= 112 && ty <= 152) {
      ledOn = !ledOn;
      digitalWrite(4, ledOn ? LOW : HIGH);
      drawSettingsMenu();
      return false;
    }
    // UTC Offset minus (x=170-206, y=162-200)
    if (ty >= 162 && ty <= 200 && tx >= 170 && tx <= 206) {
      if (tzOffset > -12) tzOffset--;
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
    // UTC Offset plus (x=266-302, y=162-200)
    if (ty >= 162 && ty <= 200 && tx >= 266 && tx <= 302) {
      if (tzOffset < 14) tzOffset++;
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
  } else if (settingsPage == 1) {
    // Page 2: Callsign (y=52 to y=92)
    if (ty >= 52 && ty <= 92) {
      entryBuffer = String(myCallsign);
      numMode = false;
      shiftOn = true;
      uiState = STATE_CALLSIGN_ENTRY;
      drawEntryScreen();
      return false;
    }
    // Grid square (y=102 to y=142)
    if (ty >= 102 && ty <= 142) {
      entryBuffer = String(myGrid);
      numMode = false;
      shiftOn = true;
      uiState = STATE_GRID_ENTRY;
      drawEntryScreen();
      return false;
    }
    // Brightness minus (x=170-206, y=152-182)
    if (ty >= 152 && ty <= 182 && tx >= 170 && tx <= 206 && !autoBrightness) {
      brightness = (brightness >= 25) ? brightness - 25 : 0;
      applyBrightness();
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
    // Brightness plus (x=266-302, y=152-182)
    if (ty >= 152 && ty <= 182 && tx >= 266 && tx <= 302 && !autoBrightness) {
      brightness = (brightness <= 230) ? brightness + 25 : 255;
      applyBrightness();
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
  } else if (settingsPage == 2) {
    // Auto-cycle toggle (y=44 to y=78)
    if (ty >= 44 && ty <= 78) {
      autoCycle = !autoCycle;
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
    // Screen timeout minus (x=150-186, y=86-120)
    if (ty >= 86 && ty <= 120 && tx >= 150 && tx <= 186) {
      if (screenTimeout > 0) screenTimeout--;
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
    // Screen timeout plus (x=246-282, y=86-120)
    if (ty >= 86 && ty <= 120 && tx >= 246 && tx <= 282) {
      if (screenTimeout < 30) screenTimeout++;
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
    // Auto Brightness toggle (y=128-162)
    if (ty >= 128 && ty <= 162) {
      autoBrightness = !autoBrightness;
      if (!autoBrightness) applyBrightness();  // restore manual brightness
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
    // Rotate 180° toggle (y=170-204)
    if (ty >= 170 && ty <= 204) {
      rotateDisplay = !rotateDisplay;
      tft.setRotation(rotateDisplay ? 3 : 1);
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
  } else if (settingsPage == 3) {
    // Cycle speed minus (x=50-100, y=108-144)
    if (ty >= 108 && ty <= 144 && tx >= 50 && tx <= 100) {
      if (cycleSpeed > 5) cycleSpeed -= 5;
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
    // Cycle speed plus (x=220-270, y=108-144)
    if (ty >= 108 && ty <= 144 && tx >= 220 && tx <= 270) {
      if (cycleSpeed < 120) cycleSpeed += 5;
      saveSettingsSD();
      drawSettingsMenu();
      return false;
    }
  }
  // Page 5: About — no interactive elements
  // Page 6: OTA Update
  else if (settingsPage == 5) {
    // "Check for Update" button (y=110 to y=154)
    if (ty >= 110 && ty <= 154 && tx >= 30 && tx <= 290) {
      performOTACheck();
      return false;
    }
  }
  else if (settingsPage == 6) {
    // Screen toggle/order grid: 2 columns × 5 rows
    if (ty >= 42 && ty < 42 + 5 * 33) {
      int col = (tx >= 164) ? 1 : 0;
      int row = (ty - 42) / 33;
      int pos = col * 5 + row;
      int ax  = col == 0 ?  4 : 164;  // arrow area x
      int bx  = col == 0 ? 22 : 182;  // button x

      if (pos >= 0 && pos < NUM_SCREENS) {
        if (tx >= ax && tx < ax + 16) {
          // Arrow area tapped — reorder
          int rowY = ty - (42 + row * 33);
          if (rowY < 14 && row > 0) {
            // ▲ — swap with item above
            int tmp = screenOrder[pos]; screenOrder[pos] = screenOrder[pos - 1]; screenOrder[pos - 1] = tmp;
            saveSettingsSD();
            drawSettingsMenu();
          } else if (rowY >= 14 && row < 4) {
            // ▼ — swap with item below
            int tmp = screenOrder[pos]; screenOrder[pos] = screenOrder[pos + 1]; screenOrder[pos + 1] = tmp;
            saveSettingsSD();
            drawSettingsMenu();
          }
        } else if (tx >= bx) {
          // Toggle button tapped
          int i = screenOrder[pos];
          if (screenEnabled[i]) {
            int cnt = 0;
            for (int j = 0; j < NUM_SCREENS; j++) if (screenEnabled[j]) cnt++;
            if (cnt <= 1) return false;
          }
          screenEnabled[i] = !screenEnabled[i];
          saveSettingsSD();
          drawSettingsMenu();
        }
        return false;
      }
    }
  }

  return false;
}

// ── Fetch ─────────────────────────────────────────────────────
bool fetchData(SolarData& d) {
  WiFiClientSecure wifiClient;
  wifiClient.setInsecure();
  HTTPClient http;
  http.begin(wifiClient, DATA_URL);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", code);
    tft.fillRect(0, 80, 320, 40, C_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_RED);
    tft.setCursor(10, 90);
    tft.printf("HTTP error: %d", code);
    http.end();
    return false;
  }
  String xml = http.getString();
  http.end();

  setField(d.solarflux,   sizeof(d.solarflux),   tagValue(xml, "solarflux"));
  setField(d.sunspots,    sizeof(d.sunspots),     tagValue(xml, "sunspots"));
  setField(d.kindex,      sizeof(d.kindex),       tagValue(xml, "kindex"));
  setField(d.aindex,      sizeof(d.aindex),       tagValue(xml, "aindex"));
  setField(d.xray,        sizeof(d.xray),         tagValue(xml, "xray"));
  setField(d.solarwind,   sizeof(d.solarwind),    tagValue(xml, "solarwind"));
  setField(d.geomagfield, sizeof(d.geomagfield),  tagValue(xml, "geomagfield"));
  setField(d.signalnoise, sizeof(d.signalnoise),  tagValue(xml, "signalnoise"));
  setField(d.updated,     sizeof(d.updated),      tagValue(xml, "updated"));
  parseBands(xml, d);
  return true;
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Brightness PWM (LEDC on GPIO 27)
  ledcAttach(27, 5000, 8);
  ledcWrite(27, 255); // full brightness initially

  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);    // red LED off (active LOW)

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);
  touch.init(33, 32, 25, 21);

  // Initialize SD card on separate SPI bus
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSPI)) {
    sdAvailable = true;
    Serial.println("SD card initialized");
    loadSettingsSD();
    applyBrightness();
    if (rotateDisplay) tft.setRotation(3);
  } else {
    Serial.println("SD card not found - using defaults");
  }

  // Set dispMode to first enabled screen in screenOrder
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (screenEnabled[screenOrder[i]]) {
      dispMode = (DisplayMode)screenOrder[i];
      prevDispMode = dispMode;
      break;
    }
  }

  // Determine WiFi credentials: SD overrides hardcoded if available
  const char* useSSID = (strlen(sdSSID) > 0) ? sdSSID : SSID;
  const char* usePass = (strlen(sdSSID) > 0) ? sdPassword : PASSWORD;

  bool hasPreset = (strlen(useSSID) > 0);

  if (hasPreset) {
    tft.setTextSize(2);
    tft.setTextColor(C_CYAN);
    tft.setCursor(10, 100);
    tft.print("Connecting to WiFi...");

    WiFi.begin(useSSID, usePass);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {
    // No preset or connection failed — show WiFi picker immediately
    tft.fillScreen(C_MENU_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_RED);
    tft.setCursor(10, 100);
    if (!hasPreset) {
      tft.print("No network configured.");
    } else {
      tft.print("WiFi failed.");
    }
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(10, 126);
    tft.print("Opening network selector...");
    delay(1200);

    uiState = STATE_WIFI_LIST;
    wifiCount = 0;
    triggerScan();
    drawWifiList();
    return;
  }

  // Kick off NTP sync (non-blocking — syncs in background)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  tft.setTextColor(C_CYAN);
  tft.setCursor(10, 130);
  tft.print("Fetching solar data...");

  fetchInProgress = true;
  if (fetchData(solar)) {
    fetchInProgress = false;
    lastFetchTime = millis();
    switch (dispMode) {
      case DISP_SOLAR:   drawDisplay(solar);           break;
      case DISP_DX:      drawDXCluster();              break;
      case DISP_MAP:     drawGreylineMap();             break;
      case DISP_CONTEST: drawContestCalendar();         break;
      case DISP_CLOCK:   drawClockDisplay();            break;
      case DISP_POTA:    drawPOTASpots();               break;
      case DISP_SOTA:    drawSOTASpots();               break;
      case DISP_WSPR:    drawWSPR();                    break;
      case DISP_VOACAP:  calcVOACAP(); drawVOACAP();   break;
      case DISP_PSKR:    drawPSKMap();                  break;
    }
  } else {
    fetchInProgress = false;
    tft.setTextColor(C_RED);
    tft.setCursor(10, 160);
    tft.print("Fetch failed.");
  }
  lastFetch = millis();

  lastActivity = millis();  // init screen timeout tracker

  // Save WiFi creds to SD on successful boot connect
  if (sdAvailable && WiFi.status() == WL_CONNECTED && strlen(sdSSID) == 0) {
    strncpy(sdSSID, WiFi.SSID().c_str(), sizeof(sdSSID) - 1);
    strncpy(sdPassword, PASSWORD, sizeof(sdPassword) - 1);
    saveSettingsSD();
  }
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
  TOUCHINFO ti;
  bool touched = (touch.getSamples(&ti) > 0);
  uint16_t rawX = touched ? ti.x[0] : 0;
  uint16_t rawY = touched ? ti.y[0] : 0;

  // CST820 is portrait (240×320). Screen is landscape (320×240).
  // rotation=1 (normal):  tx = rawY,       ty = 239 - rawX
  // rotation=3 (180°):    tx = 319 - rawY, ty = rawX
  uint16_t tx = rotateDisplay ? (319 - rawY) : rawY;
  uint16_t ty = rotateDisplay ? rawX : (239 - rawX);

  // ── Screen sleep: wake on any touch ───────────────────────
  if (touched && screenAsleep) {
    screenAsleep = false;
    applyBrightness();
    lastActivity = millis();
    // Redraw current screen
    if (uiState == STATE_MAIN) {
      switch (dispMode) {
        case DISP_SOLAR:   drawDisplay(solar); break;
        case DISP_DX:      drawDXCluster(); break;
        case DISP_MAP:     drawGreylineMap(); break;
        case DISP_CONTEST: drawContestCalendar(); break;
        case DISP_CLOCK:   drawClockDisplay(); break;
        case DISP_POTA:    drawPOTASpots(); break;
      case DISP_SOTA:    drawSOTASpots(); break;
      case DISP_WSPR:    drawWSPR();      break;
        case DISP_VOACAP:  drawVOACAP();    break;
        case DISP_PSKR:    drawPSKMap();    break;
      }
    } else if (uiState == STATE_MENU) {
      drawSettingsMenu();
    }
    wasTouched = true;
    return;
  }

  // ── Touch handling with debounce ──────────────────────────
  if (touched && !wasTouched && millis() - lastTouch > TOUCH_DEBOUNCE) {
    wasTouched  = true;
    lastTouch   = millis();
    lastActivity = millis();

    switch (uiState) {
      case STATE_MAIN:
        if (tx >= 200 && ty <= 80) {
          // Top-right corner → settings
          selectedDXSpot = -1;
          prevDispMode = dispMode;
          uiState = STATE_MENU;
          drawSettingsMenu();
        } else if (dispMode == DISP_DX) {
          if (selectedDXSpot >= 0) {
            // Dismiss bearing modal — redraw DX screen
            selectedDXSpot = -1;
            drawDXCluster();
          } else if (ty >= DX_ROW_Y0 && ty < 240 && dxCount > 0) {
            // Tap on a spot row — show bearing
            bool rightCol = (tx >= DX_COL_MID);
            int slot = (ty - DX_ROW_Y0) / DX_ROW_H;
            int idx  = rightCol ? (5 + slot) : slot;
            if (idx >= 0 && idx < dxCount) {
              selectedDXSpot = idx;
              drawDXBearingModal(selectedDXSpot);
            } else {
              advanceDisplayMode();
            }
          } else {
            advanceDisplayMode();
          }
        } else {
          // Anywhere else → skip to next screen
          advanceDisplayMode();
        }
        break;


      case STATE_MENU:
        handleMenuTouch(tx, ty);
        break;

      case STATE_WIFI_LIST:
        handleWifiListTouch(tx, ty);
        break;

      case STATE_WIFI_PASS:
        handlePassTouch(tx, ty);
        break;

      case STATE_CALLSIGN_ENTRY:
      case STATE_GRID_ENTRY:
        handleEntryTouch(tx, ty);
        break;
    }
  }

  if (!touched) wasTouched = false;

  // ── WiFi scan polling (while on wifi list screen) ─────────
  if (uiState == STATE_WIFI_LIST) {
    if (scanPending) {
      int result = WiFi.scanComplete();
      if (result >= 0) {
        scanPending = false;
        wifiCount = min(result, MAX_NETS);
        for (int i = 0; i < wifiCount; i++) {
          wifiSSIDs[i] = WiFi.SSID(i);
          wifiRSSI[i]  = WiFi.RSSI(i);
        }
        drawWifiList();
      } else if (result == WIFI_SCAN_FAILED || millis() - lastScan > 15000) {
        // Scan failed or timed out — reset so re-scan can trigger
        scanPending = false;
      }
    } else if (millis() - lastScan > 8000) {
      // Re-scan every 8 seconds
      triggerScan();
      drawWifiList(); // redraws "Scanning..."
    }
    return; // skip solar refresh while in wifi list
  }

  // ── Main screen: solar / DX alternation ───────────────────
  if (uiState == STATE_MAIN) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      delay(5000);
      return;
    }

    // Auto-cycle display
    if (autoCycle && !screenAsleep && millis() - lastSwitch >= (unsigned long)cycleSpeed * 1000UL) {
      advanceDisplayMode();
    }

    // Refresh solar data every 60 seconds (regardless of which view is showing)
    if (millis() - lastFetch >= REFRESH_MS) {
      lastFetch = millis();
      fetchInProgress = true;
      if (dispMode == DISP_SOLAR) drawRefreshIndicator();
      fetchData(solar);
      fetchInProgress = false;
      lastFetchTime = millis();
      if (dispMode == DISP_SOLAR)  drawDisplay(solar);
      if (dispMode == DISP_MAP)    drawGreylineMap();
      if (dispMode == DISP_CLOCK)  drawClockDisplay();
      if (dispMode == DISP_VOACAP) { calcVOACAP(); drawVOACAP(); }
    }

    // Refresh clock time every minute (only updates time area, no flicker)
    if (dispMode == DISP_CLOCK) {
      static unsigned long lastClockDraw = 0;
      if (millis() - lastClockDraw >= 60000) {
        lastClockDraw = millis();
        drawClockDisplay();
      }
      // Fetch weather every 30 min if grid + owmKey are set
      if (strlen(owmKey) > 0 && strlen(myGrid) >= 4 &&
          millis() - lastWxFetch >= WX_REFRESH_MS) {
        float lat = (float)(myGrid[1] - 'A') * 10.0f + (myGrid[3] - '0') + 0.5f - 90.0f;
        float lon = (float)(myGrid[0] - 'A') * 20.0f + (myGrid[2] - '0') * 2.0f + 1.0f - 180.0f;
        fetchWeather(lat, lon);
        drawClockDisplay();
      }
    }

    // Refresh SWPC alerts every 10 minutes
    if (millis() - lastSWPCFetch >= SWPC_REFRESH_MS) {
      fetchSWPCAlerts();
      if (dispMode == DISP_SOLAR) drawDisplay(solar);
    }

    // Refresh WSPR every 5 minutes when on WSPR screen
    if (dispMode == DISP_WSPR && millis() - lastWsprFetch >= WSPR_REFRESH_MS) {
      fetchInProgress = true;
      fetchWSPR();
      fetchInProgress = false;
      drawWSPR();
    }

    // Recalc VOACAP predictions every 5 minutes when on VOACAP screen
    if (dispMode == DISP_VOACAP && millis() - lastVoacapCalc >= VOACAP_REFRESH_MS) {
      calcVOACAP();
      drawVOACAP();
    }

    // Refresh PSKReporter every minute when on PSKReporter screen
    if (dispMode == DISP_PSKR && millis() - lastPSKRFetch >= PSKR_REFRESH_MS) {
      fetchInProgress = true;
      fetchPSKReporter();
      fetchInProgress = false;
      drawPSKMap();
    }

    // Auto-brightness — sample LDR every 2 seconds
    if (autoBrightness && !screenAsleep) {
      static unsigned long lastLDR = 0;
      if (millis() - lastLDR >= 2000) {
        lastLDR = millis();
        updateAutoBrightness();
      }
    }

    // Screen timeout — go to sleep after N minutes of no touch
    if (screenTimeout > 0 && !screenAsleep &&
        millis() - lastActivity > (unsigned long)screenTimeout * 60000UL) {
      screenAsleep = true;
      ledcWrite(27, 0);  // backlight off
    }
  }
}
