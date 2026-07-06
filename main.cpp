// ===========================================================================
//  Cyberdeck Radar Scope  -  ESP32-3248S035 (3.5" ST7796)
//
//  Green-phosphor PPI (plan position indicator). Polls the Pi's radar_feed
//  service for ADS-B contacts and paints them on a sweeping radar scope with
//  phosphor persistence, range rings, and a contact list.
//
//  Pair with: pi/radar_feed.py (fed by readsb + an RTL-SDR dongle).
// ===========================================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <SPI.h>
#include <LittleFS.h>
#include <math.h>

// --------------------------- USER CONFIG -----------------------------------
static const char* DEFAULT_WIFI_SSID = "MyNetwork";
static const char* DEFAULT_WIFI_PASS = "MyPassword";
static const char* DEFAULT_PI_HOST = "192.168.1.50";   // Pi running radar_feed.py
static const uint16_t DEFAULT_PI_PORT = 8090;

static const uint32_t DEFAULT_POLL_MS = 2000;   // data refresh
static const float    DEFAULT_SWEEP_DPS = 90.0; // sweep speed (degrees / second)
static const char*    DEFAULT_FONT_MED_CANDIDATES = "orbitron_med,orbitron_hero,orbitron_small";
static const char*    DEFAULT_FONT_SMALL_CANDIDATES = "orbitron_small,orbitron_med,orbitron_hero";

enum FontMode : uint8_t { FONT_MODE_DEFAULT = 0, FONT_MODE_LITTLEFS = 1 };
static const uint8_t DEFAULT_FONT_MODE = FONT_MODE_DEFAULT;
static const bool    DEFAULT_ENABLE_SPRITE_LADDER = false;
// ---------------------------------------------------------------------------

#ifndef SD_CS
#define SD_CS 5
#endif

#ifndef SD_SCK
#define SD_SCK 18
#endif

#ifndef SD_MISO
#define SD_MISO 19
#endif

#ifndef SD_MOSI
#define SD_MOSI 23
#endif

#ifndef ENABLE_SD_CONFIG
#define ENABLE_SD_CONFIG 0
#endif

SPIClass sdSpi(VSPI);

struct AppConfig {
  char wifiSsid[33];
  char wifiPass[65];
  char piHost[64];
  uint16_t piPort;
  uint32_t pollMs;
  float sweepDps;
  uint8_t fontMode;
  bool enableSpriteLadder;
  char fontMedCandidates[96];
  char fontSmallCandidates[96];
};

AppConfig cfg = {
  "",
  "",
  "",
  DEFAULT_PI_PORT,
  DEFAULT_POLL_MS,
  DEFAULT_SWEEP_DPS,
  DEFAULT_FONT_MODE,
  DEFAULT_ENABLE_SPRITE_LADDER,
  "",
  ""
};

const char* CONFIG_CANDIDATES[] = {
  "/radar.ini",
  "/config/radar.ini",
  "/radar/config.ini"
};
static const size_t CONFIG_CANDIDATE_COUNT = sizeof(CONFIG_CANDIDATES) / sizeof(CONFIG_CANDIDATES[0]);

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite scope = TFT_eSprite(&tft);

// scope geometry
static const int SX = 16, SY = 20;      // sprite top-left on screen
int scopeD = 256;                       // runtime scope sprite size (px)
int scopeCX = scopeD / 2;
int scopeCY = scopeD / 2;
int scopeR = scopeD / 2 - 6;            // outer ring radius

// 8-bit palette indices
enum { BG, RING, RING2, BLIP, SWEEPC, TRAIL1, TRAIL2, AMBER, TXT, GRIDX };
uint16_t palette[16];
bool scopePaletteMode = false;
bool scopeSupportsDecay = false;
bool littleFsReady = false;
bool orbitronMedAvailable = false;
bool orbitronSmallAvailable = false;
char activeMedFontBase[32] = "";
char activeSmallFontBase[32] = "";

enum PanelFontRole : uint8_t { PANEL_FONT_BUILTIN = 0, PANEL_FONT_MEDIUM = 1, PANEL_FONT_SMALL = 2 };
PanelFontRole activePanelFont = PANEL_FONT_BUILTIN;

static const char* ORBITRON_MED_BASE = "orbitron_med";
static const char* ORBITRON_SMALL_BASE = "orbitron_small";

struct AC {
  char fl[10];
  float rng, brg;
  int alt, trk, gs;
  float inten;     // phosphor intensity 0..1
};
static const int MAXAC = 30;
AC ac[MAXAC];
int nac = 0;
int maxnm = 40;
bool feedOk = false;

float sweep = 0;
uint32_t lastPoll = 0, lastFrame = 0;

enum VisualProfile : uint8_t { PROFILE_CLASSIC = 1, PROFILE_CRT = 2, PROFILE_FAST = 3 };
uint8_t activeProfile = PROFILE_CRT;
uint8_t phosphorDecay = 200;   // 0..255, lower = faster fade
int sweepSegments = 28;
float sweepStepDeg = 1.2f;
bool blipGlow = true;

const char* profileName(uint8_t p) {
  switch (p) {
    case PROFILE_CLASSIC: return "CLASSIC";
    case PROFILE_FAST: return "FAST";
    default: return "CRT";
  }
}

void applyVisualProfile(uint8_t p) {
  activeProfile = p;
  switch (p) {
    case PROFILE_CLASSIC:
      phosphorDecay = 220;
      sweepSegments = 16;
      sweepStepDeg = 2.3f;
      blipGlow = false;
      break;
    case PROFILE_FAST:
      phosphorDecay = 205;
      sweepSegments = 20;
      sweepStepDeg = 1.8f;
      blipGlow = false;
      break;
    default:
      phosphorDecay = 235;
      sweepSegments = 28;
      sweepStepDeg = 1.2f;
      blipGlow = true;
      break;
  }
}

void drawPanel();
void handleSerialControls();

bool useLittleFsPanelFonts() {
  return cfg.fontMode == FONT_MODE_LITTLEFS;
}

bool toBool(const String& value, bool fallback) {
  if (value.equalsIgnoreCase("1") || value.equalsIgnoreCase("true") || value.equalsIgnoreCase("yes") || value.equalsIgnoreCase("on")) return true;
  if (value.equalsIgnoreCase("0") || value.equalsIgnoreCase("false") || value.equalsIgnoreCase("no") || value.equalsIgnoreCase("off")) return false;
  return fallback;
}

bool resolveFontFromCandidates(const char* csvCandidates, char* outBase, size_t outBaseLen) {
  if (!littleFsReady || !csvCandidates || !outBase || outBaseLen == 0) return false;

  outBase[0] = 0;
  char buf[128];
  strncpy(buf, csvCandidates, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;

  char* savePtr = nullptr;
  char* token = strtok_r(buf, ",", &savePtr);
  while (token != nullptr) {
    String base = String(token);
    base.trim();
    if (base.length() > 0) {
      char filePath[48];
      snprintf(filePath, sizeof(filePath), "/%s.vlw", base.c_str());
      if (LittleFS.exists(filePath)) {
        base.toCharArray(outBase, outBaseLen);
        return true;
      }
    }
    token = strtok_r(nullptr, ",", &savePtr);
  }
  return false;
}

bool fontFileExists(const char* base) {
  if (!littleFsReady) return false;
  char filePath[48];
  snprintf(filePath, sizeof(filePath), "/%s.vlw", base);
  return LittleFS.exists(filePath);
}

bool selectPanelFont(PanelFontRole role) {
  if (!useLittleFsPanelFonts()) return false;
  if (!littleFsReady) return false;

  if (role == PANEL_FONT_MEDIUM && !orbitronMedAvailable) return false;
  if (role == PANEL_FONT_SMALL && !orbitronSmallAvailable) return false;

  if (activePanelFont == role) return true;

  if (activePanelFont != PANEL_FONT_BUILTIN) {
    tft.unloadFont();
  }

  const char* base = (role == PANEL_FONT_MEDIUM) ? activeMedFontBase : activeSmallFontBase;
  tft.loadFont(base, LittleFS);
  activePanelFont = role;
  return true;
}

void panelTextMedium(const char* s, int x, int y, int fallbackFont) {
  if (selectPanelFont(PANEL_FONT_MEDIUM)) {
    tft.drawString(s, x, y);
  } else {
    tft.drawString(s, x, y, fallbackFont);
  }
}

void panelTextSmall(const char* s, int x, int y, int fallbackFont) {
  if (selectPanelFont(PANEL_FONT_SMALL)) {
    tft.drawString(s, x, y);
  } else {
    tft.drawString(s, x, y, fallbackFont);
  }
}

void initOrbitronFont() {
  if (!LittleFS.begin(false)) {
    Serial.println("[radar] LittleFS mount failed; attempting format + remount");
    if (!LittleFS.begin(true)) {
      Serial.println("[radar] LittleFS format/remount failed; using built-in TFT fonts");
      littleFsReady = false;
      orbitronMedAvailable = false;
      orbitronSmallAvailable = false;
      return;
    }
    Serial.println("[radar] LittleFS formatted and mounted");
  }

  littleFsReady = true;
  activePanelFont = PANEL_FONT_BUILTIN;
  orbitronMedAvailable = resolveFontFromCandidates(cfg.fontMedCandidates, activeMedFontBase, sizeof(activeMedFontBase));
  orbitronSmallAvailable = resolveFontFromCandidates(cfg.fontSmallCandidates, activeSmallFontBase, sizeof(activeSmallFontBase));

  if (orbitronMedAvailable) {
    Serial.printf("[radar] panel title font: %s.vlw\n", activeMedFontBase);
  } else {
    Serial.printf("[radar] panel title font candidates not found: %s\n", cfg.fontMedCandidates);
  }
  if (orbitronSmallAvailable) {
    Serial.printf("[radar] panel body font: %s.vlw\n", activeSmallFontBase);
  } else {
    Serial.printf("[radar] panel body font candidates not found: %s\n", cfg.fontSmallCandidates);
  }

  if (!orbitronMedAvailable && !orbitronSmallAvailable) {
    Serial.println("[radar] no Orbitron panel fonts found; using built-in TFT fonts");
  }
}

void loadDefaultConfig() {
  strncpy(cfg.wifiSsid, DEFAULT_WIFI_SSID, sizeof(cfg.wifiSsid) - 1);
  cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = 0;
  strncpy(cfg.wifiPass, DEFAULT_WIFI_PASS, sizeof(cfg.wifiPass) - 1);
  cfg.wifiPass[sizeof(cfg.wifiPass) - 1] = 0;
  strncpy(cfg.piHost, DEFAULT_PI_HOST, sizeof(cfg.piHost) - 1);
  cfg.piHost[sizeof(cfg.piHost) - 1] = 0;
  cfg.piPort = DEFAULT_PI_PORT;
  cfg.pollMs = DEFAULT_POLL_MS;
  cfg.sweepDps = DEFAULT_SWEEP_DPS;
  cfg.fontMode = DEFAULT_FONT_MODE;
  cfg.enableSpriteLadder = DEFAULT_ENABLE_SPRITE_LADDER;
  strncpy(cfg.fontMedCandidates, DEFAULT_FONT_MED_CANDIDATES, sizeof(cfg.fontMedCandidates) - 1);
  cfg.fontMedCandidates[sizeof(cfg.fontMedCandidates) - 1] = 0;
  strncpy(cfg.fontSmallCandidates, DEFAULT_FONT_SMALL_CANDIDATES, sizeof(cfg.fontSmallCandidates) - 1);
  cfg.fontSmallCandidates[sizeof(cfg.fontSmallCandidates) - 1] = 0;
}

void trimInPlace(String& s) {
  s.trim();
}

void applyConfigLine(const String& key, const String& value) {
  if (key.equalsIgnoreCase("WIFI_SSID")) {
    value.toCharArray(cfg.wifiSsid, sizeof(cfg.wifiSsid));
  } else if (key.equalsIgnoreCase("WIFI_PASS")) {
    value.toCharArray(cfg.wifiPass, sizeof(cfg.wifiPass));
  } else if (key.equalsIgnoreCase("PI_HOST")) {
    value.toCharArray(cfg.piHost, sizeof(cfg.piHost));
  } else if (key.equalsIgnoreCase("PI_PORT")) {
    long v = value.toInt();
    if (v >= 1 && v <= 65535) cfg.piPort = (uint16_t)v;
  } else if (key.equalsIgnoreCase("POLL_MS")) {
    long v = value.toInt();
    if (v >= 200 && v <= 120000) cfg.pollMs = (uint32_t)v;
  } else if (key.equalsIgnoreCase("SWEEP_DPS")) {
    float v = value.toFloat();
    if (v >= 5.0f && v <= 720.0f) cfg.sweepDps = v;
  } else if (key.equalsIgnoreCase("FONT_MODE")) {
    if (value.equalsIgnoreCase("littlefs")) {
      cfg.fontMode = FONT_MODE_LITTLEFS;
    } else if (value.equalsIgnoreCase("default")) {
      cfg.fontMode = FONT_MODE_DEFAULT;
    }
  } else if (key.equalsIgnoreCase("ENABLE_SPRITE_LADDER")) {
    cfg.enableSpriteLadder = toBool(value, cfg.enableSpriteLadder);
  } else if (key.equalsIgnoreCase("FONT_MED_CANDIDATES")) {
    value.toCharArray(cfg.fontMedCandidates, sizeof(cfg.fontMedCandidates));
  } else if (key.equalsIgnoreCase("FONT_SMALL_CANDIDATES")) {
    value.toCharArray(cfg.fontSmallCandidates, sizeof(cfg.fontSmallCandidates));
  }
}

bool parseConfigFile(File& f) {
  while (f.available()) {
    String line = f.readStringUntil('\n');
    trimInPlace(line);
    if (line.length() == 0) continue;
    if (line.startsWith("#") || line.startsWith(";")) continue;

    int eq = line.indexOf('=');
    if (eq < 1) continue;

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    trimInPlace(key);
    trimInPlace(value);
    if (key.length() == 0) continue;

    applyConfigLine(key, value);
  }
  return true;
}

bool loadConfigFromSd() {
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSpi, 4000000U)) {
    Serial.printf("[radar] SD mount failed on CS=%d; using defaults\n", SD_CS);
    sdSpi.end();
    return false;
  }
  Serial.println("[radar] SD mounted; probing config paths...");

  bool loaded = false;
  for (size_t i = 0; i < CONFIG_CANDIDATE_COUNT; i++) {
    const char* path = CONFIG_CANDIDATES[i];
    bool exists = SD.exists(path);
    Serial.printf("[radar] check %s -> %s\n", path, exists ? "found" : "missing");
    if (!exists) continue;
    File f = SD.open(path, FILE_READ);
    if (!f) continue;

    bool ok = parseConfigFile(f);
    f.close();
    if (ok) {
      Serial.printf("[radar] loaded config from %s\n", path);
      loaded = true;
      break;
    }
  }

  SD.end();
  sdSpi.end();

  if (!loaded) {
    Serial.println("[radar] no config file found on SD; using defaults");
  }
  return loaded;
}

void logActiveConfig() {
  Serial.printf("[radar] cfg: ssid=%s host=%s:%u poll=%lums sweep=%.1f font=%s ladder=%s\n",
                cfg.wifiSsid,
                cfg.piHost,
                cfg.piPort,
                (unsigned long)cfg.pollMs,
                cfg.sweepDps,
                useLittleFsPanelFonts() ? "littlefs" : "default",
                cfg.enableSpriteLadder ? "on" : "off");
}

void buildPalette(bool applySpritePalette) {
  palette[BG]     = tft.color565(0, 16, 4);
  palette[RING]   = tft.color565(0, 60, 24);
  palette[RING2]  = tft.color565(0, 120, 48);
  palette[BLIP]   = tft.color565(0, 255, 96);
  palette[SWEEPC] = tft.color565(120, 255, 150);
  palette[TRAIL1] = tft.color565(0, 190, 70);
  palette[TRAIL2] = tft.color565(0, 90, 34);
  palette[AMBER]  = tft.color565(255, 176, 0);
  palette[TXT]    = tft.color565(0, 210, 90);
  palette[GRIDX]  = tft.color565(0, 45, 18);
  (void)applySpritePalette;
}

uint16_t scopeColor(uint8_t idx) {
  // Use RGB565 for draw primitives in both modes; TFT_eSprite converts as needed.
  return palette[idx];
}

uint16_t dim565(uint16_t c, uint8_t scale) {
  uint8_t r = (c >> 11) & 0x1F;
  uint8_t g = (c >> 5) & 0x3F;
  uint8_t b = c & 0x1F;
  r = (uint16_t(r) * scale) >> 8;
  g = (uint16_t(g) * scale) >> 8;
  b = (uint16_t(b) * scale) >> 8;
  return (uint16_t(r) << 11) | (uint16_t(g) << 5) | b;
}

void decayScopeSprite(uint8_t scale) {
  if (!scopeSupportsDecay) return;
  uint16_t* px = static_cast<uint16_t*>(scope.getPointer());
  if (!px) return;

  const int n = scopeD * scopeD;
  for (int i = 0; i < n; i++) {
    px[i] = dim565(px[i], scale);
  }
}

void handleSerialControls() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '1' || ch == '2' || ch == '3') {
      applyVisualProfile((uint8_t)(ch - '0'));
      Serial.printf("[radar] profile -> %s\n", profileName(activeProfile));
      drawPanel();
    } else if (ch == '?') {
      Serial.println("[radar] visual profiles: 1=CLASSIC, 2=CRT, 3=FAST");
    }
  }
}

void initScopeSprite() {
  bool useLadder = useLittleFsPanelFonts() && cfg.enableSpriteLadder;

  if (useLadder) {
    static const int CANDIDATE_SIZES[] = {256, 240, 224, 208, 192};
    static const size_t CANDIDATE_COUNT = sizeof(CANDIDATE_SIZES) / sizeof(CANDIDATE_SIZES[0]);

    // Try progressively smaller sprites until 16-bit allocation succeeds.
    for (size_t i = 0; i < CANDIDATE_COUNT; i++) {
      int s = CANDIDATE_SIZES[i];
      scope.setColorDepth(16);
      if (scope.createSprite(s, s) != nullptr) {
        scopeD = s;
        scopeCX = scopeD / 2;
        scopeCY = scopeD / 2;
        scopeR = scopeD / 2 - 6;
        scopePaletteMode = false;
        scopeSupportsDecay = true;
        buildPalette(false);
        return;
      }
    }

    // Final fallback: indexed mode at the smallest candidate size.
    scopeD = CANDIDATE_SIZES[CANDIDATE_COUNT - 1];
    scopeCX = scopeD / 2;
    scopeCY = scopeD / 2;
    scopeR = scopeD / 2 - 6;
    scope.setColorDepth(8);
    scope.createSprite(scopeD, scopeD);
    scopePaletteMode = true;
    scopeSupportsDecay = false;
    buildPalette(true);
    return;
  }

  // Fixed-size sprite path.
  scope.setColorDepth(16);
  if (scope.createSprite(scopeD, scopeD) != nullptr) {
    scopePaletteMode = false;
    scopeSupportsDecay = true;
    buildPalette(false);
    return;
  }

  // Fallback to indexed mode if 16-bit allocation fails.
  scope.setColorDepth(8);
  scope.createSprite(scopeD, scopeD);
  scopePaletteMode = true;
  scopeSupportsDecay = false;
  buildPalette(true);
}

// bearing (deg, 0=N cw) + range(nm) -> sprite pixel
void polarToXY(float rng, float brg, int &x, int &y) {
  float rr = (rng / maxnm) * scopeR;
  float a = brg * DEG_TO_RAD;
  x = scopeCX + (int)(rr * sinf(a));
  y = scopeCY - (int)(rr * cosf(a));
}

float angDiff(float a, float b) {
  float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
  return fabsf(d);
}

void drawScope() {
  if (scopeSupportsDecay) {
    decayScopeSprite(phosphorDecay);
  } else {
    // 8-bit fallback: no direct per-pixel decay path.
    scope.fillSprite(scopeColor(BG));
  }

  // range rings + labels
  for (int i = 1; i <= 4; i++) {
    int rr = scopeR * i / 4;
    uint16_t ringC = i == 4 ? scopeColor(RING2) : scopeColor(RING);
    if (scopeSupportsDecay) ringC = dim565(ringC, 210);
    scope.drawCircle(scopeCX, scopeCY, rr, ringC);
    int lbl = maxnm * i / 4;
    uint16_t txtC = scopeColor(RING2);
    if (scopeSupportsDecay) txtC = dim565(txtC, 220);
    scope.setTextColor(txtC);
    scope.drawNumber(lbl, scopeCX + 2, scopeCY - rr + 2, 1);
  }
  // crosshair + cardinals
  uint16_t gridC = scopeColor(GRIDX);
  uint16_t cardC = scopeColor(RING2);
  if (scopeSupportsDecay) {
    gridC = dim565(gridC, 220);
    cardC = dim565(cardC, 220);
  }
  scope.drawLine(scopeCX, scopeCY - scopeR, scopeCX, scopeCY + scopeR, gridC);
  scope.drawLine(scopeCX - scopeR, scopeCY, scopeCX + scopeR, scopeCY, gridC);
  scope.setTextColor(cardC);
  scope.drawString("N", scopeCX - 3, 2, 2);
  scope.drawString("S", scopeCX - 3, scopeD - 16, 2);
  scope.drawString("E", scopeD - 12, scopeCY - 6, 2);
  scope.drawString("W", 3, scopeCY - 6, 2);

  // sweep trail (fading wedge behind the leading edge)
  int seg = sweepSegments < 1 ? 1 : sweepSegments;
  for (int k = 0; k < seg; k++) {
    float a = (sweep - k * sweepStepDeg) * DEG_TO_RAD;
    int x = scopeCX + (int)(scopeR * sinf(a));
    int y = scopeCY - (int)(scopeR * cosf(a));
    uint16_t c;
    if (scopeSupportsDecay) {
      uint8_t falloff = 255 - (k * (190 / seg));
      c = dim565(scopeColor(SWEEPC), falloff);
    } else {
      c = (k < seg / 3) ? scopeColor(SWEEPC) : (k < (2 * seg) / 3 ? scopeColor(TRAIL1) : scopeColor(TRAIL2));
    }
    scope.drawLine(scopeCX, scopeCY, x, y, c);
  }

  // aircraft blips
  for (int i = 0; i < nac; i++) {
    if (ac[i].rng > maxnm) continue;
    int x, y; polarToXY(ac[i].rng, ac[i].brg, x, y);
    // refresh intensity when the sweep passes the bearing
    if (angDiff(sweep, ac[i].brg) < 4.0f) ac[i].inten = 1.0f;
    uint16_t c = ac[i].inten > 0.66f ? scopeColor(SWEEPC) : ac[i].inten > 0.25f ? scopeColor(BLIP) : scopeColor(TRAIL1);
    if (ac[i].rng < maxnm * 0.15f) c = scopeColor(AMBER);   // very close = amber
    if (scopeSupportsDecay && blipGlow) {
      scope.fillCircle(x, y, 3, dim565(c, 90));
      scope.fillCircle(x, y, 2, dim565(c, 160));
    }
    scope.fillCircle(x, y, 1, c);
    // heading leader
    float ta = ac[i].trk * DEG_TO_RAD;
    scope.drawLine(x, y, x + (int)(7 * sinf(ta)), y - (int)(7 * cosf(ta)), c);
    // label the nearest few only (reduce clutter)
    if (i < 4) { scope.setTextColor(scopeColor(TXT)); scope.drawString(ac[i].fl, x + 5, y - 3, 1); }
    ac[i].inten -= 0.02f; if (ac[i].inten < 0) ac[i].inten = 0;
  }

  if (!feedOk) {
    scope.setTextColor(scopeColor(AMBER));
    scope.drawString("NO FEED", scopeCX - 24, scopeCY + 12, 2);
  } else if (nac == 0) {
    scope.setTextColor(scopeColor(RING2));
    scope.drawString("NO CONTACTS", scopeCX - 34, scopeCY + 12, 2);
  }

  scope.pushSprite(SX, SY);
}

void drawPanel() {
  int px = SX + scopeD + 8;            // panel left
  tft.fillRect(px, 0, 480 - px, 320, TFT_BLACK);
  tft.setTextColor(palette[BLIP], TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  panelTextMedium("ADS-B", px, 8, 4);
  tft.setTextColor(palette[AMBER], TFT_BLACK);
  panelTextMedium("// PPI", px, 34, 2);

  tft.setTextColor(palette[TXT], TFT_BLACK);
  char b[24];
  snprintf(b, sizeof(b), "CONTACTS %d", nac);
  panelTextSmall(b, px, 58, 2);
  snprintf(b, sizeof(b), "RANGE %dNM", maxnm);
  panelTextSmall(b, px, 76, 2);
  snprintf(b, sizeof(b), "MODE %s", profileName(activeProfile));
  panelTextSmall(b, px, 94, 1);
  tft.drawFastHLine(px, 108, 480 - px - 6, palette[RING]);

  int n = nac < 7 ? nac : 7;
  for (int i = 0; i < n; i++) {
    int y = 116 + i * 26;
    uint16_t c = (ac[i].rng < maxnm * 0.15f) ? palette[AMBER] : palette[BLIP];
    tft.setTextColor(c, TFT_BLACK);
    panelTextSmall(ac[i].fl, px, y, 2);
    tft.setTextColor(palette[TXT], TFT_BLACK);
    snprintf(b, sizeof(b), "FL%03d  %.0fNM", ac[i].alt / 100, ac[i].rng);
    panelTextSmall(b, px, y + 14, 1);
  }
}

bool fetchRadar() {
  HTTPClient http;
  char url[80];
  snprintf(url, sizeof(url), "http://%s:%u/radar", cfg.piHost, cfg.piPort);
  http.begin(url);
  http.setTimeout(2000);
  int code = http.GET();
  if (code != 200) { http.end(); feedOk = false; return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { feedOk = false; return false; }

  maxnm = doc["max_nm"] | 40;
  feedOk = !doc["err"].is<const char*>();
  JsonArray arr = doc["ac"].as<JsonArray>();
  int i = 0;
  for (JsonObject o : arr) {
    if (i >= MAXAC) break;
    const char* fl = o["fl"] | "";
    strncpy(ac[i].fl, fl, sizeof(ac[i].fl) - 1);
    ac[i].fl[sizeof(ac[i].fl) - 1] = 0;
    ac[i].rng = o["rng"] | 0.0f;
    ac[i].brg = o["brg"] | 0.0f;
    ac[i].alt = o["alt"] | 0;
    ac[i].trk = o["trk"] | 0;
    ac[i].gs  = o["gs"]  | 0;
    ac[i].inten = 0.5f;
    i++;
  }
  nac = i;
  return true;
}

void connectWifi() {
  scope.fillSprite(scopeColor(BG));
  scope.setTextColor(scopeColor(TXT));
  scope.drawString("LINKING...", scopeCX - 30, scopeCY, 2);
  scope.pushSprite(SX, SY);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
}

void setup() {
  Serial.begin(115200);
  loadDefaultConfig();

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);

#if ENABLE_SD_CONFIG
  loadConfigFromSd();
#else
  Serial.println("[radar] SD config disabled (ENABLE_SD_CONFIG=0); using defaults");
#endif
  logActiveConfig();

  // Allocate the scope sprite before mounting LittleFS/fonts to maximize heap
  // and keep the smooth 16-bit decay path.
  initScopeSprite();

  if (useLittleFsPanelFonts()) {
    initOrbitronFont();
    if (orbitronMedAvailable || orbitronSmallAvailable) {
      Serial.printf("[radar] panel font map: title=%s body=%s\n",
                    orbitronMedAvailable ? activeMedFontBase : "builtin",
                    orbitronSmallAvailable ? activeSmallFontBase : "builtin");
    }
  } else {
    Serial.println("[radar] panel text using built-in TFT fonts");
  }

  applyVisualProfile(PROFILE_CRT);
  scope.fillSprite(scopeColor(BG));
  Serial.printf("[radar] sprite mode: %s\n", scopeSupportsDecay ? "16-bit decay" : "8-bit indexed fallback");
  Serial.println("[radar] visual profiles: 1=CLASSIC, 2=CRT, 3=FAST, ?=help");

  connectWifi();
  fetchRadar();
  drawPanel();
  lastPoll = millis();
}

void loop() {
  uint32_t now = millis();
  handleSerialControls();

  if (now - lastPoll >= cfg.pollMs) {
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    fetchRadar();
    drawPanel();
    lastPoll = now;
  }

  // animate sweep at a fixed angular rate
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;
  sweep = fmodf(sweep + cfg.sweepDps * dt, 360.0f);
  drawScope();
  delay(25);
}