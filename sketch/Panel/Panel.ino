#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <time.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>
// ArduinoJson documents that live in PSRAM instead of the internal heap.
struct SpiRamAllocator {
  void *allocate(size_t n)               { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void  deallocate(void *p)              { heap_caps_free(p); }
  void *reallocate(void *p, size_t n)    { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
using SpiRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <Update.h>
#include <MD5Builder.h>  // integrity check on downloaded firmware
#include <esp_random.h>   // hardware RNG for first-boot topic generation
// QR code for the WiFi setup screen: rendered via LVGL's built-in lv_qrcode
// widget (src/extra/libs/qrcode) instead of a hand-baked module table. This
// requires LV_USE_QRCODE set to 1 in lv_conf.h -- the widget pulls in its own
// bundled encoder (qrcodegen) automatically once that flag is on, no extra
// include needed here beyond <lvgl.h> below. Because it encodes at runtime,
// it also stays correct automatically if WIFI_PROV_AP_SSID is ever changed --
// nothing to regenerate by hand.

#include <lvgl.h>
#include <figtree_fonts.h>

#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

// =====================================================
// SmartDoorbellPanel Production Base
// CrowPanel ESP32-S3 7.0 inch
//
// Current storage:
// - SD card RGB565 backgrounds and festival assets
//
// Required SD files:
// /bg/day.rgb565
// /bg/night.rgb565
//
// Optional SD files:
// /bg/winter_night.rgb565
// /festival/*.rgb565
// /config/festivals.txt
//
// Confirmed hardware:
// Display: 800x480 RGB
// Touch: GT911 at 0x5D
// I2C: SDA IO19, SCL IO20
// External PCF8574: 0x20
// Motion sensor OUT: IO38
// Backlight: IO2
//
// PCF P0 -> Doorbell SSR
// PCF P1 -> Ceiling Light SSR
// PCF P2 -> Side Panel Lights SSR
// =====================================================

// =====================================================
// WiFi / Time / Weather config
// =====================================================

// No compile-time WiFi credentials. A fresh unit has nothing here -- it
// boots straight into the SoftAP/QR provisioning portal on first power-up.
// All real credentials live only in NVS (set via that portal), never in
// the firmware binary, so the same .bin ships to every customer unchanged.
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";

// Boot WiFi sequence:
// First WiFi.begin() happens immediately at boot.
// We do NOT interrupt the first connection attempt before 30 seconds.
// Retry timing: 30 sec -> 2 min -> 5 min cap.
const unsigned long WIFI_RETRY_DELAYS_MS[] = {
  30000UL,    // first retry after 30 sec
  120000UL,   // second retry after 2 min
  300000UL    // then keep retrying every 5 min
};
const uint8_t WIFI_RETRY_DELAY_COUNT = sizeof(WIFI_RETRY_DELAYS_MS) / sizeof(WIFI_RETRY_DELAYS_MS[0]);

const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 19800;       // India GMT+5:30
const int DAYLIGHT_OFFSET_SEC = 0;

// Weather: Open-Meteo -- no API key needed, free, global, commercial-safe at low volume.
const char* WEATHER_UNITS = "metric";
// No hardcoded city. Location is auto-detected via IP geolocation once WiFi
// connects (fetchIpGeolocation()) and cached in NVS as lat/lon -- works for
// any customer, anywhere, with zero manual setup.

const unsigned long WEATHER_UPDATE_MS = 3600000UL; // 1 hour
// Open-Meteo refreshes current conditions roughly every 15 minutes. Polling
// slower than the source is what made the panel claim "33 °C, clear" while it
// was visibly raining -- the reading was simply up to two hours old. One hour
// caps that staleness at 59 minutes and costs 24 requests/day. If a stale
// reading is ever seen again, this constant is the only thing to change.
const unsigned long WEATHER_FIRST_RETRY_MS = 20000UL; // retry every 30 sec until first success

// Non-blocking boot stage timeouts.
// UI is already visible while these run in the background.
const unsigned long BOOT_NTP_TIMEOUT_MS = 30000UL;
const unsigned long BOOT_WEATHER_TIMEOUT_MS = 12000UL;
const unsigned long NTP_RECOVERY_RETRY_MS = 10000UL; // retry NTP every 15 sec until valid time arrives


// =====================================================
// Push notifications (ntfy.sh) / Activity log
// =====================================================
//
// ntfy.sh setup (free, no account needed):
//  1. Install the "ntfy" app (iOS / Android) or open https://ntfy.sh in a browser.
//  2. Pick a topic name that's hard to guess (it's like a shared "channel" -
//     anyone who knows it can read your notifications), e.g.
//     "doorbell-7f3a2c91-front-porch".
//  3. In the app, tap "+" and subscribe to that exact topic name.
//  4. Put the same topic name below. That's it - no token/keys required.
//
const char* NTFY_TOPIC = "";

// =====================================================
// Phone controls (hosted control page)
// =====================================================
// ONE page serves every unit ever shipped: the per-unit part travels in the
// URL *fragment* (#t=<topic>), which the panel appends at runtime from its
// own NVS topic. So this constant is identical in every binary -- no
// per-device build, no serialization step, nothing to customise on the line.
// Browsers never transmit a fragment to the server, so the host never learns
// which topic belongs to which customer.
//
// IMPORTANT: point this at a domain YOU own, not a vendor subdomain. The URL
// baked into shipped firmware is permanent for those units; owning the name
// means you can move hosts forever by repointing DNS. It is also overridable
// per-unit from NVS (see nvsPhoneUrl) so even a domain change is recoverable
// on panels already in the field, without a reflash.
// Leave as the placeholder below and the Phone Setup screen will simply say
// "not configured" instead of showing a QR -- safe to ship/flash either way.
const char* PHONE_UI_BASE_URL = "https://atharvguitarist.github.io/panel/";

// =====================================================
// Firmware version + update-from-GitHub
// =====================================================
// Bump the number in FW_VERSION_TAG for every release you publish. The panel compares it
// numerically (major.minor.patch) against firmware/manifest.json on the
// control-page host, so a release is only ever installed if it is newer.
//
// Repo layout (relative to PHONE_UI_BASE_URL):
//   firmware/latest.bin      the exported Panel_v5.ino.bin
//   firmware/manifest.json   {"version":"5.6.4","size":...,"md5":"...","festivals":"<stamp>"}
//   config/festivals.txt     festival calendar, same format as the SD file
//
// "festivals" is an opaque stamp (make_release.py writes the file's md5
// prefix; a hand-typed date works too). The panel fetches config/festivals.txt
// ONLY when that stamp differs from the one it last applied -- so an
// unchanged calendar costs nothing beyond the manifest read, and an edit
// reaches every panel at its next check.
//
// "upgrade" (ntfy command, or the Install button on the update notice)
// downloads latest.bin straight to the SD card, verifies size AND md5,
// renames it to /update/firmware.bin and reboots -- the boot-time installer
// does the flashing. A short or corrupt download is deleted and nothing
// installs. The weekly check runs Sundays at UPDATE_CHECK_HOUR:30 (the panel is
// asleep then) and notifies ONCE per new version; it never installs on its
// own.
// The "PANELFW:" tag is how the repo's publish workflow reads the version
// straight out of the uploaded .bin -- change ONLY the number. FW_VERSION
// points past the tag, so every existing use is unchanged, and because it is
// used everywhere the linker can never strip the tag from the binary.
const char  FW_VERSION_TAG[] = "PANELFW:5.6.19";
const char* FW_VERSION = FW_VERSION_TAG + 8;
const char* FW_MANIFEST_PATH = "firmware/manifest.json";
const char* FW_BINARY_PATH   = "firmware/latest.bin";
const char* FESTIVAL_CONFIG_URL_PATH = "config/festivals.txt";  // same format as the SD file
const int   UPDATE_CHECK_HOUR = 3;   // 03:30 local

// Phone Setup screen palette. These are the phone page's colours (cream
// #F6E9CC, burgundy #7E102C, gold #C9A85C, warm ink) counter-tinted for
// this TFT, which lifts light tones toward white and pulls reds toward
// magenta. Tune here only; nothing else references these values.
#define PS_BG       0xEDD9AE   // renders as the phone page's cream
#define PS_CARD     0xFFFBF0   // near-white card face
#define PS_LINE     0xD6C194   // hairline card border
#define PS_INK      0x2E1D10   // warm body text
#define PS_MUTED    0x7A6646   // captions
#define PS_GOLD     0x8F6A1A   // gold-text headings
#define PS_BURGUNDY 0x88161F   // burgundy, bisected for this panel
#define PS_ON_BURG  0xF6E3C0   // cream text on burgundy

// Set true to also get a (low-priority) notification when motion is detected
// but the doorbell is NOT pressed within the grace period (i.e. whenever the
// "Welcome Home" overlay is shown). Turned on so a no-bell motion event (e.g.
// someone messing with the panel, or just loitering on the porch) still
// reaches your phone even though nobody rang the bell.
const bool NOTIFY_ON_MOTION_NO_BELL = true;

// Ceiling light "left on?" reminder (manual mode only).
// First reminder fires 1 hour after the ceiling light is switched to manual ON,
// then repeats hourly for as long as it stays on manually.
const unsigned long AUTO_MANUAL_ON_REMINDER_INTERVAL_MS = 3600000UL; // 1 hour
// During a festival the lights are MEANT to stay on all evening, so the
// hourly "did you leave this on?" check-in is noise. Stretch it out instead
// of silencing it: a light still burning at 4am on Diwali is worth a word.
const unsigned long AUTO_MANUAL_ON_FESTIVAL_INTERVAL_MS = 21600000UL; // 6 hours

// Quiet hours: reminders are suppressed in this local-time window so they
// don't disturb sleep, then resume normally once the window ends.
// Default is midnight -> 7 AM; change AUTO_REMINDER_QUIET_END_HOUR if you'd
// like a different wake-up cutoff.
const int AUTO_REMINDER_QUIET_START_HOUR = 0; // 00:00 (midnight)
const int AUTO_REMINDER_QUIET_END_HOUR   = 7; // 07:00

// Activity log file on the SD card (timestamp,event,detail per line).
const char* ACTIVITY_LOG_PATH = "/log/activity.csv";
// Declared beside the live path (not down in the Logs screen) because the
// daily retention sweep up in the service layer needs it too.
const char* ACTIVITY_LOG_OLD_PATH = "/log/activity.old.csv";


// =====================================================
// I2C / Device addresses
// =====================================================

#define I2C_SDA 19
#define I2C_SCL 20

#define GT911_ADDR 0x5D
#define PCF_ADDR   0x20

#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINT1 0x8150


// =====================================================
// Touch mapping
// =====================================================

#define TOUCHMAPX1 0
#define TOUCHMAPX2 800
#define TOUCHMAPY1 0
#define TOUCHMAPY2 480


// =====================================================
// Hardware pins
// =====================================================

#define TFT_BL 2
#define MOTION_PIN 38

// =====================================================
// SD card SPI pins - official CrowPanel 7.0 examples
// =====================================================

#define SD_MOSI 11
#define SD_MISO 13
#define SD_SCK  12
#define SD_CS   10


// =====================================================
// PCF8574 output mapping
// =====================================================

#define PCF_DOORBELL_PIN 0
#define PCF_PANEL_PIN    1
#define PCF_CEILING_PIN  2
#define PCF_SPARE_PIN    3

#define SSR_ACTIVE_LOW true

// =====================================================
// Timing / behavior settings
// =====================================================

const unsigned long DOORBELL_ON_TIME_MS = 200;
const unsigned long DOORBELL_DEBOUNCE_MS = 120;

const unsigned long MOTION_BOOT_IGNORE_MS = 15000UL;        // PIR warm-up 15 sec. ignore time -- fixed, not user-tunable
const unsigned long MOTION_SAMPLE_MS = 50;
const uint8_t MOTION_CONFIRM_LOW_SAMPLES  = 3;  // clears after 150ms stable LOW -- fixed

// ---- User-tunable settings: defaults below, real values loaded from NVS at boot ----
// See loadUserSettings() / saveUserSetting*() near the Preferences section.
unsigned long motionHoldTimeMs        = 5000;  // "Motion hold timer" — Settings > Motion (restored v1 default)
uint8_t       motionConfirmHighSamples = 10;     // "PIR sensitivity" — Settings > Motion (Low=20 Med=10 High=5 *consecutive 50ms samples* -- restored v1 streak logic, see updateMotionSensor())
unsigned long autoManualOffCooldownMs = 15000;  // "Manual mode cooldown" — Settings > Motion
unsigned long uiDimAfterMs            = 5000;    // "Auto-dim delay" — Settings > Display
bool          motionDetectionEnabled  = true;    // "Motion detection" — Settings > Motion

const unsigned long NTP_PERIODIC_RESYNC_MS = 8UL * 60UL * 60UL * 1000UL; // 8 hours

const unsigned long CLOCK_UPDATE_MS = 1000;
const unsigned long WIFI_CHECK_MS = 5000;
unsigned long lastNtpPeriodicResyncAt = 0;

const unsigned long THEME_DEFER_MS = 120000UL; // postpone 2 min if user/motion active
const unsigned long USER_IDLE_FOR_THEME_MS = 120000UL; // user idle 2 min before theme switch
const unsigned long MOTION_IDLE_FOR_THEME_MS = 120000UL; // no motion for 2 min before theme switch

const unsigned long AUTO_MANUAL_HOLD_MS = 8UL * 60UL * 60UL * 1000UL; // manual Ceiling Glow max 8 hours

const char *SD_UPDATE_FIRMWARE_PATH = "/update/firmware.bin";
const char *UPDATE_BG_PATH = "/ui/update_bg.rgb565";
const unsigned long FIRMWARE_UPLOAD_PORTAL_MS = 5UL * 60UL * 1000UL;

const uint8_t BACKLIGHT_ACTIVE = 210;
const uint8_t BACKLIGHT_DIM = 85;

// Overnight display sleep: the screen goes fully OFF (not just dimmed)
// during this local-time window once idle, and only wakes again on touch
// or motion (motion already calls wakeScreen()). Window does not wrap past
// midnight (1:30 -> 5:00 same calendar day), so plain hour*60+min compare works.
const int DISPLAY_SLEEP_START_HOUR = 1;
const int DISPLAY_SLEEP_START_MIN  = 30;
const int DISPLAY_SLEEP_END_HOUR   = 5;
const int DISPLAY_SLEEP_END_MIN    = 0;

// How long the panel must be untouched (and motion-clear) before it's
// allowed to go fully dark during the overnight window above. This is
// deliberately its own constant, separate from uiDimAfterMs (the Settings
// "Auto-dim delay", whose fastest tier is just 5s) -- that used to be the
// same value the overnight-sleep check used, so someone actively at the
// panel at 2am with a few seconds' pause between taps would get plunged
// into darkness mid-interaction. Three minutes of genuine inactivity is a
// much saner bar for "actually done", regardless of whatever the daytime
// auto-dim delay happens to be set to.
const unsigned long NIGHT_SLEEP_IDLE_MS = 180000UL; // 3 minutes

// =====================================================
// Font helpers
// =====================================================
//
// Figtree font files required in Arduino libraries:
// D:\sketchbook\libraries\figtree_fonts\figtree_fonts.h
// D:\sketchbook\libraries\figtree_fonts\figtree_18.c
// D:\sketchbook\libraries\figtree_fonts\figtree_26.c
// D:\sketchbook\libraries\figtree_fonts\figtree_30.c
//
// These keep the exact same UI sizes as the old Montserrat setup.
// CPU load stays effectively the same; only flash size increases slightly.

#define FONT_18 &figtree_18
#define FONT_26 &figtree_26
#define FONT_30 &figtree_30

// =====================================================
// Runtime state
// =====================================================

uint8_t pcfState = 0xFF;

Preferences prefs;

bool ceilingState = false;

enum AutoMode {
  AUTO_MODE_AUTO,
  AUTO_MODE_MANUAL_ON,
  AUTO_MODE_MANUAL_OFF
};

AutoMode autoMode = AUTO_MODE_AUTO;
bool panelOutputState = false;
unsigned long autoManualOffStartedAt = 0;

// Which physical light motion detection actually drives. Change this from
// Settings > Motion > "Motion controls" any time -- no re-flash needed.
// The other light simply becomes a plain manual on/off tile.
enum AutoLightTarget {
  AUTO_TARGET_CEILING,
  AUTO_TARGET_PANEL
};

AutoLightTarget autoLightTarget = AUTO_TARGET_CEILING; // "Motion controls" — Settings > Motion, persisted in NVS

bool doorbellActive = false;
unsigned long doorbellStartedAt = 0;
unsigned long lastDoorbellPressAt = 0;

bool motionState = false;
bool motionEverSeen = false;
unsigned long lastMotionSeenAt = 0;

bool motionRawState = false;
bool motionStableState = false;
bool motionArmedAfterBootClear = false;
bool motionRearmAfterManualOffClear = false;

uint8_t motionHighStreak = 0;
uint8_t motionLowStreak = 0;

unsigned long lastMotionRawChangeAt = 0;
unsigned long lastMotionStableChangeAt = 0;

unsigned long motionRawHighCount = 0;
unsigned long motionRawLowCount = 0;
unsigned long motionDetectedEvents = 0;

unsigned long lastUserActivityAt = 0;
bool screenDimmed = false;

bool timeReady = false;
bool sdMounted = false;

enum BootNetStage {
  BOOT_NET_WIFI,
  BOOT_NET_NTP,
  BOOT_NET_APPLY_THEME,
  BOOT_NET_WEATHER,
  BOOT_NET_DONE
};

BootNetStage bootNetStage = BOOT_NET_WIFI;
unsigned long bootStageStartedAt = 0;
unsigned long lastBootWiFiRetryAt = 0;
uint8_t bootWiFiRetryIndex = 0;
bool bootWeatherAttempted = false;
bool cachedWeatherLoaded = false;
bool timeReadyUpdatesApplied = false;
bool forceWeatherFetchAfterNtp = false;

int lastWeatherHttpCode = 0;
unsigned long lastWeatherUpdatedAt = 0;

char currentBackgroundPath[72] = "plain";
bool darkThemeActive = false;

void applyTheme(bool forceReload = false);
void applyThemeColors();
void scheduleNextThemeEvent();
// Manual prototypes suppress Arduino's auto-generated ones file-wide, so
// any function we declare by hand must be declared HERE, above every call
// site -- a mid-file prototype strands earlier callers (see remote module).
void clearMotionAutoHold();
void startFirmwareUploadPortal();
void showPhoneSetupScreen(bool firstRun);
void settingsPhoneSetupCb(lv_event_t *e);
void destroyPhoneSetupScreen();
String generateNtfyTopic();
void ensureNtfyTopic();
void savePhoneSetupPending(bool pending);
void showFirmwarePortalScreen(const char *message, const char *urlText, const char *hintText);
void startFirmwareUpgrade();
void startFirmwareUpdateCheck();
#ifdef PANEL_CRASH_NOTE
struct PanelPanicInfo;
extern "C" void __wrap_esp_panic_handler(PanelPanicInfo *info);
extern "C" void __real_esp_panic_handler(PanelPanicInfo *info);
#endif
int  doNtfyPublish(const char *suffix, const char *title, const char *message, const char *priority, const char *actions, int timeoutMs = 4000, const char *seq = nullptr, bool click = false);
void sendPushNotificationEx(const char *title, const char *message, const char *priority, const char *seq, uint32_t clearMs);
void updateWeatherIconDisplay(bool forceReload = false);
void updateWeather();
static void weatherTickerReset();
static void updateWeatherTicker();
void updateBootSequence();
void resetBootSequence();
void updateTimeRecovery();
void applyTimeReadyUpdatesOnce();
void loadCachedWeather();
void saveCachedWeather(int tempInt, int wmoCodeVal, const char *conditionText);
void applyCachedOrPlaceholderWeather();

const uint8_t BRIGHTNESS_LEVELS[7] = {
  40,   // Level 1: very dim
  70,   // Level 2: current dim brightness
  105,  // Level 3
  140,  // Level 4
  175,  // Level 5
  210,  // Level 6: current active brightness
  255   // Level 7: max PWM brightness
};

uint8_t brightnessLevel = 5; // 0-based. Level 6 = 210, your current brightness.


// =====================================================
// Display config
// =====================================================

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_RGB _panel_instance;

  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;

      cfg.pin_d0  = GPIO_NUM_15;
      cfg.pin_d1  = GPIO_NUM_7;
      cfg.pin_d2  = GPIO_NUM_6;
      cfg.pin_d3  = GPIO_NUM_5;
      cfg.pin_d4  = GPIO_NUM_4;

      cfg.pin_d5  = GPIO_NUM_9;
      cfg.pin_d6  = GPIO_NUM_46;
      cfg.pin_d7  = GPIO_NUM_3;
      cfg.pin_d8  = GPIO_NUM_8;
      cfg.pin_d9  = GPIO_NUM_16;
      cfg.pin_d10 = GPIO_NUM_1;

      cfg.pin_d11 = GPIO_NUM_14;
      cfg.pin_d12 = GPIO_NUM_21;
      cfg.pin_d13 = GPIO_NUM_47;
      cfg.pin_d14 = GPIO_NUM_48;
      cfg.pin_d15 = GPIO_NUM_45;

      cfg.pin_henable = GPIO_NUM_41;
      cfg.pin_vsync   = GPIO_NUM_40;
      cfg.pin_hsync   = GPIO_NUM_39;
      cfg.pin_pclk    = GPIO_NUM_0;

      cfg.freq_write = 15000000;

      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 40;
      cfg.hsync_pulse_width = 48;
      cfg.hsync_back_porch  = 40;

      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 1;
      cfg.vsync_pulse_width = 31;
      cfg.vsync_back_porch  = 13;

      cfg.pclk_active_neg = 1;
      cfg.de_idle_high    = 0;
      cfg.pclk_idle_high  = 0;

      _bus_instance.config(cfg);
    }

    {
      auto cfg = _panel_instance.config();

      cfg.memory_width  = 800;
      cfg.memory_height = 480;
      cfg.panel_width   = 800;
      cfg.panel_height  = 480;

      cfg.offset_x = 0;
      cfg.offset_y = 0;

      cfg.pin_cs   = -1;
      cfg.pin_rst  = -1;
      cfg.pin_busy = -1;

      _panel_instance.config(cfg);
    }

    _panel_instance.setBus(&_bus_instance);
    setPanel(&_panel_instance);
  }
};

LGFX lcd;


// =====================================================
// LVGL globals
// =====================================================

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// ONE draw buffer. my_disp_flush() waits for its DMA to finish before
// returning, so LVGL can never render into a second buffer while the first
// is being sent -- a second buffer here was 38.4KB of internal RAM that
// did nothing. Same frames, same speed.
static lv_color_t buf1[800 * 480 / 20];

static lv_obj_t *mainScreen;

static lv_obj_t *weatherLabel;
static lv_obj_t *dateLabel;
time_t sunriseEpoch = 0;
time_t sunsetEpoch = 0;

// Rain outlook for the home-screen weather ticker: first hourly slot within
// the next 6h where Open-Meteo's precipitation probability crosses 50%.
// 0 = none expected. Refreshed on every weather fetch, so it can never be
// staler than the weather itself; consumers additionally ignore it once the
// predicted hour has passed.
time_t rainExpectedEpoch = 0;
time_t nextThemeEventEpoch = 0;
time_t nextDateUpdateEpoch = 0;

bool weatherHasSunTimes = false;
bool initialDateSet = false;
bool initialThemeSet = false;

bool sdDayBgAvailable = false;
bool sdNightBgAvailable = false;
bool sdWinterNightBgAvailable = false;

enum ClockPillMode {
  CLOCK_PILL_BOTTOM_RIGHT,
  CLOCK_PILL_TOP_RIGHT,
  CLOCK_PILL_HIDDEN
};

char activeThemeKey[28] = "normal";
char activeThemeName[36] = "Normal";
char activeLightPath[72] = "/bg/day.rgb565";
char activeDarkPath[72] = "/bg/night.rgb565";
ClockPillMode activeLightClockMode = CLOCK_PILL_BOTTOM_RIGHT;
ClockPillMode activeDarkClockMode = CLOCK_PILL_BOTTOM_RIGHT;
bool activeFestivalTheme = false;

// Live measured precipitation (mm, current hour) from Open-Meteo. Used to make
// the ticker say something useful rather than trusting the weathercode alone.
float currentPrecipMm = 0.0f;

// Nearest festival starting within the look-ahead window. Refreshed once per
// day from the same /config/festivals.txt pass that resolves today's theme --
// no second file read, no timer, no extra widget.
char nextFestivalName[20] = "";
int  nextFestivalDaysAway = -1;            // -1 = nothing worth announcing
uint8_t nextFestivalRank = 2;              // 1 = major (10-day run-up), 2 = minor (5-day)
bool tickerWakeSyncPending = false;        // screen just woke: show an insight in ~3s
const int FESTIVAL_LOOKAHEAD_DAYS = 10;        // major festivals
const int FESTIVAL_MINOR_LOOKAHEAD_DAYS = 5;   // everything else
bool activeThemeFromConfig = false;
unsigned long lastFestivalConfigCheckAt = 0;

static lv_obj_t *bellTile;
static lv_obj_t *bellIcon;
static lv_obj_t *bellWave1;
static lv_obj_t *bellWave2;

static lv_obj_t *ceilingTile;
static lv_obj_t *ceilingLabel;
static lv_obj_t *ceilingGlowLeft = nullptr;
static lv_obj_t *ceilingGlowRight = nullptr;
bool ceilingGlowStateShown = false;

static lv_obj_t *weatherIconBox = nullptr;
static lv_obj_t *weatherIconImage = nullptr;
static lv_img_dsc_t weatherIconDsc;
static uint8_t *weatherIconBuffer = nullptr;
char currentWeatherIconPath[64] = "";
char weatherCondition[24] = "--";
int wmoCode = 0;    // WMO weather code from Open-Meteo, stored directly
int currentWeatherTemp = 0;

static lv_obj_t *ceilingDot = nullptr;
static lv_obj_t *panelDot = nullptr;
static lv_obj_t *ceilingModeBadge = nullptr;
static lv_obj_t *panelModeBadge = nullptr; // shown instead of ceilingModeBadge when autoLightTarget == AUTO_TARGET_PANEL

static lv_obj_t *panelTile;
static lv_obj_t *panelLabel;
static lv_obj_t *panelGlowTop = nullptr;
static lv_obj_t *panelGlowBottom = nullptr;
bool panelGlowStateShown = false;
unsigned long autoManualStartedAt = 0;
unsigned long lastAutoManualOnReminderAt = 0; // tracks hourly "light left on?" reminders

static lv_obj_t *clockPill;
static lv_obj_t *clockLabel;
static lv_obj_t *clockAmPmLabel;
static lv_obj_t *adminHotspot;

static lv_obj_t *brightnessBtnLabel = nullptr;
bool cachedGt911Online = false;
bool cachedPcfOnline = false;
unsigned long lastDiagHardwarePollAt = 0;
char firmwareUpdateStatus[96] = "No update file";
bool firmwareUpdateRunning = false;

WebServer firmwareUploadServer(80);
File firmwareUploadFile;
bool firmwareUploadPortalActive = false;

// Update-from-GitHub state. Written by the core-0 worker, consumed by loop().
volatile bool    firmwareDownloadActive = false;  // listener stands down while true
volatile bool    fwStaging              = false;  // loop() is writing the image to the card: no TLS, no pushes
volatile uint8_t firmwareDownloadPct    = 0;
volatile uint8_t firmwareUpdateResult   = 0;      // 0 none, see FWR_* below
char             firmwareUpdateVersion[16] = "";  // version found by check/download
char             firmwareUpdateMsg[96]     = "";  // human reason on failure
enum { FWR_NONE = 0, FWR_UP_TO_DATE, FWR_AVAILABLE, FWR_DOWNLOADED, FWR_FAILED };
// Declared here, above every function: the IDE emits fwFetchManifest()'s
// auto-prototype at the top of the file, so its return type must exist there.
struct FwManifest { char version[16]; size_t size; char md5[40]; char festivals[24]; bool ok; };
// Also up here for the same auto-prototype reason: enqueuePush() takes one.
struct PushNotificationParams {
  char title[40];
  char message[240];   // 240 (was 120): remote-command replies carry the help list
  char priority[12];
  char actions[160];   // optional ntfy "Actions" header -- buttons ON the notification
  char suffix[8];      // "" = main topic; "-ver" = silent version channel (nobody subscribes)
  // Notification hygiene (ntfy sequence IDs, Android app >= 1.22.2):
  //  seq      -- short key ("motion", "reply", ...). A later push with the same
  //              key REPLACES the earlier card on the phone instead of adding
  //              one. Empty = ordinary new card.
  //  clearMs  -- if nonzero, the worker dismisses the card this many ms after
  //              it was sent (PUT /<topic>/<seq>/clear). Needs seq.
  //  click    -- tapping the card opens the phone control page.
  char seq[24];
  uint32_t clearMs;
  bool click;
};
volatile bool    fwResultFromSilentCheck = false; // background check: no push, no screen
char             fwCheckSummary[40] = "Not checked yet"; // shown on Diagnostics
// Diagnostics counters (plain status, no cost in normal operation).
uint16_t         i2cResetCount   = 0;   // bus re-inits since boot (see i2cBusRecover)
uint16_t         touchRejectCount = 0;  // touch reads thrown out as impossible (bus noise)
unsigned long    doorbellPausedUntil = 0; // storm guard: bell ignored until this time
unsigned long    pcfOfflineSince  = 0;   // 0 = responding; else millis() it stopped
bool             pcfOfflineNotified = false;
volatile uint8_t remoteLinkState = 0;   // 0 not set up, 1 connecting, 2 listening
// Network health counters, reported hourly in the HEALTH log line and on
// Diagnostics. Cheap integers; they are what turns "it feels slow" into a
// number that points at the cause.
volatile uint16_t statListenerReconnects = 0;   // command stream re-subscribed
volatile uint16_t statWifiReconnects     = 0;   // post-boot Wi-Fi reconnects
volatile uint16_t statPushOk             = 0;   // pushes/replies ntfy accepted
volatile uint16_t statPushFail           = 0;   // pushes/replies given up on
volatile int      statPushLastFailCode   = 0;   // HTTP code / -ve = network
// One worker sends every push and reply, in order, one TLS session at a
// time (the command listener keeps its own). No more one-task-per-push.
static QueueHandle_t     pushQueue = nullptr;
static SemaphoreHandle_t tlsGate   = nullptr;   // outbound TLS: one at a time
static char              pushFailPending[96] = ""; // worker -> loop(): log line
volatile uint8_t festivalConfigSynced = 0; // 1 = SD file replaced from GitHub, 2 = already identical
char festivalConfigVer[24] = "";            // stamp of the repo file last applied (NVS "festVer")
char festivalConfigPendingVer[24] = "";     // stamp the worker just synced, stored by loop()
const char *SD_UPDATE_FAILED_PATH = "/update/firmware.failed";
unsigned long firmwareUploadPortalStartedAt = 0;
bool firmwareAutoInstallPending = false;
unsigned long firmwareAutoInstallStartedAt = 0;
uint8_t currentPortalUploadKind = 0; // 1 firmware, 2 festivals, 3 background/asset
char firmwareUploadStatus[120] = "Upload portal closed";
char portalUploadPath[96] = "";

// WiFi provisioning portal -- separate WebServer on port 81 so it can
// coexist in the same binary without touching the OTA portal on port 80.
// Opened only on demand via Settings -> Network -> "Change" button.
WebServer wifiProvServer(81);
bool wifiProvPortalActive = false;
static bool     wifiPortalSkipPressed = false;   // set by Skip button, breaks first-boot loop
static String   wifiScanOpts          = "";       // pre-built <option> HTML from non-blocking scan
static bool     wifiScanPending       = false;    // true while waiting on WiFi.scanComplete()
static uint32_t wifiScanAppliedAt     = 0;        // millis() when wifiScanOpts was last (re)built
static lv_obj_t *phoneSetupScreen   = nullptr;
static lv_obj_t *phoneSetupQrCode   = nullptr;   // lv_qrcode widget
static bool      phoneSetupFirstRun = false;     // true = post-provisioning pass

static lv_obj_t *wifiPortalScreen   = nullptr;
static lv_obj_t *wifiPortalQrCode   = nullptr;   // lv_qrcode widget (LVGL built-in, LV_USE_QRCODE)
static lv_obj_t *wifiPortalSsidLbl  = nullptr;
static lv_obj_t *wifiPortalStatusLbl = nullptr;
const char *WIFI_PROV_AP_SSID = "Panel-Setup";
const uint16_t WIFI_PROV_PORT = 81;

static lv_obj_t *updateScreen = nullptr;
static lv_obj_t *updateBgImage = nullptr;
static lv_img_dsc_t updateBgDsc;
static uint8_t *updateBgBuffer = nullptr;
static lv_obj_t *updateMessageLabel = nullptr;
static lv_obj_t *updateUrlLabel = nullptr;
static lv_obj_t *updateHintLabel = nullptr;

static lv_obj_t *backgroundImage = nullptr;
static lv_img_dsc_t bgDsc;
static uint8_t *bgBuffer = nullptr;

// Small SD-card RGB565 UI icons.
// Required files:
// /icons/bell.rgb565
// /icons/ceiling.rgb565
// /icons/panel.rgb565
// File sizes currently expected:
// /icons/bell.rgb565    = 174 x 199 x 2 = 69252 bytes
// /icons/ceiling.rgb565 = 127 x 145 x 2 = 36830 bytes
// /icons/panel.rgb565   = 96 x 132 x 2 = 25344 bytes
static lv_img_dsc_t bellIconDsc;
static lv_img_dsc_t ceilingIconDsc;
static lv_img_dsc_t panelIconDsc;

static uint8_t *bellIconBuffer = nullptr;
static uint8_t *ceilingIconBuffer = nullptr;
static uint8_t *panelIconBuffer = nullptr;

// Bell ring overlay assets.
// Required optional files:
// /icons/bell_ring_inner.rgb565 = 150 x 170 x 2 = 51000 bytes
// /icons/bell_ring_outer.rgb565 = 150 x 170 x 2 = 51000 bytes
static lv_obj_t *bellRingInner = nullptr;
static lv_obj_t *bellRingOuter = nullptr;
static lv_img_dsc_t bellRingInnerDsc;
static lv_img_dsc_t bellRingOuterDsc;
static uint8_t *bellRingInnerBuffer = nullptr;
static uint8_t *bellRingOuterBuffer = nullptr;

const uint16_t BELL_RING_W = 150;
const uint16_t BELL_RING_H = 170;
// Tweak these two values only if the ring overlay needs micro-alignment.
// Larger static pre-converted ring assets; zero runtime scaling cost.
const int BELL_RING_X = 85;
const int BELL_RING_Y = 70;

int touch_last_x = 0;
int touch_last_y = 0;

bool pcfOnline = false;
unsigned long lastPcfFailAt = 0;

static lv_obj_t *bellPressOverlay = nullptr;
static lv_obj_t *ceilingPressOverlay = nullptr;
static lv_obj_t *panelPressOverlay = nullptr;

// =====================================================
// "Welcome Home" greeting overlay
// Shown when motion is detected but the doorbell is not
// pressed within welcomeBellGraceMs.
// =====================================================
static lv_obj_t *welcomeOverlay = nullptr; // full-screen dark shield (#0F172A)
static lv_obj_t *welcomeLabel   = nullptr; // primary title
static lv_obj_t *welcomeSubLabel = nullptr; // contextual subtitle
static lv_obj_t *welcomeLine    = nullptr; // teal growing line (#2DD4BF)

bool welcomeOverlayPending = false;        // motion seen, waiting on bell-press grace period
bool welcomeOverlayActive  = false;        // greeting animation currently playing
bool welcomeOverlayArmed = true;
unsigned long motionDetectedForWelcomeAt = 0;
unsigned long motionClearedAt = 0;

unsigned long welcomeBellGraceMs = 4000;  // "Welcome screen duration" — Settings > Display, loaded from NVS
const unsigned long WELCOME_REARM_MS = 15000; // show greeting if no bell press within this time


// =====================================================
// Backlight
// =====================================================

uint8_t currentBacklight = 210;
uint8_t targetBacklight = 210;
unsigned long lastBacklightStepAt = 0;

// Settings PIN: stored as a 4-digit string. Empty string = no PIN set yet
// (first-run state -- settings open directly and prompt user to set one).
String settingsPin = "";

// Runtime WiFi / ntfy credentials -- loaded from NVS at boot, changed via
// the provisioning portal. Fall back to the compile-time consts if NVS has
// nothing yet so existing flashed units keep working with zero config change.
String nvsWifiSsid = "";
String nvsWifiPass = "";
String nvsNtfyTopic = "";
String nvsPhoneUrl   = "";   // overrides PHONE_UI_BASE_URL when non-empty
bool   phoneSetupPending = false; // show Phone Setup once after provisioning

// IP-geolocation cache: detected automatically once per boot after WiFi
// connects (fetchIpGeolocation()), persisted to NVS so weather has an
// immediate value to use on subsequent boots while a fresh lookup runs.
String nvsGeoLat = "";
String nvsGeoLon = "";

void loadSavedSettings() {
  prefs.begin("doorpanel", false);

  brightnessLevel = prefs.getUChar("brightness", 5); // default Level 6/7
  if (brightnessLevel > 6) {
    brightnessLevel = 5;
  }

  // PIR sensitivity tiers: raw consecutive 50ms HIGH samples needed before
  // motion is confirmed (Low=20 / Med=10 / High=5 -- see updateMotionSensor()).
  // Deliberately a NEW key ("pirSens3", not "pirSens2") -- "pirSens2" stored
  // an occupancy-score threshold (60-90 range) on a since-removed scoring
  // algorithm; reinterpreting that as a raw sample count would have made
  // the sensor require minutes of unbroken motion to ever trigger. A fresh
  // key means any unit with an old "pirSens2" value safely ignores it and
  // gets the new default instead.
  motionConfirmHighSamples = prefs.getUChar("pirSens3", 20);

  motionHoldTimeMs         = prefs.getULong("motHold", 10000);
  autoManualOffCooldownMs = prefs.getULong("manOff", 15000);
  uiDimAfterMs              = prefs.getULong("dimDly", 5000);
  welcomeBellGraceMs        = prefs.getULong("bellGrace", 4000);
  motionDetectionEnabled    = prefs.getBool("motEn", true);
  settingsPin                = prefs.getString("pin", "");

  // Which physical light motion detection drives -- "Motion controls" in
  // Settings > Motion. Defaults to Ceiling (0) so existing flashed units
  // keep their current behavior until the user changes it.
  autoLightTarget = (prefs.getUChar("autoTgt", 0) == 1) ? AUTO_TARGET_PANEL : AUTO_TARGET_CEILING;

  // WiFi / ntfy: load from NVS; empty string means "use compile-time default"
  // so existing flashed units work without any NVS migration.
  nvsWifiSsid  = prefs.getString("wSsid", "");
  nvsWifiPass  = prefs.getString("wPass", "");
  nvsNtfyTopic = prefs.getString("ntfy",  "");
  nvsPhoneUrl  = prefs.getString("phUrl", "");
  phoneSetupPending = prefs.getBool("phPend", false);
  prefs.getString("festVer", festivalConfigVer, sizeof(festivalConfigVer));
  nvsGeoLat    = prefs.getString("geoLat", "");
  nvsGeoLon    = prefs.getString("geoLon", "");

  prefs.end();
}

void saveBrightnessLevel() {
  prefs.begin("doorpanel", false);
  prefs.putUChar("brightness", brightnessLevel);
  prefs.end();
}

// Each setting saves itself the moment it's changed in the UI -- no
// separate "Save" button, and no risk of one screen's worth of changes
// being lost if the device loses power before a save step.
void saveMotionConfirmHighSamples() {
  prefs.begin("doorpanel", false);
  prefs.putUChar("pirSens3", motionConfirmHighSamples);
  prefs.end();
}

void saveMotionHoldTimeMs() {
  prefs.begin("doorpanel", false);
  prefs.putULong("motHold", motionHoldTimeMs);
  prefs.end();
}

void saveAutoManualOffCooldownMs() {
  prefs.begin("doorpanel", false);
  prefs.putULong("manOff", autoManualOffCooldownMs);
  prefs.end();
}

void saveAutoLightTarget() {
  prefs.begin("doorpanel", false);
  prefs.putUChar("autoTgt", autoLightTarget == AUTO_TARGET_PANEL ? 1 : 0);
  prefs.end();
}

void saveUiDimAfterMs() {
  prefs.begin("doorpanel", false);
  prefs.putULong("dimDly", uiDimAfterMs);
  prefs.end();
}

void saveWelcomeBellGraceMs() {
  prefs.begin("doorpanel", false);
  prefs.putULong("bellGrace", welcomeBellGraceMs);
  prefs.end();
}

void saveMotionDetectionEnabled() {
  prefs.begin("doorpanel", false);
  prefs.putBool("motEn", motionDetectionEnabled);
  prefs.end();
}

void saveSettingsPin(const String &newPin) {
  settingsPin = newPin;
  prefs.begin("doorpanel", false);
  prefs.putString("pin", settingsPin);
  prefs.end();
}

void saveWifiCredentials(const String &ssid, const String &pass) {
  nvsWifiSsid = ssid;
  nvsWifiPass = pass;
  prefs.begin("doorpanel", false);
  prefs.putString("wSsid", ssid);
  prefs.putString("wPass", pass);
  prefs.end();
}

void savePhoneSetupPending(bool pending) {
  phoneSetupPending = pending;
  prefs.begin("doorpanel", false);
  prefs.putBool("phPend", pending);
  prefs.end();
}

void saveNtfyTopic(const String &topic) {
  nvsNtfyTopic = topic;
  prefs.begin("doorpanel", false);
  prefs.putString("ntfy", topic);
  prefs.end();
}

// =====================================================
// First-boot ntfy topic
// =====================================================
// The customer never invents a topic name. On first boot the panel mints its
// own: two warm words plus a random suffix, e.g. "copper-threshold-k7f2m9".
//
// Why this shape:
//  - It is what the customer SEES in the ntfy app as their subscription, so
//    it should read like a product, not a serial number.
//  - 16 x 16 word pairs x 31^6 suffixes is ~227 billion combinations, which
//    matters because on ntfy.sh the topic name IS the security model -- a
//    guessable topic ("doorbell", "home") lets a stranger read your alerts
//    or send commands. Customer-chosen names are exactly the guessable kind.
//  - The suffix alphabet omits 0/O/1/l/I so it survives being read aloud or
//    copied by hand.
// Entropy comes from esp_random() (hardware RNG, not the libc PRNG), so two
// units off the same production run never collide.

// Two evocative words plus a short code. The words carry most of the
// uniqueness (64 x 64 = 4096 pairs), which lets the random tail stay short
// and clean -- "copper-hearth-k4m2" rather than a string of noise. Total
// space is 4096 x 31^4 = ~3.8 billion, far past anything worth enumerating
// for the privilege of switching off a porch light.
//
// The two lists are kept deliberately warm and domestic so the result reads
// like a name for a home, not a serial number: it is the one piece of this
// system the customer sees every day in their notifications.

static const char *TOPIC_WORD_A[64] = {
  "amber","ash","autumn","birch","bronze","cedar","clay","copper",
  "coral","cotton","cream","dawn","dusk","ember","fern","flint",
  "garnet","ginger","gold","grove","harvest","hazel","heather","honey",
  "indigo","ivory","jade","juniper","laurel","linen","maple","marble",
  "meadow","mist","moss","myrtle","oak","ochre","olive","onyx",
  "opal","pearl","pine","poppy","quartz","quiet","reed","river",
  "rose","rowan","russet","saffron","sage","sand","silk","slate",
  "spruce","stone","teak","thistle","topaz","umber","velvet","willow"
};

static const char *TOPIC_WORD_B[64] = {
  "alcove","arbor","archway","atrium","attic","balcony","beacon","bower",
  "cabin","chamber","chime","cloister","cottage","courtyard","cove","doorway",
  "entry","foyer","gable","garden","gate","grange","harbor","haven",
  "hearth","hollow","keep","landing","lantern","ledge","lodge","loft",
  "manor","mantel","nook","orchard","outlook","parlor","pathway","pavilion",
  "piazza","porch","portal","portico","quarter","rampart","refuge","retreat",
  "ridge","roost","sanctum","shelter","solace","spire","stable","steps",
  "stoop","terrace","threshold","tower","veranda","vestibule","villa","window"
};

// 31 characters, with 0/O/1/l/I left out so the code survives being read
// aloud, written on a card, or typed by hand.
static const char TOPIC_SUFFIX_ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";

String generateNtfyTopic() {
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();

  String t = String(TOPIC_WORD_A[r1 & 0x3F]) + "-" +          // 6 bits
             String(TOPIC_WORD_B[(r1 >> 8) & 0x3F]) + "-";    // 6 bits

  const uint8_t alphabetLen = sizeof(TOPIC_SUFFIX_ALPHABET) - 1;
  for (uint8_t i = 0; i < 4; i++) {
    t += TOPIC_SUFFIX_ALPHABET[r2 % alphabetLen];
    r2 /= alphabetLen;
  }
  return t;
}

// Called once at boot. Mints and persists a topic only when there is genuinely
// none -- an already-provisioned unit, or one with a compile-time default, is
// left exactly as it is, so this is safe on existing flashed panels.
void ensureNtfyTopic() {
  if (nvsNtfyTopic.length() > 0) return;
  if (strlen(NTFY_TOPIC) > 0) return;

  String fresh = generateNtfyTopic();
  saveNtfyTopic(fresh);
  logActivityEvent("SETUP", "Notification topic generated");
}


void saveGeoLocation(const String &lat, const String &lon) {
  nvsGeoLat = lat;
  nvsGeoLon = lon;
  prefs.begin("doorpanel", false);
  prefs.putString("geoLat", lat);
  prefs.putString("geoLon", lon);
  prefs.end();
}

// ---- Boot-attempt counter (crash-loop fallback) ----
// Stored as a single byte in the same "doorpanel" NVS namespace already used
// everywhere else in this file -- no new namespace, no new storage area.
const uint8_t BOOT_FAIL_THRESHOLD = 5;

uint8_t registerBootAttempt() {
  prefs.begin("doorpanel", false);
  uint8_t attempts = prefs.getUChar("bootAtt", 0);
  attempts = (attempts >= 250) ? 250 : attempts + 1; // avoid wraparound
  prefs.putUChar("bootAtt", attempts);
  prefs.end();
  return attempts;
}

void markBootSuccessful() {
  prefs.begin("doorpanel", false);
  prefs.putUChar("bootAtt", 0);
  prefs.end();
}

// Recovery code: a 6-digit code derived from this chip's unique MAC address,
// salted with a fixed constant. Never stored anywhere -- recomputed fresh
// every time it's checked, so there is nothing in flash or NVS to dump.
// This code should be hand-written on a label and stuck inside the unit
// (behind the faceplate) at install time -- print it once via the
// "PRINT_RECOVERY_CODE_ON_BOOT" serial debug block below, then remove
// that block (or leave it, it only ever goes to USB serial, not the network).
uint32_t getRecoveryCode() {
  uint64_t mac = ESP.getEfuseMac();
  const uint32_t SALT = 0x6B4A3D17UL; // fixed, project-specific, not user configurable
  uint32_t h = (uint32_t)(mac & 0xFFFFFFFFUL) ^ (uint32_t)(mac >> 32) ^ SALT;
  // simple mix so the result doesn't trivially resemble the MAC
  h = (h ^ (h >> 16)) * 0x45D9F3BUL;
  h = (h ^ (h >> 16)) * 0x45D9F3BUL;
  h = h ^ (h >> 16);
  return h % 1000000UL; // 6 digits, 000000-999999
}

#define PRINT_RECOVERY_CODE_ON_BOOT 1 // set to 0 once the label has been printed and stuck inside the unit

// ---- PIN pad / recovery gesture state ----
// The admin hotspot (top-left, 54x54) opens the PIN pad on a normal
// long-press, using LVGL's own LV_EVENT_LONG_PRESSED -- no manual timer
// needed. Recovery is reached via the "Forgot?" button INSIDE the PIN
// pad itself (see pinPadForgotCb below), not a second hidden gesture.
// This is lighter than a hand-timed hold: one existing LVGL event,
// zero extra state, zero per-frame timing checks.

// PIN attempt lockout: after 5 wrong PIN entries in a row, the pad
// refuses new digits for 30 seconds. Resets to 0 on a correct entry.
// This is intentionally simple (one counter, one timestamp, same NVS
// namespace) -- enough to stop idle guessing without adding a second
// screen or a captcha-style flow on a porch panel.
uint8_t pinWrongAttempts = 0;
unsigned long pinLockoutUntil = 0;
const uint8_t PIN_LOCKOUT_THRESHOLD = 5;
const unsigned long PIN_LOCKOUT_MS = 30000UL;

// PIN pad screen state (lazy-created, lazy-destroyed -- does not exist
// in RAM at all unless the hotspot gesture is actively in progress).
static lv_obj_t *pinPadScreen = nullptr;
static bool pinPadOpenedFromSettings = false;  // routes Back correctly: home-screen entry vs. Change PIN inside Settings
static lv_obj_t *pinPadTitle = nullptr;
static lv_obj_t *pinPadSub = nullptr;
static lv_obj_t *pinPadDots[4] = { nullptr, nullptr, nullptr, nullptr };
static lv_obj_t *pinPadForgotBtn = nullptr;
static lv_obj_t *pinPadForgotLabel = nullptr;

bool pinPadRecoveryMode = false;   // false = entering PIN, true = entering recovery code
bool pinPadIsForReset = false;     // true only when recovery code was just verified -> next 4 digits become the NEW pin
String pinPadEntry = "";
String pinPadFirstNewPin = "";     // holds first entry during "set/change PIN" two-step confirm

uint8_t getSelectedBrightness() {
  if (brightnessLevel > 6) brightnessLevel = 5;
  return BRIGHTNESS_LEVELS[brightnessLevel];
}

void updateBrightnessButtonText() {
  if (!brightnessBtnLabel) return;

  char label[24];
  snprintf(label, sizeof(label), "Brightness %u/7", brightnessLevel + 1);
  lv_label_set_text(brightnessBtnLabel, label);
}

void setBacklight(uint8_t value) {
  currentBacklight = value;
  targetBacklight = value;
  ledcWrite(1, value);
}

void fadeBacklightTo(uint8_t value) {
  targetBacklight = value;
}

void updateBacklightFade() {
  if (currentBacklight == targetBacklight) return;

  if (millis() - lastBacklightStepAt < 6) return;
  lastBacklightStepAt = millis();

  const uint8_t stepSize = 3;

  if (currentBacklight < targetBacklight) {
    uint16_t nextValue = currentBacklight + stepSize;
    currentBacklight = nextValue > targetBacklight ? targetBacklight : nextValue;
  } else {
    int16_t nextValue = currentBacklight - stepSize;
    currentBacklight = nextValue < targetBacklight ? targetBacklight : nextValue;
  }

  ledcWrite(1, currentBacklight);
}

void applyBrightnessLevel() {
  fadeBacklightTo(getSelectedBrightness());
  screenDimmed = false;
  lastUserActivityAt = millis();
  updateBrightnessButtonText();
}

void wakeScreen() {
  lastUserActivityAt = millis();

  if (screenDimmed) {
    fadeBacklightTo(getSelectedBrightness());
    screenDimmed = false;
    tickerWakeSyncPending = true;   // updateWeatherTicker() re-phases itself
  }
}

bool isDisplaySleepWindowNow() {
  if (!isTimeReadyFast()) return false; // no time yet -- don't guess, just behave normally

  struct tm t;
  getLocalTmFast(&t);

  int nowMin   = t.tm_hour * 60 + t.tm_min;
  int startMin = DISPLAY_SLEEP_START_HOUR * 60 + DISPLAY_SLEEP_START_MIN;
  int endMin   = DISPLAY_SLEEP_END_HOUR * 60 + DISPLAY_SLEEP_END_MIN;

  return nowMin >= startMin && nowMin < endMin;
}

// Called every loop while inside the overnight sleep window. Instead of
// abruptly killing the backlight the instant the clock hits 1:30 AM, this
// waits until things are actually quiet -- no touch/UI activity for
// NIGHT_SLEEP_IDLE_MS (3 min) AND no current or recent motion (within
// motionHoldTimeMs) -- before dropping to fully OFF. So if someone is
// standing nearby, motion just fired, or the panel is mid-use right at
// 1:30, the screen keeps behaving normally and only goes dark once it's
// genuinely idle for a few minutes. Touch or motion afterward wakes it via
// the existing wakeScreen() path, and it will settle back to OFF again
// after the same idle check, for as long as the window lasts.
void updateNightDisplaySleepTransition() {
  bool inWindow = isDisplaySleepWindowNow();
  if (!inWindow) return;

  bool userIdle   = millis() - lastUserActivityAt > NIGHT_SLEEP_IDLE_MS;
  bool motionIdle = !motionStableState &&
                     (lastMotionSeenAt == 0 || millis() - lastMotionSeenAt > motionHoldTimeMs);

  if (userIdle && motionIdle && !(screenDimmed && currentBacklight == 0 && targetBacklight == 0)) {
    fadeBacklightTo(0);
    screenDimmed = true;
  }
}

void updateScreenDimming() {
  // Presence = activity. While motion is confirmed, or within the same
  // hold window (motionHoldTimeMs) that keeps the auto light on, the
  // screen must not dim -- someone is standing there. This locks screen
  // brightness to the exact lifetime of the motion-driven light, and it
  // bridges the LOW gaps a PIR emits between retriggers when a person is
  // present but still. Night sleep is unaffected: its own idle check
  // already independently requires motion to be clear.
  bool presence = motionDetectionEnabled &&
                  (motionStableState ||
                   (motionEverSeen && (millis() - lastMotionSeenAt) <= motionHoldTimeMs));
  if (presence) {
    lastUserActivityAt = millis();
    if (screenDimmed) wakeScreen();
    return;
  }

  if (!screenDimmed && millis() - lastUserActivityAt > uiDimAfterMs) {
    uint8_t target = isDisplaySleepWindowNow() ? 0 : BACKLIGHT_DIM;
    fadeBacklightTo(target);
    screenDimmed = true;
  }
}


// =====================================================
// PCF8574 helpers
// =====================================================

// =====================================================
// I2C bus recovery
// =====================================================
// The GT911 touch controller and the PCF8574 output expander share one bus,
// and nothing in normal operation ever resets it -- only a reboot does,
// because Wire.begin() runs the driver's bus-clear sequence. A bus left
// wedged (a slave holding SDA after a glitch, a controller NACKing out of
// its own idle state) therefore looked like "animations alive, touch dead,
// outputs dead" until someone restarted the panel. This re-initialises the
// master in place. Called only from the UI thread, the sole user of Wire,
// so there is nothing to race with. Rate-limited so a genuinely dead slave
// cannot turn it into a reset storm.
static void i2cBusRecover(const char *who) {
  static unsigned long lastRecoverAt = 0;
  if (lastRecoverAt != 0 && millis() - lastRecoverAt < 5000UL) return;
  lastRecoverAt = millis();

  Wire.end();
  delay(2);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  if (i2cResetCount < 65535) i2cResetCount++;

  static unsigned long lastLogAt = 0;
  if (lastLogAt == 0 || millis() - lastLogAt > 60000UL) {
    lastLogAt = millis();
    char d[40];
    snprintf(d, sizeof(d), "I2C bus reset (%s)", who);
    logActivityEvent("I2C", d);
  }
}

bool pcfWriteState() {
  Wire.beginTransmission((uint8_t)PCF_ADDR);
  Wire.write(pcfState);
  uint8_t err = Wire.endTransmission();

  if (err != 0) {
    // Reset the bus and retry once. A failed output write is the one place
    // we can be sure the bus is unhealthy, so act on it immediately.
    i2cBusRecover("pcf");
    Wire.beginTransmission((uint8_t)PCF_ADDR);
    Wire.write(pcfState);
    err = Wire.endTransmission();

    static unsigned long lastLogAt = 0;
    if (millis() - lastLogAt > 60000UL) {
      lastLogAt = millis();
      char d[64];
      snprintf(d, sizeof(d), "PCF8574 write %s after reset (err %u)",
               err == 0 ? "recovered" : "FAILED", (unsigned)err);
      logActivityEvent("I2C", d);
    }
  }

  bool ok = (err == 0);
  pcfOnline = ok;
  if (!ok) {
    lastPcfFailAt = millis();
  }

  return ok;
}

bool pcfIsPresent() {
  Wire.beginTransmission((uint8_t)PCF_ADDR);
  bool ok = (Wire.endTransmission() == 0);
  pcfOnline = ok;
  return ok;
}

void pcfSetPin(uint8_t pin, bool logicalOn) {
  bool outputLow;

  if (SSR_ACTIVE_LOW) {
    outputLow = logicalOn;
  } else {
    outputLow = !logicalOn;
  }

  if (outputLow) {
    pcfState &= ~(1 << pin);
  } else {
    pcfState |= (1 << pin);
  }

  pcfWriteState();
}

void allOutputsOff() {
  pcfState = 0xFF;
  pcfWriteState();

  pcfSetPin(PCF_DOORBELL_PIN, false);
  pcfSetPin(PCF_CEILING_PIN, false);
  pcfSetPin(PCF_PANEL_PIN, false);
  pcfSetPin(PCF_SPARE_PIN, false);

  ceilingState = false;
  panelOutputState = false;
  autoMode = AUTO_MODE_AUTO;
  autoManualStartedAt = 0;
  autoManualOffStartedAt = 0;
}


// =====================================================
// GT911 raw touch helpers
// =====================================================

bool gt911WriteReg8(uint16_t reg, uint8_t value) {
  Wire.beginTransmission((uint8_t)GT911_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool gt911ReadBytes(uint16_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission((uint8_t)GT911_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t received = Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)len);

  if (received != len) {
    return false;
  }

  for (uint8_t i = 0; i < len; i++) {
    buf[i] = Wire.read();
  }

  return true;
}

bool gt911IsPresent() {
  Wire.beginTransmission((uint8_t)GT911_ADDR);
  return Wire.endTransmission() == 0;
}

bool gt911ReadTouch(int16_t *x, int16_t *y) {
  uint8_t status = 0;

  // A healthy GT911 always answers the status read, finger or no finger.
  // A run of failed status reads therefore means the bus, not the user.
  // LVGL polls this every ~30ms, so 10 misses is about a third of a second
  // -- long enough to be sure, short enough that a wedged bus is cleared
  // before anyone notices the panel stopped responding.
  static uint8_t statusFailStreak = 0;
  if (!gt911ReadBytes(GT911_REG_STATUS, &status, 1)) {
    if (++statusFailStreak >= 10) {
      statusFailStreak = 0;
      i2cBusRecover("touch");
    }
    return false;
  }
  statusFailStreak = 0;

  if ((status & 0x80) == 0) {
    return false;
  }

  uint8_t pointCount = status & 0x0F;

  if (pointCount == 0 || pointCount > 5) {
    gt911WriteReg8(GT911_REG_STATUS, 0);
    return false;
  }

  uint8_t data[6];

  if (!gt911ReadBytes(GT911_REG_POINT1, data, 6)) {
    gt911WriteReg8(GT911_REG_STATUS, 0);
    return false;
  }

  uint16_t rawX = data[0] | (data[1] << 8);
  uint16_t rawY = data[2] | (data[3] << 8);

  gt911WriteReg8(GT911_REG_STATUS, 0);

  // A corrupted read (bus noise from a switching load, a half-finished
  // transfer) hands back coordinates that cannot come from a finger. The
  // old constrain() below would have clamped such garbage INTO the screen
  // and delivered it as a tap on whatever sat at the edge -- the bell tile
  // included. Reject anything outside the panel's touch range, and count
  // it, so a noisy bus shows up as a number instead of as ghost presses.
  if (rawX > TOUCHMAPX2 + 20 || rawY > TOUCHMAPY2 + 20 ||
      (rawX == 0 && rawY == 0) || (rawX == 0xFFFF && rawY == 0xFFFF)) {
    if (touchRejectCount < 65535) touchRejectCount++;
    return false;
  }

  int mappedX = map(rawX, TOUCHMAPX1, TOUCHMAPX2, 0, lcd.width() - 1);
  int mappedY = map(rawY, TOUCHMAPY1, TOUCHMAPY2, 0, lcd.height() - 1);

  mappedX = constrain(mappedX, 0, lcd.width() - 1);
  mappedY = constrain(mappedY, 0, lcd.height() - 1);

  touch_last_x = mappedX;
  touch_last_y = mappedY;

  *x = mappedX;
  *y = mappedY;

  return true;
}

lv_obj_t *createStateDot(lv_obj_t *parent, int x, int y) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);

  lv_obj_set_size(dot, 13, 13);
  lv_obj_set_pos(dot, x, y);

  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_bg_opa(dot, 180, 0);
  lv_obj_set_style_shadow_width(dot, 0, 0);

  return dot;
}

void setStateDot(lv_obj_t *dot, bool active) {
  if (!dot) return;

  if (active) {
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x20D66B), 0);
    lv_obj_set_style_bg_opa(dot, 255, 0);

    // Softer, tighter premium glow
    lv_obj_set_style_shadow_color(dot, lv_color_hex(0x20D66B), 0);
    lv_obj_set_style_shadow_width(dot, 5, 0);
    lv_obj_set_style_shadow_spread(dot, 1, 0);
    lv_obj_set_style_shadow_opa(dot, 120, 0);
  } else {
    lv_obj_set_style_bg_color(dot, lv_color_hex(darkThemeActive ? 0x3B3830 : 0xB8B0A2), 0);
    lv_obj_set_style_bg_opa(dot, darkThemeActive ? 170 : 190, 0);

    lv_obj_set_style_shadow_width(dot, 0, 0);
    lv_obj_set_style_shadow_spread(dot, 0, 0);
    lv_obj_set_style_shadow_opa(dot, 0, 0);
  }
}

// =====================================================
// LVGL display flush
// =====================================================

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

  lcd.pushImageDMA(area->x1, area->y1, w, h, (lgfx::rgb565_t *)&color_p->full);
  lcd.waitDMA();

  lv_disp_flush_ready(disp);
}


// =====================================================
// LVGL touch read
// =====================================================

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  int16_t x, y;

  if (gt911ReadTouch(&x, &y)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;

    wakeScreen();

    // Production: no touch coordinates on main screen.
    // Coordinates remain available internally via touch_last_x / touch_last_y.
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}


// =====================================================
// WiFi / Time / Weather helpers
// =====================================================

void startWiFi() {
  // Credentials live only in NVS, set via the provisioning portal. If
  // nothing's saved yet, there's nothing to connect to -- setup() already
  // routes a fresh unit into the portal before this is ever called.
  if (nvsWifiSsid.length() == 0) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(true);
  WiFi.begin(nvsWifiSsid.c_str(), nvsWifiPass.c_str());
}

void reconnectWiFi() {
  WiFi.disconnect();
  delay(100);
  startWiFi();
}

unsigned long getWiFiRetryDelay(uint8_t index) {
  if (index >= WIFI_RETRY_DELAY_COUNT) {
    index = WIFI_RETRY_DELAY_COUNT - 1;
  }

  return WIFI_RETRY_DELAYS_MS[index];
}

bool wifiStatusDefinitelyFailed(wl_status_t status) {
  return status == WL_CONNECT_FAILED ||
         status == WL_NO_SSID_AVAIL ||
         status == WL_CONNECTION_LOST ||
         status == WL_DISCONNECTED;
}

void softWiFiRetry() {
  const char *ssid = nvsWifiSsid.length() > 0 ? nvsWifiSsid.c_str() : WIFI_SSID;
  const char *pass = nvsWifiPass.length() > 0 ? nvsWifiPass.c_str() : WIFI_PASS;
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);
}

void hardWiFiRetry() {
  const char *ssid = nvsWifiSsid.length() > 0 ? nvsWifiSsid.c_str() : WIFI_SSID;
  const char *pass = nvsWifiPass.length() > 0 ? nvsWifiPass.c_str() : WIFI_PASS;
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  delay(50);
  WiFi.begin(ssid, pass);
}

void updateWiFiStatus() {
  static unsigned long lastCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  static uint8_t wifiReconnectIndex = 0;

  // During boot, updateBootSequence() owns WiFi retry timing.
  if (bootNetStage != BOOT_NET_DONE) {
    return;
  }

  if (millis() - lastCheck < WIFI_CHECK_MS) {
    return;
  }

  lastCheck = millis();

  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    wifiReconnectIndex = 0;
    lastReconnectAttempt = millis();
    return;
  }

  unsigned long retryDelay = getWiFiRetryDelay(wifiReconnectIndex);

  if (millis() - lastReconnectAttempt >= retryDelay) {
    lastReconnectAttempt = millis();
    // Post-boot: use a clean reconnect because this is no longer the delicate
    // first boot connection window.
    hardWiFiRetry();
    if (statWifiReconnects < 65535) statWifiReconnects++;

    if (wifiReconnectIndex < WIFI_RETRY_DELAY_COUNT - 1) {
      wifiReconnectIndex++;
    }

    initTime();
  }
}

void initTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

bool isTimeReadyFast() {
  time_t now = time(nullptr);

  // Anything after 2024 means NTP is basically ready.
  return now > 1700000000;
}

void getLocalTmFast(struct tm *timeinfo) {
  time_t now = time(nullptr);
  localtime_r(&now, timeinfo);
}

time_t makeLocalMidnightPlusSeconds(int plusSeconds) {
  struct tm t;
  getLocalTmFast(&t);

  t.tm_hour = 0;
  t.tm_min = 0;
  t.tm_sec = plusSeconds;

  time_t midnightToday = mktime(&t);
  time_t now = time(nullptr);

  if (now >= midnightToday) {
    midnightToday += 86400;
  }

  return midnightToday;
}


// =====================================================
// SD asset theme / festival selection
// =====================================================

int ymdFromTm(const struct tm &t) {
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

int parseYmd(const char *dateText) {
  int y = 0, m = 0, d = 0;
  if (sscanf(dateText, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  return y * 10000 + m * 100 + d;
}

// Days since 1970-01-01 for a YYYYMMDD int. Needed because YMD integers are
// not linear -- 20261101 minus 20261028 is 73, not 4. Howard Hinnant's civil
// algorithm: branch-free, no mktime, no struct tm, no timezone involvement.
long daysFromYmd(int ymd) {
  int y = ymd / 10000;
  int m = (ymd / 100) % 100;
  int d = ymd % 100;

  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (long)era * 146097L + (long)doe - 719468L;
}

// Display name for a festivals.txt theme key. Returning nullptr means "do not
// announce this one" -- that is how winter and normal stay silent.
// Rank decides how long before the day the countdown starts, and whether
// the line is allowed to outrank weather in the final stretch.
//   1 = major: 10-day run-up, wins outright inside 3 days
//   2 = minor: 5-day run-up, keeps alternating with weather
static uint8_t festivalRankForKey(const char *key) {
  if (strcmp(key, "diwali") == 0 || strcmp(key, "holi") == 0 ||
      strcmp(key, "navratri") == 0 || strcmp(key, "christmas") == 0 ||
      strcmp(key, "new_year") == 0) return 1;
  return 2;
}

// Shown on the day itself, in place of the countdown. Bond-specific days
// (Rakhi, Mother's/Father's Day, Valentine's) deliberately get nothing --
// a wall panel wishing you those reads wrong.
static const char *festivalDayGreeting(const char *key) {
  if (strcmp(key, "diwali") == 0)           return "Happy Diwali";
  if (strcmp(key, "holi") == 0)             return "Happy Holi";
  if (strcmp(key, "navratri") == 0)         return "Happy Navratri";
  if (strcmp(key, "christmas") == 0)        return "Merry Christmas";
  if (strcmp(key, "new_year") == 0)         return "Happy New Year";
  if (strcmp(key, "ganesh_chaturthi") == 0) return "Ganpati Bappa Morya";
  if (strcmp(key, "janmashtami") == 0)      return "Happy Janmashtami";
  return nullptr;
}

// Replaces "WELCOME HOME" on the greeting overlay during a festival. Same
// rule as the ticker greeting: no bond-specific days.
static const char *festivalWelcomeTitle(const char *key) {
  if (strcmp(key, "diwali") == 0)    return "HAPPY DIWALI";
  if (strcmp(key, "holi") == 0)      return "HAPPY HOLI";
  if (strcmp(key, "navratri") == 0)  return "HAPPY NAVRATRI";
  if (strcmp(key, "christmas") == 0) return "MERRY CHRISTMAS";
  if (strcmp(key, "new_year") == 0)  return "HAPPY NEW YEAR";
  return nullptr;
}

const char *festivalDisplayName(const char *key) {
  if (strcmp(key, "diwali") == 0)           return "Diwali";
  if (strcmp(key, "holi") == 0)             return "Holi";
  if (strcmp(key, "ganesh_chaturthi") == 0 ||
      strcmp(key, "ganesh") == 0)           return "Ganesh Chaturthi";
  if (strcmp(key, "rakhi") == 0 ||
      strcmp(key, "raksha_bandhan") == 0)   return "Rakhi";
  if (strcmp(key, "janmashtami") == 0 ||
      strcmp(key, "krishna_janmashtami") == 0) return "Janmashtami";
  if (strcmp(key, "navratri") == 0)         return "Navratri";
  return nullptr;
}

// Fixed-date festivals are never in festivals.txt (the firmware handles them
// directly), so the countdown would go silent for Christmas and New Year
// without this table. Seven entries, stored in flash, read once a day.
// Gregorian weekday for a YYYYMMDD, 0 = Sunday. 1970-01-01 was a Thursday and
// daysFromYmd() returns 0 for it, hence the +4 offset. Reuses the same civil
// algorithm, so no time.h call and no struct tm.
int weekdayFromYmd(int ymd) {
  long d = daysFromYmd(ymd);
  return (int)(((d % 7L) + 11L) % 7L);
}

// YYYYMMDD of the nth given weekday in a month -- e.g. the 2nd Sunday of May.
// Mother's Day and Father's Day float, so they cannot live in the fixed table
// below; without this they would be the only themed festivals with no
// countdown, which is exactly the kind of gap that reads as broken.
int nthWeekdayYmd(int year, int mon, int weekday, int nth) {
  int firstWd = weekdayFromYmd(year * 10000 + mon * 100 + 1);
  int day = 1 + ((weekday - firstWd + 7) % 7) + (nth - 1) * 7;
  return year * 10000 + mon * 100 + day;
}

struct FixedFestival { uint8_t mon; uint8_t day; uint8_t rank; const char *name; };
const FixedFestival FIXED_FESTIVALS[] = {
  {  1,  1, 1, "New Year"      },
  {  1, 26, 2, "Republic Day"  },
  {  2, 14, 2, "Valentine's"   },
  {  8, 15, 2, "Independence"  },
  { 10, 31, 2, "Halloween"     },
  { 12, 25, 1, "Christmas"     },
  { 12, 31, 2, "New Year's Eve"}
};

// Folds the fixed table into whatever the config scan already found, keeping
// whichever lands sooner. Handles the year wrap (Dec 28 -> New Year).
// Records a candidate only if it lands inside the window and sooner than
// whatever the config scan already found. One place to change the rule.
static void considerFestivalCandidate(long todayAbs, int ymd, const char *name, uint8_t rank) {
  long delta = daysFromYmd(ymd) - todayAbs;

  // Majors get the full run-up; minors only appear in the last five days,
  // so a small day can never crowd out the week before Diwali.
  int window = (rank == 1) ? FESTIVAL_LOOKAHEAD_DAYS : FESTIVAL_MINOR_LOOKAHEAD_DAYS;
  if (delta <= 0 || delta > window) return;
  if (nextFestivalDaysAway >= 0 && delta >= nextFestivalDaysAway) return;

  nextFestivalRank = rank;
  nextFestivalDaysAway = (int)delta;
  strncpy(nextFestivalName, name, sizeof(nextFestivalName) - 1);
  nextFestivalName[sizeof(nextFestivalName) - 1] = '\0';
}

void mergeFixedFestivalCountdown(int todayYmd) {
  long todayAbs = daysFromYmd(todayYmd);
  int year = todayYmd / 10000;

  // bump = 1 handles the year wrap, so Dec 28 still finds New Year.
  for (int bump = 0; bump <= 1; bump++) {
    int y = year + bump;

    for (size_t i = 0; i < sizeof(FIXED_FESTIVALS) / sizeof(FIXED_FESTIVALS[0]); i++) {
      const FixedFestival &f = FIXED_FESTIVALS[i];
      considerFestivalCandidate(todayAbs, y * 10000 + f.mon * 100 + f.day, f.name, f.rank);
    }

    // Floating: 2nd Sunday of May, 3rd Sunday of June. Same keys the theme
    // engine uses at lines 1567-1570, so the countdown and the background
    // can never disagree about which day it is.
    considerFestivalCandidate(todayAbs, nthWeekdayYmd(y, 5, 0, 2), "Mother's Day", 2);
    considerFestivalCandidate(todayAbs, nthWeekdayYmd(y, 6, 0, 3), "Father's Day", 2);
  }
}

bool ymdInRange(int today, int startYmd, int endYmd) {
  if (startYmd == 0 || endYmd == 0) return false;

  // Normal same-year or forward range.
  if (startYmd <= endYmd) {
    return today >= startYmd && today <= endYmd;
  }

  // Range crossing year boundary, e.g. Dec 31 to Jan 01.
  return today >= startYmd || today <= endYmd;
}

bool isNthWeekdayOfMonth(const struct tm &t, int month, int weekday, int nth) {
  if ((t.tm_mon + 1) != month) return false;
  if (t.tm_wday != weekday) return false;

  int occurrence = ((t.tm_mday - 1) / 7) + 1;
  return occurrence == nth;
}

void copyPath(char *dst, size_t len, const char *src) {
  strncpy(dst, src, len);
  dst[len - 1] = '\0';
}

ClockPillMode clockModeForTheme(const char *key, bool nightMode) {
  if (strcmp(key, "holi") == 0) {
    return nightMode ? CLOCK_PILL_BOTTOM_RIGHT : CLOCK_PILL_TOP_RIGHT;
  }

  if (strcmp(key, "ganesh_chaturthi") == 0 || strcmp(key, "ganesh") == 0) {
    return nightMode ? CLOCK_PILL_HIDDEN : CLOCK_PILL_TOP_RIGHT;
  }

  if (strcmp(key, "christmas") == 0) {
    return nightMode ? CLOCK_PILL_HIDDEN : CLOCK_PILL_BOTTOM_RIGHT;
  }

  if (strcmp(key, "rakhi") == 0 || strcmp(key, "janmashtami") == 0 ||
      strcmp(key, "navratri") == 0) {
    return nightMode ? CLOCK_PILL_BOTTOM_RIGHT : CLOCK_PILL_TOP_RIGHT;
  }

  if (strcmp(key, "halloween") == 0 ||
      strcmp(key, "valentines") == 0 ||
      strcmp(key, "valentine") == 0 ||
      strcmp(key, "mothers_day") == 0 ||
      strcmp(key, "fathers_day") == 0) {
    return CLOCK_PILL_HIDDEN;
  }

  return CLOCK_PILL_BOTTOM_RIGHT;
}

void setActiveThemeProfile(const char *key, bool fromConfig) {
  activeThemeFromConfig = fromConfig;
  activeFestivalTheme = strcmp(key, "normal") != 0 && strcmp(key, "winter") != 0;

  copyPath(activeThemeKey, sizeof(activeThemeKey), key);

  if (strcmp(key, "normal") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Normal");
    copyPath(activeLightPath, sizeof(activeLightPath), "/bg/day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/bg/night.rgb565");
  } else if (strcmp(key, "winter") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Winter");
    copyPath(activeLightPath, sizeof(activeLightPath), "/bg/day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/bg/winter_night.rgb565");
  } else if (strcmp(key, "diwali") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Diwali");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/diwali_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/diwali_night.rgb565");
  } else if (strcmp(key, "holi") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Holi");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/holi_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/holi_night.rgb565");
  } else if (strcmp(key, "christmas") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Christmas");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/christmas_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/christmas_night.rgb565");
  } else if (strcmp(key, "ganesh_chaturthi") == 0 || strcmp(key, "ganesh") == 0) {
    copyPath(activeThemeKey, sizeof(activeThemeKey), "ganesh_chaturthi");
    copyPath(activeThemeName, sizeof(activeThemeName), "Ganesh Chaturthi");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/ganesh_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/ganesh_night.rgb565");
  } else if (strcmp(key, "rakhi") == 0 || strcmp(key, "raksha_bandhan") == 0) {
    copyPath(activeThemeKey, sizeof(activeThemeKey), "rakhi");
    copyPath(activeThemeName, sizeof(activeThemeName), "Raksha Bandhan");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/rakhi_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/rakhi_night.rgb565");
  } else if (strcmp(key, "janmashtami") == 0 || strcmp(key, "krishna_janmashtami") == 0) {
    copyPath(activeThemeKey, sizeof(activeThemeKey), "janmashtami");
    copyPath(activeThemeName, sizeof(activeThemeName), "Janmashtami");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/janmashtami_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/janmashtami_night.rgb565");
  } else if (strcmp(key, "navratri") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Navratri");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/navratri_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/navratri_night.rgb565");
  } else if (strcmp(key, "halloween") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Halloween");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/halloween_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/halloween_night.rgb565");
  } else if (strcmp(key, "republic") == 0 || strcmp(key, "independence") == 0) {
    copyPath(activeThemeKey, sizeof(activeThemeKey), "republic");
    copyPath(activeThemeName, sizeof(activeThemeName), "Republic / Independence");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/republic_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/republic_night.rgb565");
  } else if (strcmp(key, "valentines") == 0 || strcmp(key, "valentine") == 0) {
    copyPath(activeThemeKey, sizeof(activeThemeKey), "valentines");
    copyPath(activeThemeName, sizeof(activeThemeName), "Valentines");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/valentines_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/valentines_night.rgb565");
  } else if (strcmp(key, "mothers_day") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Mothers Day");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/mothers_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/mothers_night.rgb565");
  } else if (strcmp(key, "fathers_day") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "Fathers Day");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/fathers_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/fathers_night.rgb565");
  } else if (strcmp(key, "new_year") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "New Year");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/new_year_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/new_year_night.rgb565");
  } else if (strcmp(key, "new_year_eve") == 0) {
    copyPath(activeThemeName, sizeof(activeThemeName), "New Year Eve");
    copyPath(activeLightPath, sizeof(activeLightPath), "/festival/new_year_eve_day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/festival/new_year_night.rgb565");
  } else {
    // Unknown config key: safely fall back to normal theme.
    copyPath(activeThemeKey, sizeof(activeThemeKey), "normal");
    copyPath(activeThemeName, sizeof(activeThemeName), "Normal");
    copyPath(activeLightPath, sizeof(activeLightPath), "/bg/day.rgb565");
    copyPath(activeDarkPath, sizeof(activeDarkPath), "/bg/night.rgb565");
    activeFestivalTheme = false;
    activeThemeFromConfig = false;
  }

  activeLightClockMode = clockModeForTheme(activeThemeKey, false);
  activeDarkClockMode = clockModeForTheme(activeThemeKey, true);
}

bool readFestivalConfigForToday(int todayYmd) {
  // Countdown is recomputed from scratch on every pass so it can never go
  // stale or survive a festivals.txt that no longer lists the festival.
  nextFestivalName[0] = '\0';
  nextFestivalDaysAway = -1;
  nextFestivalRank = 2;

  if (!sdMounted) return false;
  if (!SD.exists("/config/festivals.txt")) return false;

  File f = SD.open("/config/festivals.txt", FILE_READ);
  if (!f) return false;

  long todayAbs = daysFromYmd(todayYmd);
  bool matched = false;
  char matchedKey[28] = {0};

  char line[128];

  while (f.available()) {
    int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[len] = '\0';

    // Trim CR/LF/spaces.
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
      line[--len] = '\0';
    }

    if (len == 0 || line[0] == '#') continue;

    char key[28] = {0};
    char startText[12] = {0};
    char endText[12] = {0};

    if (sscanf(line, "%27[^,],%11[^,],%11[^,]", key, startText, endText) == 3) {
      int startYmd = parseYmd(startText);
      int endYmd = parseYmd(endText);

      // First match still wins for today's theme, but the scan continues so
      // the nearest upcoming festival is found in the same read. The file is
      // ~4 KB and this runs once a day, so the extra lines are free.
      if (!matched && ymdInRange(todayYmd, startYmd, endYmd)) {
        matched = true;
        strncpy(matchedKey, key, sizeof(matchedKey) - 1);
        continue;
      }

      long delta = daysFromYmd(startYmd) - todayAbs;
      uint8_t rank = festivalRankForKey(key);
      int window = (rank == 1) ? FESTIVAL_LOOKAHEAD_DAYS : FESTIVAL_MINOR_LOOKAHEAD_DAYS;
      if (delta <= 0 || delta > window) continue;
      if (nextFestivalDaysAway >= 0 && delta >= nextFestivalDaysAway) continue;

      const char *label = festivalDisplayName(key);
      if (label == nullptr) continue;

      nextFestivalRank = rank;
      nextFestivalDaysAway = (int)delta;
      strncpy(nextFestivalName, label, sizeof(nextFestivalName) - 1);
      nextFestivalName[sizeof(nextFestivalName) - 1] = '\0';
    }
  }

  f.close();

  if (matched) {
    // Mid-festival: the background already says it. A countdown would be noise.
    nextFestivalName[0] = '\0';
    nextFestivalDaysAway = -1;
    setActiveThemeProfile(matchedKey, true);
    return true;
  }

  return false;
}

void updateActiveThemeForToday(bool forceConfigReload = false) {
  if (!isTimeReadyFast()) {
    setActiveThemeProfile("normal", false);
    return;
  }

  struct tm t;
  getLocalTmFast(&t);

  int todayYmd = ymdFromTm(t);

  if (forceConfigReload || millis() - lastFestivalConfigCheckAt > 600000UL) {
    lastFestivalConfigCheckAt = millis();

    if (readFestivalConfigForToday(todayYmd)) {
      return;
    }
    mergeFixedFestivalCountdown(todayYmd);
  } else if (activeThemeFromConfig) {
    // Keep current SD-config festival until next scheduled config/date check.
    return;
  }

  int month = t.tm_mon + 1;
  int day = t.tm_mday;

  if (month == 12 && day == 31) {
    setActiveThemeProfile("new_year_eve", false);
  } else if (month == 1 && day == 1) {
    setActiveThemeProfile("new_year", false);
  } else if (month == 12 && day == 25) {
    setActiveThemeProfile("christmas", false);
  } else if (month == 10 && day == 31) {
    setActiveThemeProfile("halloween", false);
  } else if (month == 2 && day == 14) {
    setActiveThemeProfile("valentines", false);
  } else if ((month == 1 && day == 26) || (month == 8 && day == 15)) {
    setActiveThemeProfile("republic", false);
  } else if (isNthWeekdayOfMonth(t, 5, 0, 2)) {
    setActiveThemeProfile("mothers_day", false);
  } else if (isNthWeekdayOfMonth(t, 6, 0, 3)) {
    setActiveThemeProfile("fathers_day", false);
  } else if ((month == 12 && day >= 15) || (month == 1 && day <= 31)) {
    setActiveThemeProfile("winter", false);
  } else {
    setActiveThemeProfile("normal", false);
  }
}

void applyClockPillPlacement() {
  if (!clockPill) return;

  ClockPillMode mode = darkThemeActive ? activeDarkClockMode : activeLightClockMode;

  if (mode == CLOCK_PILL_HIDDEN) {
    lv_obj_add_flag(clockPill, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_clear_flag(clockPill, LV_OBJ_FLAG_HIDDEN);

  if (mode == CLOCK_PILL_TOP_RIGHT) {
    lv_obj_set_pos(clockPill, 657, 24);
  } else {
    lv_obj_set_pos(clockPill, 657, 417);
  }
}

bool isNightNow() {
  time_t now = time(nullptr);

  if (!isTimeReadyFast()) {
    // Before NTP, start with day mode. Theme corrects itself after time/weather sync.
    return false;
  }

  // Best logic: use OpenWeather sunrise/sunset.
  if (weatherHasSunTimes && sunriseEpoch > 0 && sunsetEpoch > 0) {
    return (now < sunriseEpoch || now >= sunsetEpoch);
  }

  // Fallback if weather is unavailable.
  struct tm timeinfo;
  getLocalTmFast(&timeinfo);

  int hour = timeinfo.tm_hour;
  return (hour >= 19 || hour < 6);
}

void updateClock() {
  if (!clockLabel) return;

  static unsigned long lastUpdate = 0;

  if (millis() - lastUpdate < CLOCK_UPDATE_MS) {
    return;
  }

  lastUpdate = millis();

  if (!isTimeReadyFast()) {
    lv_label_set_text(clockLabel, "--:--");
    if (clockAmPmLabel) lv_label_set_text(clockAmPmLabel, "");
    return;
  }

  timeReady = true;

  struct tm timeinfo;
  getLocalTmFast(&timeinfo);

  int hour = timeinfo.tm_hour;
  int minute = timeinfo.tm_min;

  const char* ampm = "AM";

  if (hour >= 12) {
    ampm = "PM";
    if (hour > 12) hour -= 12;
  } else if (hour == 0) {
    hour = 12;
  }

  char timeBuffer[8];
  snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d", hour, minute);
  lv_label_set_text(clockLabel, timeBuffer);

  if (clockAmPmLabel) {
    lv_label_set_text(clockAmPmLabel, ampm);
  }
}

void updateDate() {
  if (!dateLabel) return;

  if (!isTimeReadyFast()) {
    lv_label_set_text(dateLabel, "Date loading...");
    return;
  }

  struct tm timeinfo;
  getLocalTmFast(&timeinfo);

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%d %b, %a", &timeinfo);
  lv_label_set_text(dateLabel, buffer);

  initialDateSet = true;

  // Date changes can start/end festival or winter themes.
  updateActiveThemeForToday(true);

  // Update just after midnight.
  nextDateUpdateEpoch = makeLocalMidnightPlusSeconds(2);
}

void updateDateScheduler() {
  static unsigned long lastCheckMs = 0;

  unsigned long interval = initialDateSet ? 10000UL : 1000UL;

  if (millis() - lastCheckMs < interval) {
    return;
  }

  lastCheckMs = millis();

  if (!isTimeReadyFast()) {
    return;
  }

  if (!initialDateSet) {
    updateDate();
    applyTheme(true);
    return;
  }

  time_t now = time(nullptr);

  if (nextDateUpdateEpoch == 0) {
    nextDateUpdateEpoch = makeLocalMidnightPlusSeconds(2);
    return;
  }

  if (now >= nextDateUpdateEpoch) {
    updateDate();
    applyTheme(true);
  }
}

void scheduleNextThemeEvent() {
  time_t now = time(nullptr);

  if (!isTimeReadyFast()) {
    nextThemeEventEpoch = 0;
    return;
  }

  if (weatherHasSunTimes && sunriseEpoch > 0 && sunsetEpoch > 0) {
    if (now < sunriseEpoch) {
      nextThemeEventEpoch = sunriseEpoch;
    } else if (now < sunsetEpoch) {
      nextThemeEventEpoch = sunsetEpoch;
    } else {
      // After today's sunset. Wait for next weather update to refresh tomorrow's sunrise.
      // Safe fallback: check after 1 hour.
      nextThemeEventEpoch = now + 3600;
    }
  } else {
    // Fallback schedule if weather sunrise/sunset unavailable.
    struct tm t;
    getLocalTmFast(&t);

    t.tm_min = 0;
    t.tm_sec = 0;

    if (t.tm_hour < 6) {
      t.tm_hour = 6;
      nextThemeEventEpoch = mktime(&t);
    } else if (t.tm_hour < 19) {
      t.tm_hour = 19;
      nextThemeEventEpoch = mktime(&t);
    } else {
      t.tm_mday += 1;
      t.tm_hour = 6;
      nextThemeEventEpoch = mktime(&t);
    }
  }
}

void resetBootSequence() {
  bootNetStage = BOOT_NET_WIFI;
  bootStageStartedAt = millis();
  lastBootWiFiRetryAt = millis();
  bootWiFiRetryIndex = 0;
  bootWeatherAttempted = false;
}

void applyTimeReadyUpdatesOnce() {
  if (!isTimeReadyFast()) return;

  timeReady = true;

  updateClock();
  updateDate();
  applyTheme(true);
  scheduleNextThemeEvent();

  initialThemeSet = true;
  timeReadyUpdatesApplied = true;
  forceWeatherFetchAfterNtp = true;
}

void updateNtpMaintenance() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  // If time is not ready, recovery logic handles fast retry.
  if (!isTimeReadyFast()) {
    return;
  }

  // Once time is valid, only refresh occasionally.
  if (lastNtpPeriodicResyncAt == 0 ||
      millis() - lastNtpPeriodicResyncAt >= NTP_PERIODIC_RESYNC_MS) {
    lastNtpPeriodicResyncAt = millis();
    initTime();
  }
}

void updateTimeRecovery() {
  static unsigned long lastNtpRetryAt = 0;

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (isTimeReadyFast()) {
    if (!timeReadyUpdatesApplied || !initialThemeSet || !initialDateSet) {
      applyTimeReadyUpdatesOnce();

      // Start weather only after NTP success.
      updateWeather();
    }
    return;
  }

  if (millis() - lastNtpRetryAt >= NTP_RECOVERY_RETRY_MS) {
    lastNtpRetryAt = millis();
    initTime();
  }
}

void updateBootSequence() {
  switch (bootNetStage) {
    case BOOT_NET_WIFI: {
      wl_status_t status = WiFi.status();

      if (status == WL_CONNECTED) {
        initTime();
        bootNetStage = BOOT_NET_NTP;
        bootStageStartedAt = millis();
        return;
      }

      unsigned long retryDelay = getWiFiRetryDelay(bootWiFiRetryIndex);

      if (millis() - lastBootWiFiRetryAt >= retryDelay) {
        lastBootWiFiRetryAt = millis();
        // Important: early retries do not call WiFi.disconnect().
        // If the ESP32 is still negotiating, we leave it alone so we do not
        // interrupt an almost-complete connection.
        if (bootWiFiRetryIndex < WIFI_RETRY_DELAY_COUNT - 1) {
          if (wifiStatusDefinitelyFailed(status)) {
            softWiFiRetry();
          } else {
          }

          bootWiFiRetryIndex++;
        } else {
          hardWiFiRetry();
        }
      }

      return;
    }

    case BOOT_NET_NTP:
      if (isTimeReadyFast()) {
        bootNetStage = BOOT_NET_APPLY_THEME;
        bootStageStartedAt = millis();
        return;
      }

      if (millis() - bootStageStartedAt > BOOT_NTP_TIMEOUT_MS) {
        bootNetStage = BOOT_NET_DONE;
        bootStageStartedAt = millis();
      }
      return;

    case BOOT_NET_APPLY_THEME:
      applyTimeReadyUpdatesOnce();
      bootNetStage = BOOT_NET_WEATHER;
      bootStageStartedAt = millis();
      return;

    case BOOT_NET_WEATHER:
      if (WiFi.status() == WL_CONNECTED && isTimeReadyFast() && !bootWeatherAttempted) {
        bootWeatherAttempted = true;
        updateWeather();

        bootNetStage = BOOT_NET_DONE;
        bootStageStartedAt = millis();
        return;
      }

      if (millis() - bootStageStartedAt > BOOT_WEATHER_TIMEOUT_MS) {
        bootNetStage = BOOT_NET_DONE;
        bootStageStartedAt = millis();
      }
      return;

    case BOOT_NET_DONE:
    default:
      return;
  }
}

// Auto-detects the unit's location from its public IP address -- no city
// name, no manual entry, works identically for every customer anywhere in
// the world. Called once per boot, right before the first weather fetch.
// Result is cached in NVS so weather has an immediate value on the next
// boot even before this fetch completes again.
bool fetchIpGeolocation() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  http.setTimeout(4000);
  http.begin("http://ip-api.com/json/?fields=lat,lon,status");

  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) {
    return false;
  }
  const char* statusStr = doc["status"] | "";
  if (strcmp(statusStr, "success") != 0) {
    return false;
  }

  double lat = doc["lat"] | 0.0;
  double lon = doc["lon"] | 0.0;
  if (lat == 0.0 && lon == 0.0) {
    return false;
  }

  saveGeoLocation(String(lat, 6), String(lon, 6));
  return true;
}

// =====================================================
// Home-screen weather ticker
// =====================================================
// The temperature is and stays the primary reading. When there is genuinely
// something worth saying -- rain expected, a storm, fog, an unusually clear
// evening -- the label cross-fades to one short insight line for a few
// seconds, then fades home to the temperature. When there is nothing to
// say, nothing ever moves. Runs only on the main screen, never during the
// Welcome overlay, and costs two 300ms label fades per minute at most.

enum WeatherTickerState : uint8_t {
  TICKER_TEMP,          // temperature showing (resting state)
  TICKER_FADING,        // mid cross-fade (out leg running)
  TICKER_INSIGHT        // insight showing, waiting to fade home
};

static WeatherTickerState weatherTickerState = TICKER_TEMP;
static unsigned long weatherTickerLastInsightAt = 0;  // last time an insight finished
static unsigned long weatherTickerInsightShownAt = 0;
static char weatherTickerPendingText[40] = "";

const unsigned long WEATHER_TICKER_PERIOD_MS = 60000UL; // rest between insights
const unsigned long WEATHER_TICKER_FEST_PERIOD_MS = 30000UL; // ...halved while a festival line is live
const unsigned long WEATHER_TICKER_HOLD_MS   = 7000UL;  // insight on-screen time
const unsigned long WEATHER_TICKER_FEST_HOLD_MS = 8000UL; // festival lines linger a beat longer
const unsigned long WEATHER_TICKER_WAKE_DELAY_MS = 3000UL; // after the screen lights up
const uint32_t      WEATHER_TICKER_FADE_MS   = 600;  // unhurried: premium reads as slow

// True when the line currently being shown is a festival one. Only affects
// how long it holds -- no extra work, no extra timer.
static bool weatherTickerLineIsFestival = false;

// The one place the festival line is worded. On the day itself the panel
// greets instead of counting down; before it, it counts down.
static bool buildFestivalLine(char *out, size_t n) {
  if (activeFestivalTheme) {
    const char *greet = festivalDayGreeting(activeThemeKey);
    if (greet) { snprintf(out, n, "%s", greet); return true; }
    return false;
  }
  if (nextFestivalDaysAway <= 0 || nextFestivalName[0] == '\0') return false;
  if (nextFestivalDaysAway == 1) snprintf(out, n, "%s tomorrow", nextFestivalName);
  else                           snprintf(out, n, "%s in %d days", nextFestivalName, nextFestivalDaysAway);
  return true;
}

// Builds the single best insight line for right now. Returns false when
// there is nothing worth interrupting the temperature for.
static bool buildWeatherInsight(char *out, size_t n) {
  time_t now = time(nullptr);

  // A festival is coming and something is falling out of the sky. Both are
  // worth saying, so alternate rather than letting either win permanently --
  // one bool, and the ticker's existing 60s rest does the pacing.
  static bool preferFestival = false;
  char festLine[40];
  bool haveFestival = buildFestivalLine(festLine, sizeof(festLine));

  // The last stretch before a major festival (and the day itself) is the one
  // time the panel stops taking turns: that line wins every insight.
  bool festivalOutranksWeather =
      haveFestival && (activeFestivalTheme ||
                       (nextFestivalRank == 1 && nextFestivalDaysAway <= 3));

  if (haveFestival && preferFestival) {
    preferFestival = false;
    weatherTickerLineIsFestival = true;
    snprintf(out, n, "%s", festLine);
    return true;
  }
  preferFestival = haveFestival;
  weatherTickerLineIsFestival = false;

  // 1. Rain actually falling now outranks any forecast. Measured millimetres
  //    decide the wording, so a drizzle and a downpour do not read the same.
  if ((wmoCode >= 95 && wmoCode <= 99)) { snprintf(out, n, "Storm outside"); return true; }

  if ((wmoCode >= 51 && wmoCode <= 67) ||
      (wmoCode >= 80 && wmoCode <= 82) || currentPrecipMm >= 0.1f) {
    if (currentPrecipMm >= 4.0f)      snprintf(out, n, "Heavy rain, stay in");
    else if (currentPrecipMm >= 1.0f) snprintf(out, n, "Raining, take an umbrella");
    else                              snprintf(out, n, "Drizzling outside");
    return true;
  }

  // 1b. The festival day itself, and the last three days before a major one,
  //     outrank every remaining line -- but never the two above, because
  //     weather you can walk into still matters more than a countdown.
  if (festivalOutranksWeather) {
    preferFestival = false;
    weatherTickerLineIsFestival = true;
    snprintf(out, n, "%s", festLine);
    return true;
  }

  // 2. Rain on the way -- now phrased as advice, not as trivia.
  if (rainExpectedEpoch > now) {
    struct tm rt;
    time_t re = rainExpectedEpoch;
    localtime_r(&re, &rt);
    int h12 = rt.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    long mins = (long)(rainExpectedEpoch - now) / 60L;
    if (mins <= 90) snprintf(out, n, "Rain soon, carry an umbrella");
    else            snprintf(out, n, "Rain around %d %s", h12, rt.tm_hour < 12 ? "AM" : "PM");
    return true;
  }

  // 3. Other conditions happening right now.
  if ((wmoCode >= 71 && wmoCode <= 77) ||
      wmoCode == 85 || wmoCode == 86)  { snprintf(out, n, "Snow outside");   return true; }
  if (wmoCode == 45 || wmoCode == 48)  { snprintf(out, n, "Foggy outside");  return true; }
  if (wmoCode == 999)                  { snprintf(out, n, "Windy outside");  return true; }

  // 4. Festival with nothing weather-worthy to compete against.
  if (haveFestival) {
    preferFestival = false;
    weatherTickerLineIsFestival = true;
    snprintf(out, n, "%s", festLine);
    return true;
  }

  // 5. Calm conditions -- still worth a word now and then.
  if (wmoCode <= 1) {
    snprintf(out, n, isNightNow() ? "Moonlit night" : "Clear skies today");
    return true;
  }
  if (wmoCode == 2 || wmoCode == 3) { snprintf(out, n, "Cloudy for now"); return true; }
  return false;
}

static void weatherTickerOpaCb(void *obj, int32_t v) {
  lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void weatherTickerTransCb(void *obj, int32_t v) {
  lv_obj_set_style_translate_y((lv_obj_t *)obj, (lv_coord_t)v, 0);
}

// Incoming text fades in while rising 8px into place -- the same motion
// language phone weather/notification tickers use. Two property tweens on
// one label, ~340ms, then everything is static again: zero ongoing cost.
static void weatherTickerStartFadeIn() {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, weatherLabel);
  lv_anim_set_exec_cb(&a, weatherTickerOpaCb);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_time(&a, WEATHER_TICKER_FADE_MS + 40);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  lv_anim_t t;
  lv_anim_init(&t);
  lv_anim_set_var(&t, weatherLabel);
  lv_anim_set_exec_cb(&t, weatherTickerTransCb);
  lv_anim_set_values(&t, 8, 0);
  lv_anim_set_time(&t, WEATHER_TICKER_FADE_MS + 40);
  lv_anim_set_path_cb(&t, lv_anim_path_ease_out);
  lv_anim_start(&t);
}

// Fade-out finished: swap the text, fade back in, advance the state.
static void weatherTickerSwapCb(lv_anim_t *a) {
  (void)a;
  if (!weatherLabel) return;
  lv_label_set_text(weatherLabel, weatherTickerPendingText);
  weatherTickerStartFadeIn();

  if (weatherTickerState == TICKER_FADING) {
    // Which way was this swap? If the pending text is the temperature we
    // are heading home; otherwise the insight just landed.
    if (strstr(weatherTickerPendingText, "°C") != nullptr) {
      weatherTickerState = TICKER_TEMP;
      weatherTickerLastInsightAt = millis();
    } else {
      weatherTickerState = TICKER_INSIGHT;
      weatherTickerInsightShownAt = millis();
    }
  }
}

static void weatherTickerStartSwap(const char *newText) {
  strncpy(weatherTickerPendingText, newText, sizeof(weatherTickerPendingText) - 1);
  weatherTickerPendingText[sizeof(weatherTickerPendingText) - 1] = '\0';
  weatherTickerState = TICKER_FADING;

  // Outgoing text fades while drifting 6px upward, then the swap fires.
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, weatherLabel);
  lv_anim_set_exec_cb(&a, weatherTickerOpaCb);
  lv_anim_set_values(&a, 255, 0);
  lv_anim_set_time(&a, WEATHER_TICKER_FADE_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, weatherTickerSwapCb);
  lv_anim_start(&a);

  lv_anim_t t;
  lv_anim_init(&t);
  lv_anim_set_var(&t, weatherLabel);
  lv_anim_set_exec_cb(&t, weatherTickerTransCb);
  lv_anim_set_values(&t, 0, -6);
  lv_anim_set_time(&t, WEATHER_TICKER_FADE_MS);
  lv_anim_set_path_cb(&t, lv_anim_path_ease_in);
  lv_anim_start(&t);
}

// Hard reset to the resting state. Called whenever something else rewrites
// the weather label (fresh fetch, cached restore, placeholder) so the
// ticker can never fight another writer or get stuck mid-fade.
static void weatherTickerReset() {
  if (weatherLabel) {
    lv_anim_del(weatherLabel, weatherTickerOpaCb);
    lv_anim_del(weatherLabel, weatherTickerTransCb);
    lv_obj_set_style_text_opa(weatherLabel, 255, 0);
    lv_obj_set_style_translate_y(weatherLabel, 0, 0);
  }
  weatherTickerState = TICKER_TEMP;
  weatherTickerLastInsightAt = millis(); // full rest period before next insight
}

// Called from loop().
static void updateWeatherTicker() {
  if (!weatherLabel || !isTimeReadyFast()) return;
  if (lastWeatherUpdatedAt == 0) return;          // no live weather yet
  if (lv_scr_act() != mainScreen) return;         // home screen only
  if (welcomeOverlayActive) return;               // never compete with the greeting

  unsigned long now = millis();

  // The screen just lit up. Re-phase the rest timer so the next insight
  // lands ~3s from now instead of at a random point in a free-running
  // cycle -- the reason the countdown was so rarely caught. Costs one
  // subtraction on wake, nothing per frame.
  if (tickerWakeSyncPending) {
    tickerWakeSyncPending = false;
    if (weatherTickerState == TICKER_TEMP) {
      weatherTickerLastInsightAt = now - (WEATHER_TICKER_PERIOD_MS - WEATHER_TICKER_WAKE_DELAY_MS);
    }
  }

  bool festivalLive = activeFestivalTheme ||
                      (nextFestivalDaysAway > 0 && nextFestivalName[0] != '\0');
  unsigned long period = festivalLive ? WEATHER_TICKER_FEST_PERIOD_MS : WEATHER_TICKER_PERIOD_MS;

  if (weatherTickerState == TICKER_TEMP) {
    if (now - weatherTickerLastInsightAt < period) return;
    char insight[40];
    if (!buildWeatherInsight(insight, sizeof(insight))) return;
    weatherTickerStartSwap(insight);
    return;
  }

  unsigned long hold = weatherTickerLineIsFestival ? WEATHER_TICKER_FEST_HOLD_MS
                                                   : WEATHER_TICKER_HOLD_MS;
  if (weatherTickerState == TICKER_INSIGHT &&
      now - weatherTickerInsightShownAt >= hold) {
    char temp[20];
    snprintf(temp, sizeof(temp), "%d °C", currentWeatherTemp);
    weatherTickerStartSwap(temp);
  }
}

void updateWeather() {
  static unsigned long lastWeatherAttempt = 0;
  static bool firstWeatherUpdate = true;

  if (WiFi.status() != WL_CONNECTED) {
    // Keep cached/placeholder weather visible. Do not repeatedly blank it.
    return;
  }

  // Weather is intentionally gated behind NTP. If time is not valid, internet
  // is probably not usable yet, so avoid unnecessary HTTP polling.
  if (!isTimeReadyFast()) {
    return;
  }

  unsigned long retryInterval = firstWeatherUpdate ? WEATHER_FIRST_RETRY_MS : WEATHER_UPDATE_MS;

  if (!forceWeatherFetchAfterNtp && lastWeatherAttempt != 0 && millis() - lastWeatherAttempt < retryInterval) {
    return;
  }

  forceWeatherFetchAfterNtp = false;

  // Location is auto-detected. If we don't have one cached yet, fetch it
  // now (one-time per boot, cheap) before asking for weather.
  if (nvsGeoLat.length() == 0 || nvsGeoLon.length() == 0) {
    if (!fetchIpGeolocation()) {
      return; // no location yet -- try again on next scheduled attempt
    }
  }

  lastWeatherAttempt = millis();

  HTTPClient http;

  // Open-Meteo: free, no API key, global coverage, WMO weather codes.
  // current_weather gives us temperature + weathercode + windspeed in one call.
  // daily sunrise/sunset added so theme scheduling stays accurate.
  String url = "http://api.open-meteo.com/v1/forecast?latitude=";
  url += nvsGeoLat;
  url += "&longitude=";
  url += nvsGeoLon;
  // current=precipitation adds ground truth in millimetres. The WMO weathercode
  // is a model classification and routinely reports 0-3 (clear/cloudy) through
  // light drizzle; actual measured precipitation does not. One extra field.
  url += "&current_weather=true&current=precipitation&daily=sunrise,sunset&hourly=precipitation_probability&forecast_days=1&timeformat=unixtime&timezone=auto";
  if (strcmp(WEATHER_UNITS, "metric") == 0) {
    url += "&temperature_unit=celsius";
  } else {
    url += "&temperature_unit=fahrenheit";
  }

  http.setTimeout(4000);
  http.begin(url);

  int httpCode = http.GET();
  lastWeatherHttpCode = httpCode;

  if (httpCode == 200) {
    String payload = http.getString();

    // The hourly block adds ~24 timestamps + 24 probabilities to the payload,
    // so parse with a filter (only the keys we read survive) into a transient
    // heap document instead of growing a stack-resident StaticJsonDocument.
    StaticJsonDocument<192> filter;
    filter["current_weather"] = true;
    filter["current"] = true;
    filter["daily"] = true;
    filter["hourly"]["time"] = true;
    filter["hourly"]["precipitation_probability"] = true;

    SpiRamJsonDocument doc(6144);   // PSRAM: keeps the internal heap whole for TLS
    DeserializationError error =
        deserializeJson(doc, payload, DeserializationOption::Filter(filter));

    if (!error) {
      float temp    = doc["current_weather"]["temperature"] | 0.0f;
      int   code    = doc["current_weather"]["weathercode"] | 0;
      float wind    = doc["current_weather"]["windspeed"]   | 0.0f;

      // Use high-windspeed as a secondary icon trigger (WMO 75/77/95+ already
      // cover severe wind, but for a milder windy day the code is just 0/1/2).
      // Treat sustained wind >= 40 km/h as "wind" icon even on a clear code.
      if ((code == 0 || code == 1 || code == 2) && wind >= 40.0f) {
        code = 999; // internal sentinel: use wind icon
      }

      // Measured rain overrides an optimistic weathercode. Anything at or above
      // 0.1 mm in the current hour is wet enough to matter to someone about to
      // open the door, so promote the code into the drizzle/rain band.
      float precipNow = doc["current"]["precipitation"] | 0.0f;
      currentPrecipMm = precipNow;
      if (precipNow >= 0.1f && !(code >= 51 && code <= 67) &&
          !(code >= 80 && code <= 82) && !(code >= 95 && code <= 99)) {
        code = (precipNow >= 2.5f) ? 63 : 61;
      }

      // Open-Meteo daily arrays — index 0 = today.
      sunriseEpoch = doc["daily"]["sunrise"][0] | 0L;
      sunsetEpoch  = doc["daily"]["sunset"][0]  | 0L;

      weatherHasSunTimes = (sunriseEpoch > 0 && sunsetEpoch > 0);

      // Rain outlook: first hourly slot in the next 6h at >= 50% probability.
      // Tighter window on purpose: "rain at 2 PM" shown at 2 AM is trivia,
      // not an alert. Recomputed from scratch on every fetch (incl. to "none").
      rainExpectedEpoch = 0;
      {
        JsonArrayConst ht = doc["hourly"]["time"].as<JsonArrayConst>();
        JsonArrayConst hp = doc["hourly"]["precipitation_probability"].as<JsonArrayConst>();
        if (!ht.isNull() && !hp.isNull()) {
          time_t nowT = time(nullptr);
          size_t cnt = ht.size() < hp.size() ? ht.size() : hp.size();
          for (size_t i = 0; i < cnt; i++) {
            long slot = ht[i] | 0L;
            int  prob = hp[i] | 0;
            if (slot <= (long)nowT) continue;
            if (slot > (long)nowT + 6L * 3600L) break;
            if (prob >= 50) {
              rainExpectedEpoch = (time_t)slot;
              break;
            }
          }
        }
      }

      if (weatherHasSunTimes) {
        scheduleNextThemeEvent();
        if (!initialThemeSet && isTimeReadyFast()) {
          applyTheme(true);
          initialThemeSet = true;
        }
      }

      // Build a short human-readable condition string from the WMO code.
      const char* condLabel = wmoCodeLabel(code);
      strncpy(weatherCondition, condLabel, sizeof(weatherCondition));
      weatherCondition[sizeof(weatherCondition) - 1] = '\0';
      wmoCode = code;

      currentWeatherTemp = (int)round(temp);
      char buffer[20];
      snprintf(buffer, sizeof(buffer), "%d °C", currentWeatherTemp);
      weatherTickerReset();                 // never fight the ticker mid-fade
      lv_label_set_text(weatherLabel, buffer);
      updateWeatherIconDisplay(true);

      lastWeatherUpdatedAt = millis();
      firstWeatherUpdate   = false;
      saveCachedWeather((int)round(temp), code, condLabel);
    }
  }

  http.end();
}

void loadCachedWeather() {
  prefs.begin("weather", true);

  cachedWeatherLoaded = prefs.getBool("valid", false);

  if (cachedWeatherLoaded) {
    int cachedTemp = prefs.getInt("temp", 0);
    currentWeatherTemp = cachedTemp;
    int cachedId = prefs.getInt("id", 0);   // 0 = WMO clear sky default
    String cachedCondition = prefs.getString("cond", "Clear");

    sunriseEpoch = prefs.getLong("sunrise", 0);
    sunsetEpoch = prefs.getLong("sunset", 0);
    weatherHasSunTimes = (sunriseEpoch > 0 && sunsetEpoch > 0);

    wmoCode = cachedId;
    // Cached weather is by definition not live -- never let a stale millimetre
    // reading survive a reboot and trigger a rain line under a clear sky.
    currentPrecipMm = 0.0f;
    strncpy(weatherCondition, cachedCondition.c_str(), sizeof(weatherCondition));
    weatherCondition[sizeof(weatherCondition) - 1] = '\0';

    if (weatherLabel) {
      char buffer[20];
      snprintf(buffer, sizeof(buffer), "%d °C", cachedTemp);
      weatherTickerReset();
      lv_label_set_text(weatherLabel, buffer);
    }
  }

  prefs.end();
}

void saveCachedWeather(int tempInt, int wmoCodeVal, const char *conditionText) {
  prefs.begin("weather", false);

  prefs.putBool("valid", true);
  prefs.putInt("temp", tempInt);
  prefs.putInt("id", wmoCodeVal);
  prefs.putString("cond", conditionText ? conditionText : "Clear");
  prefs.putLong("sunrise", (long)sunriseEpoch);
  prefs.putLong("sunset", (long)sunsetEpoch);

  prefs.end();

  cachedWeatherLoaded = true;
}

void applyCachedOrPlaceholderWeather() {
  if (!weatherLabel) return;

  loadCachedWeather();

  if (!cachedWeatherLoaded) {
    // First-ever boot or cache missing: use a clean placeholder.
    // Icon maps to clear, and applyTheme() will refresh day/night icon path.
    wmoCode = 0;   // WMO 0 = clear sky
    strncpy(weatherCondition, "Clear", sizeof(weatherCondition));
    weatherCondition[sizeof(weatherCondition) - 1] = '\0';
    weatherTickerReset();
    lv_label_set_text(weatherLabel, "-- °C");
  }

  updateWeatherIconDisplay(true);
}

// Short human-readable label for a WMO weather code.
// Used for the weather condition string shown on the diagnostics screen
// and stored in NVS cache.  999 is the internal "windy" sentinel.
const char* wmoCodeLabel(int code) {
  if (code == 999)                          return "Windy";
  if (code == 0)                            return "Clear";
  if (code == 1)                            return "Mostly clear";
  if (code == 2)                            return "Partly cloudy";
  if (code == 3)                            return "Overcast";
  if (code >= 45 && code <= 48)             return "Foggy";
  if (code >= 51 && code <= 57)             return "Drizzle";
  if (code >= 61 && code <= 67)             return "Rain";
  if (code >= 71 && code <= 77)             return "Snow";
  if (code >= 80 && code <= 82)             return "Rain showers";
  if (code == 85 || code == 86)             return "Snow showers";
  if (code >= 95 && code <= 99)             return "Thunderstorm";
  return "Clear";
}

// Map WMO code directly to the suffix used in your SD card icon filenames.
// SD card flat structure: /icons/day_<suffix>.rgb565 and /icons/night_<suffix>.rgb565
// Available suffixes (from your SD card): clear, clouds, mist, partly_cloudy,
//   rain, snow, thunder, wind
const char* getWeatherIconName() {
  // Internal sentinel: calm day with high windspeed -> wind icon
  if (wmoCode == 999)                       return "wind";

  // WMO 0 = clear sky
  if (wmoCode == 0)                         return "clear";

  // WMO 1 = mainly clear, 2 = partly cloudy
  if (wmoCode == 1 || wmoCode == 2)         return "partly_cloudy";

  // WMO 3 = overcast
  if (wmoCode == 3)                         return "clouds";

  // WMO 45,48 = fog/rime fog
  if (wmoCode == 45 || wmoCode == 48)       return "mist";

  // WMO 51-57 = drizzle (light/moderate/freezing) -> rain icon
  if (wmoCode >= 51 && wmoCode <= 57)       return "rain";

  // WMO 61-67 = rain (light/moderate/heavy/freezing)
  if (wmoCode >= 61 && wmoCode <= 67)       return "rain";

  // WMO 71-77 = snow (light/moderate/heavy/snow grains)
  if (wmoCode >= 71 && wmoCode <= 77)       return "snow";

  // WMO 80-82 = rain showers (slight/moderate/violent)
  if (wmoCode >= 80 && wmoCode <= 82)       return "rain";

  // WMO 85-86 = snow showers
  if (wmoCode == 85 || wmoCode == 86)       return "snow";

  // WMO 95-99 = thunderstorm (with/without hail)
  if (wmoCode >= 95 && wmoCode <= 99)       return "thunder";

  // Fallback for any unlisted code
  return "clear";
}

void getWeatherIconPath(char *buffer, size_t len) {
  const char *icon = getWeatherIconName();

  if (darkThemeActive) {
    snprintf(buffer, len, "/weather_night/%s.rgb565", icon);
  } else {
    snprintf(buffer, len, "/weather_day/%s.rgb565", icon);
  }
}

// =====================================================
// SD card listing / mount
// =====================================================

bool initSDCard() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
    sdMounted = false;
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    sdMounted = false;
    return false;
  }

  sdMounted = true;

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  sdDayBgAvailable = SD.exists("/bg/day.rgb565");
  sdNightBgAvailable = SD.exists("/bg/night.rgb565");
  sdWinterNightBgAvailable = SD.exists("/bg/winter_night.rgb565");

  return true;
}

// =====================================================
// Fast RGB565 icon loader
// =====================================================

lv_obj_t *createRGB565Icon(
  lv_obj_t *parent,
  const char *fsPath,
  int x,
  int y,
  uint16_t w,
  uint16_t h,
  lv_img_dsc_t *dsc,
  uint8_t **buffer,
  bool chromaKeyed = true
) {
  if (!sdMounted) {
    return nullptr;
  }

  if (!SD.exists(fsPath)) {
    return nullptr;
  }

  File file = SD.open(fsPath, FILE_READ);
  if (!file) {
    return nullptr;
  }

  const size_t expectedSize = (size_t)w * (size_t)h * 2;

  if (file.size() != expectedSize) {
    file.close();
    return nullptr;
  }

  if (*buffer != nullptr) {
    free(*buffer);
    *buffer = nullptr;
  }

  *buffer = (uint8_t *)ps_malloc(expectedSize);

  if (*buffer == nullptr) {
    file.close();
    return nullptr;
  }

  size_t bytesRead = file.read(*buffer, expectedSize);
  file.close();

  if (bytesRead != expectedSize) {
    free(*buffer);
    *buffer = nullptr;
    return nullptr;
  }

  dsc->header.always_zero = 0;
  dsc->header.w = w;
  dsc->header.h = h;
  // Button icons use chroma-key transparency.
  // Weather icons use normal RGB565.
  dsc->header.cf = chromaKeyed ? LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED : LV_IMG_CF_TRUE_COLOR;
  dsc->data_size = expectedSize;
  dsc->data = *buffer;

  lv_obj_t *img = lv_img_create(parent);
  lv_img_set_src(img, dsc);
  lv_obj_set_pos(img, x, y);
  lv_obj_set_size(img, w, h);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
  return img;
}

void updateWeatherIconDisplay(bool forceReload) {
  if (!weatherIconBox || !sdMounted) return;

  char path[64];
  getWeatherIconPath(path, sizeof(path));

  if (!forceReload && weatherIconImage != nullptr && strcmp(currentWeatherIconPath, path) == 0) {
    return;
  }

  if (weatherIconImage != nullptr) {
    lv_obj_del(weatherIconImage);
    weatherIconImage = nullptr;
  }

  if (weatherIconBuffer != nullptr) {
    free(weatherIconBuffer);
    weatherIconBuffer = nullptr;
  }

  currentWeatherIconPath[0] = '\0';  // invalidate cache so next load is fresh

  if (!SD.exists(path)) {
    return;
  }

  weatherIconImage = createRGB565Icon(
    weatherIconBox,
    path,
    0,
    0,
    34,
    34,
    &weatherIconDsc,
    &weatherIconBuffer,
    true   // weather icons are also chroma-keyed to avoid black/rounded boxes
  );

  if (weatherIconImage != nullptr) {
    copyPath(currentWeatherIconPath, sizeof(currentWeatherIconPath), path);
  }
}

// =====================================================
// Fast RGB565 background loader
// =====================================================

void clearBackgroundToPlain() {
  if (backgroundImage != nullptr) {
    lv_obj_del(backgroundImage);
    backgroundImage = nullptr;
  }

  if (bgBuffer != nullptr) {
    free(bgBuffer);
    bgBuffer = nullptr;
  }

  strncpy(currentBackgroundPath, "plain", sizeof(currentBackgroundPath));
  currentBackgroundPath[sizeof(currentBackgroundPath) - 1] = '\0';

  lv_obj_set_style_bg_color(mainScreen, darkThemeActive ? lv_color_hex(0x16130F) : lv_color_hex(0xEDE6D8), 0);
}

bool loadRGB565Background(const char *fsPath) {
  if (!sdMounted) {
    clearBackgroundToPlain();
    return false;
  }

  if (!SD.exists(fsPath)) {
    return false;
  }

  if (strcmp(currentBackgroundPath, fsPath) == 0 && backgroundImage != nullptr) {
    return true;
  }

  if (backgroundImage != nullptr) {
    lv_obj_del(backgroundImage);
    backgroundImage = nullptr;
  }

  if (bgBuffer != nullptr) {
    free(bgBuffer);
    bgBuffer = nullptr;
  }

  File file = SD.open(fsPath, FILE_READ);
  if (!file) {
    clearBackgroundToPlain();
    return false;
  }

  const size_t expectedSize = 800 * 480 * 2;

  if (file.size() != expectedSize) {
    file.close();
    clearBackgroundToPlain();
    return false;
  }

  bgBuffer = (uint8_t *)ps_malloc(expectedSize);

  if (bgBuffer == nullptr) {
    file.close();
    clearBackgroundToPlain();
    return false;
  }

  size_t bytesRead = file.read(bgBuffer, expectedSize);
  file.close();

  if (bytesRead != expectedSize) {
    free(bgBuffer);
    bgBuffer = nullptr;
    clearBackgroundToPlain();
    return false;
  }

  bgDsc.header.always_zero = 0;
  bgDsc.header.w = 800;
  bgDsc.header.h = 480;
  bgDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  bgDsc.data_size = expectedSize;
  bgDsc.data = bgBuffer;

  backgroundImage = lv_img_create(mainScreen);
  lv_img_set_src(backgroundImage, &bgDsc);
  lv_obj_set_pos(backgroundImage, 0, 0);
  lv_obj_set_size(backgroundImage, 800, 480);
  lv_obj_clear_flag(backgroundImage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_move_background(backgroundImage);
  lv_obj_invalidate(backgroundImage);

  strncpy(currentBackgroundPath, fsPath, sizeof(currentBackgroundPath));
  currentBackgroundPath[sizeof(currentBackgroundPath) - 1] = '\0';
  return true;
}

bool isSafeToSwitchThemeNow() {
  unsigned long nowMs = millis();

  bool userIdle = (nowMs - lastUserActivityAt) >= USER_IDLE_FOR_THEME_MS;

  bool motionIdle = true;
  if (motionEverSeen) {
    motionIdle = (nowMs - lastMotionSeenAt) >= MOTION_IDLE_FOR_THEME_MS;
  }

  return userIdle && motionIdle && !motionState;
}

void applyTheme(bool forceReload) {
  updateActiveThemeForToday(forceReload);

  bool shouldBeDark = isNightNow();

  darkThemeActive = shouldBeDark;

  const char *preferred = shouldBeDark ? activeDarkPath : activeLightPath;
  const char *fallback  = shouldBeDark ? activeLightPath : activeDarkPath;

  bool currentMatches = strcmp(currentBackgroundPath, preferred) == 0 ||
                        strcmp(currentBackgroundPath, fallback) == 0;

  if (!forceReload && currentMatches && backgroundImage != nullptr) {
    applyThemeColors();
    return;
  }
  bool loaded = false;

  if (loadRGB565Background(preferred)) {
    loaded = true;
  } else if (loadRGB565Background(fallback)) {
    loaded = true;
  } else if (shouldBeDark && loadRGB565Background("/bg/night.rgb565")) {
    loaded = true;
  } else if (!shouldBeDark && loadRGB565Background("/bg/day.rgb565")) {
    loaded = true;
  }

  if (!loaded) {
    clearBackgroundToPlain();
  }

  applyThemeColors();
  updateWeatherIconDisplay(true);
}


void updateThemeScheduler() {
  static unsigned long lastSchedulerCheckMs = 0;

  // Tiny guard: only check every 10 seconds.
  if (millis() - lastSchedulerCheckMs < 10000UL) {
    return;
  }

  lastSchedulerCheckMs = millis();

  if (!isTimeReadyFast()) {
    return;
  }

  time_t now = time(nullptr);

  if (nextThemeEventEpoch == 0) {
    scheduleNextThemeEvent();
    return;
  }

  if (now < nextThemeEventEpoch) {
    return;
  }

  if (!isSafeToSwitchThemeNow()) {
    nextThemeEventEpoch = now + (THEME_DEFER_MS / 1000);
    return;
  }

  bool shouldBeDark = isNightNow();

  if (shouldBeDark != darkThemeActive) {
    applyTheme(true);
  } else {
  }

  scheduleNextThemeEvent();
}


// =====================================================
// UI animation helpers
// =====================================================

static void animImgOpaCb(void *obj, int32_t value) {
  lv_obj_set_style_img_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

void startBellRingAnimation() {
  // Lightweight non-blocking ring animation.
  // The two red ring overlays are permanently visible, then softly fade
  // out/in one after the other when the doorbell is pressed.
  if (!bellRingInner && !bellRingOuter) return;

  if (bellRingInner) {
    lv_anim_del(bellRingInner, animImgOpaCb);
    lv_obj_set_style_img_opa(bellRingInner, LV_OPA_COVER, 0);
  }

  if (bellRingOuter) {
    lv_anim_del(bellRingOuter, animImgOpaCb);
    lv_obj_set_style_img_opa(bellRingOuter, LV_OPA_COVER, 0);
  }

  if (bellRingInner) {
    lv_anim_t inner;
    lv_anim_init(&inner);
    lv_anim_set_var(&inner, bellRingInner);
    lv_anim_set_exec_cb(&inner, animImgOpaCb);
    lv_anim_set_values(&inner, 255, 55);
    lv_anim_set_time(&inner, 170);
    lv_anim_set_playback_time(&inner, 230);
    lv_anim_set_path_cb(&inner, lv_anim_path_ease_in_out);
    lv_anim_start(&inner);
  }

  if (bellRingOuter) {
    lv_anim_t outer;
    lv_anim_init(&outer);
    lv_anim_set_var(&outer, bellRingOuter);
    lv_anim_set_exec_cb(&outer, animImgOpaCb);
    lv_anim_set_values(&outer, 255, 45);
    lv_anim_set_delay(&outer, 165);
    lv_anim_set_time(&outer, 190);
    lv_anim_set_playback_time(&outer, 250);
    lv_anim_set_path_cb(&outer, lv_anim_path_ease_in_out);
    lv_anim_start(&outer);
  }
}

void fadeInScreen(lv_obj_t *screen) {
  lv_scr_load(screen);

  lv_obj_invalidate(screen);

  for (int i = 0; i < 5; i++) {
    lv_timer_handler();
    delay(20);
  }

  lv_refr_now(NULL);
}

static void overlayOpaAnimCb(void *obj, int32_t value) {
  lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

void animatePressOverlay(lv_obj_t *overlay, lv_opa_t from, lv_opa_t to, uint16_t timeMs) {
  if (!overlay) return;

  lv_anim_del(overlay, overlayOpaAnimCb);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, overlay);
  lv_anim_set_exec_cb(&a, overlayOpaAnimCb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, timeMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

void tilePressOverlayCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);

  if (!overlay) return;

  if (code == LV_EVENT_PRESSED) {
    animatePressOverlay(overlay, 0, darkThemeActive ? 55 : 45, 45);
  }

  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    lv_opa_t currentOpa = lv_obj_get_style_bg_opa(overlay, 0);
    animatePressOverlay(overlay, currentOpa, 0, 110);
  }
}

lv_obj_t *createPressOverlay(lv_obj_t *parent, int w, int h) {
  lv_obj_t *overlay = lv_obj_create(parent);
  lv_obj_remove_style_all(overlay);

  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_size(overlay, w, h);

  lv_obj_set_style_radius(overlay, 22, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, 0, 0);

  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  return overlay;
}

void attachTilePressOverlay(lv_obj_t *tile, lv_obj_t *overlay) {
  if (!tile || !overlay) return;

  lv_obj_add_event_cb(tile, tilePressOverlayCb, LV_EVENT_PRESSED, overlay);
  lv_obj_add_event_cb(tile, tilePressOverlayCb, LV_EVENT_RELEASED, overlay);
  lv_obj_add_event_cb(tile, tilePressOverlayCb, LV_EVENT_PRESS_LOST, overlay);
}

// =====================================================
// UI styling
// =====================================================

// Tile surface palette.
//
// This is a TN panel, so a wide soft shadow is the wrong tool for depth: a
// 10px blur at ~17% opacity is a low-contrast gradient, and low-contrast
// gradients are the first thing off-axis gamma shift erases. A hairline
// border is a luminance *step*, which survives any viewing angle. The shadow
// stays for the on-axis look but is tightened and grounded with a downward
// offset; the border is what keeps the tile legible from below.
//
// Colours are warmed on the same curve the asset converter applies to the
// artwork (WARM_STRENGTH 0.55), so LVGL-drawn surfaces and SD backgrounds
// stay in the same white balance instead of the tiles reading fluorescent
// against warm photography.
const uint32_t TILE_FILL_LIGHT        = 0xF5EBD6;
const uint32_t TILE_FILL_DARK         = 0xEADBAC;
const uint32_t TILE_FILL_ACTIVE_LIGHT = 0xFFEDBB;
const uint32_t TILE_FILL_ACTIVE_DARK  = 0xF7E3AC;
const uint32_t TILE_EDGE_LIGHT        = 0xC9BB8E;
const uint32_t TILE_EDGE_DARK         = 0xB9A87C;

static void applyTileDepth(lv_obj_t *tile, bool active) {
  lv_obj_set_style_border_width(tile, 1, 0);
  lv_obj_set_style_border_color(tile, lv_color_hex(darkThemeActive ? TILE_EDGE_DARK : TILE_EDGE_LIGHT), 0);
  lv_obj_set_style_border_opa(tile, active ? 235 : 190, 0);

  lv_obj_set_style_shadow_width(tile, active ? 10 : 8, 0);
  lv_obj_set_style_shadow_ofs_y(tile, 2, 0);
  lv_obj_set_style_shadow_opa(tile, active ? (darkThemeActive ? 80 : 85)
                                           : (darkThemeActive ? 50 : 60), 0);
}

void styleTile(lv_obj_t *tile, uint32_t bgColor) {
  lv_obj_set_style_radius(tile, 22, 0);
  lv_obj_set_style_bg_color(tile, lv_color_hex(darkThemeActive ? TILE_FILL_DARK : bgColor), 0);
  lv_obj_set_style_bg_opa(tile, darkThemeActive ? 220 : 205, 0);

  applyTileDepth(tile, false);
  lv_obj_set_style_pad_all(tile, 0, 0);
}

void styleTileActive(lv_obj_t *tile, bool active) {
  if (active) {
    lv_obj_set_style_bg_color(tile, lv_color_hex(darkThemeActive ? TILE_FILL_ACTIVE_DARK : TILE_FILL_ACTIVE_LIGHT), 0);
    lv_obj_set_style_bg_opa(tile, darkThemeActive ? 240 : 225, 0);
  } else {
    lv_obj_set_style_bg_color(tile, lv_color_hex(darkThemeActive ? TILE_FILL_DARK : TILE_FILL_LIGHT), 0);
    lv_obj_set_style_bg_opa(tile, darkThemeActive ? 220 : 205, 0);
  }

  applyTileDepth(tile, active);
}


lv_obj_t *createTile(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t cb) {
  lv_obj_t *tile = lv_obj_create(parent);

  lv_obj_set_pos(tile, x, y);
  lv_obj_set_size(tile, w, h);

  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

  styleTile(tile, TILE_FILL_LIGHT);

  lv_obj_set_style_pad_all(tile, 0, 0);

  lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);

  return tile;
}

// =====================================================
// UI state updates
// =====================================================

void updateCeilingUI() {
  if (ceilingLabel) {
    lv_label_set_text(ceilingLabel, "Ceiling Glow");
  }

  if (ceilingTile) {
    styleTileActive(ceilingTile, ceilingState);
  }

  if ((ceilingGlowLeft || ceilingGlowRight) && ceilingGlowStateShown != ceilingState) {
    ceilingGlowStateShown = ceilingState;

    if (ceilingGlowLeft) {
      lv_anim_del(ceilingGlowLeft, overlayOpaAnimCb);
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, ceilingGlowLeft);
      lv_anim_set_exec_cb(&a, overlayOpaAnimCb);
      lv_anim_set_values(&a, ceilingState ? 0 : 210, ceilingState ? 210 : 0);
      lv_anim_set_time(&a, 260);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
      lv_anim_start(&a);
    }

    if (ceilingGlowRight) {
      lv_anim_del(ceilingGlowRight, overlayOpaAnimCb);
      lv_anim_t b;
      lv_anim_init(&b);
      lv_anim_set_var(&b, ceilingGlowRight);
      lv_anim_set_exec_cb(&b, overlayOpaAnimCb);
      lv_anim_set_values(&b, ceilingState ? 0 : 230, ceilingState ? 230 : 0);
      lv_anim_set_time(&b, 280);
      lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
      lv_anim_start(&b);
    }
  } else {
    if (ceilingGlowLeft) {
      lv_obj_set_style_bg_opa(ceilingGlowLeft, ceilingState ? 255 : 0, 0);
    }
    if (ceilingGlowRight) {
      lv_obj_set_style_bg_opa(ceilingGlowRight, ceilingState ? 255 : 0, 0);
    }
  }

  if (ceilingModeBadge) {
    if (autoLightTarget == AUTO_TARGET_CEILING) {
      lv_label_set_text(ceilingModeBadge, autoMode == AUTO_MODE_AUTO ? "A" : "M");
      lv_obj_set_style_text_opa(ceilingModeBadge, autoMode == AUTO_MODE_AUTO ? 190 : 240, 0);
    } else {
      // Ceiling is a plain manual light right now -- no A/M badge.
      lv_obj_set_style_text_opa(ceilingModeBadge, 0, 0);
    }
  }

  setStateDot(ceilingDot, ceilingState);
}

// ---- Generic "whichever light is currently the AUTO one" helpers ----
// Everything in the AUTO/MANUAL state machine (updateMotionSensor,
// ceilingCb/panelCb's auto branch, the manual-hold timer, the reminder)
// goes through these instead of touching PCF_CEILING_PIN/PCF_PANEL_PIN
// directly, so flipping autoLightTarget in Settings is the only thing
// that needs to change to reassign motion control to the other light.
bool autoOutputState() {
  return (autoLightTarget == AUTO_TARGET_CEILING) ? ceilingState : panelOutputState;
}

void applyAutoOutput(bool on) {
  if (autoLightTarget == AUTO_TARGET_CEILING) {
    ceilingState = on;
    pcfSetPin(PCF_CEILING_PIN, on);
  } else {
    panelOutputState = on;
    pcfSetPin(PCF_PANEL_PIN, on);
  }
}

void updateAutoLightUI() {
  if (autoLightTarget == AUTO_TARGET_CEILING) {
    updateCeilingUI();
  } else {
    updatePanelUI();
  }
}

void updatePanelUI() {
  if (panelTile) {
    styleTileActive(panelTile, panelOutputState);
  }

  if (panelLabel) {
    lv_label_set_text(panelLabel, "Wall Accent");
  }

  if (panelModeBadge) {
    if (autoLightTarget == AUTO_TARGET_PANEL) {
      lv_label_set_text(panelModeBadge, autoMode == AUTO_MODE_AUTO ? "A" : "M");
      lv_obj_set_style_text_opa(panelModeBadge, autoMode == AUTO_MODE_AUTO ? 190 : 240, 0);
    } else {
      lv_obj_set_style_text_opa(panelModeBadge, 0, 0);
    }
  }

  if ((panelGlowTop || panelGlowBottom) && panelGlowStateShown != panelOutputState) {
    panelGlowStateShown = panelOutputState;

    if (panelGlowTop) {
      lv_anim_del(panelGlowTop, overlayOpaAnimCb);
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, panelGlowTop);
      lv_anim_set_exec_cb(&a, overlayOpaAnimCb);
      lv_anim_set_values(&a, panelOutputState ? 0 : 220, panelOutputState ? 220 : 0);
      lv_anim_set_time(&a, 260);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
      lv_anim_start(&a);
    }

    if (panelGlowBottom) {
      lv_anim_del(panelGlowBottom, overlayOpaAnimCb);
      lv_anim_t b;
      lv_anim_init(&b);
      lv_anim_set_var(&b, panelGlowBottom);
      lv_anim_set_exec_cb(&b, overlayOpaAnimCb);
      lv_anim_set_values(&b, panelOutputState ? 0 : 235, panelOutputState ? 235 : 0);
      lv_anim_set_time(&b, 280);
      lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
      lv_anim_start(&b);
    }
  } else {
    if (panelGlowTop) {
      lv_obj_set_style_bg_opa(panelGlowTop, panelOutputState ? 145 : 0, 0);
      lv_obj_set_style_shadow_opa(panelGlowTop, panelOutputState ? 80 : 0, 0);
    }
    if (panelGlowBottom) {
      lv_obj_set_style_bg_opa(panelGlowBottom, panelOutputState ? 165 : 0, 0);
      lv_obj_set_style_shadow_opa(panelGlowBottom, panelOutputState ? 90 : 0, 0);
    }
  }

  setStateDot(panelDot, panelOutputState);
}

void updateStatus(const char *text) {
  // Production build: no status text on main screen.
  // Kept only so internal logic can call it safely.
  (void)text;
}

void applyThemeColors() {
  if (mainScreen == nullptr) return;

  uint32_t topTextColor    = darkThemeActive ? 0xFEF2BE : 0x1B0505;
  uint32_t tileTextColor   = 0x1B0505;
  
  if (weatherLabel) {
    lv_obj_set_style_text_color(weatherLabel, lv_color_hex(topTextColor), 0);
    lv_obj_set_style_text_opa(weatherLabel, 255, 0);
  }

  if (dateLabel) {
    lv_obj_set_style_text_color(dateLabel, lv_color_hex(topTextColor), 0);
    lv_obj_set_style_text_opa(dateLabel, 255, 0);
  }

  if (weatherIconBox) {
    // Weather icon should sit directly on the background.
    // Do not draw the old translucent black rounded square behind it.
    lv_obj_set_style_bg_opa(weatherIconBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weatherIconBox, 0, 0);
    lv_obj_set_style_shadow_width(weatherIconBox, 0, 0);
    lv_obj_set_style_radius(weatherIconBox, 0, 0);
  }

  if (clockPill) {
    lv_obj_set_style_bg_color(clockPill, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(clockPill, 190, 0); // keep in sync with creation site
    lv_obj_set_style_border_color(clockPill, lv_color_hex(0xFEF2BE), 0);
    lv_obj_set_style_border_width(clockPill, 2, 0);
    lv_obj_set_style_border_opa(clockPill, 255, 0);
    lv_obj_set_style_radius(clockPill, 30, 0);
    lv_obj_set_style_shadow_width(clockPill, 0, 0);
  }

  if (clockLabel) {
    lv_obj_set_style_text_color(clockLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(clockLabel, 255, 0);
  }

  if (clockAmPmLabel) {
    lv_obj_set_style_text_color(clockAmPmLabel, lv_color_hex(0xF2E8CC), 0);
    lv_obj_set_style_text_opa(clockAmPmLabel, 230, 0);
  }

  if (ceilingLabel) {
    lv_obj_set_style_text_color(ceilingLabel, lv_color_hex(tileTextColor), 0);
  }

  if (panelLabel) {
    lv_obj_set_style_text_color(panelLabel, lv_color_hex(tileTextColor), 0);
  }

  applyClockPillPlacement();
  updateCeilingUI();
  updatePanelUI();
}


// =====================================================
// Admin actions
// =====================================================

void adminAllOffCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  wakeScreen();
  allOutputsOff();
  updateCeilingUI();
  updatePanelUI();
  updateStatus("Admin: All outputs OFF");
}

void adminReconnectWiFiCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  wakeScreen();
  reconnectWiFi();
  updateStatus("Admin: Reconnecting WiFi");
}



void adminRestartCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  wakeScreen();
  allOutputsOff();
  delay(200);
  ESP.restart();
}

void adminBrightnessCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  wakeScreen();

  brightnessLevel++;
  if (brightnessLevel > 6) {
    brightnessLevel = 0;
  }

  applyBrightnessLevel();
  saveBrightnessLevel();
}

// =====================================================
// Button callbacks
// =====================================================

void doorbellCb(lv_event_t *e) {
  if (firmwareUpdateRunning) return;
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  unsigned long now = millis();

  if (now - lastDoorbellPressAt < DOORBELL_DEBOUNCE_MS) {
    return;
  }

  // Storm guard. A visitor presses once or twice; a noisy touch bus can
  // "press" the tile every few hundred milliseconds for as long as the
  // noise lasts -- which is what rang the chime on a loop and filled the
  // phone with alerts. More than four presses inside a minute is not a
  // visitor: the bell goes quiet for two minutes, one notice goes out, and
  // the activity log says why. The guard also stops the chime relay from
  // being switched over and over, which is itself a source of bus noise.
  if (doorbellPausedUntil && now < doorbellPausedUntil) return;
  doorbellPausedUntil = 0;
  {
    static unsigned long pressTimes[4] = {0, 0, 0, 0};
    static uint8_t pressIdx = 0;
    unsigned long oldest = pressTimes[pressIdx];
    pressTimes[pressIdx] = now;
    pressIdx = (pressIdx + 1) & 3;
    if (oldest != 0 && now - oldest < 60000UL) {
      doorbellPausedUntil = now + 120000UL;
      pcfSetPin(PCF_DOORBELL_PIN, false);
      doorbellActive = false;
      char d[72];
      snprintf(d, sizeof(d), "Storm guard: 5 presses in %lus, bell paused 2 min (touch rejects %u)",
               (now - oldest) / 1000UL, (unsigned)touchRejectCount);
      logActivityEvent("DOORBELL", d);
      sendPushNotificationEx("Doorbell", "Lots of ringing. Bell paused for 2 minutes.", "default", "bell-pause", 10UL * 60UL * 1000UL);
      updateStatus("Status: Doorbell paused (too many presses)");
      return;
    }
  }

  wakeScreen();
  lastDoorbellPressAt = now;

  // Bell was pressed in time - don't show the "Welcome Home" greeting.
  welcomeOverlayPending = false;

  pcfSetPin(PCF_DOORBELL_PIN, true);
  doorbellActive = true;
  doorbellStartedAt = now;

  startBellRingAnimation();

  logActivityEvent("DOORBELL", "Button pressed");
  sendPushNotification("Doorbell", "Someone's at the door.", "high");

  updateStatus("Status: Doorbell ringing...");
}

// Shared AUTO -> MANUAL ON -> MANUAL OFF -> MANUAL ON cycle, used by
// whichever tile (ceiling or panel) is currently assigned as the motion-
// controlled light. See autoLightTarget / applyAutoOutput / updateAutoLightUI.
void handleAutoLightTap() {
  if (autoMode == AUTO_MODE_AUTO) {
    // Manual ON: keep the light ON until the user turns it off or the
    // safety timer expires.
    autoMode = AUTO_MODE_MANUAL_ON;
    autoManualStartedAt = millis();
    autoManualOffStartedAt = 0;
    applyAutoOutput(true);
    updateStatus("Status: Auto light manual ON");
  } else if (autoMode == AUTO_MODE_MANUAL_ON) {
    autoMode = AUTO_MODE_MANUAL_OFF;
    autoManualStartedAt = 0;
    autoManualOffStartedAt = millis();

    // Clear old AUTO motion hold and require one clean sensor CLEAR
    // before AUTO is allowed to react again.
    clearMotionAutoHold();
    motionRearmAfterManualOffClear = true;

    applyAutoOutput(false);
    updateStatus("Status: Auto light manual OFF");
  } else {
    // Manual OFF -> Manual ON
    autoMode = AUTO_MODE_MANUAL_ON;

    autoManualStartedAt = millis();
    autoManualOffStartedAt = 0;
    applyAutoOutput(true);
    updateStatus("Status: Auto light manual ON");
  }

  updateAutoLightUI();
}

void panelCb(lv_event_t *e) {
  if (firmwareUpdateRunning) return;
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  wakeScreen();

  if (autoLightTarget == AUTO_TARGET_PANEL) {
    handleAutoLightTap();
    return;
  }

  // Panel isn't the motion-controlled light right now -- plain manual toggle.
  panelOutputState = !panelOutputState;
  pcfSetPin(PCF_PANEL_PIN, panelOutputState);

  updatePanelUI();
  updateStatus(panelOutputState ? "Status: Panel ON" : "Status: Panel OFF");
}

void ceilingCb(lv_event_t *e) {
  if (firmwareUpdateRunning) return;
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  wakeScreen();

  if (autoLightTarget == AUTO_TARGET_CEILING) {
    handleAutoLightTap();
    return;
  }

  // Ceiling isn't the motion-controlled light right now -- plain manual toggle.
  ceilingState = !ceilingState;
  pcfSetPin(PCF_CEILING_PIN, ceilingState);

  updateCeilingUI();
  updateStatus(ceilingState ? "Status: Ceiling ON" : "Status: Ceiling OFF");
}

void showProdDiagScreen();
void backToMainCb(lv_event_t *e);
void showPinPad(bool recoveryMode, bool forceNewPinMode = false);
void destroyPinPad();
void showSettingsScreen();
void destroySettingsScreen();
void showLogsScreen();
void showWifiPortalScreen(bool firstBoot = false);
void destroyWifiPortalScreen();
lv_obj_t *createBackButton(lv_obj_t *parent, int x, int y, lv_event_cb_t cb,
                            uint32_t bgColor = 0xE9DFC9, uint32_t textColor = 0x333333);
lv_obj_t *createRebootButton(lv_obj_t *parent, int x, int y, lv_event_cb_t cb);

// Admin hotspot press handler -- a normal LVGL long-press opens the PIN
// pad. That's the entire gesture. Recovery is no longer a separate hold
// duration; it's the "Forgot?" button inside the PIN pad (see
// pinPadForgotCb), which is simpler, lighter, and just as undiscoverable
// to a casual guest since it still requires knowing the recovery code.
void adminHotspotLongPressCb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
    wakeScreen();
    pinPadOpenedFromSettings = false;
    showPinPad(false);
  }
}


// =====================================================
// PIN pad / recovery-code pad
// One lazy-loaded modal screen serving both states (matches the
// pin_pad_mockup.html behavior: tapping "Forgot?" swaps the title,
// subtitle, and dot count's meaning in place -- no second screen).
// Created on demand, fully deleted (lv_obj_del) when left, so it
// costs zero RAM whenever the panel isn't actively unlocking settings.
// =====================================================

void pinPadUpdateDots() {
  for (uint8_t i = 0; i < 4; i++) {
    if (!pinPadDots[i]) continue;
    if (i < pinPadEntry.length()) {
      // Same green used on the Motion detection toggle (0x22B88C) so the
      // filled-dot accent reads as one consistent app color.
      lv_obj_set_style_bg_color(pinPadDots[i], lv_color_hex(0x22B88C), 0);
      lv_obj_set_style_bg_opa(pinPadDots[i], 255, 0);
    } else {
      lv_obj_set_style_bg_opa(pinPadDots[i], 0, 0);
    }
  }
}

void pinPadShakeAndClear() {
  pinPadEntry = "";
  pinPadUpdateDots();
}

// Forward decl -- defined alongside the rest of the settings screen later.
void onPinVerifiedOpenSettings();

void pinPadHandleSubmit() {
  unsigned long now = millis();

  if (pinPadRecoveryMode) {
    // ---- Recovery-code path ----
    uint32_t entered = (uint32_t)pinPadEntry.toInt();
    // Recovery code is 6 digits but the pad only takes 4 at a time in this
    // build (matches the locked-in "simple 4-digit, no complication" call) --
    // so recovery uses the SAME pad widget but expects a 4-digit code
    // derived the same way, truncated. See getRecoveryCode() usage below.
    if (entered == (getRecoveryCode() % 10000UL)) {
      // Correct recovery code -> go straight to "set a new PIN", never a
      // silent reset. User must enter a brand new 4-digit PIN twice.
      pinPadIsForReset = true;
      pinPadRecoveryMode = false;
      pinPadFirstNewPin = "";
      pinPadEntry = "";
      lv_label_set_text(pinPadTitle, "Set a new PIN");
      lv_label_set_text(pinPadSub, "Choose a 4-digit PIN");
      pinPadUpdateDots();
    } else {
      pinPadShakeAndClear();
    }
    return;
  }

  if (pinPadIsForReset) {
    // ---- New-PIN entry after a verified recovery code ----
    if (pinPadFirstNewPin.length() == 0) {
      pinPadFirstNewPin = pinPadEntry;
      pinPadEntry = "";
      lv_label_set_text(pinPadSub, "Enter the same PIN again");
      pinPadUpdateDots();
    } else {
      if (pinPadEntry == pinPadFirstNewPin) {
        saveSettingsPin(pinPadEntry);
        pinPadIsForReset = false;
        pinPadFirstNewPin = "";
        pinPadEntry = "";
        destroyPinPad();
        showSettingsScreen();
      } else {
        pinPadFirstNewPin = "";
        pinPadEntry = "";
        lv_label_set_text(pinPadSub, "Didn't match. Choose a 4-digit PIN");
        pinPadUpdateDots();
      }
    }
    return;
  }

  // ---- Normal PIN entry ----
  if (settingsPin.length() == 0) {
    // First run: no PIN set yet. Whatever the user just entered becomes
    // the PIN immediately (matches "first launch -> prompted to set one").
    saveSettingsPin(pinPadEntry);
    pinPadEntry = "";
    destroyPinPad();
    showSettingsScreen();
    return;
  }

  if (pinPadEntry == settingsPin) {
    pinWrongAttempts = 0;
    pinPadEntry = "";
    destroyPinPad();
    showSettingsScreen();
  } else {
    pinWrongAttempts++;
    if (pinWrongAttempts >= PIN_LOCKOUT_THRESHOLD) {
      pinLockoutUntil = now + PIN_LOCKOUT_MS;
      lv_label_set_text(pinPadSub, "Too many attempts. Wait 30 sec");
    }
    pinPadShakeAndClear();
  }
}

void pinPadKeyCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (millis() < pinLockoutUntil) return;  // locked out, ignore digits entirely

  const char *digit = (const char *)lv_event_get_user_data(e);
  if (pinPadEntry.length() >= 4) return;

  pinPadEntry += digit;
  pinPadUpdateDots();

  if (pinPadEntry.length() == 4) {
    pinPadHandleSubmit();
  }
}

void pinPadBackspaceCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (pinPadEntry.length() > 0) {
    pinPadEntry.remove(pinPadEntry.length() - 1);
    pinPadUpdateDots();
  }
}

void pinPadForgotCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  pinPadEntry = "";
  pinPadRecoveryMode = !pinPadRecoveryMode;

  if (pinPadRecoveryMode) {
    lv_label_set_text(pinPadTitle, "Enter recovery code");
    lv_label_set_text(pinPadSub, "Find this on the label inside the unit");
    lv_label_set_text(pinPadForgotLabel, "Back");
  } else {
    lv_label_set_text(pinPadTitle, "Enter PIN");
    lv_label_set_text(pinPadSub, "Settings are protected");
    lv_label_set_text(pinPadForgotLabel, "Forgot?");
  }
  pinPadUpdateDots();
}

void pinPadCancelCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  destroyPinPad();
  if (pinPadOpenedFromSettings) {
    showSettingsScreen();
  } else {
    lv_scr_load(mainScreen);
  }
}

void destroyPinPad() {
  if (pinPadScreen) {
    // lv_obj_del_async (not lv_obj_del) -- this is called from inside the
    // click handler of a button that is itself a child of pinPadScreen
    // (the digit keys, "Forgot?", etc). Deleting an object synchronously
    // from within its own descendant's event callback is the classic LVGL
    // use-after-free: the input device is still mid-processing the event
    // chain for the object we just freed, which is exactly what was
    // crashing/rebooting the panel on every "Back". Async defers the
    // actual free to right after the current event finishes.
    lv_obj_del_async(pinPadScreen);  // deletes all children too
  }
  pinPadScreen = nullptr;
  pinPadTitle = nullptr;
  pinPadSub = nullptr;
  pinPadForgotBtn = nullptr;
  pinPadForgotLabel = nullptr;
  for (uint8_t i = 0; i < 4; i++) pinPadDots[i] = nullptr;
}

// Manually swaps a danger-key's label color between red (rest) and white
// (pressed). Needed because LV_STATE_PRESSED belongs to the button that's
// actually touched, not to a child label -- see the comment at its call
// site in createPinKey() for the full explanation. Same idiom as this
// file's existing tilePressOverlayCb.
void pinKeyDangerLabelCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
  if (!lbl) return;

  if (code == LV_EVENT_PRESSED) {
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xE0473A), LV_PART_MAIN | LV_STATE_DEFAULT);
  }
}

lv_obj_t *createPinKey(lv_obj_t *parent, const char *label, int col, int row, lv_event_cb_t cb, void *userData,
                       int originX, int originY, bool isDanger) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 84, 56);
  // colStep/rowStep = button size + 18px gap, matching the spacing used everywhere else in this layout
  lv_obj_set_pos(btn, originX + col * 102, originY + row * 74);
  lv_obj_set_style_radius(btn, 12, 0);

  // Elegant light "card" look: near-white fill with a soft shadow and a
  // subtle warm border for definition, rather than a dark, flat-filled
  // tile. The shadow does the work of separating the key from the creme
  // background so the fill itself can stay light and modern.
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFDF6), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(btn, lv_color_hex(0xE3D8B8), 0);
  lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_shadow_color(btn, lv_color_hex(0xC9BB8E), 0);
  lv_obj_set_style_shadow_width(btn, 8, 0);
  lv_obj_set_style_shadow_opa(btn, 90, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xEFE6CC), LV_PART_MAIN | LV_STATE_PRESSED);

  if (userData) {
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, userData);
  } else {
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  }

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_font(lbl, FONT_18, 0);
  lv_obj_center(lbl);

  if (isDanger) {
    // DEL key: aesthetic (not harsh) red text at rest, and white text on a
    // filled-red button while actually pressed.
    //
    // Important: LV_STATE_PRESSED is owned by whichever object is actually
    // touched -- that's btn, since lv_btn_create() is what's clickable.
    // btn's own bg-color style correctly responds to its own pressed state
    // (that's why the red fill works), but lbl is a separate child object
    // whose pressed state never becomes true on its own, so a style set on
    // lbl for LV_STATE_PRESSED never applies -- that's why "DEL" stayed red
    // instead of turning white. Fixed the same way this file already
    // handles parent-press-affects-child elsewhere (see
    // tilePressOverlayCb/attachTilePressOverlay above): drive the label's
    // color manually off the button's PRESSED/RELEASED/PRESS_LOST events.
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xE0473A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE0473A), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, pinKeyDangerLabelCb, LV_EVENT_PRESSED, lbl);
    lv_obj_add_event_cb(btn, pinKeyDangerLabelCb, LV_EVENT_RELEASED, lbl);
    lv_obj_add_event_cb(btn, pinKeyDangerLabelCb, LV_EVENT_PRESS_LOST, lbl);
  } else {
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x111111), 0);
  }

  return lbl;  // return label so callers needing to retext (Forgot/Back) keep a handle
}

void showPinPad(bool recoveryMode, bool forceNewPinMode) {
  if (pinPadScreen) destroyPinPad();

  pinPadEntry = "";
  pinPadRecoveryMode = recoveryMode;
  pinPadIsForReset = forceNewPinMode;
  pinPadFirstNewPin = "";

  // Landscape layout: left half is status text + dots, right half is the
  // numpad. The old design was a single 340x420 centered card -- on the
  // real 800x480 panel the bottom button row sat only 4px above the
  // card's rounded bottom corner, which is what was clipping it. Using
  // the full screen width removes that constraint entirely and reads
  // better on a 7" landscape panel besides.
  pinPadScreen = lv_obj_create(NULL);
  lv_obj_clear_flag(pinPadScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(pinPadScreen, lv_color_hex(0xF6E9CC), 0);
  lv_obj_set_style_bg_opa(pinPadScreen, LV_OPA_COVER, 0);

  // Back button -- top-left, shared helper (createBackButton) so this
  // matches Diagnostics, Settings, and the update screen exactly instead
  // of separate near-identical copies. Routes via pinPadCancelCb, which
  // checks pinPadOpenedFromSettings to decide whether "back" means the
  // home screen (long-press entry) or Settings (Change PIN entry).
  createBackButton(pinPadScreen, 18, 16, pinPadCancelCb);

  // Thin vertical divider between the two panes
  lv_obj_t *divider = lv_obj_create(pinPadScreen);
  lv_obj_remove_style_all(divider);
  lv_obj_set_size(divider, 1, 320);
  lv_obj_set_pos(divider, 410, 80);
  lv_obj_set_style_bg_color(divider, lv_color_hex(0xE1D5B8), 0);
  lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

  // ---- Left pane: title, subtitle, dots ----
  pinPadTitle = lv_label_create(pinPadScreen);
  lv_obj_set_style_text_font(pinPadTitle, FONT_26, 0);
  lv_obj_set_style_text_color(pinPadTitle, lv_color_hex(0x111111), 0);
  lv_obj_set_pos(pinPadTitle, 60, 150);

  pinPadSub = lv_label_create(pinPadScreen);
  lv_obj_set_style_text_font(pinPadSub, FONT_18, 0);
  lv_obj_set_style_text_color(pinPadSub, lv_color_hex(0x707070), 0);
  lv_obj_set_pos(pinPadSub, 60, 192);
  lv_obj_set_width(pinPadSub, 330);
  lv_label_set_long_mode(pinPadSub, LV_LABEL_LONG_WRAP);

  if (recoveryMode) {
    lv_label_set_text(pinPadTitle, "Enter recovery code");
    lv_label_set_text(pinPadSub, "Find this on the label inside the unit");
  } else if (settingsPin.length() == 0) {
    lv_label_set_text(pinPadTitle, "Set a PIN");
    lv_label_set_text(pinPadSub, "Choose a 4-digit PIN for Settings");
  } else {
    lv_label_set_text(pinPadTitle, "Enter PIN");
    lv_label_set_text(pinPadSub, "Settings are protected");
  }

  // Dots -- left-aligned under the subtitle, sized up slightly since the
  // landscape layout has more breathing room than the old centered card.
  for (uint8_t i = 0; i < 4; i++) {
    pinPadDots[i] = lv_obj_create(pinPadScreen);
    lv_obj_set_size(pinPadDots[i], 18, 18);
    lv_obj_set_pos(pinPadDots[i], 60 + i * 38, 260);
    lv_obj_set_style_radius(pinPadDots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(pinPadDots[i], 2, 0);
    lv_obj_set_style_border_color(pinPadDots[i], lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_bg_opa(pinPadDots[i], 0, 0);
    lv_obj_clear_flag(pinPadDots[i], LV_OBJ_FLAG_SCROLLABLE);
  }

  // ---- Right pane: numpad ----
  // 3 cols x 4 rows, 84x56 buttons, 18px gaps -> 288x278 grid. The divider
  // moved left too (was 428, now 410) so the numpad can sit truly centered
  // in the right pane -- 51px clear of the divider and 51px clear of the
  // screen's right edge -- instead of hugging one side or the other.
  const int gridX = 461, gridY = 101;

  static const char *digitLabels[9] = {"1","2","3","4","5","6","7","8","9"};
  for (uint8_t i = 0; i < 9; i++) {
    int col = i % 3;
    int row = i / 3;
    createPinKey(pinPadScreen, digitLabels[i], col, row, pinPadKeyCb, (void *)digitLabels[i], gridX, gridY, false);
  }

  pinPadForgotLabel = createPinKey(pinPadScreen, recoveryMode ? "Back" : "Forgot?", 0, 3, pinPadForgotCb, NULL, gridX, gridY, false);
  lv_obj_set_style_text_font(pinPadForgotLabel, FONT_18, 0);
  lv_obj_set_style_text_color(pinPadForgotLabel, lv_color_hex(0x444444), 0);
  pinPadForgotBtn = lv_obj_get_parent(pinPadForgotLabel);

  createPinKey(pinPadScreen, "0", 1, 3, pinPadKeyCb, (void *)"0", gridX, gridY, false);
  createPinKey(pinPadScreen, "DEL", 2, 3, pinPadBackspaceCb, NULL, gridX, gridY, true);

  pinPadUpdateDots();
  lv_scr_load(pinPadScreen);
}


// =====================================================
// Activity log (SD card)
// =====================================================
//
// Appends one CSV line per event to /log/activity.csv:
//   2026-06-15 14:03:21,DOORBELL,Button pressed
//   2026-06-15 14:03:55,MOTION,Motion started
//   2026-06-15 14:04:02,MOTION,No doorbell - welcome shown
//
// View/download it later by pulling the SD card, or by extending the
// existing web server with a small "/log" route that streams the file.

void logActivityEvent(const char *eventType, const char *detail) {
  if (!sdMounted) return;
  // The SD library is not thread-safe, and the update download writes the
  // card from core 0 for about a minute. Log lines in that window are
  // skipped rather than risk interleaving two writers on one card.
  if (firmwareDownloadActive) return;

  if (!SD.exists("/log")) {
    SD.mkdir("/log");
  }

  char timestamp[24] = "uptime_unknown";
  if (isTimeReadyFast()) {
    time_t now = time(nullptr);
    struct tm tmInfo;
    localtime_r(&now, &tmInfo);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tmInfo);
  } else {
    snprintf(timestamp, sizeof(timestamp), "uptime_%lus", millis() / 1000UL);
  }

  // Rotate before appending so the CSV can never grow unbounded again.
  // 512 KB is roughly a year of typical events; one previous generation is
  // kept as activity.old.csv, older history is dropped.
  File chk = SD.open(ACTIVITY_LOG_PATH, FILE_READ);
  if (chk) {
    size_t sz = chk.size();
    chk.close();
    if (sz > 512UL * 1024UL) {
      SD.remove(ACTIVITY_LOG_OLD_PATH);
      SD.rename(ACTIVITY_LOG_PATH, ACTIVITY_LOG_OLD_PATH);
    }
  }

  File f = SD.open(ACTIVITY_LOG_PATH, FILE_APPEND);
  if (!f) return;
  f.printf("%s,%s,%s\n", timestamp, eventType, detail);
  f.close();
}


// =====================================================
// Push notifications (ntfy.sh)
// =====================================================
//
// Lightweight AND non-blocking:
//  - Nothing is reserved permanently - no queue, no always-running task.
//  - When a notification fires, a small one-shot FreeRTOS task is spawned
//    on core 0, does the HTTPS POST, then deletes itself. The main loop /
//    lv_timer_handler() on core 1 is never paused, even if WiFi is slow
//    or the request times out.
//  - 1.5s connect/read timeouts just bound how long that background task
//    can run before giving up - they no longer affect the UI at all.
//
// priority: "min", "low", "default", "high", "urgent" (ntfy.sh priorities)

// Explicit forward declaration: Arduino's auto-prototype generator doesn't
// reliably handle default arguments, so without this, calls to
// sendPushNotification() from earlier in the file (e.g. doorbellCb) fail
// with "was not declared in this scope".
void sendPushNotification(const char *title, const char *message, const char *priority = "default");
void sendPushNotificationEx(const char *title, const char *message, const char *priority, const char *seq, uint32_t clearMs);

// (struct PushNotificationParams is declared near the top of the file)

// Does the actual HTTPS POST to ntfy.sh and returns what really happened:
//   200        -> ntfy accepted it
//   -100       -> no Wi-Fi at all
//   -101       -> couldn't even start the request (e.g. topic is empty)
//   other <0   -> request never got a response: DNS failure, TLS handshake
//                 failure, connection refused/timed out -- a network/
//                 connectivity problem, not a topic problem
//   other >0   -> ntfy responded, just not with 200 (e.g. 400 for a bad
//                 topic name, 403 if the server is access-restricted)
// Callers decide whether to run this fire-and-forget on a background task
// (real doorbell/motion notifications, where the UI must never wait) or
// synchronously on the UI thread (Send test button, where the person is
// explicitly asking "did that actually work?" and blocking for ~1-2s to
// give a truthful answer is the right trade-off).
int doPushNotificationRequest(const char *title, const char *message, const char *priority, const char *actions) {
  return doNtfyPublish("", title, message, priority, actions, 2500, nullptr, false);   // Send test: UI waits on this
}

// The phone control page for THIS unit -- what a tapped notification opens.
static void phonePageUrl(char *out, size_t n) {
  const char *base = nvsPhoneUrl.length() > 0 ? nvsPhoneUrl.c_str() : PHONE_UI_BASE_URL;
  const char *topic = nvsNtfyTopic.length() > 0 ? nvsNtfyTopic.c_str() : NTFY_TOPIC;
  snprintf(out, n, "%s#t=%s", base, topic);
}

// Full sequence ID for a short key: prefixed with the topic so two panels
// (or two apps) can never collide on "motion".
static void ntfySeqId(const char *seq, char *out, size_t n) {
  const char *topic = nvsNtfyTopic.length() > 0 ? nvsNtfyTopic.c_str() : NTFY_TOPIC;
  snprintf(out, n, "%s-%s", topic, seq);
}

// Dismisses a card from every subscribed phone (ntfy "clear"). Best effort.
static int doNtfyClear(const char *seq, int timeoutMs) {
  if (WiFi.status() != WL_CONNECTED) return -100;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(timeoutMs);
  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  char sid[96];
  ntfySeqId(seq, sid, sizeof(sid));
  String url = String("https://ntfy.sh/") +
               (nvsNtfyTopic.length() > 0 ? nvsNtfyTopic : String(NTFY_TOPIC)) + "/" + sid + "/clear";
  if (!http.begin(client, url)) return -101;
  int code = http.PUT(String(""));
  http.end();
  return code;
}

// Same POST, to "<topic><suffix>". The suffix lets the panel answer the phone
// page on a side channel that no app subscribes to, so it never notifies.
// timeoutMs covers connect, TLS handshake and the response: on a weak Wi-Fi
// link (-75 dBm and worse) a handshake alone can take over 1.5s, which is
// what the old fixed 1.5s silently dropped replies on.
int doNtfyPublish(const char *suffix, const char *title, const char *message, const char *priority, const char *actions, int timeoutMs, const char *seq, bool click) {
  if (WiFi.status() != WL_CONNECTED) return -100;

  WiFiClientSecure client;
  client.setInsecure();    // skip TLS cert validation - fine for a best-effort push ping
  client.setTimeout(timeoutMs);

  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);

  String url = String("https://ntfy.sh/") +
               (nvsNtfyTopic.length() > 0 ? nvsNtfyTopic : String(NTFY_TOPIC)) + suffix;

  if (!http.begin(client, url)) return -101;

  if (title && title[0]) http.addHeader("Title", title);
  http.addHeader("Priority", priority);
  if (actions && actions[0]) http.addHeader("Actions", actions);
  if (seq && seq[0]) {
    char sid[96];
    ntfySeqId(seq, sid, sizeof(sid));
    http.addHeader("X-Sequence-ID", sid);
  }
  if (click && !suffix[0]) {
    char page[200];
    phonePageUrl(page, sizeof(page));
    http.addHeader("Click", page);
  }
  int httpCode = http.POST((uint8_t *)message, strlen(message));
  http.end();
  return httpCode;
}

// Hands a push to the worker. Takes ownership of p in every case.
static void enqueuePush(PushNotificationParams *p) {
  if (!pushQueue || xQueueSend(pushQueue, &p, 0) != pdTRUE) {
    delete p;
    if (statPushFail < 65535) statPushFail++;
    statPushLastFailCode = -102;   // queue full / not started
  }
}

// Core-0 worker: one push at a time. Waits for a sane free block before
// opening TLS (a handshake needs ~40KB in one piece), retries a failed send
// twice with backoff, and reports the outcome in the counters. SD is never
// touched here -- a failure is handed to loop() to log.
// Cards to dismiss later (see PushNotificationParams::clearMs). A new push
// with the same key simply moves its due time. Small: at most one entry per
// key in practice.
struct PendingClear { char seq[24]; unsigned long dueAt; };
static PendingClear pendingClears[6];

static void scheduleClear(const char *seq, uint32_t afterMs) {
  int slot = -1;
  for (int i = 0; i < 6; i++) {
    if (pendingClears[i].seq[0] && strcmp(pendingClears[i].seq, seq) == 0) { slot = i; break; }
    if (slot < 0 && !pendingClears[i].seq[0]) slot = i;
  }
  if (slot < 0) return;   // table full: skip, the card just stays
  strncpy(pendingClears[slot].seq, seq, sizeof(pendingClears[slot].seq) - 1);
  pendingClears[slot].seq[sizeof(pendingClears[slot].seq) - 1] = '\0';
  pendingClears[slot].dueAt = millis() + afterMs;
}

static void cancelClear(const char *seq) {
  for (int i = 0; i < 6; i++)
    if (pendingClears[i].seq[0] && strcmp(pendingClears[i].seq, seq) == 0) pendingClears[i].seq[0] = '\0';
}

static void runDueClears() {
  for (int i = 0; i < 6; i++) {
    if (!pendingClears[i].seq[0] || (long)(millis() - pendingClears[i].dueAt) < 0) continue;
    if (WiFi.status() != WL_CONNECTED || ESP.getMaxAllocHeap() < 44000) return;   // try next round
    char seq[24];
    strncpy(seq, pendingClears[i].seq, sizeof(seq)); seq[sizeof(seq) - 1] = '\0';
    pendingClears[i].seq[0] = '\0';
    xSemaphoreTake(tlsGate, portMAX_DELAY);
    doNtfyClear(seq, 6000);
    xSemaphoreGive(tlsGate);
  }
}

static void pushWorkerTask(void *param) {
  (void)param;
  PushNotificationParams *p = nullptr;
  for (;;) {
    // While loop() writes the update image to the card, nothing here may
    // open a TLS session (45KB of internal heap and a busy core).
    if (fwStaging) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    runDueClears();
    if (xQueueReceive(pushQueue, &p, pdMS_TO_TICKS(1000)) != pdTRUE || !p) continue;

    int code = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
      unsigned long t0 = millis();
      while (ESP.getMaxAllocHeap() < 44000 && millis() - t0 < 10000UL) vTaskDelay(pdMS_TO_TICKS(250));
      xSemaphoreTake(tlsGate, portMAX_DELAY);
      code = doNtfyPublish(p->suffix, p->title, p->message, p->priority, p->actions, 6000, p->seq, p->click);
      xSemaphoreGive(tlsGate);
      if (code == 200 || code == -100) break;          // sent, or no Wi-Fi at all
      if (code >= 400 && code < 500 && code != 429) break;  // ntfy rejected it; retrying won't help
      vTaskDelay(pdMS_TO_TICKS(2000 * (attempt + 1)));
    }

    if (code == 200) {
      if (statPushOk < 65535) statPushOk++;
      if (p->seq[0]) { if (p->clearMs) scheduleClear(p->seq, p->clearMs); else cancelClear(p->seq); }
    } else {
      if (statPushFail < 65535) statPushFail++;
      statPushLastFailCode = code;
      if (pushFailPending[0] == '\0') {
        snprintf(pushFailPending, sizeof(pushFailPending), "Push failed (%d) heap %uK blk %uK: %.40s",
                 code, (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024),
                 p->message);
      }
    }
    delete p;
    p = nullptr;
  }
}

// loop(): writes the worker's last failure to the activity log (SD is
// UI-thread only), at most once a minute so a dead link cannot flood it.
static void processPushFailures() {
  static unsigned long lastAt = 0;
  if (pushFailPending[0] == '\0') return;
  if (lastAt != 0 && millis() - lastAt < 60000UL) return;
  lastAt = millis();
  logActivityEvent("PUSH", pushFailPending);
  pushFailPending[0] = '\0';
}

void sendPushNotification(const char *title, const char *message, const char *priority) {
  sendPushNotificationEx(title, message, priority, "", 0);
}

// seq: short key so a later push REPLACES this card instead of stacking a
// new one ("" = plain new card). clearMs: dismiss it after this long.
void sendPushNotificationEx(const char *title, const char *message, const char *priority, const char *seq, uint32_t clearMs) {
  if (WiFi.status() != WL_CONNECTED) return; // nothing useful we can do offline - skip entirely

  PushNotificationParams *p = new PushNotificationParams();   // zero-initialised
  strncpy(p->title, title, sizeof(p->title) - 1);
  p->title[sizeof(p->title) - 1] = '\0';
  strncpy(p->message, message, sizeof(p->message) - 1);
  p->message[sizeof(p->message) - 1] = '\0';
  strncpy(p->priority, priority, sizeof(p->priority) - 1);
  p->priority[sizeof(p->priority) - 1] = '\0';
  if (seq) { strncpy(p->seq, seq, sizeof(p->seq) - 1); p->seq[sizeof(p->seq) - 1] = '\0'; }
  p->clearMs = clearMs;
  p->click = true;

  enqueuePush(p);
}

// =====================================================
// Remote admin commands (ntfy control channel)
// =====================================================
//
// The reverse of the push pipeline: the panel keeps ONE lightweight
// subscription to a second, derived topic ("<push-topic>-cmd" -- no new
// configuration anywhere) and executes a tiny whitelist of commands sent
// there, either by the phone control page's buttons or by typing the word
// in the ntfy app. Confirmations are published to the MAIN topic, prefixed
// with a checkmark, so the customer needs only ONE subscription:
//
//   [doorbell]  Someone is at the door
//   [checkmark] All lights off. Motion automation back to AUTO.
//
// Commands: off / restart / update / status (or ping) / help -- and any
// unrecognized message gets a short "commands are..." reply, so a layman
// can discover the controls by typing literally anything.
//
// Design notes:
//  - The stream task (core 0) NEVER touches LVGL or light state: it only
//    parses and sets pendingRemoteCommand. loop() on the LVGL thread
//    executes it. This is the only thread-safe shape.
//  - Subscribed with HTTP/1.1 like every other ntfy client; chunk framing
//    lines never start with '{' and are skipped by the line parser.
//  - Live-only, no "since=" replay: replaying a cached "restart" after the
//    reboot it caused would be a boot loop. Commands sent while the panel
//    is offline are dropped -- the correct failure mode for admin actions.
//  - One TLS connection held open (~45-50KB heap while connected, near-zero
//    CPU: the task blocks on reads). ntfy keepalives arrive every ~45s;
//    90s of total silence forces a reconnect with backoff.
//  - Security = topic secrecy, same model as the push topic it derives
//    from. Low-stakes by construction: worst case is a reboot, lights off,
//    or opening the LAN-only update portal for 5 minutes.

enum RemoteCommand : uint8_t {
  RCMD_NONE = 0,
  RCMD_RESTART,
  RCMD_LIGHTS_OFF,
  RCMD_LIGHTS_ON,
  RCMD_UPDATE_PORTAL,
  RCMD_STATUS,
  RCMD_HELP,
  RCMD_UPGRADE,
  RCMD_VERSION,   // silent: the phone page asking which firmware is running
  RCMD_UNKNOWN
};

static volatile uint8_t pendingRemoteCommand = RCMD_NONE;
static TaskHandle_t     remoteTaskHandle = nullptr;
static unsigned long pendingRemoteRestartAt = 0; // nonzero = restart scheduled

// Replies carry the title "Panel"; the phone page recognises them by that.

// Builds the command topic from the configured push topic. Empty push
// topic (ntfy not set up) -> empty command topic -> listener stays idle.
static void remoteCommandTopic(char *out, size_t n) {
  const char *base = nvsNtfyTopic.length() > 0 ? nvsNtfyTopic.c_str() : NTFY_TOPIC;
  if (base[0] == '\0') { out[0] = '\0'; return; }
  snprintf(out, n, "%s-cmd", base);
}

// Reply on the MAIN notification topic (fire-and-forget, same one-shot task
// pattern as normal pushes) -- NOT the -cmd channel. The customer subscribes
// to exactly one topic and sees doorbell alerts and command confirmations in
// the same stream; the -cmd topic is a write-only address their buttons post
// to and that they never have to see. The checkmark prefix is added here,
// once, so a confirmation is visually distinct from a doorbell alert.
// Quiet by design: the phone page shows the reply the moment it lands, so
// the card is silent ("low"), replaces the previous reply instead of
// stacking, and clears itself after five minutes.
static void sendRemoteReply(const char *message) {
  sendPushNotificationEx("Panel", message, "low", "reply", 5UL * 60UL * 1000UL);
}

// Runs on the LISTENER TASK: parse one line of the ntfy JSON stream and,
// if it is a message event, map its text onto the whitelist. No LVGL here.
// Last message id seen by the poll fallback: the next poll asks for
// everything after it, so nothing is executed twice.
static char remoteLastPollId[24] = "";

// minTime > 0 (poll fallback): ignore anything older -- a cached "restart"
// from before a reboot must never run again.
static void remoteHandleStreamLine(const char *line, time_t minTime) {
  if (line[0] != '{') return; // headers / blank lines, not an event

  StaticJsonDocument<128> filter;
  filter["event"] = true;
  filter["message"] = true;
  filter["time"] = true;
  filter["id"] = true;

  // On the task's own stack: no heap churn for every keepalive line.
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, line, DeserializationOption::Filter(filter))) return;
  if (strcmp(doc["event"] | "", "message") != 0) return;

  if (minTime > 0) {
    const char *id = doc["id"] | "";
    if (id[0]) { strncpy(remoteLastPollId, id, sizeof(remoteLastPollId) - 1); remoteLastPollId[sizeof(remoteLastPollId) - 1] = '\0'; }
    long t = doc["time"] | 0L;
    if (t < (long)minTime) return;
  }

  const char *msg = doc["message"] | "";
  if (msg[0] == '\0') return;

  char cmd[64];
  size_t j = 0;
  for (size_t i = 0; msg[i] != '\0' && j < sizeof(cmd) - 1; i++) {
    char c = msg[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    cmd[j++] = c;
  }
  cmd[j] = '\0';

  // Exact matches, checked first: the phone page sends these.
  if      (strcmp(cmd, "ver") == 0 || strcmp(cmd, "version") == 0) pendingRemoteCommand = RCMD_VERSION;
  else if (strcmp(cmd, "on") == 0 || strcmp(cmd, "lights on") == 0) pendingRemoteCommand = RCMD_LIGHTS_ON;
  else if (strstr(cmd, "help"))                          pendingRemoteCommand = RCMD_HELP;
  else if (strstr(cmd, "upgrade") || strstr(cmd, "install")) pendingRemoteCommand = RCMD_UPGRADE;
  else if (strstr(cmd, "restart") || strstr(cmd, "reboot")) pendingRemoteCommand = RCMD_RESTART;
  else if (strstr(cmd, "update"))                        pendingRemoteCommand = RCMD_UPDATE_PORTAL;
  else if (strstr(cmd, "off"))                           pendingRemoteCommand = RCMD_LIGHTS_OFF;
  else if (strstr(cmd, "status") || strstr(cmd, "ping")) pendingRemoteCommand = RCMD_STATUS;
  else                                                   pendingRemoteCommand = RCMD_UNKNOWN;
}

// Poll fallback: while the held-open stream is down, ask the relay for
// anything new on the command topic. One small HTTPS GET; executes only
// messages from the last 90 s and never one it has already seen.
static void remotePollOnce(const char *topic) {
  if (!isTimeReadyFast()) return;            // no trustworthy clock: cannot age-filter safely
  if (ESP.getMaxAllocHeap() < 44000) return;
  time_t now = time(nullptr);
  char url[200];
  if (remoteLastPollId[0])
    snprintf(url, sizeof(url), "https://ntfy.sh/%s/json?poll=1&since=%s", topic, remoteLastPollId);
  else
    snprintf(url, sizeof(url), "https://ntfy.sh/%s/json?poll=1&since=%ld", topic, (long)(now - 60));

  xSemaphoreTake(tlsGate, portMAX_DELAY);
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8000);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      int start = 0;
      while (start < (int)body.length()) {
        int nl = body.indexOf('\n', start);
        if (nl < 0) nl = body.length();
        if (nl - start > 1 && nl - start < 1000) {
          String line = body.substring(start, nl);
          remoteHandleStreamLine(line.c_str(), now - 90);
          if (pendingRemoteCommand != RCMD_NONE) { vTaskDelay(pdMS_TO_TICKS(300)); }  // let loop() take it before the next
        }
        start = nl + 1;
      }
    }
    http.end();
  }
  xSemaphoreGive(tlsGate);
}

// Waits ms in one-second slices; polls the command topic every 30 s meanwhile.
static void remoteIdleWait(unsigned long ms, const char *topic) {
  static unsigned long lastPollAt = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    if (WiFi.status() == WL_CONNECTED && millis() - lastPollAt >= 30000UL &&
        !firmwareUploadPortalActive && !firmwareUpdateRunning && !firmwareDownloadActive && !fwStaging) {
      lastPollAt = millis();
      remotePollOnce(topic);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void remoteControlTask(void *param) {
  (void)param;
  char topic[80];
  static char lineBuf[1024];
  unsigned long backoffMs = 5000;   // doubles on repeated failures, capped at 5 min

  for (;;) {
    remoteCommandTopic(topic, sizeof(topic));

    // Stand down while the update portal is open or a flash write is in
    // progress: drops the ~45KB TLS session so the upload and the flash
    // write get the heap, and guarantees nothing on core 0 is mid-TLS while
    // the flash cache is being disabled.
    if (topic[0] == '\0' || WiFi.status() != WL_CONNECTED ||
        firmwareUploadPortalActive || firmwareUpdateRunning || firmwareDownloadActive || fwStaging) {
      remoteLinkState = (topic[0] == '\0') ? 0 : 1;
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    remoteLinkState = 1;

    WiFiClientSecure client;
    client.setInsecure(); // same trust model as the push sender

    // The socket read timeout MUST exceed ntfy's keepalive interval (45s).
    // The core's WiFiClientSecure defaults to 30s, and on a timeout its
    // available() does not just return 0 -- it stop()s the connection. With
    // the default, every quiet 30s tore the subscription down and rebuilt
    // it (full TLS handshake, ~40KB churn), with a ~5s blind window each
    // cycle in which commands were lost. 70s rides through the keepalive
    // with margin, and a genuinely dead link still gets reaped by it.
    // Must be set BEFORE connect(): that is when it reaches the socket.
    client.setTimeout(70000);

    {
      static bool firstConnect = true;
      if (!firstConnect && statListenerReconnects < 65535) statListenerReconnects++;
      firstConnect = false;
    }
    if (!client.connect("ntfy.sh", 443)) {
      remoteIdleWait(backoffMs, topic);
      if (backoffMs < 300000UL) backoffMs *= 2;
      continue;
    }

    // HTTP/1.1, as every ntfy client uses. The body is chunked; each event
    // is written and flushed as one chunk, so the byte stream is
    // "<hex>\r\n{json}\n\r\n" per event. The line splitter below hands
    // only '{'-prefixed lines to the parser, which skips the framing.
    client.printf("GET /%s/json HTTP/1.1\r\n"
                  "Host: ntfy.sh\r\n"
                  "Accept: application/x-ndjson\r\n"
                  "Connection: keep-alive\r\n\r\n", topic);
    // The relay must answer 200 before this counts as subscribed. A 429
    // (rate limit) or 5xx used to be read as a stream that simply never
    // spoke, and the task sat on it until the socket timed out.
    {
      String status = client.readStringUntil('\n');
      if (status.indexOf(" 200") < 0) {
        client.stop();
        remoteLinkState = 1;
        remoteIdleWait(backoffMs, topic);
        if (backoffMs < 300000UL) backoffMs *= 2;
        continue;
      }
    }
    remoteLinkState = 2;
    backoffMs = 5000;

    size_t lineLen = 0;
    unsigned long lastByteAt = millis();

    // Liveness: ntfy sends a keepalive every ~45 s. 100 s of silence means
    // the link is dead even if the socket still claims to be connected.
    while (client.connected() && WiFi.status() == WL_CONNECTED &&
           millis() - lastByteAt < 100000UL &&
           !firmwareUploadPortalActive && !firmwareUpdateRunning && !firmwareDownloadActive && !fwStaging) {
      // available() blocks (up to the socket timeout) until a TLS record
      // arrives, so this task sleeps in recv() while idle: no polling cost.
      while (client.available() > 0) {
        int ci = client.read();
        if (ci < 0) break;
        lastByteAt = millis();
        char c = (char)ci;
        if (c == '\n') {
          lineBuf[lineLen] = '\0';
          remoteHandleStreamLine(lineBuf, 0);
          lineLen = 0;
        } else if (c != '\r' && lineLen < sizeof(lineBuf) - 1) {
          lineBuf[lineLen++] = c;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    client.stop();
    remoteLinkState = 1;
    lineLen = 0;
    remoteIdleWait(backoffMs, topic); // backoff (polling meanwhile), then resubscribe
  }
}

// Hourly health line in the activity log (first one 5 minutes after boot).
// Free heap, the lowest it has ever been, the largest single free block
// (the fragmentation tell), the command task's remaining stack, and PCF
// state. If the panel ever goes quiet again, the Logs screen shows the
// trend that led there instead of leaving us to guess.
// Every 20s: is the switch driver (PCF8574) answering? A latched-up or
// reset chip stays silent until power is cycled; this makes that state
// visible within a minute instead of being discovered when a light fails
// to respond, and the moment the chip answers again its outputs are
// rewritten so lights come back to the state the panel intended.
static void updateSwitchDriverWatch() {
  static unsigned long lastAt = 0;
  if (millis() - lastAt < 20000UL) return;
  lastAt = millis();
  if (firmwareUpdateRunning) return;

  bool wasOnline = (pcfOfflineSince == 0);
  bool nowOnline = pcfIsPresent();

  if (wasOnline && !nowOnline) {
    pcfOfflineSince = millis();
    pcfOfflineNotified = false;
    logActivityEvent("I2C", "Switch driver stopped responding");
  } else if (!wasOnline && nowOnline) {
    unsigned long downSec = (millis() - pcfOfflineSince) / 1000UL;
    pcfOfflineSince = 0;
    pcfWriteState();                       // restore every output to the intended state
    char d[72];
    snprintf(d, sizeof(d), "Switch driver back after %lus, outputs restored", downSec);
    logActivityEvent("I2C", d);
    if (pcfOfflineNotified) sendPushNotificationEx("Panel", "Lights are working again.", "default", "fault", 10UL * 60UL * 1000UL);
    pcfOfflineNotified = false;
  } else if (!nowOnline && !pcfOfflineNotified && millis() - pcfOfflineSince > 90000UL) {
    // Down for a minute and a half: the bus resets have not brought it
    // back, so it almost certainly needs power removed. Say so, once.
    pcfOfflineNotified = true;
    sendPushNotificationEx("Panel",
      "Lights can't be switched right now. Please turn the panel's power off and on at the mains.",
      "high", "fault", 0);
  }
}

// Returns the date on a log file's FIRST line as a YYYYMMDD int, or 0 if the
// file is missing or its first line has no real date (pre-NTP "uptime_" rows).
// Reads ~11 bytes: cheap enough to do once a day.
static int logFileFirstYmd(const char *path) {
  if (!sdMounted || !SD.exists(path)) return 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  char head[11] = {0};
  int n = f.read((uint8_t *)head, 10);
  f.close();
  if (n < 10 || head[4] != '-' || head[7] != '-') return 0;
  return parseYmd(head);
}

// Keeps the activity log to roughly the last twelve months. The live file is
// rotated once its oldest entry passes six months, and the rotated copy is
// deleted once ITS oldest entry passes twelve -- so the two generations
// together span a year at most, and nothing older survives. Runs once a day;
// the 512KB size cap in logActivityEvent() still guards against a runaway
// event rate between daily checks.
static void updateLogRetention() {
  static int lastRunYday = -1;
  if (!sdMounted || !isTimeReadyFast()) return;

  struct tm t;
  getLocalTmFast(&t);
  if (t.tm_yday == lastRunYday) return;
  lastRunYday = t.tm_yday;

  long todayAbs = daysFromYmd((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday);

  int oldYmd = logFileFirstYmd(ACTIVITY_LOG_OLD_PATH);
  if (oldYmd && todayAbs - daysFromYmd(oldYmd) > 365) {
    SD.remove(ACTIVITY_LOG_OLD_PATH);
    logActivityEvent("LOG", "Cleared history older than one year");
  }

  int liveYmd = logFileFirstYmd(ACTIVITY_LOG_PATH);
  if (liveYmd && todayAbs - daysFromYmd(liveYmd) > 180) {
    SD.remove(ACTIVITY_LOG_OLD_PATH);
    SD.rename(ACTIVITY_LOG_PATH, ACTIVITY_LOG_OLD_PATH);
    logActivityEvent("LOG", "Rotated activity log (six months old)");
  }
}

static void updateHealthLog() {
  static unsigned long lastAt = 0;
  unsigned long interval = (lastAt == 0) ? 300000UL : 3600000UL;
  if (millis() - lastAt < interval) return;
  lastAt = millis();

  unsigned stackFree = remoteTaskHandle
      ? (unsigned)uxTaskGetStackHighWaterMark(remoteTaskHandle) : 0;
  char d[96];
  snprintf(d, sizeof(d), "heap %uK min %uK blk %uK|stk %u|pcf %s|i2c %u tch %u|link %u|wifi %u|push %u/%u(%d)",
           (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getMinFreeHeap() / 1024),
           (unsigned)(ESP.getMaxAllocHeap() / 1024),
           stackFree,
           pcfOnline ? "ok" : "FAIL",
           (unsigned)i2cResetCount, (unsigned)touchRejectCount,
           (unsigned)statListenerReconnects, (unsigned)statWifiReconnects,
           (unsigned)statPushOk, (unsigned)statPushFail, statPushLastFailCode);
  logActivityEvent("HEALTH", d);
}

// =====================================================
// Update from GitHub
// =====================================================

// "5.6.1" vs "5.6.0" -> true. Non-numeric or malformed input never wins.
static bool fwVersionIsNewer(const char *candidate, const char *current) {
  int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  if (sscanf(candidate, "%d.%d.%d", &a[0], &a[1], &a[2]) < 1) return false;
  sscanf(current, "%d.%d.%d", &b[0], &b[1], &b[2]);
  for (int i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;
}

static void fwBaseUrl(char *out, size_t n) {
  const char *base = nvsPhoneUrl.length() > 0 ? nvsPhoneUrl.c_str() : PHONE_UI_BASE_URL;
  strncpy(out, base, n - 1);
  out[n - 1] = '\0';
}

// Core-0 worker: fetch manifest.json and report whether a newer version
// exists. Tiny request, ~1s, run from the daily check and before any upgrade.
static FwManifest fwFetchManifest() {
  FwManifest m; m.ok = false; m.version[0] = 0; m.size = 0; m.md5[0] = 0;
  char url[200];
  fwBaseUrl(url, sizeof(url));
  strncat(url, FW_MANIFEST_PATH, sizeof(url) - strlen(url) - 1);
  // GitHub Pages sits behind a CDN that caches every file for ~10 minutes.
  // A unique query string makes each request a cache miss, so the panel
  // always reads the manifest as it is in the repo right now.
  {
    size_t L = strlen(url);
    snprintf(url + L, sizeof(url) - L, "?t=%lu", (unsigned long)(esp_random() & 0x7FFFFFFF));
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8000);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return m;
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      strncpy(m.version, doc["version"] | "", sizeof(m.version) - 1);
      m.size = doc["size"] | 0;
      strncpy(m.md5, doc["md5"] | "", sizeof(m.md5) - 1);
      strncpy(m.festivals, doc["festivals"] | "", sizeof(m.festivals) - 1);
      m.ok = m.version[0] != 0;
    }
  }
  http.end();
  return m;
}

// Festival dates from the repo -- fetched ONLY when the manifest's
// "festivals" stamp differs from the one this panel last applied. A stable
// repo therefore costs nothing beyond the manifest read that happens anyway;
// a change reaches every panel at its next check. The file is accepted only
// if EVERY line is blank, a comment, or "key,YYYY-MM-DD,YYYY-MM-DD" with sane
// dates -- a typo in the repo can never blank out the festival calendar. The
// SD copy is replaced only when the bytes differ.
static void fwSyncFestivalConfig(const char *stamp) {
  if (!sdMounted) return;
  strncpy(festivalConfigPendingVer, stamp, sizeof(festivalConfigPendingVer) - 1);
  festivalConfigPendingVer[sizeof(festivalConfigPendingVer) - 1] = '\0';
  char url[200];
  fwBaseUrl(url, sizeof(url));
  strncat(url, FESTIVAL_CONFIG_URL_PATH, sizeof(url) - strlen(url) - 1);
  {
    size_t L = strlen(url);   // keyed to the stamp: never a stale CDN copy
    snprintf(url + L, sizeof(url) - L, "?v=%s", stamp);
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8000);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  String body = http.getString();
  http.end();

  if (body.length() < 8 || body.length() > 12000) return;

  // Validate every line.
  int valid = 0;
  const char *pStr = body.c_str();
  while (*pStr) {
    const char *nl = strchr(pStr, '\n');
    size_t len = nl ? (size_t)(nl - pStr) : strlen(pStr);
    char line[128];
    if (len >= sizeof(line)) return;                  // absurd line: reject file
    memcpy(line, pStr, len); line[len] = '\0';
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
    if (len > 0 && line[0] != '#') {
      char key[28] = {0}, a[12] = {0}, b[12] = {0};
      if (sscanf(line, "%27[^,],%11[^,],%11[^,]", key, a, b) != 3) return;
      int ya = parseYmd(a), yb = parseYmd(b);
      if (ya < 20000101 || yb < ya || yb > 21001231) return;
      valid++;
    }
    if (!nl) break;
    pStr = nl + 1;
  }
  if (valid == 0) return;

  // Same as what is on the card already? Then nothing to do.
  File cur = SD.open("/config/festivals.txt", FILE_READ);
  if (cur) {
    bool same = (cur.size() == body.length());
    if (same) {
      size_t off = 0; uint8_t chunk[256];
      const char *src = body.c_str();
      while (same && cur.available()) {
        int n = cur.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        if (memcmp(chunk, src + off, n) != 0) same = false;
        off += n;
      }
    }
    cur.close();
    if (same) { festivalConfigSynced = 2; return; }   // card already matches: just remember the stamp
  }

  SD.mkdir("/config");
  SD.remove("/config/festivals.tmp");
  File f = SD.open("/config/festivals.tmp", FILE_WRITE);
  if (!f) return;
  size_t wrote = f.print(body);
  f.close();
  if (wrote != body.length()) { SD.remove("/config/festivals.tmp"); return; }
  SD.remove("/config/festivals.txt");
  if (SD.rename("/config/festivals.tmp", "/config/festivals.txt")) festivalConfigSynced = 1;
}

static void fwCheckTask(void *param) {
  (void)param;
  xSemaphoreTake(tlsGate, portMAX_DELAY);
  FwManifest m = fwFetchManifest();
  if (m.ok && m.festivals[0] != '\0' && strcmp(m.festivals, festivalConfigVer) != 0) {
    fwSyncFestivalConfig(m.festivals);
  }
  xSemaphoreGive(tlsGate);
  fwResultFromSilentCheck = true;   // read by processFirmwareUpdateResults()
  if (!m.ok) {
    snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Could not read update manifest");
    firmwareUpdateResult = FWR_FAILED;
  } else if (fwVersionIsNewer(m.version, FW_VERSION)) {
    strncpy(firmwareUpdateVersion, m.version, sizeof(firmwareUpdateVersion) - 1);
    firmwareUpdateResult = FWR_AVAILABLE;
  } else {
    strncpy(firmwareUpdateVersion, m.version, sizeof(firmwareUpdateVersion) - 1);
    firmwareUpdateResult = FWR_UP_TO_DATE;
  }
  vTaskDelete(nullptr);
}

// Core-0 worker: stream latest.bin to the SD card with an MD5 running over
// it, verify, then hand over to the boot-time installer via a rename.
// Download buffer handed from the core-0 worker to loop(). The worker only
// touches PSRAM and the network; the SD card is written exclusively by the
// UI thread, after the download has finished and been verified. Writing the
// card from core 0 while the UI thread also used it is what made earlier
// downloads die at random byte counts.
static uint8_t *fwStagedBuf = nullptr;
static size_t   fwStagedLen = 0;

static void fwUpgradeTask(void *param) {
  (void)param;
  firmwareDownloadActive = true;
  firmwareDownloadPct = 0;
  fwResultFromSilentCheck = false;  // user asked: reply and show progress

  // Let the "Downloading update" reply leave and the command listener drop
  // its TLS session first, so this task never shares the heap with two
  // other secure connections at once.
  vTaskDelay(pdMS_TO_TICKS(3000));

  // Hold the outbound-TLS gate for the whole download: pushes queue behind
  // it and go out the moment it is released.
  xSemaphoreTake(tlsGate, portMAX_DELAY);
  FwManifest m = fwFetchManifest();
  if (!m.ok) {
    xSemaphoreGive(tlsGate);
    snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Could not read update manifest");
    firmwareUpdateResult = FWR_FAILED; firmwareDownloadActive = false; vTaskDelete(nullptr); return;
  }
  if (!fwVersionIsNewer(m.version, FW_VERSION)) {
    strncpy(firmwareUpdateVersion, m.version, sizeof(firmwareUpdateVersion) - 1);
    xSemaphoreGive(tlsGate); firmwareUpdateResult = FWR_UP_TO_DATE; firmwareDownloadActive = false; vTaskDelete(nullptr); return;
  }
  strncpy(firmwareUpdateVersion, m.version, sizeof(firmwareUpdateVersion) - 1);

  if (m.size < 200000 || m.size > 4000000) {
    snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Manifest size %u is not a firmware image", (unsigned)m.size);
    xSemaphoreGive(tlsGate); firmwareUpdateResult = FWR_FAILED; firmwareDownloadActive = false; vTaskDelete(nullptr); return;
  }
  uint8_t *img = (uint8_t *)ps_malloc(m.size);
  if (!img) {
    snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Not enough PSRAM for %u bytes", (unsigned)m.size);
    xSemaphoreGive(tlsGate); firmwareUpdateResult = FWR_FAILED; firmwareDownloadActive = false; vTaskDelete(nullptr); return;
  }

  char url[200];
  fwBaseUrl(url, sizeof(url));
  strncat(url, FW_BINARY_PATH, sizeof(url) - strlen(url) - 1);
  // Keyed to THIS release's checksum: the CDN can never answer with a
  // cached copy of the previous latest.bin (same name, possibly the same
  // size), which is exactly what a checksum mismatch right after
  // publishing turned out to be.
  {
    size_t L = strlen(url);
    snprintf(url + L, sizeof(url) - L, "?v=%s", m.md5[0] ? m.md5 : m.version);
  }

  // RESUMABLE download into PSRAM. A wall panel on weak Wi-Fi will drop a
  // 1.3MB TLS stream now and then; each attempt asks for the rest of the
  // file with an HTTP Range header and continues from the last byte held.
  // If the server ever ignores Range (200 instead of 206), it restarts at 0.
  const int MAX_ATTEMPTS = 8;
  size_t total = 0;
  int lastCode = 0, attempts = 0;

  while (attempts < MAX_ATTEMPTS && total < m.size) {
    attempts++;
    if (attempts > 1) vTaskDelay(pdMS_TO_TICKS(3000));
    if (WiFi.status() != WL_CONNECTED) { lastCode = -100; continue; }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) { lastCode = -101; continue; }
    if (total > 0) {
      char range[40];
      snprintf(range, sizeof(range), "bytes=%u-", (unsigned)total);
      http.addHeader("Range", range);
    }
    int code = http.GET();
    lastCode = code;
    if (code == 200) total = 0;                      // whole file (first try, or Range ignored)
    if (code != 200 && code != 206) { http.end(); continue; }

    WiFiClient *stream = http.getStreamPtr();
    unsigned long lastDataAt = millis();
    while (total < m.size && http.connected()) {
      size_t avail = stream->available();
      if (avail) {
        size_t want = m.size - total;
        if (want > avail) want = avail;
        if (want > 4096)  want = 4096;
        int n = stream->readBytes(img + total, want);
        if (n <= 0) break;
        total += n;
        firmwareDownloadPct = (uint8_t)((total * 100ULL) / m.size);
        lastDataAt = millis();
      } else {
        if (millis() - lastDataAt > 15000UL) break;   // stalled: reconnect and resume
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    http.end();
  }
  xSemaphoreGive(tlsGate);   // network part is over; verification needs no TLS

  if (total != m.size) {
    free(img);
    snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Download incomplete (%u of %u bytes, %d tries, last code %d)",
             (unsigned)total, (unsigned)m.size, attempts, lastCode);
    firmwareUpdateResult = FWR_FAILED; firmwareDownloadActive = false; vTaskDelete(nullptr); return;
  }

  // MD5Builder::add() takes a 16-bit length on this core, so a single call
  // over a 1.3MB image silently hashes only the low 16 bits' worth of it
  // (12,832 bytes of 1,323,552) and the checksum can never match. Feed it
  // in 32KB pieces.
  MD5Builder md5;
  md5.begin();
  for (size_t off = 0; off < total; off += 32768) {
    size_t n = total - off; if (n > 32768) n = 32768;
    md5.add(img + off, (uint16_t)n);
  }
  md5.calculate();
  if (m.md5[0] && !md5.toString().equalsIgnoreCase(m.md5)) {
    free(img);
    snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Checksum mismatch -- download discarded");
    firmwareUpdateResult = FWR_FAILED; firmwareDownloadActive = false; vTaskDelete(nullptr); return;
  }
  if (img[0] != 0xE9) {
    free(img);
    snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Downloaded file is not a firmware image");
    firmwareUpdateResult = FWR_FAILED; firmwareDownloadActive = false; vTaskDelete(nullptr); return;
  }

  // Verified in RAM. loop() writes it to the card and restarts.
  fwStagedBuf = img;
  fwStagedLen = total;
  firmwareDownloadPct = 100;
  firmwareUpdateResult = FWR_DOWNLOADED;
  firmwareDownloadActive = false;
  vTaskDelete(nullptr);
}

// Update breadcrumb, kept in NVS so a crash during an update names its own
// stage on the next boot (UI/boot thread only).
//   1 saving to card   2 staged on card   3 restart issued
//   4 boot installer writing flash   5 installer finished, restarting
static void fwStageMark(uint8_t stage) {
  prefs.begin("doorpanel", false);
  prefs.putUChar("updstage", stage);
  prefs.end();
}

// UI thread: write the verified image to the card, read it back and check
// it, then stage it for the boot-time installer. Takes a couple of seconds;
// the screen says so. Returns false with firmwareUpdateMsg set on failure.
static bool fwWriteStagedImageToSd() {
  const char *tmpPath = "/update/firmware.tmp";
  bool ok = false;
  uint8_t *rb = nullptr;
  if (!fwStagedBuf || !fwStagedLen) { snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Nothing to stage"); return false; }
  if (!sdMounted) { snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "SD card not ready"); goto done; }

  {
    if (!SD.exists("/update")) SD.mkdir("/update");
    SD.remove(tmpPath);
    File f = SD.open(tmpPath, FILE_WRITE);
    if (!f) { snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "SD card not writable"); goto done; }
    size_t off = 0;
    unsigned chunks = 0;
    while (off < fwStagedLen) {
      size_t n = fwStagedLen - off;
      if (n > 8192) n = 8192;
      if (f.write(fwStagedBuf + off, n) != n) break;
      off += n;
      if ((++chunks & 7) == 0) vTaskDelay(1);   // let the idle task and Wi-Fi stack breathe
    }
    f.close();
    if (off != fwStagedLen) {
      snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "SD card write failed at %u of %u bytes",
               (unsigned)off, (unsigned)fwStagedLen);
      SD.remove(tmpPath);
      goto done;
    }

    // Read back and compare, so a card that silently corrupts writes can
    // never hand the boot installer a damaged image. 4KB reads from the
    // internal heap (falls back to a small stack buffer if it is tight).
    const size_t RB = 4096;
    rb = (uint8_t *)malloc(RB);
    uint8_t small[512];
    uint8_t *chunk = rb ? rb : small;
    size_t chunkCap = rb ? RB : sizeof(small);
    File r = SD.open(tmpPath, FILE_READ);
    bool same = r && r.size() == fwStagedLen;
    size_t pos = 0;
    chunks = 0;
    while (same && pos < fwStagedLen) {
      int n = r.read(chunk, chunkCap);
      if (n <= 0 || memcmp(chunk, fwStagedBuf + pos, n) != 0) { same = false; break; }
      pos += n;
      if ((++chunks & 15) == 0) vTaskDelay(1);
    }
    if (r) r.close();
    if (!same || pos != fwStagedLen) {
      snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "SD card read-back mismatch -- card may be failing");
      SD.remove(tmpPath);
      goto done;
    }

    SD.remove(SD_UPDATE_FIRMWARE_PATH);
    if (!SD.rename(tmpPath, SD_UPDATE_FIRMWARE_PATH)) {
      snprintf(firmwareUpdateMsg, sizeof(firmwareUpdateMsg), "Could not stage firmware on SD");
      SD.remove(tmpPath);
      goto done;
    }
    ok = true;
  }

done:
  if (rb) free(rb);
  free(fwStagedBuf);
  fwStagedBuf = nullptr;
  fwStagedLen = 0;
  return ok;
}

void startFirmwareUpgrade() {
  if (firmwareDownloadActive || firmwareUpdateRunning || firmwareUploadPortalActive) return;
  if (WiFi.status() != WL_CONNECTED) {
    sendRemoteReply("No Wi-Fi, so I can't download the update right now.");
    return;
  }
  if (!sdMounted) {
    sendRemoteReply("SD card isn't ready, so I can't stage the update.");
    return;
  }
  wakeScreen();
  showFirmwarePortalScreen("", "Downloading update...", "Keep power on.");
  logActivityEvent("UPDATE", "Download started");
  sendRemoteReply("Downloading the update. I'll restart to install it.");
  xTaskCreatePinnedToCore(fwUpgradeTask, "fwUpgrade", 12288, nullptr, 1, nullptr, 0);
}

void startFirmwareUpdateCheck() {
  if (firmwareDownloadActive || WiFi.status() != WL_CONNECTED) return;
  xTaskCreatePinnedToCore(fwCheckTask, "fwCheck", 10240, nullptr, 1, nullptr, 0);
}

// Weekly, Sunday at UPDATE_CHECK_HOUR:30 local. Notifies ONCE per new version.
// The phone page checks on demand whenever it is opened, so this is only the
// safety net for households that never open it.
static void updateFirmwareUpdateCheck() {
  static int lastCheckedDay = -1;
  static bool bootCheckDone = false;
  if (!isTimeReadyFast()) return;

  // One check ~2 minutes after boot (network settled, heap at its calmest),
  // so a fresh unit or a reflashed one picks up the festival file and any
  // pending release the same day instead of waiting for 03:30.
  // At ~5 minutes, and only once the largest free block can take a TLS
  // handshake comfortably -- otherwise it tries again a minute later.
  if (!bootCheckDone && bootNetStage == BOOT_NET_DONE && millis() > 300000UL) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry < 60000UL) return;
    lastTry = millis();
    if (ESP.getMaxAllocHeap() < 60000) return;
    bootCheckDone = true;
    startFirmwareUpdateCheck();
    return;
  }

  struct tm t;
  getLocalTmFast(&t);
  if (t.tm_wday != 0 || t.tm_hour != UPDATE_CHECK_HOUR || t.tm_min != 30) return;
  if (t.tm_yday == lastCheckedDay) return;
  lastCheckedDay = t.tm_yday;
  startFirmwareUpdateCheck();
}

// loop(): act on whatever the workers reported. UI, pushes and the restart
// all happen here, on the LVGL thread.
static void processFirmwareUpdateResults() {
  if (festivalConfigSynced) {
    uint8_t how = festivalConfigSynced;
    festivalConfigSynced = 0;
    // Remember the stamp (NVS is touched from this thread only).
    strncpy(festivalConfigVer, festivalConfigPendingVer, sizeof(festivalConfigVer) - 1);
    prefs.begin("doorpanel", false);
    prefs.putString("festVer", festivalConfigVer);
    prefs.end();
    if (how == 1) {
      updateActiveThemeForToday(true);   // same sequence the portal upload uses
      applyTheme(true);
      updateClock();
      char d[64];
      snprintf(d, sizeof(d), "Festival dates updated from GitHub (%s)", festivalConfigVer);
      logActivityEvent("CONFIG", d);
    }
  }

  // Progress while a download runs (coarse: every 10%).
  if (firmwareDownloadActive) {
    static uint8_t shownPct = 255;
    uint8_t pct = firmwareDownloadPct;
    if (pct / 10 != shownPct / 10) {
      shownPct = pct;
      char m[48];
      snprintf(m, sizeof(m), "Downloading update... %u%%", (unsigned)pct);
      showFirmwarePortalScreen("", m, "Keep power on.");
    }
    return;
  }

  uint8_t r = firmwareUpdateResult;
  if (r == FWR_NONE) return;
  firmwareUpdateResult = FWR_NONE;
  bool silent = fwResultFromSilentCheck;

  // Diagnostics summary, whichever path produced the result.
  switch (r) {
    case FWR_AVAILABLE:  snprintf(fwCheckSummary, sizeof(fwCheckSummary), "v%s available", firmwareUpdateVersion); break;
    case FWR_UP_TO_DATE: snprintf(fwCheckSummary, sizeof(fwCheckSummary), "Up to date"); break;
    case FWR_FAILED:     snprintf(fwCheckSummary, sizeof(fwCheckSummary), silent ? "Check failed" : "Last attempt failed"); break;
    default: break;
  }

  // The background check is silent by design: nothing is pushed to the phone and
  // the screen stays asleep unless a newer version is actually available.
  if (silent && r != FWR_AVAILABLE) {
    if (r == FWR_FAILED) {
      char d[120];
      snprintf(d, sizeof(d), "Check failed: %s", firmwareUpdateMsg);
      logActivityEvent("UPDATE", d);
    }
    return;
  }

  char msg[200];
  switch (r) {
    case FWR_AVAILABLE: {
      // Only from the background check (an upgrade proceeds past this).
      // Notify once per version: remembered in NVS so it never nags.
      prefs.begin("doorpanel", false);
      String notified = prefs.getString("fwNotif", "");
      prefs.end();
      if (notified != String(firmwareUpdateVersion)) {
        prefs.begin("doorpanel", false);
        prefs.putString("fwNotif", firmwareUpdateVersion);
        prefs.end();
        // The Install button lives ON the notification: one tap sends the
        // upgrade command, nothing to open, nothing to type.
        char topic[80];
        remoteCommandTopic(topic, sizeof(topic));
        PushNotificationParams *p = new PushNotificationParams();   // zero-initialised
        strncpy(p->title, "Panel", sizeof(p->title) - 1); p->title[sizeof(p->title) - 1] = 0;
        snprintf(p->message, sizeof(p->message),
                 "Update %s is ready. Installing takes about a minute.",
                 firmwareUpdateVersion);
        strncpy(p->priority, "default", sizeof(p->priority) - 1); p->priority[sizeof(p->priority) - 1] = 0;
        strncpy(p->seq, "update", sizeof(p->seq) - 1);   // one card per panel, replaced by newer versions
        p->click = true;
        snprintf(p->actions, sizeof(p->actions),
                 "http, Install, https://ntfy.sh/%s, method=POST, body=upgrade, clear=true", topic);
        enqueuePush(p);
        snprintf(msg, sizeof(msg), "Update %s available", firmwareUpdateVersion);
        logActivityEvent("UPDATE", msg);
      }
      break;
    }
    case FWR_UP_TO_DATE:
      snprintf(msg, sizeof(msg), "Already up to date (%s). Nothing to do.", FW_VERSION);
      sendRemoteReply(msg);
      showFirmwarePortalScreen("", "Already up to date", "No newer version published.");
      break;
    case FWR_FAILED:
      snprintf(msg, sizeof(msg), "Update failed: %s", firmwareUpdateMsg);
      logActivityEvent("UPDATE", msg);
      sendRemoteReply(msg);
      showFirmwarePortalScreen("", "Update failed", firmwareUpdateMsg);
      break;
    case FWR_DOWNLOADED: {
      showFirmwarePortalScreen("", "Saving update...", "Keep power on.");
      lv_timer_handler();                      // show it before the card write blocks
      logActivityEvent("UPDATE", "Saving to card");
      fwStageMark(1);
      fwStaging = true;                        // radio side stands down for the write
      vTaskDelay(pdMS_TO_TICKS(600));          // let any TLS already in flight finish first
      bool staged = fwWriteStagedImageToSd();
      fwStaging = false;
      if (!staged) {
        fwStageMark(0);
        snprintf(msg, sizeof(msg), "Update failed: %s", firmwareUpdateMsg);
        logActivityEvent("UPDATE", msg);
        sendRemoteReply(msg);
        showFirmwarePortalScreen("", "Update failed", firmwareUpdateMsg);
        snprintf(fwCheckSummary, sizeof(fwCheckSummary), "Last attempt failed");
        break;
      }
      fwStageMark(2);
      snprintf(msg, sizeof(msg), "Downloaded %s, installing", firmwareUpdateVersion);
      logActivityEvent("UPDATE", msg);
      snprintf(msg, sizeof(msg), "Update %s downloaded. Installing now, back in about a minute.",
               firmwareUpdateVersion);
      sendRemoteReply(msg);
      showFirmwarePortalScreen("", "Update downloaded", "Restarting to install. Keep power on.");
      pendingRemoteRestartAt = millis() + 2500;   // let the reply leave first
      break;
    }
  }
}

// Runs on the LVGL THREAD (loop). All UI / light-state work happens here.
static void processRemoteCommand() {
  // Scheduled restart: fires a beat after the ack reply was dispatched so
  // the one-shot push task gets time to deliver before the chip goes down.
  if (pendingRemoteRestartAt != 0 && millis() >= pendingRemoteRestartAt) {
    // Restart only when no push is queued or mid-TLS (the reply that
    // announces the restart has to leave first), but never wait past 9 s.
    static unsigned long dueSince = 0;
    if (dueSince == 0) dueSince = millis();
    bool quiet = uxQueueMessagesWaiting(pushQueue) == 0 && xSemaphoreTake(tlsGate, 0) == pdTRUE;
    if (quiet || millis() - dueSince > 9000UL) {
      if (fwStagedBuf == nullptr && SD.exists(SD_UPDATE_FIRMWARE_PATH)) fwStageMark(3);
      delay(200);
      ESP.restart();
    }
  }

  uint8_t cmd = pendingRemoteCommand;
  if (cmd == RCMD_NONE) return;
  pendingRemoteCommand = RCMD_NONE;

  if (firmwareUpdateRunning) return; // never execute anything mid-flash

  switch (cmd) {
    case RCMD_LIGHTS_OFF: {
      // The non-motion light: plain off.
      if (autoLightTarget == AUTO_TARGET_CEILING) {
        panelOutputState = false;
        pcfSetPin(PCF_PANEL_PIN, false);
        updatePanelUI();
      } else {
        ceilingState = false;
        pcfSetPin(PCF_CEILING_PIN, false);
        updateCeilingUI();
      }
      // The motion light: end any manual session, drop any auto hold, and
      // hand control back to AUTO (one clean sensor CLEAR required before
      // it may react again -- same discipline as a manual-off tap).
      autoMode = AUTO_MODE_AUTO;
      autoManualStartedAt = 0;
      autoManualOffStartedAt = 0;
      clearMotionAutoHold();
      motionRearmAfterManualOffClear = true;
      applyAutoOutput(false);
      updateAutoLightUI();
      updateStatus("Status: Lights OFF (remote)");
      logActivityEvent("REMOTE", "Lights switched off remotely");
      sendRemoteReply("Lights off. Motion sensing is back in charge.");
      break;
    }

    case RCMD_LIGHTS_ON: {
      // The non-motion light: plain on.
      if (autoLightTarget == AUTO_TARGET_CEILING) {
        panelOutputState = true;
        pcfSetPin(PCF_PANEL_PIN, true);
        updatePanelUI();
      } else {
        ceilingState = true;
        pcfSetPin(PCF_CEILING_PIN, true);
        updateCeilingUI();
      }
      // The motion light: a manual-on session, exactly like tapping its
      // tile on the panel -- it stays on until switched off or the usual
      // sunrise / safety cutoff.
      autoMode = AUTO_MODE_MANUAL_ON;
      autoManualStartedAt = millis();
      autoManualOffStartedAt = 0;
      applyAutoOutput(true);
      updateAutoLightUI();
      updateStatus("Status: Lights ON (remote)");
      logActivityEvent("REMOTE", "Lights switched on remotely");
      sendRemoteReply("Lights on. They stay on until you say otherwise, or sunrise.");
      break;
    }

    case RCMD_UPDATE_PORTAL: {
      logActivityEvent("REMOTE", "Update portal opened remotely");
      char msg[160];
      IPAddress ip = WiFi.localIP();
      snprintf(msg, sizeof(msg),
               "Update portal open for 5 minutes, home Wi-Fi only: http://%u.%u.%u.%u/update",
               ip[0], ip[1], ip[2], ip[3]);
      sendRemoteReply(msg);
      startFirmwareUploadPortal(); // has its own 5-minute auto-close window
      break;
    }

    case RCMD_RESTART: {
      logActivityEvent("REMOTE", "Restart requested remotely");
      sendRemoteReply("Restarting. Back in about 20 seconds.");
      pendingRemoteRestartAt = millis() + 2500; // let the reply leave first
      break;
    }

    case RCMD_STATUS: {
      char msg[200];
      unsigned long upMin = millis() / 60000UL;
      int rssi = WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0;
      const char *wifi = rssi == 0 ? "off" : rssi >= -65 ? "strong" : rssi >= -75 ? "good" : "weak";
      if (upMin >= 60)
        snprintf(msg, sizeof(msg), "All good. v%s, up %lu h %lu min, Wi-Fi %s. Ceiling %s, wall light %s.",
                 FW_VERSION, upMin / 60, upMin % 60, wifi, ceilingState ? "on" : "off", panelOutputState ? "on" : "off");
      else
        snprintf(msg, sizeof(msg), "All good. v%s, up %lu min, Wi-Fi %s. Ceiling %s, wall light %s.",
                 FW_VERSION, upMin, wifi, ceilingState ? "on" : "off", panelOutputState ? "on" : "off");
      sendRemoteReply(msg);
      break;
    }

    case RCMD_UPGRADE:
      startFirmwareUpgrade();
      break;

    case RCMD_VERSION: {
      // Answered on "<topic>-ver", which no phone subscribes to: the page
      // reads it, nobody gets a notification. At most once per 20s, so a
      // page left reloading can never keep the panel busy with TLS.
      static unsigned long lastVerReplyAt = 0;
      if (lastVerReplyAt != 0 && millis() - lastVerReplyAt < 20000UL) break;
      if (WiFi.status() != WL_CONNECTED) break;
      lastVerReplyAt = millis();
      PushNotificationParams *p = new PushNotificationParams();   // zero-initialised
      strncpy(p->suffix, "-ver", sizeof(p->suffix) - 1);
      strncpy(p->message, FW_VERSION, sizeof(p->message) - 1);
      strncpy(p->priority, "min", sizeof(p->priority) - 1);
      enqueuePush(p);
      break;
    }

    case RCMD_HELP: {
      sendRemoteReply(
        "Send me a word:\n"
        "on / off = lights\n"
        "status = how I'm doing\n"
        "restart = reboot\n"
        "upgrade = install the latest firmware\n"
        "update = open the update portal (5 min)");
      break;
    }

    case RCMD_UNKNOWN: {
      sendRemoteReply("Didn't catch that. Try: on, off, status, restart, upgrade, help.");
      break;
    }
  }
}

// True while local time falls inside the configured quiet-hours window
// (default midnight -> 7 AM). If NTP time isn't ready yet, we don't suppress
// anything since we can't tell what time it actually is.
bool isAutoReminderQuietHoursNow() {
  if (!isTimeReadyFast()) return false;

  struct tm t;
  getLocalTmFast(&t);
  int hour = t.tm_hour;

  if (AUTO_REMINDER_QUIET_START_HOUR < AUTO_REMINDER_QUIET_END_HOUR) {
    return hour >= AUTO_REMINDER_QUIET_START_HOUR && hour < AUTO_REMINDER_QUIET_END_HOUR;
  }
  // Window wraps past midnight (e.g. 22 -> 6).
  return hour >= AUTO_REMINDER_QUIET_START_HOUR || hour < AUTO_REMINDER_QUIET_END_HOUR;
}

// Gentle "is the ceiling light still needed?" reminder. Only applies
// while the ceiling light was switched ON manually (Settings/dashboard
// toggle), not while it's being driven automatically by motion. Fires once
// an hour, and stays silent during quiet hours so it doesn't wake you up.
void updateAutoLightManualOnReminder() {
  if (autoMode != AUTO_MODE_MANUAL_ON || autoManualStartedAt == 0) {
    lastAutoManualOnReminderAt = 0; // reset so the next manual-ON session starts fresh
    return;
  }

  unsigned long interval = activeFestivalTheme ? AUTO_MANUAL_ON_FESTIVAL_INTERVAL_MS
                                               : AUTO_MANUAL_ON_REMINDER_INTERVAL_MS;

  unsigned long onForMs = millis() - autoManualStartedAt;
  if (onForMs < interval) return;

  if (lastAutoManualOnReminderAt != 0 &&
      millis() - lastAutoManualOnReminderAt < interval) {
    return;
  }

  // Mark this check-in regardless, so quiet hours don't cause a backlog of
  // reminders to fire all at once once the window ends.
  lastAutoManualOnReminderAt = millis();

  if (isAutoReminderQuietHoursNow()) return;

  const char *lightName = (autoLightTarget == AUTO_TARGET_CEILING) ? "Ceiling Light" : "Wall Light";
  char message[120];
  int hrs = (int)(onForMs / 3600000UL);
  snprintf(message, sizeof(message), "Still on after %d hour%s. Left on by mistake?", hrs, hrs == 1 ? "" : "s");

  // Replaces the previous reminder rather than stacking one per hour.
  sendPushNotificationEx(lightName, message, "default", "light", 0);
}


// =====================================================
// "Welcome Home" greeting overlay
// =====================================================
//
// Layout target (800x480):
//  1. Full-screen backdrop          #18181B  deep charcoal (neutral, not blue)
//  2. Angled rounded bar, left side #27272A  slightly lighter charcoal card
//  3. "Welcome Home!" text          #FAFAFA  soft white, right-aligned, FONT_30
//  4. Teal line growing from centre #2DD4BF
//
// Total animation = exactly 4000ms (slower/cozier than before):
//   0    -> 450ms    backdrop fades in
//   120  -> 600ms    angled bar fades in
//   280  -> 760ms    text fades in
//   900  -> 1900ms   teal line grows outward from centre (slow, ease-in-out)
//   2800 -> 4000ms   everything fades out together, then hides
//
// Triggered if the doorbell isn't pressed within welcomeBellGraceMs
// (5000ms) of motion being detected.
//
// Tweak the position/size numbers below to match your panel exactly.

static void welcomeBgOpaCb(void *obj, int32_t value) {
  lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void welcomeTextOpaCb(void *obj, int32_t value) {
  lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

// Soft rise for the title: translate is continuous motion of an already
// laid-out label, so unlike letter-spacing it cannot step.
static void welcomeTitleRiseCb(void *obj, int32_t value) {
  lv_obj_set_style_translate_y((lv_obj_t *)obj, (lv_coord_t)value, 0);
}

static void welcomeLineWidthCb(void *obj, int32_t value) {
  lv_obj_t *line = (lv_obj_t *)obj;

  lv_obj_set_width(line, value);

  // Teal line below WELCOME HOME
  lv_obj_align_to(line, welcomeLabel, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

  // Subtitle below teal line
  lv_obj_align_to(welcomeSubLabel, line, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
}

static void welcomeOverlayHideCb(lv_anim_t *a) {
  lv_obj_add_flag(welcomeOverlay, LV_OBJ_FLAG_HIDDEN);
  welcomeOverlayActive = false;
}

// Shared helper: every welcome-overlay fade is "init -> var -> exec -> values
// -> time -> delay -> path -> (ready) -> start". This avoids repeating that
// 8-line boilerplate eight times below.
static void startWelcomeAnim(lv_obj_t *obj, lv_anim_exec_xcb_t execCb,
                              int32_t fromVal, int32_t toVal,
                              uint16_t timeMs, uint16_t delayMs,
                              lv_anim_path_cb_t pathCb,
                              lv_anim_ready_cb_t readyCb = nullptr) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, execCb);
  lv_anim_set_values(&a, fromVal, toVal);
  lv_anim_set_time(&a, timeMs);
  lv_anim_set_delay(&a, delayMs);
  lv_anim_set_path_cb(&a, pathCb);
  if (readyCb) lv_anim_set_ready_cb(&a, readyCb);
  lv_anim_start(&a);
}

void createWelcomeOverlay() {
  // Full-screen dark shield on the top layer so it appears above the dashboard
  // regardless of which screen is active.
  welcomeOverlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(welcomeOverlay);
  lv_obj_set_size(welcomeOverlay, 800, 480);
  lv_obj_set_pos(welcomeOverlay, 0, 0);
  lv_obj_clear_flag(welcomeOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(welcomeOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(welcomeOverlay, 0, 0);
  lv_obj_set_style_bg_color(welcomeOverlay, lv_color_hex(0x100C0D), 0); // warm near-black, a step deeper -- widens the contrast window this TFT compresses
  lv_obj_set_style_bg_opa(welcomeOverlay, 0, 0);
  lv_obj_add_flag(welcomeOverlay, LV_OBJ_FLAG_HIDDEN);

  // Primary title: centered, wide-tracked, warm champagne against the
  // near-black backdrop. Reads like an engraved plaque, not a status toast.
  welcomeLabel = lv_label_create(welcomeOverlay);
  lv_label_set_text(welcomeLabel, "WELCOME HOME");
  lv_obj_set_width(welcomeLabel, 560);
  lv_label_set_long_mode(welcomeLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(welcomeLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(welcomeLabel, FONT_30, 0);
  lv_obj_set_style_text_color(welcomeLabel, lv_color_hex(0xF3DFB0), 0); // fuller cream -- lighter and warmer so the panel shows actual cream, not off-white
  lv_obj_set_style_text_letter_space(welcomeLabel, 6, 0);
  lv_obj_set_style_text_opa(welcomeLabel, 0, 0);
  lv_obj_align(welcomeLabel, LV_ALIGN_CENTER, 0, -46);

  // Secondary contextual message.
  welcomeSubLabel = lv_label_create(welcomeOverlay);
  lv_label_set_text(welcomeSubLabel, "");
  lv_obj_set_width(welcomeSubLabel, 560);
  lv_label_set_long_mode(welcomeSubLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(welcomeSubLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(welcomeSubLabel, FONT_18, 0);
  lv_obj_set_style_text_color(welcomeSubLabel, lv_color_hex(0xE39A7C), 0); // terracotta rose-gold: blue stripped so the magenta-shifting panel renders dusty rose, brightened so it stops submerging into the dark backdrop
  lv_obj_set_style_text_letter_space(welcomeSubLabel, 1, 0);
  lv_obj_set_style_text_opa(welcomeSubLabel, 0, 0);

  // Muted gold hairline, grows outward from the centre underneath the title.
  welcomeLine = lv_obj_create(welcomeOverlay);
  lv_obj_remove_style_all(welcomeLine);
  lv_obj_clear_flag(welcomeLine, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(welcomeLine, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_height(welcomeLine, 3);
  lv_obj_set_width(welcomeLine, 0);
  lv_obj_set_style_radius(welcomeLine, LV_RADIUS_CIRCLE, 0);
  // Calibrating burgundy for this TFT by bisection: palette 0x7E102C (blue
  // 0x2C) rendered PURPLE on the panel; the counter-tint 0x8E1E12 (blue
  // 0x12) rendered RUST. The wine lives between those blue levels, so this
  // sits at the midpoint. If it still misses: MORE blue -> toward purple
  // (try 0x841826), LESS blue -> toward rust (try 0x8A1618). Only the last
  // byte needs to move.
  lv_obj_set_style_bg_color(welcomeLine, lv_color_hex(0x88161F), 0);
  lv_obj_set_style_bg_opa(welcomeLine, 0, 0);
}

const char* getWelcomeMessage() {
  static char msg[64];
  struct tm t;
  getLocalTmFast(&t);

  // Cheap 2-way rotation so back-to-back greetings don't repeat verbatim.
  uint8_t pick = (uint8_t)(millis() / 1000UL) & 0x01;

  // A festival outranks the weather: walking in on Diwali night should not
  // be greeted with the temperature. Only the days the title also covers.
  if (activeFestivalTheme && festivalWelcomeTitle(activeThemeKey)) {
    if (strcmp(activeThemeKey, "diwali") == 0) {
      strcpy(msg, pick ? "The lights are already on for you."
                       : "Welcome home. The house is glowing.");
      return msg;
    }
    if (strcmp(activeThemeKey, "holi") == 0) {
      strcpy(msg, pick ? "Wash the colour off. Keep the day."
                       : "Welcome back, in every colour.");
      return msg;
    }
    if (strcmp(activeThemeKey, "navratri") == 0) {
      strcpy(msg, pick ? "Nine nights. You're home for this one."
                       : "Welcome home. The evening's just starting.");
      return msg;
    }
    if (strcmp(activeThemeKey, "christmas") == 0) {
      strcpy(msg, pick ? "Warm inside, where it should be."
                       : "Welcome home. The season suits you.");
      return msg;
    }
    strcpy(msg, pick ? "New year, same door. Welcome back."
                     : "First steps of the year, taken home.");
    return msg;
  }

  // Weather has the strongest claim on the moment; time of day is the fallback.
  if (wmoCode >= 95 && wmoCode <= 99) {
    strcpy(msg, pick ? "Wild out there. Good thing you're in."
                     : "Let the storm have the outside.");
  }
  else if ((wmoCode >= 51 && wmoCode <= 67) ||
           (wmoCode >= 80 && wmoCode <= 82)) {
    strcpy(msg, pick ? "You beat the rain. Just barely."
                     : "The rain can wait at the door.");
  }
  else if (currentWeatherTemp >= 32) {
    // No degree glyph here: figtree_18 may not include U+00B0 (the weather
    // label renders its degree sign with FONT_30, not FONT_18).
    snprintf(msg, sizeof(msg), "%d degrees out. You made the right call.",
             currentWeatherTemp);
  }
  else if (currentWeatherTemp > 0 && currentWeatherTemp <= 15) {
    strcpy(msg, pick ? "Cold out there. Warm in here."
                     : "Leave the cold where it belongs.");
  }
  else if (t.tm_hour >= 22 || t.tm_hour < 5) {
    strcpy(msg, "Late one. The lights waited up.");
  }
  else if (t.tm_hour < 12) {
    strcpy(msg, pick ? "Good morning. The day's all yours."
                     : "Early start. Make it count.");
  }
  else if (t.tm_hour < 18) {
    strcpy(msg, pick ? "Good afternoon. The hard part's done."
                     : "Good afternoon. Right on time.");
  }
  else {
    strcpy(msg, pick ? "Good evening. The day can wait outside."
                     : "Evening. You're exactly where you should be.");
  }

  return msg;
}

void playWelcomeOverlay() {
  if (!welcomeOverlay || welcomeOverlayActive) return;

  welcomeOverlayActive = true;
  const char *festTitle = activeFestivalTheme ? festivalWelcomeTitle(activeThemeKey) : nullptr;
  lv_label_set_text(welcomeLabel, festTitle ? festTitle : "WELCOME HOME");
  lv_label_set_text(welcomeSubLabel, getWelcomeMessage());

  // Reset everything to its hidden/collapsed starting state.
  lv_obj_set_style_bg_opa(welcomeOverlay, 0, 0);
  lv_obj_set_style_text_opa(welcomeLabel, 0, 0);
  lv_obj_set_style_text_opa(welcomeSubLabel, 0, 0);
  lv_obj_set_style_bg_opa(welcomeLine, 0, 0);
  lv_obj_set_width(welcomeLine, 0);

  lv_obj_clear_flag(welcomeOverlay, LV_OBJ_FLAG_HIDDEN);

  // ---- Fade in (0 - 760ms) ----
  // 1. Backdrop
  startWelcomeAnim(welcomeOverlay, welcomeBgOpaCb,   0, 255, 450,   0, lv_anim_path_ease_out);
  // 3. "Welcome Home" text -- fades in while rising softly into place.
  //    (The earlier letter-spacing expansion stepped visibly on this panel:
  //    tracking animates in whole pixels and every increment re-lays-out
  //    the glyphs. A translate is continuous motion of a static layout, so
  //    it cannot produce that artifact.)
  lv_obj_set_style_translate_y(welcomeLabel, 8, 0);
  startWelcomeAnim(welcomeLabel,   welcomeTextOpaCb, 0, 255, 480, 280, lv_anim_path_ease_out);
  startWelcomeAnim(welcomeLabel,   welcomeTitleRiseCb, 8, 0, 700, 280, lv_anim_path_ease_out);
  startWelcomeAnim(welcomeSubLabel, welcomeTextOpaCb, 0, 255, 420, 420, lv_anim_path_ease_out);

  // ---- 4. Teal line grows outward from the centre, slow & cozy (900 - 1900ms) ----
  lv_obj_set_style_bg_opa(welcomeLine, 255, 0); // line itself stays fully opaque; width animates
  startWelcomeAnim(welcomeLine, welcomeLineWidthCb, 0, 320, 1000, 900, lv_anim_path_ease_in_out);

  // ---- 5. Everything fades out together (2800 - 4000ms), then hides ----
  startWelcomeAnim(welcomeOverlay, welcomeBgOpaCb, 255, 0, 450, 4100, lv_anim_path_ease_in, welcomeOverlayHideCb); // +1s hold: ~4.55s on screen total
}

// Called from loop(). If motion fired and the doorbell hasn't been pressed
// within welcomeBellGraceMs, play the greeting once.
void checkWelcomeOverlayTrigger() {
  if (!welcomeOverlayPending) return;

  if (millis() - motionDetectedForWelcomeAt >= welcomeBellGraceMs) {
    welcomeOverlayPending = false;

    logActivityEvent("MOTION", "No doorbell - welcome shown");

    if (NOTIFY_ON_MOTION_NO_BELL) {
      // One silent card that counts up ("3 times since 6:10 pm") instead of
      // a new card per event; a quiet half hour starts a fresh count and the
      // old card dismisses itself.
      static unsigned long burstStartedAt = 0, lastAt = 0;
      static int count = 0;
      static char sinceText[16] = "";
      if (lastAt == 0 || millis() - lastAt > 30UL * 60UL * 1000UL) {
        count = 0; burstStartedAt = millis();
        sinceText[0] = '\0';
        if (isTimeReadyFast()) {
          struct tm t; getLocalTmFast(&t);
          int h12 = t.tm_hour % 12; if (h12 == 0) h12 = 12;
          snprintf(sinceText, sizeof(sinceText), "%d:%02d %s", h12, t.tm_min, t.tm_hour < 12 ? "am" : "pm");
        }
      }
      lastAt = millis();
      count++;
      char msg[96];
      if (count == 1)           snprintf(msg, sizeof(msg), "Movement outside.");
      else if (sinceText[0])    snprintf(msg, sizeof(msg), "Movement outside, %d times since %s.", count, sinceText);
      else                      snprintf(msg, sizeof(msg), "Movement outside, %d times.", count);
      (void)burstStartedAt;
      sendPushNotificationEx("Front Porch", msg, "low", "motion", 30UL * 60UL * 1000UL);
    }

    // Only greet on the main dashboard (skip diagnostics/admin/portal screens).
    if (lv_scr_act() == mainScreen) {
      playWelcomeOverlay();
    }
  }
}


// =====================================================
// Main UI
// =====================================================

void createMainUI() {
  mainScreen = lv_obj_create(NULL);
  lv_obj_clear_flag(mainScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(mainScreen, lv_color_hex(0xEDE6D8), 0);

  applyTheme(true);

  weatherIconBox = lv_obj_create(mainScreen);
  lv_obj_remove_style_all(weatherIconBox);
  lv_obj_set_size(weatherIconBox, 34, 34);
  lv_obj_set_pos(weatherIconBox, 58, 24);

  weatherLabel = lv_label_create(mainScreen);
  lv_label_set_text(weatherLabel, "-- °C");
  lv_obj_set_style_text_color(weatherLabel, lv_color_hex(darkThemeActive ? 0xFEF2BE : 0x1B0505), 0);
  lv_obj_set_style_text_font(weatherLabel, FONT_30, 0);
  lv_obj_set_pos(weatherLabel, 104, 26);

  // Show cached weather immediately. On first-ever boot, use clear icon + -- °C.
  applyCachedOrPlaceholderWeather();

  dateLabel = lv_label_create(mainScreen);
  lv_label_set_text(dateLabel, "Date loading...");
  lv_obj_set_style_text_color(dateLabel, lv_color_hex(darkThemeActive ? 0xFEF2BE : 0x1B0505), 0);
  lv_obj_set_style_text_font(dateLabel, FONT_30, 0);
  lv_obj_set_pos(dateLabel, 57, 71);

  clockPill = lv_obj_create(mainScreen);
  lv_obj_add_flag(clockPill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(clockPill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(clockPill, 657, 417);
  lv_obj_set_size(clockPill, 129, 47);
  lv_obj_set_style_radius(clockPill, 30, 0);
  lv_obj_set_style_bg_color(clockPill, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(clockPill, 190, 0); // was 95: 37% black gave the digits nothing to sit on through a washed TFT
  lv_obj_set_style_border_color(clockPill, lv_color_hex(0xFEF2BE), 0);
  lv_obj_set_style_border_width(clockPill, 2, 0);
  lv_obj_set_style_border_opa(clockPill, 255, 0);
  lv_obj_set_style_shadow_width(clockPill, 0, 0);
  // Admin is no longer opened from the clock pill.

  clockLabel = lv_label_create(clockPill);
  lv_label_set_text(clockLabel, "--:--");
  lv_obj_set_style_text_color(clockLabel, lv_color_hex(0xFFFFFF), 0); // pure white digits for max contrast
  lv_obj_set_style_text_font(clockLabel, FONT_26, 0);
  lv_obj_set_width(clockLabel, 74);
  lv_obj_set_style_text_align(clockLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(clockLabel, -15, -9);

  clockAmPmLabel = lv_label_create(clockPill);
  lv_label_set_text(clockAmPmLabel, "");
  lv_obj_set_style_text_color(clockAmPmLabel, lv_color_hex(0xF2E8CC), 0);
  lv_obj_set_style_text_opa(clockAmPmLabel, 230, 0);
  lv_obj_set_style_text_font(clockAmPmLabel, FONT_18, 0);
  lv_obj_set_pos(clockAmPmLabel, 64, -3);

  // Hidden admin hotspot: long-press the top-left corner, not the clock or
  // main buttons. Opens the PIN pad directly. Recovery is reached from
  // inside the PIN pad itself ("Forgot?"), not a second gesture here.
  adminHotspot = lv_obj_create(mainScreen);
  lv_obj_remove_style_all(adminHotspot);
  lv_obj_set_pos(adminHotspot, 0, 0);
  lv_obj_set_size(adminHotspot, 54, 54);
  lv_obj_add_flag(adminHotspot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(adminHotspot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(adminHotspot, adminHotspotLongPressCb, LV_EVENT_LONG_PRESSED, NULL);

  bellTile = createTile(mainScreen, 40, 118, 300, 341, doorbellCb);
  styleTileActive(bellTile, false);
  bellPressOverlay = createPressOverlay(bellTile, 300, 341);
  attachTilePressOverlay(bellTile, bellPressOverlay);

  bellIcon = createRGB565Icon(bellTile, "/icons/bell.rgb565", 63, 64, 174, 199, &bellIconDsc, &bellIconBuffer);

  if (!bellIcon) {
    bellIcon = lv_label_create(bellTile);
    lv_label_set_text(bellIcon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(bellIcon, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_font(bellIcon, FONT_30, 0);
    lv_obj_set_pos(bellIcon, 135, 145);
  }
  
  bellRingInner = createRGB565Icon(
    bellTile,
    "/icons/bell_ring_inner.rgb565",
    BELL_RING_X,
    BELL_RING_Y,
    BELL_RING_W,
    BELL_RING_H,
    &bellRingInnerDsc,
    &bellRingInnerBuffer,
    true
  );

  bellRingOuter = createRGB565Icon(
    bellTile,
    "/icons/bell_ring_outer.rgb565",
    BELL_RING_X,
    BELL_RING_Y,
    BELL_RING_W,
    BELL_RING_H,
    &bellRingOuterDsc,
    &bellRingOuterBuffer,
    true
  );

  if (bellRingInner) {
    lv_obj_set_style_img_opa(bellRingInner, LV_OPA_COVER, 0);
  }

  if (bellRingOuter) {
    lv_obj_set_style_img_opa(bellRingOuter, LV_OPA_COVER, 0);
  }

  // No external red wave labels here.
  // The bell icon already includes the red ring marks, and the old LVGL text waves
  // overlapped the centered bell icon.
  bellWave1 = nullptr;
  bellWave2 = nullptr;

  ceilingTile = createTile(mainScreen, 382, 118, 233, 160, ceilingCb);
  ceilingPressOverlay = createPressOverlay(ceilingTile, 233, 160);
  attachTilePressOverlay(ceilingTile, ceilingPressOverlay);

  // Two tiny glow pools under the two lamps. These are much lighter than
  // image animation and avoid the old rectangular glow look.
  ceilingGlowLeft = lv_obj_create(ceilingTile);
  lv_obj_remove_style_all(ceilingGlowLeft);
  lv_obj_set_pos(ceilingGlowLeft, 19, 89);
  lv_obj_set_size(ceilingGlowLeft, 43, 20);
  lv_obj_set_style_radius(ceilingGlowLeft, 30, 0);
  lv_obj_set_style_bg_color(ceilingGlowLeft, lv_color_hex(0xF0B83A), 0);
  lv_obj_set_style_bg_grad_color(ceilingGlowLeft, lv_color_hex(0xFFE078), 0);
  lv_obj_set_style_bg_grad_dir(ceilingGlowLeft, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(ceilingGlowLeft, 0, 0);
  lv_obj_clear_flag(ceilingGlowLeft, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(ceilingGlowLeft, LV_OBJ_FLAG_SCROLLABLE);

  ceilingGlowRight = lv_obj_create(ceilingTile);
  lv_obj_remove_style_all(ceilingGlowRight);
  lv_obj_set_pos(ceilingGlowRight, 76, 62);
  lv_obj_set_size(ceilingGlowRight, 45, 20);
  lv_obj_set_style_radius(ceilingGlowRight, 36, 0);
  lv_obj_set_style_bg_color(ceilingGlowRight, lv_color_hex(0xF0B83A), 0);
  lv_obj_set_style_bg_grad_color(ceilingGlowRight, lv_color_hex(0xFFE078), 0);
  lv_obj_set_style_bg_grad_dir(ceilingGlowRight, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(ceilingGlowRight, 0, 0);
  lv_obj_clear_flag(ceilingGlowRight, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(ceilingGlowRight, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ceilingIcon = createRGB565Icon(ceilingTile, "/icons/ceiling.rgb565", 4, -18, 127, 145, &ceilingIconDsc, &ceilingIconBuffer);

  if (!ceilingIcon) {
    ceilingIcon = lv_label_create(ceilingTile);
    lv_label_set_text(ceilingIcon, "●");
    lv_obj_set_style_text_color(ceilingIcon, lv_color_hex(0xB06B3A), 0);
    lv_obj_set_style_text_font(ceilingIcon, FONT_30, 0);
    lv_obj_set_pos(ceilingIcon, 38, 30);
  }

  ceilingLabel = lv_label_create(ceilingTile);
  lv_label_set_text(ceilingLabel, "Ceiling Glow");
  lv_obj_set_style_text_color(ceilingLabel, lv_color_hex(0x1B0505), 0);
  lv_obj_set_style_text_font(ceilingLabel, FONT_26, 0);
  lv_obj_set_width(ceilingLabel, 205);
  lv_obj_set_style_text_align(ceilingLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(ceilingLabel, 0, 116);

  ceilingDot = createStateDot(ceilingTile, 202, 16);

  ceilingModeBadge = lv_label_create(ceilingTile);
  lv_label_set_text(ceilingModeBadge, "A");
  lv_obj_set_style_text_color(ceilingModeBadge, lv_color_hex(0x1B0505), 0);
  lv_obj_set_style_text_opa(ceilingModeBadge, 210, 0);
  lv_obj_set_style_text_font(ceilingModeBadge, FONT_18, 0);
  lv_obj_set_pos(ceilingModeBadge, 182, 14);

  panelTile = createTile(mainScreen, 382, 299, 233, 160, panelCb);
  panelPressOverlay = createPressOverlay(panelTile, 233, 160);
  attachTilePressOverlay(panelTile, panelPressOverlay);

  // Wall Accent soft glow, shown behind the icon.
  // Keep it simple: two warm vertical light pools from the left-side wall decor.
  panelGlowTop = lv_obj_create(panelTile);
  lv_obj_remove_style_all(panelGlowTop);
  lv_obj_set_pos(panelGlowTop, 39, 12);
  lv_obj_set_size(panelGlowTop, 12, 18);
  lv_obj_set_style_radius(panelGlowTop, LV_RADIUS_CIRCLE, 0);
  // Top glow visually rotated 180° by reversing gradient colors
  lv_obj_set_style_bg_color(panelGlowTop, lv_color_hex(0xFFE7A6), 0);
  lv_obj_set_style_bg_grad_color(panelGlowTop, lv_color_hex(0xF6C15A), 0);
  lv_obj_set_style_bg_grad_dir(panelGlowTop, LV_GRAD_DIR_VER, 0);  lv_obj_set_style_bg_opa(panelGlowTop, 0, 0);
  lv_obj_set_style_shadow_width(panelGlowTop, 12, 0);
  lv_obj_set_style_shadow_spread(panelGlowTop, 2, 0);
  lv_obj_set_style_shadow_color(panelGlowTop, lv_color_hex(0xF6C15A), 0);
  lv_obj_set_style_shadow_opa(panelGlowTop, 0, 0);
  lv_obj_clear_flag(panelGlowTop, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(panelGlowTop, LV_OBJ_FLAG_SCROLLABLE);

  panelGlowBottom = lv_obj_create(panelTile);
  lv_obj_remove_style_all(panelGlowBottom);
  lv_obj_set_pos(panelGlowBottom, 39, 105);
  lv_obj_set_size(panelGlowBottom, 12, 18);
  lv_obj_set_style_radius(panelGlowBottom, 24, 0);
  lv_obj_set_style_bg_color(panelGlowBottom, lv_color_hex(0xF6C15A), 0);
  lv_obj_set_style_bg_grad_color(panelGlowBottom, lv_color_hex(0xFFE7A6), 0);
  lv_obj_set_style_bg_grad_dir(panelGlowBottom, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(panelGlowBottom, 0, 0);
  lv_obj_set_style_shadow_width(panelGlowBottom, 12, 0);
  lv_obj_set_style_shadow_spread(panelGlowBottom, 2, 0);
  lv_obj_set_style_shadow_color(panelGlowBottom, lv_color_hex(0xF6C15A), 0);
  lv_obj_set_style_shadow_opa(panelGlowBottom, 0, 0);
  lv_obj_clear_flag(panelGlowBottom, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(panelGlowBottom, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *panelIcon = createRGB565Icon(panelTile, "/icons/panel.rgb565", 0, 22, 68, 94, &panelIconDsc, &panelIconBuffer);
  if (!panelIcon) {
    panelIcon = lv_label_create(panelTile);
    lv_label_set_text(panelIcon, "●");
    lv_obj_set_style_text_color(panelIcon, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_font(panelIcon, FONT_30, 0);
    lv_obj_set_pos(panelIcon, 38, 30);
  }

  panelLabel = lv_label_create(panelTile);
  lv_label_set_text(panelLabel, "Wall Accent");
  lv_obj_set_style_text_color(panelLabel, lv_color_hex(0x1B0505), 0);
  lv_obj_set_style_text_font(panelLabel, FONT_26, 0);
  lv_obj_set_width(panelLabel, 205);
  lv_obj_set_style_text_align(panelLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(panelLabel, 2, 112);

  panelDot = createStateDot(panelTile, 202, 16);

  panelModeBadge = lv_label_create(panelTile);
  lv_label_set_text(panelModeBadge, "A");
  lv_obj_set_style_text_color(panelModeBadge, lv_color_hex(0x1B0505), 0);
  lv_obj_set_style_text_opa(panelModeBadge, 0, 0); // hidden unless panel is the current auto target
  lv_obj_set_style_text_font(panelModeBadge, FONT_18, 0);
  lv_obj_set_pos(panelModeBadge, 182, 14);

  updateCeilingUI();
  updatePanelUI();
  applyThemeColors();

  createWelcomeOverlay();
}


// =====================================================
// Diagnostics screen
// =====================================================

// Shared "< Back" pill -- one definition used by every screen (PIN pad,
// Diagnostics, Settings, and the OTA update screen) instead of four
// near-identical copies. Cream fill + dark text are the defaults,
// matching the three warm-palette screens; the update screen (dark
// background image) passes its own bg/text colors instead, so callers
// never need to reach into child objects to restyle after the fact.
// Navigation logic is untouched either way -- callers pass their own
// existing callback (settingsBackCb, prodDiagBackCb, pinPadCancelCb,
// backToMainCb) unchanged.
lv_obj_t *createBackButton(lv_obj_t *parent, int x, int y, lv_event_cb_t cb,
                            uint32_t bgColor, uint32_t textColor) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_pos(btn, x, y);
  // Height/radius matched to the "Update device" button so every button
  // in a screen's top bar reads as one consistent family -- only fill/
  // text/border colors differ per button.
  lv_obj_set_size(btn, 110, 46);
  lv_obj_set_style_bg_color(btn, lv_color_hex(bgColor), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 16, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, "< Back");
  lv_obj_set_style_text_font(lbl, FONT_18, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(textColor), 0);
  lv_obj_center(lbl);

  return btn;
}

// Shared vibrant-red Reboot pill -- one definition used by both
// Diagnostics and Settings instead of two near-identical copies. x/y is
// the top-left position at creation; callers needing a different anchor
// (Settings' bottom-right isolated placement) call lv_obj_align() on the
// returned handle afterward, which replaces the position outright.
lv_obj_t *createRebootButton(lv_obj_t *parent, int x, int y, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_pos(btn, x, y);
  // Sizing/radius/border-weight matched to the "Update device" button so
  // the top bar reads as one consistent button family -- only the red
  // color scheme differs.
  lv_obj_set_size(btn, 130, 46);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xFCE4E1), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 16, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0xD32F1F), 0);
  lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, "Reboot");
  lv_obj_set_style_text_font(lbl, FONT_18, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xD32F1F), 0);
  lv_obj_center(lbl);

  return btn;
}

lv_obj_t *createBrightnessButton(lv_obj_t *parent, int x, int y) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, 155, 46);
  lv_obj_set_style_radius(btn, 16, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
  lv_obj_add_event_cb(btn, adminBrightnessCb, LV_EVENT_CLICKED, NULL);

  brightnessBtnLabel = lv_label_create(btn);
  updateBrightnessButtonText();
  lv_obj_set_style_text_color(brightnessBtnLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(brightnessBtnLabel, FONT_18, 0);
  lv_obj_center(brightnessBtnLabel);

  return btn;
}

void updateDiagHardwareCache(bool force = false) {
  if (!force && millis() - lastDiagHardwarePollAt < 10000UL) {
    return;
  }

  lastDiagHardwarePollAt = millis();

  cachedGt911Online = gt911IsPresent();
  cachedPcfOnline = pcfIsPresent();
}


bool loadRGB565ImageToObject(
  lv_obj_t *parent,
  const char *fsPath,
  lv_obj_t **imgObj,
  lv_img_dsc_t *dsc,
  uint8_t **buffer
) {
  if (!sdMounted || !SD.exists(fsPath)) {
    return false;
  }

  File file = SD.open(fsPath, FILE_READ);
  if (!file) {
    return false;
  }

  const size_t expectedSize = 800 * 480 * 2;

  if (file.size() != expectedSize) {
    file.close();
    return false;
  }

  if (*imgObj != nullptr) {
    lv_obj_del(*imgObj);
    *imgObj = nullptr;
  }

  if (*buffer != nullptr) {
    free(*buffer);
    *buffer = nullptr;
  }

  *buffer = (uint8_t *)ps_malloc(expectedSize);
  if (*buffer == nullptr) {
    file.close();
    return false;
  }

  size_t bytesRead = file.read(*buffer, expectedSize);
  file.close();

  if (bytesRead != expectedSize) {
    free(*buffer);
    *buffer = nullptr;
    return false;
  }

  dsc->header.always_zero = 0;
  dsc->header.w = 800;
  dsc->header.h = 480;
  dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
  dsc->data_size = expectedSize;
  dsc->data = *buffer;

  *imgObj = lv_img_create(parent);
  lv_img_set_src(*imgObj, dsc);
  lv_obj_set_pos(*imgObj, 0, 0);
  lv_obj_set_size(*imgObj, 800, 480);
  lv_obj_clear_flag(*imgObj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_move_background(*imgObj);

  return true;
}

void backToMainCb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    wakeScreen();
    lv_scr_load(mainScreen);
  }
}

void createUpdateScreenShell() {
  if (updateScreen) return;

  updateScreen = lv_obj_create(NULL);
  lv_obj_clear_flag(updateScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(updateScreen, lv_color_hex(0x111722), 0);

  loadRGB565ImageToObject(updateScreen, UPDATE_BG_PATH, &updateBgImage, &updateBgDsc, &updateBgBuffer);

  // Shared Back button used everywhere else (Settings, Diagnostics, PIN
  // pad), top-left, default cream styling -- previously this screen used
  // a bespoke top-right dark variant, inconsistent with the rest of the
  // app. Same callback (backToMainCb), same navigation behavior.
  createBackButton(updateScreen, 24, 20, backToMainCb);

  // The update_bg image already contains the premium card/title/progress outline.
  // Runtime only overlays changing instructions/status.
  updateMessageLabel = lv_label_create(updateScreen);
  lv_label_set_text(updateMessageLabel, "");
  lv_obj_set_width(updateMessageLabel, 800);
  lv_obj_set_style_text_align(updateMessageLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(updateMessageLabel, lv_color_hex(0xFFF4E2), 0);
  lv_obj_set_style_text_font(updateMessageLabel, FONT_18, 0);
  lv_obj_set_pos(updateMessageLabel, 0, 244);

  updateUrlLabel = lv_label_create(updateScreen);
  lv_label_set_text(updateUrlLabel, "");
  lv_obj_set_width(updateUrlLabel, 800);
  lv_obj_set_style_text_align(updateUrlLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(updateUrlLabel, lv_color_hex(0xF3C28D), 0);
  lv_obj_set_style_text_font(updateUrlLabel, FONT_18, 0);
  lv_obj_set_pos(updateUrlLabel, 0, 274);

  updateHintLabel = lv_label_create(updateScreen);
  lv_label_set_text(updateHintLabel, "");
  lv_obj_set_width(updateHintLabel, 800);
  lv_obj_set_style_text_align(updateHintLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(updateHintLabel, lv_color_hex(0xFFF4E2), 0);
  lv_obj_set_style_text_font(updateHintLabel, FONT_18, 0);
  lv_obj_set_pos(updateHintLabel, 0, 304);
}

void showFirmwarePortalScreen(const char *message, const char *urlText, const char *hintText) {
  createUpdateScreenShell();

  if (updateMessageLabel) lv_label_set_text(updateMessageLabel, message ? message : "");
  if (updateUrlLabel) lv_label_set_text(updateUrlLabel, urlText ? urlText : "");
  if (updateHintLabel) lv_label_set_text(updateHintLabel, hintText ? hintText : "");

  if (lv_scr_act() != updateScreen) {
    lv_scr_load(updateScreen);
  }

  lv_timer_handler();
}


// The update page is fixed content, so it lives in flash as one literal and
// is streamed straight from there (send_P) -- no 1.5KB heap copy per request.
static const char FW_UPLOAD_PAGE_HTML[] PROGMEM =
  "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Panel Update</title>"
  "<style>body{margin:0;background:#111827;color:#f8f1e8;font-family:system-ui;display:grid;place-items:center;min-height:100vh}.c{width:min(92vw,420px);padding:18px;border:1px solid #e8bd8666;border-radius:22px;background:#ffffff12}h1{margin:0;font-size:24px}.s{color:#e8bd86;margin:4px 0 12px}.b{border-top:1px solid #ffffff22;padding:11px 0}.h{font-size:12px;color:#d8d0c8;margin:6px 0 0}input,button{width:100%;box-sizing:border-box;margin-top:7px;font:14px system-ui}input{padding:8px;border:1px solid #ffffff33;border-radius:11px;background:#0005;color:#fff}button{padding:10px;border:0;border-radius:11px;background:#e8bd86;color:#17110c;font-weight:700}</style>"
  "<body><main class=c><h1>Panel Update</h1><p class=s>Choose one file to upload</p>"
  "<section class=b><b>System Update</b><form method=POST action=/upload-fw enctype=multipart/form-data><input type=file name=file accept=.bin required><button>Upload</button></form><p class=h>Installs 5s after upload.</p></section>"
  "<section class=b><b>Festival Dates</b><form method=POST action=/upload-festivals enctype=multipart/form-data><input type=file name=file accept=.txt required><button>Upload</button></form><p class=h>Updates festival schedule.</p></section>"
  "<section class=b><b>Background Images</b><form method=POST action=/upload-bg enctype=multipart/form-data><input type=file name=file accept=.rgb565 required><button>Upload</button></form><p class=h>Upload converted .rgb565 file.</p></section>"
  "</main>";

void handleFirmwareUploadPage() {
  firmwareUploadServer.send_P(200, "text/html", FW_UPLOAD_PAGE_HTML);
}

bool resolveBackgroundUploadPath(const String &uploadName, char *out, size_t outLen) {
  String name = uploadName;
  name.replace("\\", "/");
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);

  name.toLowerCase();
  name.replace(" ", "_");
  name.replace("-", "_");

  if (name.endsWith(".rgb565")) {
    name = name.substring(0, name.length() - 7);
  }

  if (name == "day" || name == "night" || name == "winter_night") {
    snprintf(out, outLen, "/bg/%s.rgb565", name.c_str());
    return true;
  }

  if (name == "update_bg" || name == "update_screen" || name == "updatescreen") {
    snprintf(out, outLen, "/ui/update_bg.rgb565");
    return true;
  }

  const char *festivalAssets[] = {
    "diwali_day", "diwali_night",
    "holi_day", "holi_night",
    "christmas_day", "christmas_night",
    "ganesh_day", "ganesh_night",
    "rakhi_day", "rakhi_night",
    "janmashtami_day", "janmashtami_night",
    "navratri_day", "navratri_night",
    "halloween_day", "halloween_night",
    "republic_day", "republic_night",
    "valentines_day", "valentines_night",
    "mothers_day", "mothers_night",
    "fathers_day", "fathers_night",
    "new_year_day", "new_year_night",
    "new_year_eve_day"
  };

  for (size_t i = 0; i < sizeof(festivalAssets) / sizeof(festivalAssets[0]); i++) {
    if (name == festivalAssets[i]) {
      snprintf(out, outLen, "/festival/%s.rgb565", name.c_str());
      return true;
    }
  }

  return false;
}

void handleUploadPost() {
  if (currentPortalUploadKind == 4) {
    firmwareUploadServer.send(500, "text/plain", "Upload failed: SD card write error. Nothing was changed.");
  } else if (currentPortalUploadKind == 1) {
    firmwareUploadServer.send(200, "text/plain", "Firmware uploaded. The panel restarts in 5 seconds to install it.");
  } else if (currentPortalUploadKind == 2) {
    firmwareUploadServer.send(200, "text/plain", "Festival dates uploaded and applied.");
  } else if (currentPortalUploadKind == 3) {
    firmwareUploadServer.send(200, "text/plain", "Background asset uploaded.");
  } else {
    firmwareUploadServer.send(200, "text/plain", "Upload complete.");
  }
}

void handlePortalUploadStream() {
  HTTPUpload &upload = firmwareUploadServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String uri = firmwareUploadServer.uri();

    currentPortalUploadKind = 0;
    portalUploadPath[0] = '\0';

    if (uri == "/upload-fw") {
      currentPortalUploadKind = 1;
      SD.mkdir("/update");
      snprintf(portalUploadPath, sizeof(portalUploadPath), "%s", SD_UPDATE_FIRMWARE_PATH);
    } else if (uri == "/upload-festivals") {
      currentPortalUploadKind = 2;
      SD.mkdir("/config");
      snprintf(portalUploadPath, sizeof(portalUploadPath), "/config/festivals.txt");
    } else if (uri == "/upload-bg") {
      currentPortalUploadKind = 3;

      if (!resolveBackgroundUploadPath(upload.filename, portalUploadPath, sizeof(portalUploadPath))) {
        snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Unsupported asset name");
        showFirmwarePortalScreen("", "Unsupported file name", "Use a supported .rgb565 name.");
        return;
      }

      String path = String(portalUploadPath);
      int lastSlash = path.lastIndexOf('/');
      if (lastSlash > 0) {
        SD.mkdir(path.substring(0, lastSlash));
      }
    }

    if (currentPortalUploadKind == 0 || portalUploadPath[0] == '\0') {
      snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Upload route error");
      return;
    }

    if (SD.exists(portalUploadPath)) {
      SD.remove(portalUploadPath);
    }

    firmwareUploadFile = SD.open(portalUploadPath, FILE_WRITE);
    if (!firmwareUploadFile) {
      snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "File open failed");
      showFirmwarePortalScreen("", "Upload failed", "Could not save to SD.");
      return;
    }

    if (currentPortalUploadKind == 1) {
      firmwareAutoInstallPending = false;
      snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Receiving firmware...");
      showFirmwarePortalScreen("", "Receiving firmware...", "Installs in 5 seconds.");
    } else if (currentPortalUploadKind == 2) {
      snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Receiving festivals.txt...");
      showFirmwarePortalScreen("", "Receiving dates...", "Applies without reboot.");
    } else {
      snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Receiving asset...");
      showFirmwarePortalScreen(portalUploadPath, "Receiving image...", "");
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (firmwareUploadFile) {
      // A short SD write would leave a truncated file that the installer
      // then rejects; catch it here, where the cause is still known.
      if (firmwareUploadFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
        firmwareUploadFile.close();
        SD.remove(portalUploadPath);
        currentPortalUploadKind = 4;   // failed: END does nothing, POST reply says why
        snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "SD write failed");
        showFirmwarePortalScreen("", "Upload failed", "SD card write error. Try again.");
      }
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (firmwareUploadFile) {
      firmwareUploadFile.close();
    }

    if (currentPortalUploadKind == 1) {
      // Installation is NOT attempted here, with WiFi, TLS and the display
      // all live. The panel restarts and the boot-time installer (the one
      // path that has proven reliable, used by the GitHub update too) picks
      // the file up before anything else starts.
      firmwareAutoInstallPending = true;
      firmwareAutoInstallStartedAt = millis();

      snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Firmware saved. Restarting to install");
      showFirmwarePortalScreen(
        "",
        "Uploaded. Restarting to install...",
        "Keep power on. Back in about a minute."
      );

    } else if (currentPortalUploadKind == 2) {
      snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Festival dates updated");
      updateActiveThemeForToday(true);
      applyTheme(true);
      updateClock();

      showFirmwarePortalScreen(
        "",
        "Festival dates updated",
        "Schedule updated. No reboot needed."
      );

    } else if (currentPortalUploadKind == 3) {
      snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Asset updated");

      // If the currently displayed background was updated, reload it once.
      if (strcmp(currentBackgroundPath, portalUploadPath) == 0) {
        currentBackgroundPath[0] = '\0';
        loadRGB565Background(portalUploadPath);
      }

      showFirmwarePortalScreen(
        portalUploadPath,
        "Background updated",
        "No reboot required."
      );
    }

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (firmwareUploadFile) {
      firmwareUploadFile.close();
    }

    if (portalUploadPath[0] != '\0' && SD.exists(portalUploadPath)) {
      SD.remove(portalUploadPath);
    }

    firmwareAutoInstallPending = false;
    snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Upload aborted");
    showFirmwarePortalScreen("", "Upload failed. Try again.", "Nothing was installed.");
  }
}

void startFirmwareUploadPortal() {
  wakeScreen();

  if (!sdMounted) {
    snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "SD not ready");
    showFirmwarePortalScreen("", "SD not ready", "Check SD card.");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "WiFi not connected");
    showFirmwarePortalScreen("", "WiFi not connected", "Connect WiFi.");
    return;
  }

  if (!firmwareUploadPortalActive) {
    firmwareUploadServer.on("/", HTTP_GET, handleFirmwareUploadPage);
    firmwareUploadServer.on("/update", HTTP_GET, handleFirmwareUploadPage);
    firmwareUploadServer.on("/upload-fw", HTTP_POST, handleUploadPost, handlePortalUploadStream);
    firmwareUploadServer.on("/upload-festivals", HTTP_POST, handleUploadPost, handlePortalUploadStream);
    firmwareUploadServer.on("/upload-bg", HTTP_POST, handleUploadPost, handlePortalUploadStream);
    firmwareUploadServer.begin();
    firmwareUploadPortalActive = true;
  }

  firmwareUploadPortalStartedAt = millis();
  firmwareAutoInstallPending = false;

  IPAddress ip = WiFi.localIP();
  char url[48];
  snprintf(url, sizeof(url), "http://%u.%u.%u.%u/update", ip[0], ip[1], ip[2], ip[3]);

  snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Upload open: %s", url);

  showFirmwarePortalScreen(
    url,
    "Waiting for upload...",
    ""
  );
}

void stopFirmwareUploadPortal() {
  if (!firmwareUploadPortalActive) return;

  firmwareUploadServer.stop();
  firmwareUploadPortalActive = false;
  snprintf(firmwareUploadStatus, sizeof(firmwareUploadStatus), "Upload portal closed");
}

void updateFirmwareUploadPortal() {
  if (firmwareUploadPortalActive) {
    firmwareUploadServer.handleClient();

    if (millis() - firmwareUploadPortalStartedAt >= FIRMWARE_UPLOAD_PORTAL_MS && !firmwareAutoInstallPending) {
      stopFirmwareUploadPortal();
    }
  }

  if (firmwareAutoInstallPending &&
      millis() - firmwareAutoInstallStartedAt >= 5000UL) {
    firmwareAutoInstallPending = false;
    stopFirmwareUploadPortal();
    logActivityEvent("UPDATE", "Portal upload saved, restarting to install");
    delay(300);
    ESP.restart();   // boot-time installer takes it from here
  }
}


void markSdUpdateCleanupPending() {
  prefs.begin("doorpanel", false);
  prefs.putBool("sd_upd_clean", true);
  prefs.end();
}

void cleanupSdUpdateAfterSuccessfulBoot() {
  prefs.begin("doorpanel", false);
  bool shouldClean = prefs.getBool("sd_upd_clean", false);

  if (shouldClean && sdMounted) {
    if (SD.exists(SD_UPDATE_FIRMWARE_PATH)) {
      SD.remove(SD_UPDATE_FIRMWARE_PATH);
    }

    prefs.putBool("sd_upd_clean", false);
  }

  prefs.end();
}

bool installFirmwareFromSD() {
  if (firmwareUpdateRunning) {
    return false;
  }

  wakeScreen();

  if (!sdMounted) {
    snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "SD not ready");
    return false;
  }

  if (!SD.exists(SD_UPDATE_FIRMWARE_PATH)) {
    snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "No firmware.bin");
    return false;
  }

  File updateFile = SD.open(SD_UPDATE_FIRMWARE_PATH, FILE_READ);
  if (!updateFile) {
    snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "Open failed");
    return false;
  }

  size_t updateSize = updateFile.size();
  if (updateSize < 1024) {
    updateFile.close();
    snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "Invalid update file");
    return false;
  }

  firmwareUpdateRunning = true;
  fwStageMark(4);
  snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "Installing %uk", (unsigned)(updateSize / 1024));
  showFirmwarePortalScreen(
    "",
    "Updating. Screen will turn off.",
    "Do not power off. Panel will restart."
  );

  // Let the message render once, then turn the backlight off before flash writing.
  // This prevents the ugly RGB panel glitching during OTA flash writes.
  delay(1200);
  lv_timer_handler();
  setBacklight(0);
  delay(150);
  // Safety: all controlled outputs OFF before flashing.
  allOutputsOff();
  updateCeilingUI();
  updatePanelUI();

  if (!Update.begin(updateSize, U_FLASH)) {
    updateFile.close();
    firmwareUpdateRunning = false;
    snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "Begin failed: %s", Update.errorString());
    setBacklight(getSelectedBrightness());
    showFirmwarePortalScreen("", "Update failed", "Check firmware.bin.");
    return false;
  }

  size_t written = Update.writeStream(updateFile);
  updateFile.close();

  if (written != updateSize) {
    firmwareUpdateRunning = false;
    snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "Write failed at %uK of %uK: %s",
             (unsigned)(written / 1024), (unsigned)(updateSize / 1024), Update.errorString());
    setBacklight(getSelectedBrightness());
    showFirmwarePortalScreen("", "Write failed", "Check SD/firmware.");
    return false;
  }

  if (!Update.end(true)) {
    firmwareUpdateRunning = false;
    snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "End failed: %s", Update.errorString());
    setBacklight(getSelectedBrightness());
    showFirmwarePortalScreen("", "Finish failed", "Try again.");
    return false;
  }

  if (!Update.isFinished()) {
    firmwareUpdateRunning = false;
    snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "Incomplete update");
    setBacklight(getSelectedBrightness());
    showFirmwarePortalScreen("", "Incomplete update", "Try again.");
    return false;
  }

  snprintf(firmwareUpdateStatus, sizeof(firmwareUpdateStatus), "Update OK, rebooting");

  // Delete firmware.bin only after the new firmware boots successfully.
  markSdUpdateCleanupPending();
  fwStageMark(5);

  delay(700);
  ESP.restart();
  return true;
}

void adminSdUpdateCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (firmwareUpdateRunning) return;
  startFirmwareUploadPortal();
}


// =====================================================
// Settings screen
// Lazy-created on open, fully lv_obj_del'd on close -- exists in RAM
// only while a person is actually using it, exactly like the PIN pad.
// Four flat "pill" section switches (Motion/Display/Network/Security)
// swap which row-group is visible; nothing scrolls. Every row commits
// to NVS immediately via the existing saveXxx() helpers from Step 1 --
// there is no Save button anywhere on this screen.
// =====================================================

static lv_obj_t *settingsScreen = nullptr;
static lv_obj_t *settingsPills[4] = { nullptr, nullptr, nullptr, nullptr };
static lv_obj_t *settingsPanels[4] = { nullptr, nullptr, nullptr, nullptr };
enum SettingsTab { TAB_MOTION = 0, TAB_DISPLAY = 1, TAB_NETWORK = 2, TAB_SECURITY = 3 };

// Value labels that need live re-text when their row's value changes.
static lv_obj_t *lblMotionHold = nullptr;
static lv_obj_t *lblPirSens = nullptr;
static lv_obj_t *lblManualCooldown = nullptr;
static lv_obj_t *lblAutoTarget = nullptr;
static lv_obj_t *lblAutoDim = nullptr;
static lv_obj_t *lblBrightnessRow = nullptr;
static lv_obj_t *lblWelcomeDuration = nullptr;
static lv_obj_t *toggleMotionKnob = nullptr;
static lv_obj_t *toggleMotionTrack = nullptr;

// ---- Cycle-tier tables (kept tiny and constant -- no dynamic alloc) ----
const unsigned long MOTION_HOLD_TIERS[]  = {5000, 10000, 20000, 30000, 60000};
const uint8_t MOTION_HOLD_TIER_COUNT = sizeof(MOTION_HOLD_TIERS) / sizeof(MOTION_HOLD_TIERS[0]);

// PIR sensitivity tiers -- raw consecutive 50ms HIGH samples required
// before motion is confirmed (the original, stable v1 detection logic --
// see updateMotionSensor()). Lower sample count = triggers faster = more
// sensitive. Medium (10 samples / 500ms) matches v1's fixed factory
// default exactly, so a unit that has never touched this setting behaves
// identically to the original stable firmware.
const uint8_t PIR_TIERS[] = {30, 20, 10}; // raw consecutive 50ms samples (1000ms / 500ms / 250ms)
const char *PIR_TIER_NAMES[] = {"Low", "Medium", "High"};
const uint8_t PIR_TIER_COUNT = sizeof(PIR_TIERS) / sizeof(PIR_TIERS[0]);

const unsigned long MANUAL_COOLDOWN_TIERS[] = {5000, 10000, 15000, 30000, 60000};
const uint8_t MANUAL_COOLDOWN_TIER_COUNT = sizeof(MANUAL_COOLDOWN_TIERS) / sizeof(MANUAL_COOLDOWN_TIERS[0]);

// Auto-dim: 0 is used internally to mean "Never" (see uiDimAfterMs usage in
// updateScreenDimming) by setting an effectively-unreachable large value
// instead of true 0, so no existing comparison logic needs to change.
const unsigned long AUTO_DIM_TIERS[] = {5000, 10000, 15000, 30000, 60000, 0xFFFFFFFFUL};
const uint8_t AUTO_DIM_TIER_COUNT = sizeof(AUTO_DIM_TIERS) / sizeof(AUTO_DIM_TIERS[0]);

const unsigned long WELCOME_DURATION_TIERS[] = {2000, 4000, 6000, 8000};
const uint8_t WELCOME_DURATION_TIER_COUNT = sizeof(WELCOME_DURATION_TIERS) / sizeof(WELCOME_DURATION_TIERS[0]);

uint8_t tierIndexULong(unsigned long value, const unsigned long *tiers, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (tiers[i] == value) return i;
  }
  return 0;
}

uint8_t tierIndexU8(uint8_t value, const uint8_t *tiers, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (tiers[i] == value) return i;
  }
  return 0;
}

void formatSecondsLabel(char *buf, size_t len, unsigned long ms) {
  if (ms == 0xFFFFFFFFUL) {
    snprintf(buf, len, "Never");
  } else {
    snprintf(buf, len, "%lu sec", ms / 1000UL);
  }
}

// ---- Row value refreshers ----

void refreshMotionHoldLabel() {
  char buf[16];
  formatSecondsLabel(buf, sizeof(buf), motionHoldTimeMs);
  if (lblMotionHold) lv_label_set_text(lblMotionHold, buf);
}

void refreshPirSensLabel() {
  uint8_t idx = tierIndexU8(motionConfirmHighSamples, PIR_TIERS, PIR_TIER_COUNT);
  if (lblPirSens) lv_label_set_text(lblPirSens, PIR_TIER_NAMES[idx]);
}

void refreshManualCooldownLabel() {
  char buf[16];
  formatSecondsLabel(buf, sizeof(buf), autoManualOffCooldownMs);
  if (lblManualCooldown) lv_label_set_text(lblManualCooldown, buf);
}

void refreshAutoTargetLabel() {
  if (lblAutoTarget) {
    lv_label_set_text(lblAutoTarget, autoLightTarget == AUTO_TARGET_CEILING ? "Ceiling" : "Panel");
  }
}

void refreshAutoDimLabel() {
  char buf[16];
  formatSecondsLabel(buf, sizeof(buf), uiDimAfterMs);
  if (lblAutoDim) lv_label_set_text(lblAutoDim, buf);
}

void refreshBrightnessRowLabel() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u / 7", brightnessLevel + 1);
  if (lblBrightnessRow) lv_label_set_text(lblBrightnessRow, buf);
}

void refreshWelcomeDurationLabel() {
  char buf[16];
  formatSecondsLabel(buf, sizeof(buf), welcomeBellGraceMs);
  if (lblWelcomeDuration) lv_label_set_text(lblWelcomeDuration, buf);
}

void refreshMotionToggleVisual() {
  if (!toggleMotionTrack || !toggleMotionKnob) return;
  if (motionDetectionEnabled) {
    lv_obj_set_style_bg_color(toggleMotionTrack, lv_color_hex(0x22B88C), 0);
    lv_obj_align(toggleMotionKnob, LV_ALIGN_RIGHT_MID, -3, 0);
  } else {
    lv_obj_set_style_bg_color(toggleMotionTrack, lv_color_hex(0xE9DFC6), 0);
    lv_obj_align(toggleMotionKnob, LV_ALIGN_LEFT_MID, 3, 0);
  }
}

// ---- Cycle-row callbacks: each settings row is two small chevron
// buttons flanking a value label. user_data encodes direction (+1/-1)
// packed as intptr to avoid any extra heap allocation per row. ----

void cycleMotionHoldCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  uint8_t idx = tierIndexULong(motionHoldTimeMs, MOTION_HOLD_TIERS, MOTION_HOLD_TIER_COUNT);
  idx = (idx + dir + MOTION_HOLD_TIER_COUNT) % MOTION_HOLD_TIER_COUNT;
  motionHoldTimeMs = MOTION_HOLD_TIERS[idx];
  saveMotionHoldTimeMs();
  refreshMotionHoldLabel();
}

void cyclePirSensCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  uint8_t idx = tierIndexU8(motionConfirmHighSamples, PIR_TIERS, PIR_TIER_COUNT);
  idx = (idx + dir + PIR_TIER_COUNT) % PIR_TIER_COUNT;
  motionConfirmHighSamples = PIR_TIERS[idx];
  saveMotionConfirmHighSamples();
  refreshPirSensLabel();
}

void cycleManualCooldownCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  uint8_t idx = tierIndexULong(autoManualOffCooldownMs, MANUAL_COOLDOWN_TIERS, MANUAL_COOLDOWN_TIER_COUNT);
  idx = (idx + dir + MANUAL_COOLDOWN_TIER_COUNT) % MANUAL_COOLDOWN_TIER_COUNT;
  autoManualOffCooldownMs = MANUAL_COOLDOWN_TIERS[idx];
  saveAutoManualOffCooldownMs();
  refreshManualCooldownLabel();
}

// Reassigns which physical light motion detection drives. Gives both
// lights a clean slate rather than leaving either one holding stale
// AUTO/MANUAL state that no longer applies to it:
//   - the light losing AUTO duty is simply turned off (it becomes a
//     plain manual light -- any manual-mode timers on it no longer apply)
//   - the light gaining AUTO duty starts back at AUTO/off and requires
//     one fresh motion clear -> trigger cycle before it can turn on, so
//     it doesn't immediately snap on from stale motion state
void applyAutoLightTargetChange(AutoLightTarget newTarget) {
  if (newTarget == autoLightTarget) return;

  // Cleanly park whichever light is about to lose AUTO duty.
  applyAutoOutput(false);
  updateAutoLightUI();

  autoLightTarget = newTarget;

  autoMode = AUTO_MODE_AUTO;
  autoManualStartedAt = 0;
  autoManualOffStartedAt = 0;
  applyAutoOutput(false);
  clearMotionAutoHold();
  motionRearmAfterManualOffClear = true;

  updateCeilingUI();
  updatePanelUI();
}

void cycleAutoTargetCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  AutoLightTarget newTarget = (autoLightTarget == AUTO_TARGET_CEILING) ? AUTO_TARGET_PANEL : AUTO_TARGET_CEILING;
  applyAutoLightTargetChange(newTarget);
  saveAutoLightTarget();
  refreshAutoTargetLabel();
}

void cycleAutoDimCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  uint8_t idx = tierIndexULong(uiDimAfterMs, AUTO_DIM_TIERS, AUTO_DIM_TIER_COUNT);
  idx = (idx + dir + AUTO_DIM_TIER_COUNT) % AUTO_DIM_TIER_COUNT;
  uiDimAfterMs = AUTO_DIM_TIERS[idx];
  saveUiDimAfterMs();
  refreshAutoDimLabel();
}

void cycleBrightnessRowCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  int8_t lvl = (int8_t)brightnessLevel + dir;
  if (lvl < 0) lvl = 6;
  if (lvl > 6) lvl = 0;
  brightnessLevel = (uint8_t)lvl;
  applyBrightnessLevel();
  saveBrightnessLevel();
  refreshBrightnessRowLabel();
}

void cycleWelcomeDurationCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  uint8_t idx = tierIndexULong(welcomeBellGraceMs, WELCOME_DURATION_TIERS, WELCOME_DURATION_TIER_COUNT);
  idx = (idx + dir + WELCOME_DURATION_TIER_COUNT) % WELCOME_DURATION_TIER_COUNT;
  welcomeBellGraceMs = WELCOME_DURATION_TIERS[idx];
  saveWelcomeBellGraceMs();
  refreshWelcomeDurationLabel();
}

void toggleMotionDetectionCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  motionDetectionEnabled = !motionDetectionEnabled;
  saveMotionDetectionEnabled();
  refreshMotionToggleVisual();
}

void settingsAllOffCb(lv_event_t *e) {
  // Reuses the existing admin "all outputs off" action -- no new logic.
  adminAllOffCb(e);
}

void settingsChangePinCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  // Re-enter the PIN pad directly in "set new PIN" mode -- reuses the
  // exact same widget and saveSettingsPin() call as first-run setup and
  // the recovery flow, just entered from inside Settings instead.
  pinPadOpenedFromSettings = true;
  showPinPad(false, true);
  lv_label_set_text(pinPadTitle, "Change PIN");
  lv_label_set_text(pinPadSub, "Choose a new 4-digit PIN");
}

void settingsBackCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  destroySettingsScreen();
  lv_scr_load(mainScreen);
}

void settingsDiagCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  destroySettingsScreen();  // free settings RAM now instead of holding it while Diagnostics is open
  showProdDiagScreen();
}

void settingsLogsCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  destroySettingsScreen();  // same reasoning as Diagnostics -- don't hold Settings RAM while Logs is open
  showLogsScreen();
}

// ─── Production Diagnostics Screen ────────────────────────────────────────────
// Lazy-created when opened from Settings, fully destroyed on Back/Reboot.
// Two-column card grid, plain-language status, no pre-prod jargon visible.
// Everything the old hidden developer row carried (RSSI, heap totals) is now
// in the visible rows, so there is no secret tap target any more.

static lv_obj_t *prodDiagScreen = NULL;

static void destroyProdDiagScreen() {
  if (prodDiagScreen) {
    // Async delete -- see destroyPinPad() for why. prodDiagBackCb fires
    // from a child of prodDiagScreen, so deleting it synchronously here
    // was the other half of the reboot-on-Back crash.
    lv_obj_del_async(prodDiagScreen);
    prodDiagScreen = NULL;
  }
}

static void prodDiagBackCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  destroyProdDiagScreen();
  showSettingsScreen();
}

static void prodDiagRebootCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  destroyProdDiagScreen();
  adminRestartCb(e);
}


// Helper: create a card container at (x,y) with given w/h
// Helper: card background
static lv_obj_t *makeDiagCard(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                               lv_coord_t w, lv_coord_t h) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, w, h);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  // Elevated "floating card" look: near-white fill, hairline border, and a
  // soft warm drop shadow. The shadow renders once when the (static) screen
  // is drawn, so there is no ongoing cost. On a washed TFT the defined edge
  // + shadow is what keeps the tiles readable head-on, without resorting to
  // a darker background.
  lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFDF6), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xE3D3AC), 0);
  lv_obj_set_style_shadow_width(card, 18, 0);
  lv_obj_set_style_shadow_ofs_y(card, 4, 0);
  lv_obj_set_style_shadow_opa(card, 70, 0);
  lv_obj_set_style_shadow_color(card, lv_color_hex(0x6B5B36), 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 16, 0);
  return card;
}

// Helper: card section header — plain text label. (Previously paired with
// an LV_SYMBOL_* icon, but those glyphs weren't rendering on this hardware's
// font build -- showing as blank boxes -- so the icon was dropped entirely
// rather than chase a font/glyph-table mismatch.)
static lv_obj_t *makeDiagCardHeader(lv_obj_t *card, const char *text) {
  lv_obj_t *lbl = lv_label_create(card);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, FONT_18, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x7E6E54), 0);
  lv_obj_set_pos(lbl, 0, 0);
  return lbl;
}

// Helper: one status row — label on left, value on right
// Row tone: facts are plain ink; only a verdict gets colour. Green for a
// confirmed-good state, red for a problem. Numbers (addresses, sizes,
// temperatures) stay neutral so the eye lands only on what needs it.
enum { DIAG_PLAIN = 0, DIAG_GOOD = 1, DIAG_BAD = 2 };

static void makeDiagRow(lv_obj_t *card, const char *label, const char *value,
                        uint8_t tone, lv_coord_t y) {
  lv_obj_t *lbl = lv_label_create(card);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_font(lbl, FONT_18, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x6E6456), 0);
  lv_obj_set_pos(lbl, 0, y);

  lv_obj_t *val = lv_label_create(card);
  lv_label_set_text(val, value);
  lv_obj_set_style_text_font(val, FONT_18, 0);
  lv_obj_set_style_text_color(val,
      tone == DIAG_GOOD ? lv_color_hex(0x1F7A4D) :
      tone == DIAG_BAD  ? lv_color_hex(0xB8322B) :
                          lv_color_hex(0x2E2A24), 0);
  lv_obj_align(val, LV_ALIGN_TOP_RIGHT, 0, y);
}

// Plain-language reset cause for the System card. Anything other than a
// power-on or a deliberate restart is flagged, because an unexplained
// restart is the one thing a wall panel should never hide.
static const char *resetReasonText(bool *good) {
  *good = true;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_SW:        return "Software restart";
    case ESP_RST_EXT:       return "Reset pin";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_PANIC:     *good = false; return "Crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       *good = false; return "Watchdog";
    case ESP_RST_BROWNOUT:  *good = false; return "Brownout (power dip)";
    default:                return "Unknown";
  }
}

void showProdDiagScreen() {
  destroyProdDiagScreen();
  wakeScreen();

  // Force a fresh probe before reading the cached booleans below -- without
  // this, the very first open of this screen reads whatever was cached at
  // boot (both default to false), which is exactly why Touch/I2C showed as
  // "Check connection" even though they were working fine. The periodic
  // refresh in updateDiagnosticsIfVisible() only kicks in once this screen
  // is already the active one, so it never gets a chance to fix the very
  // first render.
  updateDiagHardwareCache(true);

  // Memory is read BEFORE this screen's own widgets exist, so the figures
  // describe the panel as it normally runs, not the panel plus this page.
  uint32_t heapTotal = ESP.getHeapSize();
  uint32_t heapFree  = ESP.getFreeHeap();
  uint32_t heapMin   = ESP.getMinFreeHeap();
  uint32_t psTotal   = ESP.getPsramSize();
  uint32_t psFree    = ESP.getFreePsram();

  prodDiagScreen = lv_obj_create(NULL);
  lv_obj_clear_flag(prodDiagScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(prodDiagScreen, lv_color_hex(0xF6E9CC), 0);

  // ── Top bar ───────────────────────────────────────────────────────────────
  createBackButton(prodDiagScreen, 18, 16, prodDiagBackCb);
  createRebootButton(prodDiagScreen, 646, 16, prodDiagRebootCb);

  // ── Card grid — 2 columns × 2 rows ───────────────────────────────────────
  // 32 + 362 + 12 + 362 + 32 = 800. Four rows per card on a 34px pitch,
  // header set apart by a wider first gap; five rows only on System.
  const lv_coord_t CX = 32, CY = 64, CW = 362, CH = 196, GAP = 12;
  const lv_coord_t R0 = 30, RP = 33;

  bool connected = (WiFi.status() == WL_CONNECTED);

  // ── Connection (top-left) ─────────────────────────────────────────────────
  {
    lv_obj_t *card = makeDiagCard(prodDiagScreen, CX, CY, CW, CH);
    makeDiagCardHeader(card, "Connection");

    int rssi = connected ? WiFi.RSSI() : 0;
    char wifiVal[40];
    if (!connected)       snprintf(wifiVal, sizeof(wifiVal), "Not connected");
    else if (rssi >= -60) snprintf(wifiVal, sizeof(wifiVal), "Strong, %d dBm", rssi);
    else if (rssi >= -75) snprintf(wifiVal, sizeof(wifiVal), "Good, %d dBm", rssi);
    else                  snprintf(wifiVal, sizeof(wifiVal), "Weak, %d dBm", rssi);
    makeDiagRow(card, "Wi-Fi signal", wifiVal,
                !connected ? DIAG_BAD : rssi >= -75 ? DIAG_GOOD : DIAG_PLAIN, R0);

    char netBuf[34] = "--";
    if (connected) WiFi.SSID().toCharArray(netBuf, sizeof(netBuf));
    makeDiagRow(card, "Network", netBuf, DIAG_PLAIN, R0 + RP);

    char ipBuf[18] = "--";
    if (connected) WiFi.localIP().toString().toCharArray(ipBuf, sizeof(ipBuf));
    makeDiagRow(card, "Address", ipBuf, DIAG_PLAIN, R0 + RP * 2);

    // The command channel from the phone app / control page.
    char linkBuf[32];
    uint8_t linkTone;
    if (remoteLinkState == 2) {
      if (statListenerReconnects) snprintf(linkBuf, sizeof(linkBuf), "Connected, %u drops", (unsigned)statListenerReconnects);
      else                        snprintf(linkBuf, sizeof(linkBuf), "Connected");
      linkTone = DIAG_GOOD;
    } else if (remoteLinkState == 1) { snprintf(linkBuf, sizeof(linkBuf), "Reconnecting"); linkTone = DIAG_BAD; }
    else                             { snprintf(linkBuf, sizeof(linkBuf), "Not set up");   linkTone = DIAG_PLAIN; }
    makeDiagRow(card, "Phone app", linkBuf, linkTone, R0 + RP * 3);
  }

  // ── Sensors (top-right) ───────────────────────────────────────────────────
  {
    lv_obj_t *card = makeDiagCard(prodDiagScreen, CX + CW + GAP, CY, CW, CH);
    makeDiagCardHeader(card, "Sensors");

    makeDiagRow(card, "Motion sensor", motionStableState ? "Working (motion now)" : "Working",
                DIAG_GOOD, R0);
    makeDiagRow(card, "Touch screen",
                cachedGt911Online ? "Working" : "Check connection",
                cachedGt911Online ? DIAG_GOOD : DIAG_BAD, R0 + RP);
    makeDiagRow(card, "Switch driver",
                cachedPcfOnline ? "Working" : "Not responding, power-cycle",
                cachedPcfOnline ? DIAG_GOOD : DIAG_BAD, R0 + RP * 2);

    // Shared sensor bus: how often it had to be re-initialised. Zero is
    // normal; anything else is the number to quote if touch ever went dead.
    char busBuf[32];
    if (i2cResetCount == 0 && touchRejectCount == 0) snprintf(busBuf, sizeof(busBuf), "Healthy");
    else snprintf(busBuf, sizeof(busBuf), "%u resets, %u noise", (unsigned)i2cResetCount, (unsigned)touchRejectCount);
    makeDiagRow(card, "Sensor bus", busBuf, (i2cResetCount == 0 && touchRejectCount < 20) ? DIAG_GOOD : DIAG_BAD, R0 + RP * 3);
  }

  // ── Memory (bottom-left) ──────────────────────────────────────────────────
  {
    lv_obj_t *card = makeDiagCard(prodDiagScreen, CX, CY + CH + GAP, CW, CH);
    makeDiagCardHeader(card, "Memory");

    // Internal RAM: the fast on-chip memory Wi-Fi, secure connections and
    // the screen all need. "Used / total", then the tightest it has been.
    char ramBuf[32];
    snprintf(ramBuf, sizeof(ramBuf), "%u / %u KB used",
             (unsigned)((heapTotal - heapFree) / 1024), (unsigned)(heapTotal / 1024));
    makeDiagRow(card, "Internal RAM", ramBuf, heapFree < 50000 ? DIAG_BAD : DIAG_PLAIN, R0);

    char lowBuf[24];
    snprintf(lowBuf, sizeof(lowBuf), "%u KB free", (unsigned)(heapMin / 1024));
    makeDiagRow(card, "Lowest free RAM", lowBuf, heapMin < 25000 ? DIAG_BAD : DIAG_PLAIN, R0 + RP);

    // PSRAM: the 8MB side chip holding the screen image, backgrounds and
    // downloads.
    char psBuf[32];
    if (psTotal) snprintf(psBuf, sizeof(psBuf), "%.1f / %.1f MB used",
                          (psTotal - psFree) / 1048576.0f, psTotal / 1048576.0f);
    else         snprintf(psBuf, sizeof(psBuf), "Not found");
    makeDiagRow(card, "PSRAM", psBuf, psTotal ? DIAG_PLAIN : DIAG_BAD, R0 + RP * 2);

    char sdBuf[28];
    uint8_t sdTone = DIAG_BAD;
    if (!sdMounted) snprintf(sdBuf, sizeof(sdBuf), "Not found");
    else {
      uint64_t totalMB = SD.cardSize() / (1024ULL * 1024ULL);
      uint64_t usedMB  = SD.usedBytes() / (1024ULL * 1024ULL);
      uint64_t freeMB  = totalMB > usedMB ? totalMB - usedMB : 0;
      if (freeMB >= 1024) snprintf(sdBuf, sizeof(sdBuf), "OK, %.1f GB free", freeMB / 1024.0f);
      else                snprintf(sdBuf, sizeof(sdBuf), "OK, %llu MB free", freeMB);
      sdTone = DIAG_GOOD;
    }
    makeDiagRow(card, "SD card", sdBuf, sdTone, R0 + RP * 3);
  }

  // ── System (bottom-right) ─────────────────────────────────────────────────
  {
    lv_obj_t *card = makeDiagCard(prodDiagScreen, CX + CW + GAP, CY + CH + GAP, CW, CH);
    makeDiagCardHeader(card, "System");
    const lv_coord_t R0s = 28, RPs = 31;   // five rows: slightly tighter

    char fwBuf[24];
    snprintf(fwBuf, sizeof(fwBuf), "v%s", FW_VERSION);
    makeDiagRow(card, "Firmware", fwBuf, DIAG_PLAIN, R0s);

    unsigned long secs = millis() / 1000UL;
    unsigned long mins = secs / 60; secs %= 60;
    unsigned long hrs  = mins / 60; mins %= 60;
    unsigned long days = hrs  / 24; hrs  %= 24;
    char uptimeBuf[32];
    if (days > 0) snprintf(uptimeBuf, sizeof(uptimeBuf), "%lu d %lu h", days, hrs);
    else          snprintf(uptimeBuf, sizeof(uptimeBuf), "%lu h %lu min", hrs, mins);
    makeDiagRow(card, "Running for", uptimeBuf, DIAG_PLAIN, R0s + RPs);

    bool rstGood;
    const char *rstStr = resetReasonText(&rstGood);
    makeDiagRow(card, "Last restart", rstStr, rstGood ? DIAG_PLAIN : DIAG_BAD, R0s + RPs * 2);

    float chipTempC = temperatureRead();
    char  tempBuf[20];
    snprintf(tempBuf, sizeof(tempBuf), "%.0f C", chipTempC);
    makeDiagRow(card, "Chip temperature", tempBuf, chipTempC < 80.0f ? DIAG_PLAIN : DIAG_BAD, R0s + RPs * 3);

    // One line for updates: what is staged on the card wins over what the
    // last check said, because it is what happens at the next restart.
    const char *updStr; uint8_t updTone;
    if (sdMounted && SD.exists(SD_UPDATE_FIRMWARE_PATH))    { updStr = "Installs on restart";  updTone = DIAG_GOOD; }
    else if (sdMounted && SD.exists(SD_UPDATE_FAILED_PATH)) { updStr = "Last install failed";  updTone = DIAG_BAD; }
    else {
      updStr  = fwCheckSummary;
      updTone = strstr(fwCheckSummary, "failed") ? DIAG_BAD :
                strstr(fwCheckSummary, "available") ? DIAG_GOOD : DIAG_PLAIN;
    }
    makeDiagRow(card, "Updates", updStr, updTone, R0s + RPs * 4);
  }

  lv_scr_load(prodDiagScreen);
}


void settingsRebootCb(lv_event_t *e) {
  adminRestartCb(e);
}

// ---- Factory reset ----
// Wipes both NVS namespaces the app uses (settings/WiFi/PIN/ntfy in
// "doorpanel", cached weather/location in "weather") and reboots. After
// restart, nvsWifiSsid will be empty again so setup() drops straight back
// into the first-boot SoftAP/QR provisioning portal, same as a brand-new
// unit -- no separate "wipe" code path needed elsewhere.
void wipeFactoryDefaults() {
  prefs.begin("doorpanel", false);
  prefs.clear();
  prefs.end();

  prefs.begin("weather", false);
  prefs.clear();
  prefs.end();

  ESP.restart();
}

// Two-tap arm/confirm guard so a single stray tap can't wipe the unit.
// First tap relabels the button; a second tap within the window actually
// wipes. Reuses the button's own label (set via createSettingsActionBtn)
// instead of adding a popup/dialog widget.
bool factoryResetArmed = false;
unsigned long factoryResetArmedAt = 0;
const unsigned long FACTORY_RESET_CONFIRM_WINDOW_MS = 4000UL;
lv_obj_t *factoryResetLabel = nullptr;

// "Send test" button on Settings > Network > ntfy row -- lets the person
// confirm a notification actually leaves the device without needing to
// trigger the doorbell or walk in front of the motion sensor.
lv_obj_t *ntfyTestLabel = nullptr;
unsigned long ntfyTestSentAt = 0;
const unsigned long NTFY_TEST_LABEL_RESET_MS = 4000UL;

void factoryResetCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  if (!factoryResetArmed) {
    factoryResetArmed = true;
    factoryResetArmedAt = millis();
    if (factoryResetLabel) lv_label_set_text(factoryResetLabel, "Tap again to confirm");
    return;
  }

  wipeFactoryDefaults();
}

// Called from loop() -- if the confirm tap doesn't come in time, drop back
// to the normal label instead of leaving the button armed indefinitely.
void updateFactoryResetArmTimeout() {
  if (factoryResetArmed && millis() - factoryResetArmedAt > FACTORY_RESET_CONFIRM_WINDOW_MS) {
    factoryResetArmed = false;
    if (factoryResetLabel) lv_label_set_text(factoryResetLabel, "Factory Reset");
  }
}

void settingsNtfyTestCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  // This is a manual, explicit "did that actually work?" check -- worth
  // blocking the UI for up to ~2s to give a real answer instead of
  // firing-and-forgetting and lying about the result.
  if (ntfyTestLabel) lv_label_set_text(ntfyTestLabel, "Sending...");
  for (int i = 0; i < 3; i++) {
    lv_timer_handler();
    delay(10);
  }
  lv_refr_now(NULL);

  int httpCode = doPushNotificationRequest(
    "Test",
    "Test notification from your panel -- if you see this, ntfy delivery is working.",
    "default", "");

  char resultBuf[24];
  if (httpCode == 200) {
    snprintf(resultBuf, sizeof(resultBuf), "Sent OK");
  } else if (httpCode == -100) {
    snprintf(resultBuf, sizeof(resultBuf), "No Wi-Fi");
  } else if (httpCode == -101) {
    snprintf(resultBuf, sizeof(resultBuf), "No topic set");
  } else if (httpCode < 0) {
    // Never got a response at all: DNS failure, TLS handshake failure,
    // connection refused/timed out. Network/connectivity problem, not a
    // topic-name problem -- check internet access, not the topic field.
    snprintf(resultBuf, sizeof(resultBuf), "No response");
  } else {
    // ntfy answered, just not with 200 -- e.g. 400 for a malformed topic.
    snprintf(resultBuf, sizeof(resultBuf), "HTTP %d", httpCode);
  }

  if (ntfyTestLabel) lv_label_set_text(ntfyTestLabel, resultBuf);
  ntfyTestSentAt = millis();
}

// Reverts the "Send test" button label back from its result text a few
// seconds after tapping it.
void updateNtfyTestLabelTimeout() {
  if (ntfyTestSentAt != 0 && millis() - ntfyTestSentAt > NTFY_TEST_LABEL_RESET_MS) {
    ntfyTestSentAt = 0;
    if (ntfyTestLabel) lv_label_set_text(ntfyTestLabel, "Send test");
  }
}

void settingsWifiRetryCb(lv_event_t *e) {
  adminReconnectWiFiCb(e);
}

void settingsUpdateDeviceCb(lv_event_t *e) {
  adminSdUpdateCb(e);
}

// Both the Wi-Fi "Change" button and the ntfy "Set up" button open the
// same provisioning portal -- shared here instead of two identical
// inline lambdas at each call site.
void settingsOpenWifiPortalCb(lv_event_t *e) {
  destroySettingsScreen();
  showWifiPortalScreen();
}

void settingsPhoneSetupCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  wakeScreen();
  destroySettingsScreen();   // don't hold Settings RAM while this screen is open
  showPhoneSetupScreen(false);
}

// =====================================================
// WiFi Provisioning Portal
// Opens a SoftAP + tiny web form so the user can set
// SSID/password (and ntfy topic) from any phone browser.
// A QR code for the setup network is drawn on-screen.
// Triggered manually only: Settings -> Network -> "Change"
// =====================================================

static void handleWifiProvRoot() {
  // Serve the pre-scanned network list built when the portal opened.
  // WiFi.scanNetworks() is NOT called here -- it blocks for 5-10 seconds
  // and freezes the LVGL main loop (hangs the touchscreen). The scan runs
  // once up-front in showWifiPortalScreen() before lv_scr_load(), and its
  // results are stored in wifiScanOpts.
  String html = "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px;}"
    "h2{color:#1A9E72;}label{display:block;margin-top:16px;font-weight:bold;}"
    "input,select{width:100%;padding:10px;margin-top:6px;border:1px solid #ccc;border-radius:8px;box-sizing:border-box;font-size:16px;}"
    "button{margin-top:24px;width:100%;padding:14px;background:#1A9E72;color:white;border:none;border-radius:8px;font-size:18px;cursor:pointer;}"
    "</style></head><body>"
    "<h2>Panel WiFi Setup</h2>"
    "<form method='POST' action='/save'>"
    "<label>Select network</label>"
    "<select name='ssid'>" + wifiScanOpts + "</select>"
    "<label>Or type network name (hidden SSID)</label>"
    "<input name='ssid_manual' placeholder='Leave blank to use selection above'>"
    "<label>Password</label>"
    "<input type='password' name='pass'>"
    "<label>ntfy topic (optional)</label>"
    "<input name='ntfy' placeholder='e.g. doorbell-myhome' value='"
    + (nvsNtfyTopic.length() > 0 ? nvsNtfyTopic : String(NTFY_TOPIC)) + "'>"
    "<label>Control page URL (advanced, leave blank for default)</label>"
    "<input name='phurl' placeholder='https://...' value='" + nvsPhoneUrl + "'>"
    "<button type='submit'>Save &amp; Restart</button>"
    "</form></body></html>";

  wifiProvServer.send(200, "text/html", html);
}

static void handleWifiProvSave() {
  String ssid = wifiProvServer.arg("ssid_manual");
  if (ssid.length() == 0) ssid = wifiProvServer.arg("ssid");
  String pass = wifiProvServer.arg("pass");
  String ntfy = wifiProvServer.arg("ntfy");
  String phurl = wifiProvServer.arg("phurl");
  phurl.trim();

  wifiProvServer.send(200, "text/html",
    "<!DOCTYPE html><html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2 style='color:#1A9E72'>Saved!</h2>"
    "<p>The panel will now restart and connect to <strong>" + ssid + "</strong>.</p>"
    "</body></html>");

  delay(1500);

  // Save to NVS then reboot into STA mode.
  saveWifiCredentials(ssid, pass);
  if (ntfy.length() > 0) saveNtfyTopic(ntfy);

  // Control page override: only accepted as an https URL ending in '/',
  // and an empty field clears the override (back to the compiled default).
  if (phurl.length() == 0 ||
      (phurl.startsWith("https://") && phurl.endsWith("/") && phurl.length() < 120)) {
    nvsPhoneUrl = phurl;
    prefs.begin("doorpanel", false);
    prefs.putString("phUrl", phurl);
    prefs.end();
  }

  // Arm the "one last scan" step. The portal save always reboots, so the
  // Phone Setup screen is shown once on the next boot (after a topic exists)
  // and then disarms itself -- closing the setup story without turning this
  // handler into a wizard state machine.
  savePhoneSetupPending(true);

  WiFi.softAPdisconnect(true);
  ESP.restart();
}

static void wifiPortalSkipCb(lv_event_t *e) {
  // First-boot only: user wants to use the panel as a switchboard right now
  // and will set up WiFi later via Settings -> Network -> Change.
  // Setting this flag breaks the while(true) loop in setup().
  wifiPortalSkipPressed = true;
}

static void wifiPortalBackCb(lv_event_t *e) {
  destroyWifiPortalScreen();
  showSettingsScreen();
}

void destroyWifiPortalScreen() {
  wifiProvPortalActive = false;
  wifiProvServer.stop();
  if (wifiScanPending) {
    WiFi.scanDelete();   // safe to call even if scan already finished or wasn't running
    wifiScanPending = false;
  }
  wifiScanOpts = String();   // release the scan-list HTML; rebuilt on the next portal open
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);   // hand back to STA so startWiFi() works on next boot

  if (wifiPortalScreen) {
    lv_obj_del_async(wifiPortalScreen);
    wifiPortalScreen   = nullptr;
    wifiPortalQrCode   = nullptr;
    wifiPortalSsidLbl  = nullptr;
    wifiPortalStatusLbl = nullptr;
  }
  // No manual buffer free needed here: lv_qrcode is built on lv_canvas and
  // allocates/frees its own internal indexed-1bit pixel buffer via its own
  // constructor/destructor, which LVGL runs automatically as a child of
  // wifiPortalScreen when the screen above is deleted.
}

// =====================================================
// Phone Setup screen ("one last scan")
// =====================================================
// Closes the setup story: WiFi QR -> topic -> this. Scanning puts the four
// controls on the phone's home screen and hands over the notification topic
// in one step. Reachable forever from Settings > Network > "Phone setup" so
// a second family member, or a replaced phone, needs no factory reset.
//
// The QR payload is PHONE_UI_BASE_URL + "#t=" + this unit's topic, built at
// runtime -- identical firmware in every unit, the fragment does the per-unit
// work, and fragments are never sent to the host.

static void phoneSetupDoneCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  bool wasFirstRun = phoneSetupFirstRun;
  destroyPhoneSetupScreen();
  if (wasFirstRun) {
    savePhoneSetupPending(false);   // never auto-shown again
    lv_scr_load(mainScreen);
  } else {
    showSettingsScreen();
  }
}

void destroyPhoneSetupScreen() {
  if (phoneSetupScreen) {
    // Async delete: the handler that triggers this teardown is a child of
    // phoneSetupScreen (same reason as destroyPinPad/destroyLogsScreen).
    lv_obj_del_async(phoneSetupScreen);
    phoneSetupScreen = nullptr;
    phoneSetupQrCode = nullptr;
    // lv_qrcode owns and frees its indexed-1bit canvas buffer in its own
    // destructor, which LVGL runs as part of the screen teardown above.
  }
}

void showPhoneSetupScreen(bool firstRun) {
  if (phoneSetupScreen) destroyPhoneSetupScreen();
  if (settingsScreen) { lv_obj_del_async(settingsScreen); settingsScreen = nullptr; }

  phoneSetupFirstRun = firstRun;

  // Two aligned columns on a 32px grid. Left: words. Right: a QR card and,
  // directly beneath it on the same edges, the one button. The two columns
  // share a bottom edge (y = 432) so nothing floats.
  const lv_coord_t M      = 48;             // outer margin
  const lv_coord_t LEFT_W = 400;            // left column width
  const lv_coord_t RX     = 512, RW = 240;  // right column x / width
  const lv_coord_t QR     = 196, QR_PAD = 14;

  phoneSetupScreen = lv_obj_create(NULL);
  lv_obj_clear_flag(phoneSetupScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(phoneSetupScreen, lv_color_hex(PS_BG), 0);

  // ---- Left column ----
  lv_obj_t *title = lv_label_create(phoneSetupScreen);
  lv_label_set_text(title, "Phone Setup");
  lv_obj_set_style_text_font(title, FONT_30, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(PS_GOLD), 0);
  lv_obj_set_pos(title, M, 40);

  lv_obj_t *sub = lv_label_create(phoneSetupScreen);
  lv_label_set_text(sub, firstRun ? "One last scan and you are done."
                                  : "Add the controls to another phone.");
  lv_obj_set_style_text_font(sub, FONT_18, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(PS_MUTED), 0);
  lv_obj_set_pos(sub, M, 82);

  lv_obj_t *steps = lv_label_create(phoneSetupScreen);
  lv_label_set_text(steps,
    "1   Point your phone camera at the code.\n"
    "2   Tap Share, then Add to Home Screen.\n"
    "3   On that page, install the free \"ntfy\" app\n"
    "     and subscribe to the topic below for\n"
    "     doorbell and motion alerts.");
  lv_obj_set_style_text_font(steps, FONT_18, 0);
  lv_obj_set_style_text_color(steps, lv_color_hex(PS_INK), 0);
  lv_obj_set_style_text_line_space(steps, 9, 0);
  lv_label_set_long_mode(steps, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(steps, LEFT_W);
  lv_obj_set_pos(steps, M, 128);

  // Topic card: same face as the phone page's Notifications card.
  const char *activeTopic = nvsNtfyTopic.length() > 0 ? nvsNtfyTopic.c_str() : NTFY_TOPIC;
  lv_obj_t *topicCard = lv_obj_create(phoneSetupScreen);
  lv_obj_remove_style_all(topicCard);
  lv_obj_set_pos(topicCard, M, 328);
  lv_obj_set_size(topicCard, LEFT_W, 104);
  lv_obj_clear_flag(topicCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(topicCard, lv_color_hex(PS_CARD), 0);
  lv_obj_set_style_bg_opa(topicCard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(topicCard, 1, 0);
  lv_obj_set_style_border_color(topicCard, lv_color_hex(PS_LINE), 0);
  lv_obj_set_style_border_opa(topicCard, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(topicCard, 14, 0);
  lv_obj_set_style_pad_all(topicCard, 16, 0);

  lv_obj_t *topicCaption = lv_label_create(topicCard);
  lv_label_set_text(topicCaption, "Notification topic");
  lv_obj_set_style_text_font(topicCaption, FONT_18, 0);
  lv_obj_set_style_text_color(topicCaption, lv_color_hex(PS_MUTED), 0);
  lv_obj_set_pos(topicCaption, 0, 4);

  lv_obj_t *topicLbl = lv_label_create(topicCard);
  lv_label_set_text(topicLbl, (activeTopic[0] != '\0') ? activeTopic : "not set");
  lv_obj_set_style_text_font(topicLbl, FONT_26, 0);
  lv_obj_set_style_text_color(topicLbl, lv_color_hex(PS_BURGUNDY), 0);
  lv_label_set_long_mode(topicLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(topicLbl, LEFT_W - 32);
  lv_obj_set_pos(topicLbl, 0, 36);

  // ---- Right column: QR card ----
  // Resolve the base URL: NVS override wins, so a host/domain change stays
  // recoverable on units already in the field without a reflash.
  const char *baseUrl = nvsPhoneUrl.length() > 0 ? nvsPhoneUrl.c_str() : PHONE_UI_BASE_URL;
  bool urlConfigured = (baseUrl[0] != '\0') &&
                       (strstr(baseUrl, "panel.example.com") == nullptr);

  char qrPayload[160];
  snprintf(qrPayload, sizeof(qrPayload), "%s#t=%s", baseUrl, activeTopic);

  lv_obj_t *qrCard = lv_obj_create(phoneSetupScreen);
  lv_obj_remove_style_all(qrCard);
  lv_obj_set_pos(qrCard, RX, 40);
  lv_obj_set_size(qrCard, RW, 316);
  lv_obj_clear_flag(qrCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(qrCard, lv_color_hex(PS_CARD), 0);
  lv_obj_set_style_bg_opa(qrCard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(qrCard, 1, 0);
  lv_obj_set_style_border_color(qrCard, lv_color_hex(PS_LINE), 0);
  lv_obj_set_style_border_opa(qrCard, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(qrCard, 18, 0);
  lv_obj_set_style_pad_all(qrCard, 0, 0);

  bool qrOk = false;
  if (urlConfigured && activeTopic[0] != '\0') {
    phoneSetupQrCode = lv_qrcode_create(qrCard, QR, lv_color_black(), lv_color_white());
    if (phoneSetupQrCode) {
      qrOk = (lv_qrcode_update(phoneSetupQrCode, qrPayload, strlen(qrPayload)) == LV_RES_OK);
      if (qrOk) {
        lv_obj_set_style_border_color(phoneSetupQrCode, lv_color_white(), 0);
        lv_obj_set_style_border_width(phoneSetupQrCode, QR_PAD, 0);
        lv_obj_align(phoneSetupQrCode, LV_ALIGN_TOP_MID, 0, 20);
      } else {
        lv_obj_del(phoneSetupQrCode);   // never crash a sealed unit over a QR
        phoneSetupQrCode = nullptr;
      }
    }
  }

  lv_obj_t *qrCaption = lv_label_create(qrCard);
  if (qrOk) {
    lv_label_set_text(qrCaption, "Scan with your camera");
  } else {
    lv_label_set_text(qrCaption,
      !urlConfigured ? "Phone controls not\nconfigured on this firmware."
                     : "QR unavailable.");
  }
  lv_obj_set_style_text_font(qrCaption, FONT_18, 0);
  lv_obj_set_style_text_align(qrCaption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(qrCaption, lv_color_hex(PS_MUTED), 0);
  lv_obj_align(qrCaption, LV_ALIGN_BOTTOM_MID, 0, -22);

  // ---- Done / Back: burgundy primary, same edges as the QR card ----
  lv_obj_t *doneBtn = lv_btn_create(phoneSetupScreen);
  lv_obj_set_pos(doneBtn, RX, 376);
  lv_obj_set_size(doneBtn, RW, 56);
  lv_obj_set_style_radius(doneBtn, 16, 0);
  lv_obj_set_style_bg_color(doneBtn, lv_color_hex(PS_BURGUNDY), 0);
  lv_obj_set_style_bg_opa(doneBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(doneBtn, 0, 0);
  lv_obj_set_style_shadow_color(doneBtn, lv_color_hex(0x6B4A2A), 0);
  lv_obj_set_style_shadow_width(doneBtn, 14, 0);
  lv_obj_set_style_shadow_ofs_y(doneBtn, 3, 0);
  lv_obj_set_style_shadow_opa(doneBtn, 70, 0);
  lv_obj_add_event_cb(doneBtn, phoneSetupDoneCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *doneLbl = lv_label_create(doneBtn);
  lv_label_set_text(doneLbl, firstRun ? "Done" : "Back");
  lv_obj_set_style_text_font(doneLbl, FONT_18, 0);
  lv_obj_set_style_text_color(doneLbl, lv_color_hex(PS_ON_BURG), 0);
  lv_obj_center(doneLbl);

  lv_scr_load(phoneSetupScreen);
}

void showWifiPortalScreen(bool firstBoot) {
  if (settingsScreen)   { lv_obj_del_async(settingsScreen);   settingsScreen   = nullptr; }
  if (prodDiagScreen)   { lv_obj_del_async(prodDiagScreen);   prodDiagScreen   = nullptr; }
  if (pinPadScreen)     { lv_obj_del_async(pinPadScreen);     pinPadScreen     = nullptr; }

  // -- WiFi scan: ASYNC, never blocks --
  // WiFi.scanNetworks() (sync) takes 2-10s and freezes the entire LVGL loop
  // for that whole time -- touch input, animations, and updateMotionSensor()
  // in loop() all stop dead, which is exactly what looked like a "hang" when
  // opening this screen from Settings while the panel was already running.
  // WiFi.scanNetworks(true) starts the scan and returns immediately; results
  // are picked up later by updateWifiProvPortal() polling WiFi.scanComplete(),
  // which IS safe to call every loop() tick since it just checks a status flag.
  wifiScanOpts     = "<option value=''>(scanning... refresh page in a few seconds)</option>";
  wifiScanPending   = true;
  wifiScanAppliedAt = 0;

  // -- Start AP + STA together so the scan radio is available immediately,
  //    and the AP/portal are reachable right away rather than waiting on
  //    the scan to finish first.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WIFI_PROV_AP_SSID);   // open, no password per your decision
  WiFi.scanNetworks(true);          // async=true -- returns immediately

  // -- Set up web server --
  wifiProvServer.on("/",     HTTP_GET,  handleWifiProvRoot);
  wifiProvServer.on("/save", HTTP_POST, handleWifiProvSave);
  wifiProvServer.begin();
  wifiProvPortalActive  = true;
  wifiPortalSkipPressed = false;

  // -- Build screen --
  wifiPortalScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(wifiPortalScreen, lv_color_hex(0xF6E9CC), 0);
  lv_obj_set_style_bg_opa(wifiPortalScreen,   LV_OPA_COVER, 0);

  // Top-left: Back (when re-provisioning from Settings) or nothing (first boot)
  // Top-right: Skip (first boot only -- no main screen exists yet, but the
  //   user might just want the switchboard working now and WiFi later)
  if (!firstBoot) {
    createBackButton(wifiPortalScreen, 16, 14, wifiPortalBackCb);
  } else {
    // Skip button -- top right, cream style, firstBoot only
    lv_obj_t *skipBtn = lv_btn_create(wifiPortalScreen);
    lv_obj_set_size(skipBtn, 110, 36);
    lv_obj_set_pos(skipBtn, 674, 14);   // right-aligned: 800 - 110 - 16 = 674
    lv_obj_set_style_radius(skipBtn, 10, 0);
    lv_obj_set_style_bg_color(skipBtn, lv_color_hex(0xE9DFC9), 0);
    lv_obj_set_style_bg_opa(skipBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(skipBtn, 0, 0);
    lv_obj_add_event_cb(skipBtn, wifiPortalSkipCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *skipLbl = lv_label_create(skipBtn);
    lv_label_set_text(skipLbl, "Skip for now");
    lv_obj_set_style_text_font(skipLbl, FONT_18, 0);
    lv_obj_set_style_text_color(skipLbl, lv_color_hex(0x333333), 0);
    lv_obj_center(skipLbl);
  }

  // Title -- on the re-provisioning flow the Back button lives top-left, so
  // the title is right-aligned instead to avoid sitting underneath it. On
  // first boot there's no Back button, so the title stays top-left (the
  // Skip button already owns the top-right corner there).
  lv_obj_t *title = lv_label_create(wifiPortalScreen);
  lv_label_set_text(title, "WiFi Setup");
  lv_obj_set_style_text_font(title, FONT_26, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x1A9E72), 0);
  if (!firstBoot) {
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -16, 18);
  } else {
    lv_obj_set_pos(title, 16, 18);
  }

  // Left column: instructions
  lv_obj_t *infoCol = lv_obj_create(wifiPortalScreen);
  lv_obj_remove_style_all(infoCol);
  lv_obj_set_pos(infoCol, 30, 72);
  lv_obj_set_size(infoCol, 360, 370);
  lv_obj_clear_flag(infoCol, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *step1 = lv_label_create(infoCol);
  lv_label_set_text(step1, "1.  On your phone, go to WiFi settings\n    and connect to:");
  lv_obj_set_style_text_font(step1, FONT_18, 0);
  lv_obj_set_style_text_color(step1, lv_color_hex(0x333333), 0);
  lv_label_set_long_mode(step1, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(step1, 340);
  lv_obj_set_pos(step1, 0, 0);

  wifiPortalSsidLbl = lv_label_create(infoCol);
  lv_label_set_text(wifiPortalSsidLbl, WIFI_PROV_AP_SSID);
  lv_obj_set_style_text_font(wifiPortalSsidLbl, FONT_26, 0);
  lv_obj_set_style_text_color(wifiPortalSsidLbl, lv_color_hex(0x1A9E72), 0);
  lv_obj_set_pos(wifiPortalSsidLbl, 0, 66);

  lv_obj_t *step2 = lv_label_create(infoCol);
  lv_label_set_text(step2, "2.  Open your browser and go to:");
  lv_obj_set_style_text_font(step2, FONT_18, 0);
  lv_obj_set_style_text_color(step2, lv_color_hex(0x333333), 0);
  lv_obj_set_pos(step2, 0, 114);

  lv_obj_t *ipLbl = lv_label_create(infoCol);
  lv_label_set_text(ipLbl, "192.168.4.1:81");
  lv_obj_set_style_text_font(ipLbl, FONT_26, 0);
  lv_obj_set_style_text_color(ipLbl, lv_color_hex(0x1A9E72), 0);
  lv_obj_set_pos(ipLbl, 0, 150);

  lv_obj_t *step3 = lv_label_create(infoCol);
  lv_label_set_text(step3, "3.  Fill in your network details\n    and tap Save & Restart.");
  lv_obj_set_style_text_font(step3, FONT_18, 0);
  lv_obj_set_style_text_color(step3, lv_color_hex(0x333333), 0);
  lv_label_set_long_mode(step3, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(step3, 340);
  lv_obj_set_pos(step3, 0, 198);

  wifiPortalStatusLbl = lv_label_create(infoCol);
  lv_label_set_text(wifiPortalStatusLbl, "Waiting for connection...");
  lv_obj_set_style_text_font(wifiPortalStatusLbl, FONT_18, 0);
  lv_obj_set_style_text_color(wifiPortalStatusLbl, lv_color_hex(0x707070), 0);
  lv_obj_set_pos(wifiPortalStatusLbl, 0, 270);

  // Right column: rendered QR code (scan to auto-join) + SSID/URL fallback
  // for phones that don't auto-detect the WiFi QR format.
  lv_obj_t *rightPanel = lv_obj_create(wifiPortalScreen);
  lv_obj_remove_style_all(rightPanel);
  lv_obj_set_pos(rightPanel, 420, 72);
  lv_obj_set_size(rightPanel, 352, 370);
  lv_obj_clear_flag(rightPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(rightPanel, lv_color_hex(0xA89F86), 0);
  lv_obj_set_style_bg_opa(rightPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(rightPanel, 16, 0);

  lv_obj_t *rHead = lv_label_create(rightPanel);
  lv_label_set_text(rHead, "Scan to join");
  lv_obj_set_style_text_font(rHead, FONT_18, 0);
  lv_obj_set_style_text_color(rHead, lv_color_hex(0x3D3830), 0);
  lv_obj_set_pos(rHead, 20, 16);

  // QR code, rendered by LVGL's built-in lv_qrcode widget (LV_USE_QRCODE in
  // lv_conf.h). The widget is itself a thin wrapper around lv_canvas using
  // the same lightweight LV_IMG_CF_INDEXED_1BIT format (1 bit/pixel, 2-color
  // palette) this screen used manually before -- a 200x200 QR here costs
  // roughly 5KB of transient RAM, not the ~80KB a true-color canvas this
  // size would need. LVGL allocates that buffer in lv_qrcode_create() and
  // frees it automatically in its own destructor the moment this screen is
  // torn down (lv_obj_del_async on wifiPortalScreen cascades to its
  // children), so there's no manual malloc/free to manage here anymore.
  //
  // The payload is built fresh from WIFI_PROV_AP_SSID every time this screen
  // opens and encoded at runtime by LVGL's bundled qrcodegen library -- if
  // WIFI_PROV_AP_SSID is ever changed, this stays correct automatically,
  // nothing to hand-regenerate.
  char qrPayload[64];
  snprintf(qrPayload, sizeof(qrPayload), "WIFI:T:nopass;S:%s;;", WIFI_PROV_AP_SSID);

  const int qrSize   = 200;   // px, the encoded QR's square content area
  const int qrBorder = 16;    // px, light-color border standing in as the quiet zone

  wifiPortalQrCode = lv_qrcode_create(rightPanel, qrSize, lv_color_black(), lv_color_white());
  bool qrOk = false;
  if (wifiPortalQrCode) {
    qrOk = (lv_qrcode_update(wifiPortalQrCode, qrPayload, strlen(qrPayload)) == LV_RES_OK);
    if (qrOk) {
      lv_obj_set_style_border_color(wifiPortalQrCode, lv_color_white(), 0);
      lv_obj_set_style_border_width(wifiPortalQrCode, qrBorder, 0);
      lv_obj_align(wifiPortalQrCode, LV_ALIGN_TOP_MID, 0, 50);
    } else {
      // Encoding failed (shouldn't happen for a payload this short, but
      // never crash a sealed unit over a QR convenience) -- tear down the
      // empty widget and fall back to plain text instead.
      lv_obj_del(wifiPortalQrCode);
      wifiPortalQrCode = nullptr;
    }
  }
  if (!qrOk) {
    lv_obj_t *qrFallback = lv_label_create(rightPanel);
    lv_label_set_text(qrFallback, "QR unavailable --\nuse the steps on the left.");
    lv_obj_set_style_text_font(qrFallback, FONT_18, 0);
    lv_obj_set_style_text_color(qrFallback, lv_color_hex(0x3D3830), 0);
    lv_obj_set_pos(qrFallback, 20, 140);
  }

  lv_obj_t *rNote = lv_label_create(rightPanel);
  lv_label_set_text(rNote, "Or join \"Panel-Setup\" manually --\nno password needed.");
  lv_obj_set_style_text_font(rNote, FONT_18, 0);
  lv_obj_set_style_text_color(rNote, lv_color_hex(0x6E6656), 0);
  lv_label_set_long_mode(rNote, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(rNote, 312);
  lv_obj_set_style_text_align(rNote, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(rNote, LV_ALIGN_BOTTOM_MID, 0, -16);

  lv_scr_load(wifiPortalScreen);
}

// Tick the provisioning server in loop() while active (same pattern as OTA portal).
// Also polls the async WiFi scan kicked off in showWifiPortalScreen() -- this
// is the safe, non-blocking way to pick up scan results: WiFi.scanComplete()
// just reads a status flag, it never waits.
void updateWifiProvPortal() {
  if (!wifiProvPortalActive) return;
  wifiProvServer.handleClient();

  if (wifiScanPending) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      // still scanning, nothing to do yet -- check again next tick
    } else if (n == WIFI_SCAN_FAILED) {
      wifiScanOpts = "<option value=''>(scan failed -- type network name manually below)</option>";
      wifiScanPending = false;
      WiFi.scanDelete();
    } else {
      String opts = "";
      for (int i = 0; i < n && i < 20; i++) {
        String s = WiFi.SSID(i);
        opts += "<option value=\"" + s + "\">" + s + "</option>";
      }
      wifiScanOpts = (n > 0) ? opts : "<option value=''>(no networks found -- type name manually below)</option>";
      wifiScanPending   = false;
      wifiScanAppliedAt = millis();
      WiFi.scanDelete();
    }
  }
}

void destroySettingsScreen() {
  if (settingsScreen) {
    // See destroyPinPad() for why this must be async, not lv_obj_del --
    // settingsBackCb is a click handler on a child of settingsScreen, so
    // synchronously freeing settingsScreen here was exactly what caused
    // the reboot-on-Back crash.
    lv_obj_del_async(settingsScreen);
  }
  settingsScreen = nullptr;
  for (uint8_t i = 0; i < 4; i++) {
    settingsPills[i] = nullptr;
    settingsPanels[i] = nullptr;
  }
  lblMotionHold = nullptr;
  lblPirSens = nullptr;
  lblManualCooldown = nullptr;
  lblAutoTarget = nullptr;
  lblAutoDim = nullptr;
  lblBrightnessRow = nullptr;
  lblWelcomeDuration = nullptr;
  toggleMotionKnob = nullptr;
  toggleMotionTrack = nullptr;
  ntfyTestLabel = nullptr;
  ntfyTestSentAt = 0;
}

void settingsSelectTab(uint8_t tab) {
  for (uint8_t i = 0; i < 4; i++) {
    if (!settingsPanels[i] || !settingsPills[i]) continue;
    if (i == tab) {
      lv_obj_clear_flag(settingsPanels[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(settingsPills[i], lv_color_hex(0xF4EBD3), 0);
      lv_obj_set_style_bg_opa(settingsPills[i], 255, 0);
    } else {
      lv_obj_add_flag(settingsPanels[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_opa(settingsPills[i], 0, 0);
    }
  }
}

void settingsPillCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  uint8_t tab = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  settingsSelectTab(tab);
}

// Bare row container used by several Settings rows that hold a label
// plus one or two action buttons (as opposed to createCycleRow's
// label + chevron-value shape below). Just the wrapper -- callers add
// their own label(s)/button(s) on top, since those vary too much
// (single line vs title+subtitle vs wrapped paragraph, 1 vs 2 buttons)
// to force into one rigid layout.
lv_obj_t *createSettingsRow(lv_obj_t *parent, int y, int h) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_size(row, 712, h);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

// Small action button used inside a settings row -- button + centered
// label + click callback in one call. align/xofs let each caller place
// it where it needs to sit (mid-right, top-right, or offset left of a
// sibling button like the Wi-Fi row's Retry+Change pair).
lv_obj_t *createSettingsActionBtn(lv_obj_t *parent, int w, int h,
                                   lv_align_t align, int xofs,
                                   uint32_t bgColor, uint32_t txtColor,
                                   const char *text, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, w, h);
  lv_obj_align(btn, align, xofs, 0);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(bgColor), 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, FONT_18, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(txtColor), 0);
  lv_obj_center(lbl);
  return btn;
}

// One settings row: label on the left, two small chevron buttons
// flanking a value label on the right. Returns the value label so the
// caller can store it for live updates. cb receives +1 (right chevron)
// or -1 (left chevron) packed directly as the event user_data pointer.
lv_obj_t *createCycleRow(lv_obj_t *parent, const char *labelText, int y, lv_event_cb_t cb, const char *initialValue) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 712, 46);
  lv_obj_set_pos(row, 0, y);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(0xE9DFC6), 0);

  lv_obj_t *lbl = lv_label_create(row);
  lv_label_set_text(lbl, labelText);
  lv_obj_set_style_text_font(lbl, FONT_18, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x222222), 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

  lv_obj_t *valLbl = lv_label_create(row);
  lv_label_set_text(valLbl, initialValue);
  lv_obj_set_style_text_font(valLbl, FONT_18, 0);
  lv_obj_set_style_text_color(valLbl, lv_color_hex(0x666666), 0);
  // Fixed width + center alignment, positioned at a fixed offset from the
  // left edge (not RIGHT_MID off auto-fit content width). The old version
  // anchored the label's *right* edge and let it grow leftward with the
  // text -- "10 sec" is wider than "5 sec", so longer values pushed the
  // label's left edge straight into the left arrow button. A fixed slot
  // means the gap to both arrows is constant no matter what the text says.
  lv_obj_set_width(valLbl, 76);
  lv_obj_set_style_text_align(valLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(valLbl, LV_ALIGN_LEFT_MID, 552, 0);

  lv_obj_t *leftBtn = lv_btn_create(row);
  lv_obj_set_size(leftBtn, 30, 30);
  lv_obj_align(leftBtn, LV_ALIGN_LEFT_MID, 514, 0);
  lv_obj_set_style_radius(leftBtn, 8, 0);
  lv_obj_set_style_bg_opa(leftBtn, 0, 0);
  lv_obj_set_style_shadow_width(leftBtn, 0, 0);
  lv_obj_add_event_cb(leftBtn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)(-1));
  lv_obj_t *leftIcon = lv_label_create(leftBtn);
  lv_label_set_text(leftIcon, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(leftIcon, lv_color_hex(0x999999), 0);
  lv_obj_center(leftIcon);

  lv_obj_t *rightBtn = lv_btn_create(row);
  lv_obj_set_size(rightBtn, 30, 30);
  lv_obj_align(rightBtn, LV_ALIGN_LEFT_MID, 654, 0);
  lv_obj_set_style_radius(rightBtn, 8, 0);
  lv_obj_set_style_bg_opa(rightBtn, 0, 0);
  lv_obj_set_style_shadow_width(rightBtn, 0, 0);
  lv_obj_add_event_cb(rightBtn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)(1));
  lv_obj_t *rightIcon = lv_label_create(rightBtn);
  lv_label_set_text(rightIcon, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(rightIcon, lv_color_hex(0x999999), 0);
  lv_obj_center(rightIcon);

  return valLbl;
}

lv_obj_t *createPillButton(lv_obj_t *parent, const char *text, int x, int w, uint8_t tabIndex) {
  // Built the same explicit way as the rest of the settings UI (lv_obj_create
  // + remove_style_all + set every property by hand) instead of lv_btn_create.
  // lv_btn_create carries hidden theme-default styling for properties we
  // never touched (notably border_opa), which is the most likely reason the
  // "box" around each tab wasn't actually rendering on the panel even though
  // border_width/border_color were set.
  lv_obj_t *pill = lv_obj_create(parent);
  lv_obj_remove_style_all(pill);
  lv_obj_set_pos(pill, x, 0);
  lv_obj_set_size(pill, w, 38);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(pill, 10, 0);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_set_style_border_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(0xCDBC96), 0);
  lv_obj_set_style_bg_opa(pill, 0, 0);
  lv_obj_add_event_cb(pill, settingsPillCb, LV_EVENT_CLICKED, (void *)(intptr_t)tabIndex);

  lv_obj_t *lbl = lv_label_create(pill);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, FONT_18, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x333333), 0);
  lv_obj_center(lbl);

  return pill;
}

// ─── Activity Logs Screen ──────────────────────────────────────────────────
// Lazy-created when opened from Settings, fully torn down on Back -- same
// pattern as Diagnostics. Deliberately built from nothing heavier than
// labels and plain buttons: no lv_textarea/lv_keyboard, no lv_list, no
// scrollbar. Date/hour filtering reuses the same left/right "cycle row"
// idiom already used throughout Settings, instead of adding a whole new
// text-input flow just for this screen.
//
// Log lines are read from the SD card fresh each time this screen opens
// (single sequential pass, bounded to the most recent LOGS_MAX_ENTRIES
// lines via a circular buffer so memory use is fixed regardless of how
// large the log file has grown) and freed the moment the screen closes --
// nothing about this screen is kept allocated while it's not open.
struct LogRow {
  char ts[20];      // "YYYY-MM-DD HH:MM:SS"
  char type[16];
  char detail[96];
};

static lv_obj_t *logsScreen      = nullptr;
static lv_obj_t *logsBodyLabel   = nullptr;
static lv_obj_t *logsDateValLbl  = nullptr;
static lv_obj_t *logsHourValLbl  = nullptr;
static lv_obj_t *logsPageLbl     = nullptr;

static LogRow *logsEntries    = nullptr;
static int     logsEntryCount = 0;
static int     logsDateOffset = -1;  // -1 = All dates, 0 = today, 1 = yesterday, ...
static int     logsHourFilter = -1;  // -1 = All hours, 0-23 = that hour
static int     logsPageIndex  = 0;

static const int LOGS_MAX_ENTRIES     = 200;
static const int LOGS_MAX_DAY_OFFSET  = 30;
static const int LOGS_LINES_PER_PAGE  = 9;

static void logsRefreshDateLabel();  // defined below logsCycleDateCb, used inside it

static void logsFormatDateLabel(char *buf, size_t n) {
  if (logsDateOffset == -1) { snprintf(buf, n, "All dates"); return; }
  time_t t = time(nullptr) - (time_t)logsDateOffset * 86400L;
  struct tm tmInfo;
  localtime_r(&t, &tmInfo);
  strftime(buf, n, "%b %d", &tmInfo);
}

static void logsFormatHourLabel(char *buf, size_t n) {
  if (logsHourFilter == -1) snprintf(buf, n, "All hours");
  else snprintf(buf, n, "%02d:00", logsHourFilter);
}

// ---- Log loading ----
// The activity log lives in up to TWO generations on the SD card: the live
// file (ACTIVITY_LOG_PATH) plus one rotated generation, created the moment
// the live file crosses 512KB in logActivityEvent(). A month of busy motion
// history is very likely to have been rotated -- which is exactly why "the
// logs must be there but the screen shows nothing" happens if only the live
// file is read. Every loader below therefore reads the OLD generation first
// (strictly older lines), then the live file, into one shared ring.

// Tail reads never touch more than this many bytes of a file, regardless of
// how large it has grown (~350 lines at ~45 B/line -- comfortably more than
// the LOGS_MAX_ENTRIES ring can hold anyway).
static const size_t LOGS_TAIL_BYTES = 16 * 1024;

// Shared tail of every load: turn the ring + running line count into the
// oldest-first logsEntries[] layout that logsRenderPage() expects.
static void logsFinalizeRing(long totalSeen) {
  logsEntryCount = (totalSeen < LOGS_MAX_ENTRIES) ? (int)totalSeen : LOGS_MAX_ENTRIES;

  if (totalSeen > LOGS_MAX_ENTRIES) {
    int startIdx = (int)(totalSeen % LOGS_MAX_ENTRIES);
    LogRow *rotated = (LogRow *)ps_malloc(sizeof(LogRow) * LOGS_MAX_ENTRIES);
    if (rotated) {
      for (int k = 0; k < LOGS_MAX_ENTRIES; k++) {
        rotated[k] = logsEntries[(startIdx + k) % LOGS_MAX_ENTRIES];
      }
      memcpy(logsEntries, rotated, sizeof(LogRow) * LOGS_MAX_ENTRIES);
      free(rotated);
    }
  }
}

// Parses one CSV line in place (destructive: writes NULs over the commas)
// into the ring if it has the ts,type,detail shape. Returns nothing; the
// running counter advances only on a successfully parsed row.
static void logsRingAdd(char *p, size_t len, long *totalSeen) {
  while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) p[--len] = '\0';
  if (!len) return;

  char *c1 = strchr(p, ',');
  char *c2 = c1 ? strchr(c1 + 1, ',') : nullptr;
  if (!c1 || !c2) return;

  *c1 = '\0';
  *c2 = '\0';
  LogRow &row = logsEntries[*totalSeen % LOGS_MAX_ENTRIES];
  strncpy(row.ts, p, sizeof(row.ts) - 1);
  row.ts[sizeof(row.ts) - 1] = '\0';
  strncpy(row.type, c1 + 1, sizeof(row.type) - 1);
  row.type[sizeof(row.type) - 1] = '\0';
  strncpy(row.detail, c2 + 1, sizeof(row.detail) - 1);
  row.detail[sizeof(row.detail) - 1] = '\0';
  (*totalSeen)++;
}

// Tail-read of ONE file: never reads more than the last LOGS_TAIL_BYTES.
// One seek + one bulk read + in-place C parsing -- no Arduino String, no
// per-line heap churn, constant cost regardless of file size.
static void logsTailParseFile(const char *path, long *totalSeen) {
  File f = SD.open(path, FILE_READ);
  if (!f) return;

  size_t fileSize = f.size();
  if (fileSize == 0) { f.close(); return; }
  size_t readLen = fileSize < LOGS_TAIL_BYTES ? fileSize : LOGS_TAIL_BYTES;

  char *buf = (char *)malloc(readLen + 1);
  if (!buf) { f.close(); return; }

  f.seek(fileSize - readLen);
  size_t got = f.read((uint8_t *)buf, readLen);
  f.close();
  buf[got] = '\0';

  // If the read started mid-line, skip forward to the first complete line.
  char *p = buf;
  if (readLen < fileSize) {
    p = strchr(buf, '\n');
    if (!p) { free(buf); return; }
    p++;
  }

  while (p && *p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    logsRingAdd(p, strlen(p), totalSeen);
    p = nl ? nl + 1 : nullptr;
  }
  free(buf);
}

// "All dates" view: newest LOGS_MAX_ENTRIES lines across both generations.
static void logsLoadEntries() {
  logsEntryCount = 0;
  if (!sdMounted || !logsEntries) return;

  long totalSeen = 0;
  logsTailParseFile(ACTIVITY_LOG_OLD_PATH, &totalSeen); // rotated (older) first
  logsTailParseFile(ACTIVITY_LOG_PATH, &totalSeen);     // live file second
  logsFinalizeRing(totalSeen);
}

// ---- Date-filtered loading ----
// A specific date is loaded straight from the file(s): the CSV is appended
// chronologically, so a binary search over byte offsets finds the day's
// first line in ~a dozen 1KB probes, then one short forward scan collects
// the day's rows (the last LOGS_MAX_ENTRIES of the day if it is huge).
// Cost is independent of how large the files have grown.

// Finds the first *dated* line starting at or after byte `pos`, skipping
// the "uptime_..." lines written before NTP sync. Returns false if no dated
// line exists between pos and EOF. On success fills lineStart (byte offset
// of the line) and dateOut ("YYYY-MM-DD").
static bool logsFirstDatedLineFrom(File &f, size_t pos, bool atLineStart,
                                   size_t fileSize, size_t *lineStart,
                                   char dateOut[11]) {
  char win[1025];
  while (pos < fileSize) {
    f.seek(pos);
    size_t n = fileSize - pos < 1024 ? fileSize - pos : 1024;
    size_t got = f.read((uint8_t *)win, n);
    if (got == 0) return false;
    win[got] = '\0';

    size_t i = 0;
    if (!atLineStart) {
      char *nl = (char *)memchr(win, '\n', got);
      if (!nl) { pos += got; continue; }  // still mid-line, keep sliding
      i = (size_t)(nl - win) + 1;
      atLineStart = true;
    }

    while (i < got) {
      if (isdigit((unsigned char)win[i])) {
        if (got - i < 10) break;          // date split by window edge: refill
        memcpy(dateOut, win + i, 10);
        dateOut[10] = '\0';
        *lineStart = pos + i;
        return true;
      }
      char *nl = (char *)memchr(win + i, '\n', got - i);
      if (!nl) { atLineStart = false; break; }
      i = (size_t)(nl - win) + 1;
    }

    pos += (i > 0) ? i : got;             // guaranteed forward progress
  }
  return false;
}

// Binary search over byte offsets for the start of `targetDate`'s region.
// Invariant: every dated line strictly before the returned offset is older
// than targetDate. Dates compare correctly as strings ("YYYY-MM-DD").
static size_t logsFindDayStart(File &f, size_t fileSize, const char *targetDate) {
  size_t lo = 0, hi = fileSize;
  while (hi > lo + 1024) {
    size_t mid = lo + (hi - lo) / 2;
    size_t ls;
    char d[11];
    if (!logsFirstDatedLineFrom(f, mid, false, fileSize, &ls, d)) {
      hi = mid;                            // nothing dated after mid
      continue;
    }
    if (strncmp(d, targetDate, 10) < 0) {
      lo = ls + 1;                         // that line is before the day
    } else {
      hi = ls;                             // at/after the day
    }
  }
  return lo;
}

// Collects `targetDate`'s rows from ONE file into the shared ring.
static void logsDayParseFile(const char *path, const char *targetDate,
                             long *totalSeen) {
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  size_t fileSize = f.size();
  if (fileSize == 0) { f.close(); return; }

  size_t start = logsFindDayStart(f, fileSize, targetDate);
  bool atLineStart = (start == 0);
  size_t pos = atLineStart ? 0 : start - 1; // back off 1 byte so realignment
                                            // cannot skip a line that starts
                                            // exactly at `start`

  const size_t CHUNK = 4096;
  char *buf = (char *)malloc(CHUNK + 1);
  if (!buf) { f.close(); return; }

  size_t carry = 0;                        // partial line held between chunks
  bool done = false;
  size_t guard = 0;                        // hard cap, belt-and-braces

  while (!done && pos < fileSize && guard < 1024UL * 1024UL) {
    f.seek(pos);
    size_t space = CHUNK - carry;
    size_t n = fileSize - pos < space ? fileSize - pos : space;
    size_t got = f.read((uint8_t *)(buf + carry), n);
    if (got == 0) break;
    guard += got;

    size_t len = carry + got;
    buf[len] = '\0';

    size_t i = 0;
    if (!atLineStart) {
      char *nl = (char *)memchr(buf, '\n', len);
      if (!nl) { pos += got; carry = 0; continue; }
      i = (size_t)(nl - buf) + 1;
      atLineStart = true;
    }

    size_t consumed = i;
    while (i < len) {
      char *nl = (char *)memchr(buf + i, '\n', len - i);
      if (!nl) break;                      // partial line -> carry to next chunk
      *nl = '\0';

      char *p = buf + i;
      size_t ll = (size_t)(nl - (buf + i));
      // Only dated lines participate in the date filter; pre-NTP
      // "uptime_..." rows are intentionally invisible here (they still
      // appear in the All-dates view).
      if (ll >= 10 && isdigit((unsigned char)p[0])) {
        int cmp = strncmp(p, targetDate, 10);
        if (cmp > 0) { done = true; break; } // chronological: past the day, stop
        if (cmp == 0) {
          logsRingAdd(p, ll, totalSeen);
        }
      }
      i = (size_t)(nl - buf) + 1;
      consumed = i;
    }

    if (done) break;

    carry = len - consumed;
    if (carry >= CHUNK) { carry = 0; atLineStart = false; } // pathological long line
    else if (carry) memmove(buf, buf + consumed, carry);
    pos += got;
  }

  free(buf);
  f.close();
}

static void logsLoadEntriesForDay(const char *targetDate) {
  logsEntryCount = 0;
  if (!sdMounted || !logsEntries) return;

  long totalSeen = 0;
  logsDayParseFile(ACTIVITY_LOG_OLD_PATH, targetDate, &totalSeen); // older generation
  logsDayParseFile(ACTIVITY_LOG_PATH, targetDate, &totalSeen);     // live file
  logsFinalizeRing(totalSeen);
}

// One entry point for (re)loading: All dates -> fast tail view; a specific
// date -> targeted day load across both generations. Called on screen open
// and whenever the Date filter changes.
static void logsReload() {
  if (logsDateOffset < 0) {
    logsLoadEntries();
    return;
  }
  time_t t = time(nullptr) - (time_t)logsDateOffset * 86400L;
  struct tm tmInfo;
  localtime_r(&t, &tmInfo);
  char targetDate[11];
  strftime(targetDate, sizeof(targetDate), "%Y-%m-%d", &tmInfo);
  logsLoadEntriesForDay(targetDate);
}

// Counts how many loaded entries match the current Date/Hour filter --
// used only to clamp page navigation to a valid range. Cheap: at most
// LOGS_MAX_ENTRIES (200) comparisons.
static int logsCountMatches() {
  char targetDate[11] = "";
  if (logsDateOffset >= 0) {
    time_t t = time(nullptr) - (time_t)logsDateOffset * 86400L;
    struct tm tmInfo;
    localtime_r(&t, &tmInfo);
    strftime(targetDate, sizeof(targetDate), "%Y-%m-%d", &tmInfo);
  }
  char targetHour[3] = "";
  if (logsHourFilter >= 0) snprintf(targetHour, sizeof(targetHour), "%02d", logsHourFilter);

  int count = 0;
  for (int i = 0; i < logsEntryCount; i++) {
    LogRow &r = logsEntries[i];
    if (targetDate[0] && strncmp(r.ts, targetDate, 10) != 0) continue;
    if (targetHour[0] && strncmp(r.ts + 11, targetHour, 2) != 0) continue;
    count++;
  }
  return count;
}

// Renders exactly one page (LOGS_LINES_PER_PAGE lines) of matching entries
// into logsBodyLabel, newest first. The label is a fixed-size LONG_CLIP
// label, so even if a future edit changed the math, output would clip
// rather than overflow the screen -- no scrollbar, no scroll needed.
static void logsRenderPage() {
  if (!logsBodyLabel) return;

  char targetDate[11] = "";
  if (logsDateOffset >= 0) {
    time_t t = time(nullptr) - (time_t)logsDateOffset * 86400L;
    struct tm tmInfo;
    localtime_r(&t, &tmInfo);
    strftime(targetDate, sizeof(targetDate), "%Y-%m-%d", &tmInfo);
  }
  char targetHour[3] = "";
  if (logsHourFilter >= 0) snprintf(targetHour, sizeof(targetHour), "%02d", logsHourFilter);

  // Page text is assembled in a static buffer (keeps 1.8KB off the UI task's
  // stack) and appended by offset, so the cost is linear in the page size.
  static char body[1800];
  size_t bodyLen = 0;
  body[0] = '\0';
  int matched = 0, shown = 0;
  int skip = logsPageIndex * LOGS_LINES_PER_PAGE;

  for (int i = logsEntryCount - 1; i >= 0 && shown < LOGS_LINES_PER_PAGE; i--) {
    LogRow &r = logsEntries[i];
    if (targetDate[0] && strncmp(r.ts, targetDate, 10) != 0) continue;
    if (targetHour[0] && strncmp(r.ts + 11, targetHour, 2) != 0) continue;

    matched++;
    if (matched <= skip) continue;

    size_t room = sizeof(body) - bodyLen;
    int n = snprintf(body + bodyLen, room, "%s  %-9s %s\n", r.ts, r.type, r.detail);
    if (n < 0) break;
    bodyLen += ((size_t)n < room) ? (size_t)n : room - 1;   // snprintf reports the untruncated length
    shown++;
  }

  if (shown == 0) {
    lv_label_set_text(logsBodyLabel, logsEntryCount == 0 ?
      (logsDateOffset >= 0 ? "No entries on this date." : "No activity logged yet.") :
      (logsPageIndex == 0 ? "No entries match this filter." : "No more entries."));
  } else {
    lv_label_set_text(logsBodyLabel, body);
  }

  if (logsPageLbl) {
    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "Page %d", logsPageIndex + 1);
    lv_label_set_text(logsPageLbl, pageBuf);
  }
}

static void logsCycleDateCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  logsDateOffset += dir;
  if (logsDateOffset < -1) logsDateOffset = LOGS_MAX_DAY_OFFSET;
  if (logsDateOffset > LOGS_MAX_DAY_OFFSET) logsDateOffset = -1;
  logsPageIndex = 0;

  logsRefreshDateLabel();
  logsReload();       // date changed: pull that day's rows from SD
  logsRenderPage();
}

static void logsCycleHourCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  logsHourFilter += dir;
  if (logsHourFilter < -1) logsHourFilter = 23;
  if (logsHourFilter > 23) logsHourFilter = -1;
  logsPageIndex = 0;

  char buf[16];
  logsFormatHourLabel(buf, sizeof(buf));
  if (logsHourValLbl) lv_label_set_text(logsHourValLbl, buf);
  logsRenderPage();
}

static void logsRefreshDateLabel() {
  char buf[16];
  logsFormatDateLabel(buf, sizeof(buf));
  if (logsDateValLbl) lv_label_set_text(logsDateValLbl, buf);
}

static void logsPageNavCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  int total = logsCountMatches();
  int maxPage = total > 0 ? (total - 1) / LOGS_LINES_PER_PAGE : 0;

  // Date continuity: with a specific date selected, walking past either end
  // of that day rolls into the adjacent day instead of dead-ending. Empty
  // days are skipped automatically (each hop is just a cheap binary search).
  if (logsDateOffset >= 0) {
    if (dir > 0 && logsPageIndex >= maxPage && logsDateOffset < LOGS_MAX_DAY_OFFSET) {
      do {
        logsDateOffset++;
        logsReload();
      } while (logsCountMatches() == 0 && logsDateOffset < LOGS_MAX_DAY_OFFSET);
      logsPageIndex = 0;                    // land on the day's newest page
      logsRefreshDateLabel();
      logsRenderPage();
      return;
    }
    if (dir < 0 && logsPageIndex == 0 && logsDateOffset > 0) {
      do {
        logsDateOffset--;
        logsReload();
      } while (logsCountMatches() == 0 && logsDateOffset > 0);
      int t2 = logsCountMatches();
      logsPageIndex = t2 > 0 ? (t2 - 1) / LOGS_LINES_PER_PAGE : 0; // its oldest page
      logsRefreshDateLabel();
      logsRenderPage();
      return;
    }
  }

  logsPageIndex += dir;
  if (logsPageIndex < 0) logsPageIndex = 0;
  if (logsPageIndex > maxPage) logsPageIndex = maxPage;
  logsRenderPage();
}

static void destroyLogsScreen() {
  if (logsScreen) {
    // Same reason as destroySettingsScreen()/destroyPinPad(): the click
    // handler that triggers this teardown is itself a child of logsScreen,
    // so deleting synchronously here is what causes the crash-on-Back bug
    // elsewhere in this codebase. Async delete avoids it.
    lv_obj_del_async(logsScreen);
  }
  logsScreen     = nullptr;
  logsBodyLabel  = nullptr;
  logsDateValLbl = nullptr;
  logsHourValLbl = nullptr;
  logsPageLbl    = nullptr;

  if (logsEntries) {
    free(logsEntries);
    logsEntries = nullptr;
  }
  logsEntryCount = 0;
  logsPageIndex  = 0;
  // logsDateOffset/logsHourFilter deliberately survive teardown -- just two
  // ints, and keeping the last-used filter is friendlier if reopened soon.
}

static void logsBackCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  destroyLogsScreen();
  showSettingsScreen();
}

void showLogsScreen() {
  if (logsScreen) destroyLogsScreen();

  logsEntries = (LogRow *)ps_malloc(sizeof(LogRow) * LOGS_MAX_ENTRIES);   // PSRAM: 26KB, only while Logs is open
  logsReload();

  logsScreen = lv_obj_create(NULL);
  lv_obj_clear_flag(logsScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(logsScreen, lv_color_hex(0xF6E9CC), 0);

  createBackButton(logsScreen, 24, 20, logsBackCb);

  lv_obj_t *title = lv_label_create(logsScreen);
  lv_label_set_text(title, "Activity Logs");
  lv_obj_set_style_text_font(title, FONT_26, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x1A9E72), 0);
  lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -24, 30);

  // Date + Hour filters, reusing the same cycle-row control used all over
  // Settings -- no keyboard, no text entry, just left/right arrows.
  char dateBuf[16], hourBuf[16];
  logsFormatDateLabel(dateBuf, sizeof(dateBuf));
  logsFormatHourLabel(hourBuf, sizeof(hourBuf));

  lv_obj_t *dateValLbl = createCycleRow(logsScreen, "Date", 0, logsCycleDateCb, dateBuf);
  lv_obj_set_pos(lv_obj_get_parent(dateValLbl), 44, 84);
  logsDateValLbl = dateValLbl;

  lv_obj_t *hourValLbl = createCycleRow(logsScreen, "Hour", 0, logsCycleHourCb, hourBuf);
  lv_obj_set_pos(lv_obj_get_parent(hourValLbl), 44, 132);
  logsHourValLbl = hourValLbl;

  // ---- Page navigation ----
  lv_obj_t *pageRow = lv_obj_create(logsScreen);
  lv_obj_remove_style_all(pageRow);
  lv_obj_set_pos(pageRow, 44, 184);
  lv_obj_set_size(pageRow, 712, 34);
  lv_obj_clear_flag(pageRow, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *prevBtn = lv_btn_create(pageRow);
  lv_obj_set_size(prevBtn, 110, 34);
  lv_obj_align(prevBtn, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_radius(prevBtn, 10, 0);
  lv_obj_set_style_bg_color(prevBtn, lv_color_hex(0xF4EBD3), 0);
  lv_obj_add_event_cb(prevBtn, logsPageNavCb, LV_EVENT_CLICKED, (void *)(intptr_t)(-1));
  lv_obj_t *prevLbl = lv_label_create(prevBtn);
  lv_label_set_text(prevLbl, "< Newer"); // ASCII: LV_SYMBOL_* glyphs are not in the Figtree fonts
  lv_obj_set_style_text_font(prevLbl, FONT_18, 0);
  lv_obj_set_style_text_color(prevLbl, lv_color_hex(0x222222), 0);
  lv_obj_center(prevLbl);

  logsPageLbl = lv_label_create(pageRow);
  lv_label_set_text(logsPageLbl, "Page 1");
  lv_obj_set_style_text_font(logsPageLbl, FONT_18, 0);
  lv_obj_set_style_text_color(logsPageLbl, lv_color_hex(0x707070), 0);
  lv_obj_center(logsPageLbl);

  lv_obj_t *nextBtn = lv_btn_create(pageRow);
  lv_obj_set_size(nextBtn, 110, 34);
  lv_obj_align(nextBtn, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_radius(nextBtn, 10, 0);
  lv_obj_set_style_bg_color(nextBtn, lv_color_hex(0xF4EBD3), 0);
  lv_obj_add_event_cb(nextBtn, logsPageNavCb, LV_EVENT_CLICKED, (void *)(intptr_t)(1));
  lv_obj_t *nextLbl = lv_label_create(nextBtn);
  lv_label_set_text(nextLbl, "Older >"); // ASCII: LV_SYMBOL_* glyphs are not in the Figtree fonts
  lv_obj_set_style_text_font(nextLbl, FONT_18, 0);
  lv_obj_set_style_text_color(nextLbl, lv_color_hex(0x222222), 0);
  lv_obj_center(nextLbl);

  // ---- Log body: plain text, fixed size, clips instead of overflowing ----
  logsBodyLabel = lv_label_create(logsScreen);
  lv_obj_set_pos(logsBodyLabel, 44, 228);
  lv_obj_set_size(logsBodyLabel, 712, 224);
  lv_label_set_long_mode(logsBodyLabel, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(logsBodyLabel, FONT_18, 0);
  lv_obj_set_style_text_color(logsBodyLabel, lv_color_hex(0x222222), 0);
  lv_label_set_text(logsBodyLabel, "Loading...");

  logsRenderPage();

  lv_scr_load(logsScreen);
}

void showSettingsScreen() {
  if (settingsScreen) destroySettingsScreen();

  settingsScreen = lv_obj_create(NULL);
  lv_obj_clear_flag(settingsScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(settingsScreen, lv_color_hex(0xF6E9CC), 0);

  // ---- Top row: Back (left) | Diagnostics, Logs, Sys. Update, Reboot (right) ----
  createBackButton(settingsScreen, 24, 20, settingsBackCb);

  // Top-right cluster, right-aligned as a group ending 24px from the
  // screen edge: Diagnostics, Logs, Sys. Update, then Reboot rightmost.
  // "Update device" became "Sys. Update" -- shorter, still plain-language,
  // and frees up enough width to fit the new Logs button without the row
  // feeling clogged.
  lv_obj_t *diagBtn = lv_btn_create(settingsScreen);
  lv_obj_set_pos(diagBtn, 230, 20);
  lv_obj_set_size(diagBtn, 140, 46);
  lv_obj_set_style_radius(diagBtn, 16, 0);
  lv_obj_set_style_bg_color(diagBtn, lv_color_hex(0xF4EBD3), 0);
  lv_obj_add_event_cb(diagBtn, settingsDiagCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *diagLbl = lv_label_create(diagBtn);
  lv_label_set_text(diagLbl, "Diagnostics");
  lv_obj_set_style_text_font(diagLbl, FONT_18, 0);
  lv_obj_set_style_text_color(diagLbl, lv_color_hex(0x222222), 0);
  lv_obj_center(diagLbl);

  // Logs -- same bordered "pill" family as the rest of this row.
  lv_obj_t *logsBtn = lv_btn_create(settingsScreen);
  lv_obj_set_pos(logsBtn, 382, 20);
  lv_obj_set_size(logsBtn, 110, 46);
  lv_obj_set_style_radius(logsBtn, 16, 0);
  lv_obj_set_style_bg_color(logsBtn, lv_color_hex(0xF6E2AE), 0); // solid amber -- old cream-on-cream washed out on this TFT
  lv_obj_set_style_bg_opa(logsBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(logsBtn, 2, 0);
  lv_obj_set_style_border_color(logsBtn, lv_color_hex(0xA8842E), 0);
  lv_obj_set_style_border_opa(logsBtn, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(logsBtn, settingsLogsCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *logsLbl = lv_label_create(logsBtn);
  lv_label_set_text(logsLbl, "Logs");
  lv_obj_set_style_text_font(logsLbl, FONT_18, 0);
  lv_obj_set_style_text_color(logsLbl, lv_color_hex(0x5F4712), 0);
  lv_obj_center(logsLbl);

  lv_obj_t *updateBtn = lv_btn_create(settingsScreen);
  lv_obj_set_pos(updateBtn, 504, 20);
  lv_obj_set_size(updateBtn, 130, 46);
  lv_obj_set_style_radius(updateBtn, 16, 0);
  lv_obj_set_style_bg_color(updateBtn, lv_color_hex(0xC8F0E0), 0);
  lv_obj_set_style_bg_opa(updateBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(updateBtn, 2, 0);
  lv_obj_set_style_border_color(updateBtn, lv_color_hex(0x1A9E72), 0);
  lv_obj_set_style_border_opa(updateBtn, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(updateBtn, settingsUpdateDeviceCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *updateLbl = lv_label_create(updateBtn);
  lv_label_set_text(updateLbl, "Sys. Update");
  lv_obj_set_style_text_font(updateLbl, FONT_18, 0);
  lv_obj_set_style_text_color(updateLbl, lv_color_hex(0x0A5C3A), 0);
  lv_obj_center(updateLbl);

  createRebootButton(settingsScreen, 646, 20, settingsRebootCb);

  // ---- Section pills ----
  int pillsY = 84;
  lv_obj_t *pillRow = lv_obj_create(settingsScreen);
  lv_obj_remove_style_all(pillRow);
  // 4 pills x 178px + 3 gaps x 8px = 736px of content. Centering that in
  // the 800px screen gives an equal 32px margin on both sides -- the old
  // values (x=24, width=752) left content occupying only the first 736px
  // of that 752px box, so the real margins ended up 24px left / 40px
  // right. That lopsided gap is what looked like "barely any space on one
  // side" on the panel.
  lv_obj_set_pos(pillRow, 32, pillsY);
  lv_obj_set_size(pillRow, 736, 38);
  lv_obj_clear_flag(pillRow, LV_OBJ_FLAG_SCROLLABLE);

  settingsPills[TAB_MOTION]   = createPillButton(pillRow, "Motion", 0, 178, TAB_MOTION);
  settingsPills[TAB_DISPLAY]  = createPillButton(pillRow, "Display", 186, 178, TAB_DISPLAY);
  settingsPills[TAB_NETWORK]  = createPillButton(pillRow, "Network", 372, 178, TAB_NETWORK);
  settingsPills[TAB_SECURITY] = createPillButton(pillRow, "Security", 558, 178, TAB_SECURITY);

  // ---- Panel container ----
  int panelY = 144;
  int panelX = 44;  // centers the 712px-wide panel in the 800px screen (44px each side), matching the tab row above

  // -- Motion panel --
  settingsPanels[TAB_MOTION] = lv_obj_create(settingsScreen);
  lv_obj_remove_style_all(settingsPanels[TAB_MOTION]);
  lv_obj_set_pos(settingsPanels[TAB_MOTION], panelX, panelY);
  lv_obj_set_size(settingsPanels[TAB_MOTION], 712, 330);
  lv_obj_clear_flag(settingsPanels[TAB_MOTION], LV_OBJ_FLAG_SCROLLABLE);

  {
    // Row 1 — Motion detection toggle
    lv_obj_t *toggleRow = lv_obj_create(settingsPanels[TAB_MOTION]);
    lv_obj_remove_style_all(toggleRow);
    lv_obj_set_pos(toggleRow, 0, 0);
    lv_obj_set_size(toggleRow, 712, 46);
    lv_obj_clear_flag(toggleRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(toggleRow, 1, 0);
    lv_obj_set_style_border_side(toggleRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(toggleRow, lv_color_hex(0xE6DFCB), 0);

    lv_obj_t *toggleLbl = lv_label_create(toggleRow);
    lv_label_set_text(toggleLbl, "Motion detection");
    lv_obj_set_style_text_font(toggleLbl, FONT_18, 0);
    lv_obj_set_style_text_color(toggleLbl, lv_color_hex(0x222222), 0);
    lv_obj_align(toggleLbl, LV_ALIGN_LEFT_MID, 4, 0);

    toggleMotionTrack = lv_obj_create(toggleRow);
    lv_obj_remove_style_all(toggleMotionTrack);
    lv_obj_set_size(toggleMotionTrack, 46, 26);
    lv_obj_align(toggleMotionTrack, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_radius(toggleMotionTrack, 13, 0);
    lv_obj_set_style_bg_opa(toggleMotionTrack, LV_OPA_COVER, 0);  // remove_style_all also zeroes opacity -- this is why the toggle was invisible
    lv_obj_clear_flag(toggleMotionTrack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(toggleMotionTrack, toggleMotionDetectionCb, LV_EVENT_CLICKED, NULL);

    toggleMotionKnob = lv_obj_create(toggleMotionTrack);
    lv_obj_remove_style_all(toggleMotionKnob);
    lv_obj_set_size(toggleMotionKnob, 20, 20);
    lv_obj_set_style_radius(toggleMotionKnob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(toggleMotionKnob, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(toggleMotionKnob, LV_OPA_COVER, 0);   // same fix -- knob was also fully transparent
    lv_obj_clear_flag(toggleMotionKnob, LV_OBJ_FLAG_SCROLLABLE);
    refreshMotionToggleVisual();
  }

  {
    char buf[16];
    // Row 2 — Motion controls: which light motion detection drives (new)
    lblAutoTarget = createCycleRow(settingsPanels[TAB_MOTION], "Motion controls", 50, cycleAutoTargetCb,
      autoLightTarget == AUTO_TARGET_CEILING ? "Ceiling" : "Panel");

    // Row 3 — Motion hold timer (shifted down 50px)
    formatSecondsLabel(buf, sizeof(buf), motionHoldTimeMs);
    lblMotionHold = createCycleRow(settingsPanels[TAB_MOTION], "Motion hold timer", 100, cycleMotionHoldCb, buf);

    // Row 4 — PIR sensitivity (shifted down 50px)
    lblPirSens = createCycleRow(settingsPanels[TAB_MOTION], "PIR sensitivity", 150, cyclePirSensCb,
      PIR_TIER_NAMES[tierIndexU8(motionConfirmHighSamples, PIR_TIERS, PIR_TIER_COUNT)]);

    // Row 5 — Manual mode cooldown (shifted down 50px)
    formatSecondsLabel(buf, sizeof(buf), autoManualOffCooldownMs);
    lblManualCooldown = createCycleRow(settingsPanels[TAB_MOTION], "Manual mode cooldown", 200, cycleManualCooldownCb, buf);
  }

  lv_obj_t *allOffRow = createSettingsRow(settingsPanels[TAB_MOTION], 260, 46);
  lv_obj_t *allOffLbl = lv_label_create(allOffRow);
  lv_label_set_text(allOffLbl, "Lights stuck on?");
  lv_obj_set_style_text_font(allOffLbl, FONT_18, 0);
  lv_obj_set_style_text_color(allOffLbl, lv_color_hex(0x222222), 0);
  lv_obj_align(allOffLbl, LV_ALIGN_LEFT_MID, 4, 0);
  createSettingsActionBtn(allOffRow, 180, 36, LV_ALIGN_RIGHT_MID, -4,
                           0x222222, 0xFFFFFF, "Turn off all lights", settingsAllOffCb);

  // -- Display panel --
  settingsPanels[TAB_DISPLAY] = lv_obj_create(settingsScreen);
  lv_obj_remove_style_all(settingsPanels[TAB_DISPLAY]);
  lv_obj_set_pos(settingsPanels[TAB_DISPLAY], panelX, panelY);
  lv_obj_set_size(settingsPanels[TAB_DISPLAY], 712, 280);
  lv_obj_clear_flag(settingsPanels[TAB_DISPLAY], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(settingsPanels[TAB_DISPLAY], LV_OBJ_FLAG_HIDDEN);

  {
    char buf[16];
    formatSecondsLabel(buf, sizeof(buf), uiDimAfterMs);
    lblAutoDim = createCycleRow(settingsPanels[TAB_DISPLAY], "Auto-dim delay", 0, cycleAutoDimCb, buf);

    snprintf(buf, sizeof(buf), "%u / 7", brightnessLevel + 1);
    lblBrightnessRow = createCycleRow(settingsPanels[TAB_DISPLAY], "Brightness", 50, cycleBrightnessRowCb, buf);

    formatSecondsLabel(buf, sizeof(buf), welcomeBellGraceMs);
    lblWelcomeDuration = createCycleRow(settingsPanels[TAB_DISPLAY], "Welcome delay after motion", 100, cycleWelcomeDurationCb, buf);
  }

  // -- Network panel --
  settingsPanels[TAB_NETWORK] = lv_obj_create(settingsScreen);
  lv_obj_remove_style_all(settingsPanels[TAB_NETWORK]);
  lv_obj_set_pos(settingsPanels[TAB_NETWORK], panelX, panelY);
  lv_obj_set_size(settingsPanels[TAB_NETWORK], 712, 280);
  lv_obj_clear_flag(settingsPanels[TAB_NETWORK], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(settingsPanels[TAB_NETWORK], LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *wifiRow = createSettingsRow(settingsPanels[TAB_NETWORK], 0, 56);

  lv_obj_t *wifiTitle = lv_label_create(wifiRow);
  lv_label_set_text(wifiTitle, "Wi-Fi network");
  lv_obj_set_style_text_font(wifiTitle, FONT_18, 0);
  lv_obj_set_style_text_color(wifiTitle, lv_color_hex(0x222222), 0);
  lv_obj_set_pos(wifiTitle, 4, 2);

  lv_obj_t *wifiSub = lv_label_create(wifiRow);
  char wifiSubBuf[48];
  snprintf(wifiSubBuf, sizeof(wifiSubBuf), "Connected to %s",
           WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : WIFI_SSID);
  lv_label_set_text(wifiSub, wifiSubBuf);
  lv_obj_set_style_text_font(wifiSub, FONT_18, 0);
  lv_obj_set_style_text_color(wifiSub, lv_color_hex(0x707070), 0);
  lv_obj_set_pos(wifiSub, 4, 26);

  createSettingsActionBtn(wifiRow, 100, 38, LV_ALIGN_RIGHT_MID, -108,
                           0xF4EBD3, 0x222222, "Retry", settingsWifiRetryCb);
  createSettingsActionBtn(wifiRow, 100, 38, LV_ALIGN_RIGHT_MID, -4,
                           0xF4EBD3, 0x222222, "Change", settingsOpenWifiPortalCb);

  lv_obj_t *ntfyRow = createSettingsRow(settingsPanels[TAB_NETWORK], 92, 170);

  lv_obj_t *ntfyTitle = lv_label_create(ntfyRow);
  lv_label_set_text(ntfyTitle, "ntfy notifications");
  lv_obj_set_style_text_font(ntfyTitle, FONT_18, 0);
  lv_obj_set_style_text_color(ntfyTitle, lv_color_hex(0x222222), 0);
  lv_obj_set_pos(ntfyTitle, 4, 2);

  // Plain-language explainer, no jargon -- per your "not everyone knows
  // what this is" note. Shown directly, not behind a tap-to-reveal icon.
  lv_obj_t *ntfyExplain = lv_label_create(ntfyRow);
  lv_label_set_text(ntfyExplain,
    "Get a phone alert when the doorbell rings or motion is detected --\n"
    "no hub or app login needed. Install the free \"ntfy\" app, then\n"
    "enter a topic name here to connect it.");
  lv_obj_set_style_text_font(ntfyExplain, FONT_18, 0);
  lv_obj_set_style_text_color(ntfyExplain, lv_color_hex(0x707070), 0);
  lv_label_set_long_mode(ntfyExplain, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ntfyExplain, 540);
  lv_obj_set_pos(ntfyExplain, 4, 26);

  // Shows exactly what's actually saved right now -- factory reset wipes
  // this along with everything else, and it's easy to walk away from the
  // WiFi/ntfy setup form thinking it saved when the topic field was
  // actually left blank. No way to be sure "it's the same" without this.
  lv_obj_t *ntfyCurrentTopic = lv_label_create(ntfyRow);
  char ntfyCurrentBuf[64];
  if (nvsNtfyTopic.length() > 0) {
    snprintf(ntfyCurrentBuf, sizeof(ntfyCurrentBuf), "Current topic: %s", nvsNtfyTopic.c_str());
  } else if (strlen(NTFY_TOPIC) > 0) {
    snprintf(ntfyCurrentBuf, sizeof(ntfyCurrentBuf), "Current topic: %s (built-in default)", NTFY_TOPIC);
  } else {
    snprintf(ntfyCurrentBuf, sizeof(ntfyCurrentBuf), "No topic set -- notifications will not send");
  }
  lv_label_set_text(ntfyCurrentTopic, ntfyCurrentBuf);
  lv_obj_set_style_text_font(ntfyCurrentTopic, FONT_18, 0);
  lv_obj_set_style_text_color(ntfyCurrentTopic,
    nvsNtfyTopic.length() > 0 || strlen(NTFY_TOPIC) > 0 ? lv_color_hex(0x1A9E72) : lv_color_hex(0xCC3333), 0);
  lv_obj_set_pos(ntfyCurrentTopic, 4, 104);

  // Buttons anchored to the bottom-right of the (now taller) row instead
  // of the top-right -- top-right sat right over the tail end of the
  // wrapped explainer text above. Bottom placement keeps clear of all
  // three text lines regardless of row width.
  createSettingsActionBtn(ntfyRow, 100, 38, LV_ALIGN_BOTTOM_RIGHT, -4,
                           0xF4EBD3, 0x222222, "Set up", settingsOpenWifiPortalCb);

  // Amber like the Logs button: this is the one customers get sent to, so it
  // should not read as just another cream pill in the row.
  createSettingsActionBtn(ntfyRow, 118, 38, LV_ALIGN_BOTTOM_RIGHT, -212,
                           0xF6E2AE, 0x5F4712, "Phone setup", settingsPhoneSetupCb);

  lv_obj_t *ntfyTestBtn = createSettingsActionBtn(ntfyRow, 100, 38, LV_ALIGN_BOTTOM_RIGHT, -108,
                           0xF4EBD3, 0x222222, "Send test", settingsNtfyTestCb);
  ntfyTestLabel = lv_obj_get_child(ntfyTestBtn, 0);

  // -- Security panel --
  settingsPanels[TAB_SECURITY] = lv_obj_create(settingsScreen);
  lv_obj_remove_style_all(settingsPanels[TAB_SECURITY]);
  lv_obj_set_pos(settingsPanels[TAB_SECURITY], panelX, panelY);
  lv_obj_set_size(settingsPanels[TAB_SECURITY], 712, 280);
  lv_obj_clear_flag(settingsPanels[TAB_SECURITY], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(settingsPanels[TAB_SECURITY], LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *pinRow = createSettingsRow(settingsPanels[TAB_SECURITY], 0, 46);
  lv_obj_t *pinLbl = lv_label_create(pinRow);
  lv_label_set_text(pinLbl, "Settings PIN");
  lv_obj_set_style_text_font(pinLbl, FONT_18, 0);
  lv_obj_set_style_text_color(pinLbl, lv_color_hex(0x222222), 0);
  lv_obj_align(pinLbl, LV_ALIGN_LEFT_MID, 4, 0);
  createSettingsActionBtn(pinRow, 130, 38, LV_ALIGN_RIGHT_MID, -4,
                           0xF4EBD3, 0x222222, "Change PIN", settingsChangePinCb);

  // No on-screen "Forgot PIN?" breadcrumb here -- that path is reached
  // from inside the PIN pad itself, not advertised on this screen.

  lv_obj_t *resetRow = createSettingsRow(settingsPanels[TAB_SECURITY], 96, 70);

  lv_obj_t *resetLbl = lv_label_create(resetRow);
  lv_label_set_text(resetLbl, "Factory Reset");
  lv_obj_set_style_text_font(resetLbl, FONT_18, 0);
  lv_obj_set_style_text_color(resetLbl, lv_color_hex(0x222222), 0);
  lv_obj_set_pos(resetLbl, 4, 2);

  lv_obj_t *resetSub = lv_label_create(resetRow);
  lv_label_set_text(resetSub, "Erases Wi-Fi, PIN, ntfy topic, and all settings.");
  lv_obj_set_style_text_font(resetSub, FONT_18, 0);
  lv_obj_set_style_text_color(resetSub, lv_color_hex(0x707070), 0);
  lv_obj_set_pos(resetSub, 4, 26);

  // Same solid-fill action button used elsewhere in Settings, just colored
  // with the strong destructive red used for "Reset"/"Erase" actions on
  // iOS and Android, rather than the lighter Reboot-pill accent.
  lv_obj_t *factoryResetBtn = createSettingsActionBtn(resetRow, 160, 38, LV_ALIGN_TOP_RIGHT, -4,
                           0xFF3B30, 0xFFFFFF, "Factory Reset", factoryResetCb);
  factoryResetLabel = lv_obj_get_child(factoryResetBtn, 0);
  factoryResetArmed = false;

  settingsSelectTab(TAB_MOTION);

  lv_scr_load(settingsScreen);
}

// =====================================================
// Runtime updates
// =====================================================

void updateDoorbellTimer() {
  if (doorbellActive && millis() - doorbellStartedAt >= DOORBELL_ON_TIME_MS) {
    pcfSetPin(PCF_DOORBELL_PIN, false);
    doorbellActive = false;

    updateStatus("Status: Doorbell complete");
  }
}


void clearMotionAutoHold() {
  motionEverSeen = false;
  lastMotionSeenAt = 0;
  motionStableState = false;
  motionState = false;
  motionHighStreak = 0;
  motionLowStreak = 0;
}

void updateMotionSensor() {
  static unsigned long lastCheck = 0;

  if (!motionDetectionEnabled) {
    // User disabled motion detection from Settings > Motion. Keep the ceiling
    // light wherever its current mode left it (manual ON/OFF still works);
    // just stop feeding new motion events into AUTO mode.
    return;
  }

  if (millis() - lastCheck < MOTION_SAMPLE_MS) {
    return;
  }

  lastCheck = millis();

  bool rawNow = digitalRead(MOTION_PIN) == HIGH;

  if (rawNow) {
    motionRawHighCount++;
  } else {
    motionRawLowCount++;
  }

  if (rawNow != motionRawState) {
    motionRawState = rawNow;
    lastMotionRawChangeAt = millis();
  }
  // PIR warm-up protection:
  // Many PIR modules boot HIGH for a while. During warm-up, ignore motion
  // and keep AUTO output OFF. After warm-up, require one clean LOW before
  // arming motion detection.
  if (!motionArmedAfterBootClear) {
    motionHighStreak = 0;
    motionLowStreak = 0;
    motionStableState = false;
    motionState = false;

    if (millis() < MOTION_BOOT_IGNORE_MS) {
      return;
    }

    if (!rawNow) {
      motionArmedAfterBootClear = true;
    } else {
      return;
    }
  }

  // After a manual OFF, do not reuse any old AUTO hold.
  // Require the sensor to go CLEAR once before AUTO can react to motion again.
  if (motionRearmAfterManualOffClear) {
    clearMotionAutoHold();

    if (!rawNow) {
      motionRearmAfterManualOffClear = false;
      motionLowStreak = MOTION_CONFIRM_LOW_SAMPLES;
    } else {
      return;
    }
  }

  if (rawNow) {
    if (motionHighStreak < 255) motionHighStreak++;
    motionLowStreak = 0;
  } else {
    if (motionLowStreak < 255) motionLowStreak++;
    motionHighStreak = 0;
  }

  bool newStableState = motionStableState;

  if (!motionStableState && motionHighStreak >= motionConfirmHighSamples) {
    newStableState = true;
  }

  if (motionStableState && motionLowStreak >= MOTION_CONFIRM_LOW_SAMPLES) {
    newStableState = false;
  }

  if (newStableState != motionStableState) {
    motionStableState = newStableState;
    motionState = motionStableState;
    lastMotionStableChangeAt = millis();

    if (motionStableState) {
      motionEverSeen = true;
      lastMotionSeenAt = millis();
      motionDetectedEvents++;
      wakeScreen();  // TEST: motion immediately wakes/brightens screen

      logActivityEvent("MOTION", "Motion started");

      // Start the welcome grace period only once until motion clears.
      if (welcomeOverlayArmed) {
        welcomeOverlayPending = true;
        motionDetectedForWelcomeAt = millis();
        welcomeOverlayArmed = false;
      }
    } else {
      motionClearedAt = millis();
    }
  }

  if (motionStableState) {
    motionEverSeen = true;
    lastMotionSeenAt = millis();
  }

  if (autoMode == AUTO_MODE_AUTO && !firmwareUpdateRunning) {
    bool shouldBeOn = motionEverSeen && ((millis() - lastMotionSeenAt) <= motionHoldTimeMs);

    if (shouldBeOn != autoOutputState()) {
      applyAutoOutput(shouldBeOn);
      updateAutoLightUI();

      if (shouldBeOn) {
        updateStatus("Status: Auto light AUTO ON");
      } else {
        updateStatus("Status: Auto light AUTO OFF");
      }
    }
  }
}

bool manualOnShouldExpire() {
  if (autoMode != AUTO_MODE_MANUAL_ON) return false;

  // Best case: use sunrise as the natural cutoff.
  if (isTimeReadyFast() && weatherHasSunTimes && sunriseEpoch > 0) {
    time_t now = time(nullptr);
    time_t nextSunrise = sunriseEpoch;

    // If today's sunrise already passed, use tomorrow's approximate sunrise.
    if (nextSunrise <= now) {
      nextSunrise += 86400;
    }

    return now >= nextSunrise;
  }

  // Fallback only if sunrise is unavailable.
  return autoManualStartedAt != 0 &&
         millis() - autoManualStartedAt >= AUTO_MANUAL_HOLD_MS;
}

void updateAutoLightManualHoldTimer() {
  if (autoMode == AUTO_MODE_MANUAL_OFF) {
    if (autoManualOffStartedAt != 0 &&
        millis() - autoManualOffStartedAt >= autoManualOffCooldownMs) {
      autoMode = AUTO_MODE_AUTO;
      autoManualOffStartedAt = 0;

      // Return to AUTO from a clean OFF state.
      // AUTO will only turn on after a fresh sensor clear -> motion cycle.
      clearMotionAutoHold();
      motionRearmAfterManualOffClear = true;

      applyAutoOutput(false);
      updateAutoLightUI();
    }

    return;
  }

  if (autoMode != AUTO_MODE_MANUAL_ON) return;
  if (autoManualStartedAt == 0) return;

  if (manualOnShouldExpire()) {
    autoMode = AUTO_MODE_AUTO;
    autoManualStartedAt = 0;

    bool shouldBeOn = motionEverSeen && ((millis() - lastMotionSeenAt) <= motionHoldTimeMs);

    applyAutoOutput(shouldBeOn);
    updateAutoLightUI();
  }
}

void updateDiagnosticsIfVisible() {
  static unsigned long lastDiagUpdate = 0;

  if (lv_scr_act() != prodDiagScreen) {
    return;
  }

  // Prod diag screen is static content rendered at open time.
  // Hardware cache is refreshed periodically so cachedGt911Online /
  // cachedPcfOnline stay current if someone leaves the screen open.
  if (millis() - lastDiagUpdate > 5000) {
    updateDiagHardwareCache(false);
    lastDiagUpdate = millis();
  }
}


// =====================================================
// Setup / loop
// =====================================================

// ---- Crash recorder ----
// RTC slow memory survives a panic reset (not a power cut), so the next boot
// can write what the chip was doing when it died into the activity log,
// instead of Diagnostics saying only "Crash (panic)". Enabled by build.yml,
// which compiles with -DPANEL_CRASH_NOTE and links with
// -Wl,--wrap=esp_panic_handler: the system calls __wrap_ first, it notes the
// facts and hands over to the real handler. A build without the flag simply
// has no recorder.
struct CrashNote { uint32_t magic; uint32_t pc; uint32_t ra; int core; char reason[40]; char desc[88]; };
RTC_NOINIT_ATTR static CrashNote crashNote;
static const uint32_t CRASH_NOTE_MAGIC = 0xC0FFEE42;

#ifdef PANEL_CRASH_NOTE
// Leading fields of the system's panic_info_t (IDF 4.4), which is all that is read.
struct PanelPanicInfo {
  int core; int exception; const char *reason; const char *description;
  void *details; void *state; const void *addr; const void *frame; bool pseudo_excause;
};
static inline void crashCopy(char *dst, const char *src, size_t n) {
  size_t i = 0;
  if (src) for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
  dst[i] = '\0';
}
// No IRAM_ATTR here: the Arduino prototype generator cannot copy a signature
// that carries a section attribute. A cache-disabled panic therefore leaves no
// note (the chip still resets as before); every other kind does.
extern "C" void __wrap_esp_panic_handler(PanelPanicInfo *info) {
  crashNote.magic = CRASH_NOTE_MAGIC;
  crashNote.core  = info->core;
  crashNote.pc    = (uint32_t)info->addr;
  // Exception frame layout (xtensa_context.h): exit, pc, ps, a0 -- a0 is the
  // return address, i.e. the caller of the function that faulted.
  crashNote.ra    = info->frame ? ((const uint32_t *)info->frame)[3] : 0;
  crashCopy(crashNote.reason, info->reason, sizeof(crashNote.reason));
  crashCopy(crashNote.desc, info->description, sizeof(crashNote.desc));
  __real_esp_panic_handler(info);
}
#endif

// Boot: if the last reset was a recorded crash, put it in the log. The two
// addresses resolve to function names with firmware/symbols.txt.gz from the
// same build (the Action publishes it beside latest.bin).
static void reportCrashNote() {
  if (crashNote.magic != CRASH_NOTE_MAGIC) return;
  crashNote.magic = 0;
  char d[200];
  snprintf(d, sizeof(d), "core %d %s PC %08x RA %08x %s",
           crashNote.core, crashNote.reason, (unsigned)crashNote.pc, (unsigned)crashNote.ra, crashNote.desc);
  logActivityEvent("CRASH", d);
}

// Boot: say what became of an update that was in flight at the last reset.
static void reportInterruptedUpdate() {
  prefs.begin("doorpanel", true);
  uint8_t st = prefs.getUChar("updstage", 0);
  prefs.end();
  if (st == 0) return;
  bool good = true;
  const char *why = resetReasonText(&good);
  static const char *names[] = { "", "saving to card", "staged on card", "restart issued",
                                 "writing flash", "installed, restarting" };
  char d[110];
  if (st == 5 && good) snprintf(d, sizeof(d), "Update installed, now running %s", FW_VERSION);
  else snprintf(d, sizeof(d), "Update stopped while %s (%s)", st < 6 ? names[st] : "?", why);
  logActivityEvent("UPDATE", d);
  fwStageMark(0);
}

void setup() {
  delay(700);
  // Push plumbing exists before anything can call sendPushNotification().
  tlsGate   = xSemaphoreCreateMutex();
  pushQueue = xQueueCreate(8, sizeof(PushNotificationParams *));
  ledcSetup(1, 15000, 8);
  ledcAttachPin(TFT_BL, 1);

  loadSavedSettings();
  setBacklight(getSelectedBrightness());

  lastUserActivityAt = millis();

  // INPUT_PULLDOWN makes disconnected RCWL safe: LOW = no motion.
  pinMode(MOTION_PIN, INPUT_PULLDOWN);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  allOutputsOff();
  lcd.begin();
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextSize(2);
  lv_init();

  initSDCard();
  cleanupSdUpdateAfterSuccessfulBoot();
  reportCrashNote();
  reportInterruptedUpdate();

  // ---- Boot-attempt counter ----
  // Counts setup() calls that never reach markBootSuccessful() at the end of
  // setup(). A crash-loop (firmware that boots but dies before the main UI
  // ever comes up) increments this every reboot; a healthy boot resets it to
  // 0. Purely software, same NVS namespace already used elsewhere -- no new
  // partitions, no bootloader changes.
  uint8_t bootAttempts = registerBootAttempt();

  // Register the display + touch drivers BEFORE anything that might try to
  // draw to the screen (recovery screens, failure screens). This matches
  // where your original code registered them, just moved earlier so the
  // OTA-recovery check below (which can render a screen) is safe to run.
  uint32_t screenWidth = lcd.width();
  uint32_t screenHeight = lcd.height();

  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, screenWidth * screenHeight / 20);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // ---- OTA lockout recovery ----
  // If a firmware.bin is sitting on the SD card, install it immediately at
  // boot -- before WiFi, before the main UI -- so a bad update never locks
  // you out of reaching the Settings > Update Device screen. The file is
  // deleted automatically on the next CLEAN boot by
  // cleanupSdUpdateAfterSuccessfulBoot() above; if the new firmware is also
  // bad, the file is still there and you just drop a better firmware.bin on
  // the card and power-cycle again.
  if (SD.exists(SD_UPDATE_FIRMWARE_PATH)) {
    if (!installFirmwareFromSD()) {
      // A file the installer rejected is set aside (not deleted) so the
      // panel boots normally instead of re-failing on every power cycle.
      // Diagnostics reports it; Logs carries the exact reason.
      SD.remove(SD_UPDATE_FAILED_PATH);
      SD.rename(SD_UPDATE_FIRMWARE_PATH, SD_UPDATE_FAILED_PATH);
      char d[120];
      snprintf(d, sizeof(d), "Install failed: %s", firmwareUpdateStatus);
      logActivityEvent("UPDATE", d);
      fwStageMark(0);
      Update.abort();
      delay(2500);   // let the on-screen reason be read before the UI replaces it
    }
  }

  // ---- Crash-loop fallback ----
  // Only reached if there's no firmware.bin to auto-install (the case above)
  // AND this boot has failed to reach a working main screen several times in
  // a row. Shows a plain message instead of proceeding into whatever is
  // crashing, so the panel doesn't sit on a dark/frozen screen with no
  // explanation. Reuses the existing update-screen UI, no new screen/assets.
  if (bootAttempts >= BOOT_FAIL_THRESHOLD) {
    showFirmwarePortalScreen(
      "",
      "Repeated startup failure",
      "Place firmware.bin in /update/ on the SD card, then power-cycle."
    );
    while (true) {
      lv_timer_handler();
      delay(5);
    }
  }

#if PRINT_RECOVERY_CODE_ON_BOOT
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n[SETUP] PIN recovery code for this unit: %06u\n", getRecoveryCode());
  Serial.println("[SETUP] Write this on the label inside the unit, then set PRINT_RECOVERY_CODE_ON_BOOT to 0.\n");
#endif

  // ---- Fresh-unit first boot: no WiFi credentials saved anywhere ----
  // No compile-time SSID/password exist in this firmware (by design -- the
  // same .bin ships to every customer). If NVS has nothing either, this is
  // a brand-new unit: go straight to the SoftAP/QR provisioning portal
  // instead of trying to connect to nothing. The portal saves credentials
  // to NVS and reboots on submit, so this loop only runs once per unit,
  // ever -- every later boot has nvsWifiSsid populated and skips straight
  // past this block into the normal startup path below.
  if (nvsWifiSsid.length() == 0) {
    showWifiPortalScreen(true);
    while (!wifiPortalSkipPressed) {
      lv_timer_handler();
      updateWifiProvPortal();
      delay(5);
    }
    // User tapped "Skip for now" -- tear down the AP/portal cleanly and fall
    // through into the normal startup path below exactly as if WiFi had
    // simply not been configured yet (because it hasn't). The Wi-Fi setup
    // screen stays reachable later from Settings -> Network -> Change.
    destroyWifiPortalScreen();
  }

  // Start WiFi as early as possible. The first connection attempt is not
  // interrupted for at least 30 seconds, so weak WiFi gets time to complete.
  startWiFi();
  initTime();
  resetBootSequence();

  // Remote admin command listener (ntfy "<topic>-cmd"). The task idles on
  // its own while WiFi is down or ntfy is unconfigured, so starting it
  // unconditionally here is safe. Core 0, same as the push sender.
  xTaskCreatePinnedToCore(pushWorkerTask, "pushWorker", 8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(remoteControlTask, "ntfyCmd", 12288, nullptr, 1, &remoteTaskHandle, 0);

  createMainUI();

  fadeInScreen(mainScreen);

  // Mint this unit's notification topic if it has none. Runs after
  // createMainUI() so logActivityEvent() has a live UI/SD context, and does
  // nothing at all on units that already have a topic.
  ensureNtfyTopic();

  // "One last scan": shown once, right after the provisioning reboot, so the
  // customer goes WiFi QR -> topic -> controls QR -> Done without hunting
  // through Settings. Disarms itself when they tap Done.
  if (phoneSetupPending) {
    const char *t = nvsNtfyTopic.length() > 0 ? nvsNtfyTopic.c_str() : NTFY_TOPIC;
    if (t[0] != '\0') {
      showPhoneSetupScreen(true);
    } else {
      savePhoneSetupPending(false); // nothing to show; don't nag on every boot
    }
  }

  markBootSuccessful();
}

void loop() {
  static uint32_t lastTick = millis();
  uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;

  lv_timer_handler();

  updateDoorbellTimer();
  updateMotionSensor();
  if (!motionStableState && !welcomeOverlayArmed && millis() - motionClearedAt >= WELCOME_REARM_MS) {
    welcomeOverlayArmed = true;
  }

  checkWelcomeOverlayTrigger();
  updateAutoLightManualHoldTimer();
  updateAutoLightManualOnReminder();
  updateNightDisplaySleepTransition();
  updateFactoryResetArmTimeout();
  updateNtfyTestLabelTimeout();
  updateScreenDimming();
  processRemoteCommand();
  processPushFailures();
  processFirmwareUpdateResults();
  updateFirmwareUpdateCheck();
  updateHealthLog();
  updateLogRetention();
  updateSwitchDriverWatch();
  updateWeatherTicker();
  updateBacklightFade();
  updateWiFiStatus();
  updateBootSequence();
  updateTimeRecovery();
  updateNtpMaintenance();
  updateClock();
  updateDateScheduler();

  // First boot weather is handled by updateBootSequence().
  // After boot, updateWeather() handles normal 2-hour updates and first-success retries.
  if (bootNetStage == BOOT_NET_DONE) {
    updateWeather();
  }

  updateThemeScheduler();
  updateDiagnosticsIfVisible();
  updateFirmwareUploadPortal();
  updateWifiProvPortal();

  delay(5);
}
