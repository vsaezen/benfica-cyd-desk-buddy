#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_wifi.h>

#include <AudioFileSourceSD.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <driver/i2s.h>

#include "BenficaAssets.h"
#include "BenficaFont.h"

// Freenove FNK0114L / E32R32P, 3.2", ST7789, 240x320.
// The interface is drawn in landscape at 320x240.
static constexpr uint8_t PIN_TFT_BACKLIGHT = 27;
static constexpr uint8_t PIN_AUDIO_ENABLE = 4;
static constexpr uint8_t PIN_SD_SCK = 18;
static constexpr uint8_t PIN_SD_MISO = 19;
static constexpr uint8_t PIN_SD_MOSI = 23;
static constexpr uint8_t PIN_SD_CS = 5;
static constexpr uint8_t PIN_HAPTIC_SDA = 32;
// On this exact FNK0114L_3P2 revision the I2C socket is silk-screened
// IO32(SDA) / IO25(SCL). GPIO25 is also the onboard amplifier input, so the
// firmware explicitly switches that shared pin between I2C and DAC use.
static constexpr uint8_t PIN_HAPTIC_SCL = 25;
static constexpr uint8_t HAPTIC_ADDRESS = 0x5A;

// The speaker is now installed: all MP3 files play directly from the microSD.
#define LOCAL_SPEAKER_ENABLED 1
// GPIO25 is shared by this board's I2C socket and analogue amplifier input.
// The haptic controller is deliberately disabled so GPIO25 remains dedicated
// to uninterrupted audio and touch/UI actions never wait on I2C timeouts.
#define HAPTIC_ENABLED 0
// Online refresh runs on CPU 0; the last successful data remains cached so
// the interface is immediate even when the server or Wi-Fi is unavailable.
#define ONLINE_REFRESH_ENABLED 1

static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 240;
static constexpr unsigned long HINO_DURATION_MS = 107000UL;

static const char *AUDIO_GOAL = "/Benfica/goal.mp3";
static const char *AUDIO_GLORIOSO = "/Benfica/glorioso.mp3";
static const char *AUDIO_PAPOILAS = "/Benfica/papoilas.mp3";
static const char *AUDIO_HINO = "/Benfica/hino.mp3";
static const char *LYRICS_HINO = "/Benfica/hino.lrc";
static const char *WIFI_AP_NAME = "Benfica-CYD-Setup";
static constexpr uint16_t WIFI_DNS_PORT = 53;
static constexpr unsigned long LIVE_REFRESH_MS = 5000UL;
static constexpr unsigned long NEAR_MATCH_REFRESH_MS = 60000UL;
static constexpr unsigned long IDLE_EVENT_REFRESH_MS = 60000UL;
static constexpr unsigned long SCHEDULE_REFRESH_MS = 10800000UL;
static constexpr unsigned long TABLE_REFRESH_MS = 21600000UL;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite canvas = TFT_eSprite(&tft);
SPIClass sdSpi(VSPI);
Preferences preferences;
bool preferencesReady = false;
DNSServer dnsServer;
WebServer setupServer(80);
SemaphoreHandle_t dataMutex = nullptr;
SemaphoreHandle_t networkAudioMutex = nullptr;

// ESP8266Audio normally removes and recreates the I2S driver for every song.
// Keeping it installed makes playback start instantly, avoids DMA allocation
// failures while Wi-Fi is active, and lets network updates continue normally.
class PersistentDacOutput : public AudioOutputI2S {
 public:
  PersistentDacOutput(int port, int buffers)
      : AudioOutputI2S(port, AudioOutputI2S::INTERNAL_DAC, buffers) {}

  bool stop() override {
    if (i2sOn) i2s_zero_dma_buffer((i2s_port_t)portNo);
    return true;
  }
};

// ArduinoJson reads a Stream one byte at a time. With a long standings reply,
// a continuously available TCP buffer can keep CPU 0 busy long enough for the
// idle-task watchdog to fire. Yielding briefly every few hundred bytes keeps
// Wi-Fi and the watchdog healthy without slowing the visible interface.
class CooperativeStream : public Stream {
 public:
  explicit CooperativeStream(Stream &source) : source_(source) {}

  int available() override { return source_.available(); }
  int peek() override {
    unsigned long started = millis();
    int value = source_.peek();
    while (value < 0 && millis() - started < source_.getTimeout()) {
      vTaskDelay(pdMS_TO_TICKS(1));
      value = source_.peek();
    }
    return value;
  }
  void flush() override { source_.flush(); }
  size_t write(uint8_t value) override { return source_.write(value); }

  int read() override {
    int value = source_.read();
    // TCP data arrives in packets. An immediate -1 only means that the next
    // packet is not in the receive buffer yet, not that the JSON document has
    // ended. Wait up to the socket timeout so ArduinoJson never mistakes a
    // short network pause for end-of-file.
    unsigned long started = millis();
    while (value < 0 && millis() - started < source_.getTimeout()) {
      vTaskDelay(pdMS_TO_TICKS(1));
      value = source_.read();
    }
    cooperate(value >= 0 ? 1 : 0);
    return value;
  }

  size_t readBytes(char *buffer, size_t length) override {
    size_t count = source_.readBytes(buffer, length);
    cooperate(count);
    return count;
  }

 private:
  void cooperate(size_t count) {
    bytesSinceYield_ += count;
    if (bytesSinceYield_ >= 256 || count == 0) {
      bytesSinceYield_ = 0;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  Stream &source_;
  size_t bytesSinceYield_ = 0;
};

enum Page : uint8_t {
  PAGE_HOME,
  PAGE_MATCH,
  PAGE_TABLE,
  PAGE_HYMN,
  PAGE_COUNT
};

enum Overlay : uint8_t {
  OVERLAY_NONE,
  OVERLAY_GLORIOSO,
  OVERLAY_PAPOILAS,
  OVERLAY_GOAL
};

// Read only a short value after a known JSON marker. This avoids retaining or
// deserializing the server's 60-290 KB documents on a non-PSRAM ESP32.
static bool readJsonText(Stream &stream, String &value, size_t maximumBytes) {
  value = "";
  bool escaped = false;
  while (value.length() < maximumBytes) {
    int next = stream.read();
    if (next < 0) return false;
    char character = (char)next;
    if (escaped) {
      value += character;
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == '"') {
      return true;
    } else {
      value += character;
    }
  }
  return false;
}

static bool findJsonText(Stream &stream, const char *marker, String &value,
                         size_t maximumBytes) {
  return stream.find(marker) && readJsonText(stream, value, maximumBytes);
}

static bool readJsonInteger(Stream &stream, int &value) {
  String number;
  int next = stream.read();
  while (next == ' ' || next == '\r' || next == '\n' || next == '\t') {
    next = stream.read();
  }
  while ((next >= '0' && next <= '9') || next == '-' || next == '.') {
    number += (char)next;
    next = stream.read();
  }
  if (!number.length()) return false;
  value = number.toInt();
  return true;
}

AudioFileSourceSD *audioFile = nullptr;
AudioFileSourceID3 *audioId3 = nullptr;
AudioOutputI2S *audioOutput = nullptr;
AudioGeneratorMP3 *audioDecoder = nullptr;
alignas(8) uint8_t audioDecoderMemory[AudioGeneratorMP3::preAllocSize()];

uint16_t C_BG;
uint16_t C_BG_2;
uint16_t C_SURFACE;
uint16_t C_SURFACE_2;
uint16_t C_RED;
uint16_t C_RED_DARK;
uint16_t C_GOLD;
uint16_t C_WHITE;
uint16_t C_MUTED;
uint16_t C_BORDER;
uint16_t C_GREEN;
uint16_t C_BLACK;

Page currentPage = PAGE_HOME;
Overlay overlay = OVERLAY_NONE;

bool sdReady = false;
bool hapticReady = false;
bool canvasReady = false;
bool audioPlaying = false;
bool audioDacInstalled = false;
bool remoteAudioPlaying = false;
bool audioOwnsNetworkMutex = false;
bool matchActive = false;
bool goalIsDemo = false;
bool hymnPlaying = false;
volatile bool wifiReady = false;
volatile bool pendingNetworkGoal = false;
volatile bool forceNetworkRefresh = false;
volatile uint8_t networkStage = 0;
volatile uint8_t lastWifiDisconnectReason = 0;
TaskHandle_t networkTaskHandle = nullptr;
static constexpr uint32_t NETWORK_STACK_BYTES = 8192;
StaticTask_t networkTaskControl;
StackType_t networkTaskStack[NETWORK_STACK_BYTES / sizeof(StackType_t)];
bool championsMatch = false;
bool nextIsChampions = false;
String competitionText = "LIGA PORTUGUESA";
String activeEventId;
String activeLeague;
time_t activeEventEpoch = 0;
int lastNetworkBenficaScore = -1;

String scoreText = "SLB 0-0 RIVAL";
String scorerText = "A AGUARDAR";
String goalHistory;
String nextOpponent = "A ATUALIZAR";
String nextDate = "PROXIMO JOGO";
String nextTimePortugal = "--:-- PT";
String nextTimeSpain = "--:-- ES";
String wifiSsid;
String wifiPassword;
String serialLine;
File uploadFile;
String uploadPath;
uint32_t uploadExpected = 0;
uint32_t uploadReceived = 0;
volatile bool uploadActive = false;
volatile bool transferSession = false;

static constexpr uint8_t MAX_TABLE_TEAMS = 18;
String tableTeam[MAX_TABLE_TEAMS];
uint8_t tablePoints[MAX_TABLE_TEAMS];
uint8_t tableCount = 0;

struct LyricLine {
  uint32_t atMs;
  String text;
};

struct BuiltinLyricLine {
  uint32_t atMs;
  const char *text;
};

// Timed against the exact 1:47 MP3 stored as /Benfica/hino.mp3. The words were
// supplied directly by the user; onset timings were aligned to the recording.
static const BuiltinLyricLine BUILTIN_LYRICS[] = {
  { 7860, "Sou do Benfica" },
  { 13300, "E isso me envaidece" },
  { 17940, "Tenho a genica" },
  { 23300, "Que a qualquer engrandece" },
  { 30500, "Sou de um clube lutador" },
  { 38660, "Que na luta, com fervor" },
  { 46980, "Nunca encontrou rival" },
  { 54180, "Neste nosso Portugal" },
  { 61920, "Ser Benfiquista" },
  { 65880, "É ter na alma a chama imensa" },
  { 71180, "Que nos conquista" },
  { 73700, "E leva à palma a luz intensa" },
  { 77740, "Do Sol que lá no céu," },
  { 80500, "risonho, vem beijar" },
  { 83500, "Com orgulho muito seu" },
  { 87140, "As camisolas berrantes" },
  { 90680, "Que nos campos a vibrar" },
  { 95100, "São papoilas saltitantes" },
};

static constexpr uint8_t MAX_LYRIC_LINES = 48;
LyricLine lyrics[MAX_LYRIC_LINES];
uint8_t lyricCount = 0;

bool touchWasDown = false;
int touchStartX = 0;
int touchStartY = 0;
int touchLastX = 0;
int touchLastY = 0;

bool transitionActive = false;
Page transitionFrom = PAGE_HOME;
Page transitionTo = PAGE_HOME;
int transitionDirection = 1;
unsigned long transitionStarted = 0;

unsigned long lastTouchPoll = 0;
unsigned long lastTouchEvent = 0;
unsigned long lastFrame = 0;
unsigned long overlayStarted = 0;
unsigned long remoteAudioUntil = 0;
unsigned long hymnStartedAt = 0;

// DRV2605L registers used by the built-in ERM waveform player.
static constexpr uint8_t DRV_REG_MODE = 0x01;
static constexpr uint8_t DRV_REG_RTP = 0x02;
static constexpr uint8_t DRV_REG_LIBRARY = 0x03;
static constexpr uint8_t DRV_REG_WAVESEQ1 = 0x04;
static constexpr uint8_t DRV_REG_GO = 0x0C;
static constexpr uint8_t DRV_REG_OVERDRIVE = 0x0D;
static constexpr uint8_t DRV_REG_SUSTAIN_POS = 0x0E;
static constexpr uint8_t DRV_REG_SUSTAIN_NEG = 0x0F;
static constexpr uint8_t DRV_REG_BRAKE = 0x10;
static constexpr uint8_t DRV_REG_AUDIO_MAX = 0x13;
static constexpr uint8_t DRV_REG_FEEDBACK = 0x1A;
static constexpr uint8_t DRV_REG_CONTROL3 = 0x1D;

static void selectHapticBus() {
#if HAPTIC_ENABLED
#if LOCAL_SPEAKER_ENABLED
  if (audioDacInstalled) {
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
  }
#endif
  Wire.end();
  Wire.begin(PIN_HAPTIC_SDA, PIN_HAPTIC_SCL, 100000);
  Wire.setTimeOut(40);
#endif
}

static void selectAudioDac() {
#if LOCAL_SPEAKER_ENABLED
#if HAPTIC_ENABLED
  Wire.end();
  delay(1);
#endif
  if (audioDacInstalled) {
    i2s_set_pin(I2S_NUM_0, nullptr);
    i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
    i2s_zero_dma_buffer(I2S_NUM_0);
  }
#endif
}

static bool hapticWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(HAPTIC_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool hapticReadRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(HAPTIC_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(HAPTIC_ADDRESS, (uint8_t)1) != 1) return false;
  value = Wire.read();
  return true;
}

static bool initHaptics() {
#if !HAPTIC_ENABLED
  Serial.println("HAPTIC|DISABLED");
  return false;
#else
  Wire.begin(PIN_HAPTIC_SDA, PIN_HAPTIC_SCL, 100000);
  Wire.setTimeOut(40);
  delay(8);

  Wire.beginTransmission(HAPTIC_ADDRESS);
  if (Wire.endTransmission() != 0) {
    Serial.println("HAPTIC|MISSING|0x5A");
    return false;
  }

  uint8_t feedback = 0;
  uint8_t control3 = 0;
  bool ok = hapticReadRegister(DRV_REG_FEEDBACK, feedback) &&
            hapticReadRegister(DRV_REG_CONTROL3, control3);
  ok = hapticWriteRegister(DRV_REG_MODE, 0x00) && ok;
  ok = hapticWriteRegister(DRV_REG_RTP, 0x00) && ok;
  ok = hapticWriteRegister(DRV_REG_LIBRARY, 0x01) && ok;
  ok = hapticWriteRegister(DRV_REG_WAVESEQ1, 0x01) && ok;
  ok = hapticWriteRegister(DRV_REG_WAVESEQ1 + 1, 0x00) && ok;
  ok = hapticWriteRegister(DRV_REG_OVERDRIVE, 0x00) && ok;
  ok = hapticWriteRegister(DRV_REG_SUSTAIN_POS, 0x00) && ok;
  ok = hapticWriteRegister(DRV_REG_SUSTAIN_NEG, 0x00) && ok;
  ok = hapticWriteRegister(DRV_REG_BRAKE, 0x00) && ok;
  ok = hapticWriteRegister(DRV_REG_AUDIO_MAX, 0x64) && ok;
  ok = hapticWriteRegister(DRV_REG_FEEDBACK, feedback & 0x7F) && ok;
  ok = hapticWriteRegister(DRV_REG_CONTROL3, control3 | 0x20) && ok;
  Serial.printf("HAPTIC|%s|0x5A\n", ok ? "READY" : "INIT_ERROR");
  return ok;
#endif
}

static void playHapticSequence(const uint8_t *sequence, size_t count) {
  if (!hapticReady) return;
  hapticWriteRegister(DRV_REG_GO, 0x00);
  for (size_t slot = 0; slot < 8; ++slot) {
    hapticWriteRegister(DRV_REG_WAVESEQ1 + slot,
                        slot < count ? sequence[slot] : 0x00);
  }
  hapticWriteRegister(DRV_REG_GO, 0x01);
}

static void hapticSoftTap() {
  const uint8_t sequence[] = {7};
  playHapticSequence(sequence, sizeof(sequence));
}

static void hapticConfirm() {
  const uint8_t sequence[] = {1};
  playHapticSequence(sequence, sizeof(sequence));
}

static void hapticGoal() {
  const uint8_t sequence[] = {1, 0x84, 10, 0x84, 14};
  playHapticSequence(sequence, sizeof(sequence));
}

static uint16_t mix565(uint16_t a, uint16_t b, uint8_t amount) {
  uint8_t ar = (a >> 11) & 0x1F;
  uint8_t ag = (a >> 5) & 0x3F;
  uint8_t ab = a & 0x1F;
  uint8_t br = (b >> 11) & 0x1F;
  uint8_t bg = (b >> 5) & 0x3F;
  uint8_t bb = b & 0x1F;
  uint8_t rr = ar + ((int16_t)(br - ar) * amount) / 255;
  uint8_t rg = ag + ((int16_t)(bg - ag) * amount) / 255;
  uint8_t rb = ab + ((int16_t)(bb - ab) * amount) / 255;
  return (rr << 11) | (rg << 5) | rb;
}

static void fillGradient(int x, int y, int w, int h,
                         uint16_t top, uint16_t bottom) {
  for (int row = 0; row < h; ++row) {
    uint8_t amount = h <= 1 ? 255 : (uint32_t)row * 255 / (h - 1);
    canvas.drawFastHLine(x, y + row, w, mix565(top, bottom, amount));
  }
}

static void centeredText(const String &text, int x, int y,
                         const GFXfont *font, uint16_t color) {
  canvas.setFreeFont(font);
  canvas.setTextColor(color);
  canvas.setTextDatum(MC_DATUM);
  canvas.drawString(text, x, y);
}

static void leftText(const String &text, int x, int y,
                     const GFXfont *font, uint16_t color) {
  canvas.setFreeFont(font);
  canvas.setTextColor(color);
  canvas.setTextDatum(ML_DATUM);
  canvas.drawString(text, x, y);
}

static void smallText(const String &text, int x, int y, uint16_t color,
                      uint8_t datum = ML_DATUM) {
  canvas.setFreeFont(nullptr);
  canvas.setTextFont(1);
  canvas.setTextSize(1);
  canvas.setTextColor(color);
  canvas.setTextDatum(datum);
  canvas.drawString(text, x, y);
}

static String shortened(const String &text, uint8_t length) {
  if (text.length() <= length) return text;
  if (length < 4) return text.substring(0, length);
  return text.substring(0, length - 3) + "...";
}

static void panel(int x, int y, int w, int h, int radius,
                  uint16_t fill, bool border = true) {
  canvas.fillRoundRect(x + 2, y + 3, w, h, radius, C_BLACK);
  canvas.fillRoundRect(x, y, w, h, radius, fill);
  if (border) canvas.drawRoundRect(x, y, w, h, radius, C_BORDER);
}

static void pushTransparentAsset(int x, int y, int w, int h,
                                 const uint16_t *asset) {
  // Copy horizontal opaque runs instead of calling drawPixel for every pixel.
  // On the 8-bit framebuffer this is several times faster, especially for the
  // moving eagle and the crest on the home screen.
  for (int py = 0; py < h; ++py) {
    int dy = y + py;
    if (dy < 0 || dy >= SCREEN_H) continue;
    int px = 0;
    while (px < w) {
      while (px < w && pgm_read_word(asset + py * w + px) == BENFICA_IMAGE_KEY) ++px;
      int runStart = px;
      while (px < w && pgm_read_word(asset + py * w + px) != BENFICA_IMAGE_KEY) ++px;
      int runLength = px - runStart;
      int dx = x + runStart;
      if (runLength > 0 && dx < SCREEN_W && dx + runLength > 0) {
        int cropLeft = max(0, -dx);
        int cropRight = max(0, dx + runLength - SCREEN_W);
        int visible = runLength - cropLeft - cropRight;
        if (visible > 0) {
          canvas.pushImage(dx + cropLeft, dy, visible, 1,
                           asset + py * w + runStart + cropLeft);
        }
      }
    }
  }
}

static void drawTopBar(const String &section, int ox) {
  canvas.fillRect(ox, 0, SCREEN_W, 34, C_BLACK);
  canvas.fillRect(ox, 0, 6, 34, C_RED);
  leftText("SL BENFICA", ox + 16, 13, &Graduate18, C_WHITE);
  canvas.fillCircle(ox + 302, 17, 3, wifiReady ? C_GREEN : C_RED);
  smallText(section, ox + 292, 18, C_MUTED, MR_DATUM);
}

static void drawChampionsBadge(int cx, int cy, uint16_t color) {
  // Compact monochrome star-ball mark for Champions fixtures.
  const float step = 0.78539816f;
  int px[8];
  int py[8];
  for (uint8_t i = 0; i < 8; ++i) {
    float angle = -1.5707963f + i * step;
    px[i] = cx + (int)(14.0f * cosf(angle));
    py[i] = cy + (int)(14.0f * sinf(angle));
  }
  for (uint8_t i = 0; i < 8; ++i) {
    uint8_t next = (i + 1) & 7;
    canvas.drawLine(px[i], py[i], px[next], py[next], color);
    canvas.fillTriangle(px[i], py[i],
                        cx + (px[i] - cx) / 2 - (py[i] - cy) / 5,
                        cy + (py[i] - cy) / 2 + (px[i] - cx) / 5,
                        cx + (px[i] - cx) / 2 + (py[i] - cy) / 5,
                        cy + (py[i] - cy) / 2 - (px[i] - cx) / 5,
                        color);
  }
  canvas.drawCircle(cx, cy, 7, color);
}

static void drawPageDots(Page active, int ox) {
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    int x = ox + 142 + i * 12;
    if (i == active) canvas.fillRoundRect(x - 3, 229, 8, 5, 2, C_RED);
    else canvas.fillCircle(x, 231, 2, C_MUTED);
  }
}

static void drawButton(int x, int y, int w, int h, const String &label,
                       bool primary) {
  uint16_t fill = primary ? C_RED : C_SURFACE_2;
  canvas.fillRoundRect(x + 2, y + 3, w, h, 10, C_BLACK);
  canvas.fillRoundRect(x, y, w, h, 10, fill);
  canvas.drawRoundRect(x, y, w, h, 10, primary ? C_WHITE : C_BORDER);
  centeredText(label, x + w / 2, y + h / 2 + 1, &Graduate18, C_WHITE);
}

static const uint16_t *eagleFrame(uint8_t frame) {
  switch (frame & 3) {
    case 0: return EAGLE_FLIGHT_0;
    case 1: return EAGLE_FLIGHT_1;
    case 2: return EAGLE_FLIGHT_2;
    default: return EAGLE_FLIGHT_3;
  }
}

static void drawHome(int ox) {
  fillGradient(ox, 0, SCREEN_W, SCREEN_H, C_BG_2, C_BLACK);
  drawTopBar("INICIO", ox);

  // Restrained hero card: the crest remains optically centred while the eagle
  // moves above it on a separate visual plane.
  panel(ox + 10, 40, 300, 130, 18, C_RED_DARK, true);
  canvas.fillRoundRect(ox + 13, 43, 294, 124, 16, C_RED);
  canvas.fillCircle(ox + 160, 104, 51, C_WHITE);
  pushTransparentAsset(ox + 112, 56, BENFICA_CREST_WIDTH,
                       BENFICA_CREST_HEIGHT, BENFICA_CREST);

  unsigned long flight = millis() % 6600UL;
  int eagleX = ox - 104 + (int)((uint32_t)flight * 440UL / 6600UL);
  int eagleY = 31 + (int)(7.0f * sinf(flight * 0.0045f));
  uint8_t frame = (millis() / 145UL) & 3;
  pushTransparentAsset(eagleX, eagleY, EAGLE_FLIGHT_0_WIDTH,
                       EAGLE_FLIGHT_0_HEIGHT, eagleFrame(frame));

  drawButton(ox + 10, 180, 145, 40, "SLB GLORIOSO", false);
  drawButton(ox + 165, 180, 145, 40, "PAPOILAS", true);
  drawPageDots(PAGE_HOME, ox);
}

static String historyEntry(uint8_t wanted) {
  int start = 0;
  for (uint8_t index = 0; index <= wanted; ++index) {
    int end = goalHistory.indexOf(';', start);
    if (index == wanted) {
      if (end < 0) return goalHistory.substring(start);
      return goalHistory.substring(start, end);
    }
    if (end < 0) return "";
    start = end + 1;
  }
  return "";
}

static void drawMatch(int ox) {
  fillGradient(ox, 0, SCREEN_W, SCREEN_H, C_BG_2, C_BLACK);
  drawTopBar(matchActive ? competitionText : "PROXIMO JOGO", ox);

  if (matchActive) {
    panel(ox + 10, 42, 300, 58, 14, C_SURFACE, true);
    canvas.fillRoundRect(ox + 10, 42, 7, 58, 4, C_RED);
    centeredText(shortened(scoreText, 20), ox + (championsMatch ? 145 : 160), 71,
                 scoreText.length() > 15 ? &FreeSansBold18pt7b
                                         : &Graduate26,
                 C_WHITE);
    if (championsMatch) drawChampionsBadge(ox + 282, 71, C_WHITE);
    smallText("GOLOS", ox + 19, 113, C_RED);

    bool any = false;
    for (uint8_t i = 0; i < 5; ++i) {
      String item = historyEntry(i);
      item.trim();
      if (!item.length()) break;
      any = true;
      int y = 132 + i * 18;
      canvas.fillCircle(ox + 22, y, 3, C_GOLD);
      leftText(shortened(item, 37), ox + 32, y,
               &FreeSansBold9pt7b, C_WHITE);
    }
    if (!any) {
      centeredText("A AGUARDAR O PRIMEIRO GOLO", ox + 160, 154,
                   &FreeSansBold9pt7b, C_MUTED);
    }
  } else {
    panel(ox + 10, 42, 300, 174, 18, C_SURFACE, true);
    canvas.fillCircle(ox + 74, 104, 49, C_WHITE);
    pushTransparentAsset(ox + 26, 56, BENFICA_CREST_WIDTH,
                         BENFICA_CREST_HEIGHT, BENFICA_CREST);
    smallText(nextDate, ox + 139, 68, C_RED);
    if (nextIsChampions) drawChampionsBadge(ox + 282, 72, C_WHITE);
    leftText(shortened(nextOpponent, 18), ox + 139, 94,
             &Graduate18, C_WHITE);
    canvas.drawFastHLine(ox + 139, 119, 148, C_BORDER);
    smallText("PORTUGAL", ox + 139, 138, C_MUTED);
    leftText(nextTimePortugal, ox + 214, 138, &FreeSansBold12pt7b, C_WHITE);
    smallText("ESPANHA", ox + 139, 171, C_MUTED);
    leftText(nextTimeSpain, ox + 214, 171, &FreeSansBold12pt7b, C_WHITE);
    smallText(wifiReady ? "ATUALIZACAO DIRETA POR WIFI" : "WIFI A RECONECTAR",
              ox + 160, 203,
              C_MUTED, MC_DATUM);
  }
  drawPageDots(PAGE_MATCH, ox);
}

static void drawTable(int ox) {
  fillGradient(ox, 0, SCREEN_W, SCREEN_H, C_BG_2, C_BLACK);
  drawTopBar("LIGA PORTUGUESA", ox);

  uint8_t chunk = tableCount ? (millis() / 5000UL) % ((tableCount + 6) / 7) : 0;
  uint8_t start = chunk * 7;
  smallText("#", ox + 17, 45, C_MUTED);
  smallText("CLUBE", ox + 45, 45, C_MUTED);
  smallText("PTS", ox + 296, 45, C_MUTED, MR_DATUM);

  if (!tableCount) {
    panel(ox + 18, 76, 284, 91, 15, C_SURFACE, true);
    centeredText("A ATUALIZAR A TABELA", ox + 160, 111,
                 &Graduate18, C_WHITE);
    smallText("A LER OS DADOS DIRETAMENTE POR WIFI", ox + 160, 143,
              C_MUTED, MC_DATUM);
  } else {
    for (uint8_t row = 0; row < 7; ++row) {
      uint8_t index = start + row;
      if (index >= tableCount) break;
      int y = 61 + row * 23;
      String upper = tableTeam[index];
      upper.toUpperCase();
      bool isBenfica = upper.indexOf("BENFICA") >= 0 || upper == "SLB";
      if (isBenfica) {
        canvas.fillRoundRect(ox + 9, y - 9, 302, 21, 7, C_RED);
      } else if (row & 1) {
        canvas.fillRoundRect(ox + 9, y - 9, 302, 21, 7, C_SURFACE);
      }
      uint16_t color = isBenfica ? C_WHITE : C_MUTED;
      smallText(String(index + 1), ox + 22, y, color, MC_DATUM);
      leftText(shortened(tableTeam[index], 23), ox + 43, y,
               &FreeSansBold9pt7b, C_WHITE);
      smallText(String(tablePoints[index]), ox + 295, y, color, MR_DATUM);
    }
  }
  drawPageDots(PAGE_TABLE, ox);
}

static String currentLyric(uint32_t elapsed) {
  String line;
  for (uint8_t i = 0; i < lyricCount; ++i) {
    if (lyrics[i].atMs > elapsed) break;
    line = lyrics[i].text;
  }
  return line;
}

static void drawWrappedLyric(const String &lyric, int cx, int cy) {
  canvas.setFreeFont(&Graduate14);
  if (canvas.textWidth(lyric) <= 252) {
    centeredText(lyric, cx, cy, &Graduate14, C_WHITE);
    return;
  }

  int midpoint = lyric.length() / 2;
  int split = -1;
  for (int offset = 0; offset < midpoint; ++offset) {
    int right = midpoint + offset;
    int left = midpoint - offset;
    if (right < (int)lyric.length() && lyric[right] == ' ') {
      split = right;
      break;
    }
    if (left > 0 && lyric[left] == ' ') {
      split = left;
      break;
    }
  }
  if (split < 0) split = midpoint;
  centeredText(lyric.substring(0, split), cx, cy - 9, &Graduate14, C_WHITE);
  centeredText(lyric.substring(split + 1), cx, cy + 10, &Graduate14, C_WHITE);
}

static void drawHymn(int ox) {
  fillGradient(ox, 0, SCREEN_W, SCREEN_H, C_RED_DARK, C_BLACK);
  drawTopBar("HINO", ox);

  panel(ox + 20, 45, 280, 171, 20, C_SURFACE, true);
  centeredText("1904", ox + 160, 91, &Graduate42, C_WHITE);
  canvas.drawFastHLine(ox + 74, 122, 172, C_RED);

  if (!hymnPlaying) {
    centeredText("TOQUE PARA OUVIR", ox + 160, 151,
                 &Graduate18, C_RED);
    smallText("O HINO DO SPORT LISBOA E BENFICA", ox + 160, 181,
              C_MUTED, MC_DATUM);
  } else {
    uint32_t elapsed = millis() - hymnStartedAt;
    String lyric = currentLyric(elapsed);
    if (!lyric.length()) lyric = "HINO DO BENFICA";
    if (lyricCount) {
      drawWrappedLyric(lyric, ox + 160, 153);
    } else {
      centeredText("LETRA LRC NAO INSTALADA", ox + 160, 151,
                   &FreeSansBold9pt7b, C_WHITE);
      smallText("COPIE /BENFICA/HINO.LRC NA MICROSD", ox + 160, 169,
                C_MUTED, MC_DATUM);
    }
    int progress = constrain((uint32_t)elapsed * 238UL / HINO_DURATION_MS,
                             0UL, 238UL);
    canvas.fillRoundRect(ox + 41, 185, 238, 6, 3, C_BG_2);
    canvas.fillRoundRect(ox + 41, 185, progress, 6, 3, C_RED);
    smallText("DESLIZE PARA PARAR", ox + 160, 205, C_MUTED, MC_DATUM);
  }
  drawPageDots(PAGE_HYMN, ox);
}

static void drawPage(Page page, int ox) {
  switch (page) {
    case PAGE_HOME: drawHome(ox); break;
    case PAGE_MATCH: drawMatch(ox); break;
    case PAGE_TABLE: drawTable(ox); break;
    case PAGE_HYMN: drawHymn(ox); break;
    default: break;
  }
}

static void drawPoppy(int x, int y, int scale, uint16_t petal) {
  int r = 6 * scale;
  canvas.drawLine(x, y + r, x - scale, y + 22 * scale, C_GREEN);
  canvas.fillTriangle(x - scale, y + 14 * scale,
                      x - 10 * scale, y + 18 * scale,
                      x - scale, y + 21 * scale, C_GREEN);
  canvas.fillCircle(x - r, y, r, petal);
  canvas.fillCircle(x + r, y, r, petal);
  canvas.fillCircle(x, y - r, r, C_RED);
  canvas.fillCircle(x, y + r, r, C_RED_DARK);
  canvas.fillCircle(x, y, 4 * scale, C_BLACK);
  canvas.fillCircle(x - scale, y - scale, scale, C_GOLD);
}

static void drawGloriosoOverlay() {
  fillGradient(0, 0, SCREEN_W, SCREEN_H, C_RED_DARK, C_BLACK);
  for (int y = -40; y < 280; y += 36) {
    canvas.drawLine(0, y, 320, y + 90, mix565(C_RED, C_WHITE, 35));
    canvas.drawLine(0, y + 1, 320, y + 91, mix565(C_RED, C_WHITE, 35));
  }
  canvas.fillRoundRect(21, 31, 278, 178, 20, C_WHITE);
  canvas.fillRoundRect(26, 36, 268, 168, 17, C_RED);
  pushTransparentAsset(112, 43, BENFICA_CREST_WIDTH,
                       BENFICA_CREST_HEIGHT, BENFICA_CREST);
  centeredText("SLB GLORIOSO SLB", 160, 173, &Graduate18, C_WHITE);
  smallText("TOQUE PARA PARAR", 160, 198, C_WHITE, MC_DATUM);
}

static void drawPapoilasOverlay() {
  fillGradient(0, 0, SCREEN_W, SCREEN_H, C_BG_2, C_BLACK);
  centeredText("PAPOILAS", 160, 29, &Graduate26, C_WHITE);
  centeredText("SALTITANTES", 160, 58, &Graduate18, C_RED);

  unsigned long tick = millis() - overlayStarted;
  for (uint8_t i = 0; i < 7; ++i) {
    float phase = tick * 0.006f + i * 0.82f;
    int x = 30 + i * 43;
    int baseY = 151 + (i & 1) * 15;
    int y = baseY - abs((int)(52.0f * sinf(phase)));
    drawPoppy(x, y, i % 3 == 0 ? 2 : 1,
              i & 1 ? C_RED : mix565(C_RED, C_WHITE, 35));
  }
  canvas.fillRoundRect(65, 190, 190, 28, 14, C_RED);
  centeredText("PAPOILAS", 160, 205, &Graduate18, C_WHITE);
}

static void drawGoalOverlay() {
  fillGradient(0, 0, SCREEN_W, SCREEN_H, C_RED_DARK, C_BG);
  unsigned long frame = (millis() - overlayStarted) / 48UL;
  for (uint8_t i = 0; i < 30; ++i) {
    int x = (i * 47 + frame * (2 + i % 4)) % SCREEN_W;
    int y = (i * 29 + frame * (3 + i % 3)) % SCREEN_H;
    uint16_t color = i % 3 == 0 ? C_GOLD : (i % 3 == 1 ? C_WHITE : C_RED);
    if (i & 1) canvas.fillRect(x, y, 3, 7, color);
    else canvas.fillCircle(x, y, 2, color);
  }
  int pulse = 2 + (int)(3.0f * (1.0f + sinf(frame * 0.25f)) * 0.5f);
  canvas.fillCircle(160, 79, 50 + pulse, C_WHITE);
  pushTransparentAsset(112, 31, BENFICA_CREST_WIDTH,
                       BENFICA_CREST_HEIGHT, BENFICA_CREST);
  centeredText("GOLO!", 160, 145, &Graduate42, C_WHITE);
  centeredText(goalIsDemo ? "DO BENFICA" : shortened(scoreText, 20),
               160, 181, &Graduate18, C_WHITE);
  smallText(goalIsDemo ? "DEMONSTRACAO" : shortened(scorerText, 32),
            160, 210, C_WHITE, MC_DATUM);
}

static void renderFrame() {
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  canvas.fillScreen(C_BLACK);
  if (transitionActive) {
    unsigned long elapsed = millis() - transitionStarted;
    int progress = min(320UL, elapsed * 320UL / 270UL);
    int fromX = transitionDirection > 0 ? -progress : progress;
    int toX = transitionDirection > 0 ? SCREEN_W - progress
                                      : -SCREEN_W + progress;
    drawPage(transitionFrom, fromX);
    drawPage(transitionTo, toX);
    if (progress >= SCREEN_W) {
      currentPage = transitionTo;
      transitionActive = false;
    }
  } else if (overlay != OVERLAY_NONE) {
    switch (overlay) {
      case OVERLAY_GLORIOSO: drawGloriosoOverlay(); break;
      case OVERLAY_PAPOILAS: drawPapoilasOverlay(); break;
      case OVERLAY_GOAL: drawGoalOverlay(); break;
      default: break;
    }
  } else {
    drawPage(currentPage, 0);
  }
  canvas.pushSprite(0, 0);
  if (dataMutex) xSemaphoreGive(dataMutex);
}

static void stopAudioOnly(bool notifyMac) {
#if LOCAL_SPEAKER_ENABLED
  if (audioDecoder && audioDecoder->isRunning()) audioDecoder->stop();
  if (audioFile) audioFile->close();
  digitalWrite(PIN_AUDIO_ENABLE, HIGH);
  selectHapticBus();
#endif
  audioPlaying = false;
  remoteAudioPlaying = false;
  remoteAudioUntil = 0;
  hymnPlaying = false;
  audioOwnsNetworkMutex = false;
  if (notifyMac) Serial.println("AUDIO|STOP");
}

static bool playLocalAudio(const char *path) {
#if LOCAL_SPEAKER_ENABLED
  if (!sdReady) {
    Serial.printf("AUDIO|ERROR|SD_MISSING|%s\n", path);
    return false;
  }
  if (!SD.exists(path)) {
    Serial.printf("AUDIO|ERROR|FILE_MISSING|%s\n", path);
    return false;
  }
  if (!audioFile || !audioDecoder || !audioOutput) {
    Serial.printf("AUDIO|ERROR|OBJECTS|%s\n", path);
    return false;
  }
  if (!audioFile->open(path)) {
    Serial.printf("AUDIO|ERROR|OPEN|%s\n", path);
    return false;
  }
  uint32_t fileSize = audioFile->getSize();
  if (audioId3) delete audioId3;
  audioId3 = new AudioFileSourceID3(audioFile);
  if (!audioId3) {
    Serial.printf("AUDIO|ERROR|ID3|%s\n", path);
    audioFile->close();
    return false;
  }
  // GPIO25 is shared by the I2C clock and the onboard analogue amplifier.
  // The haptic waveform is already running autonomously, so release I2C and
  // hand the pin to the internal DAC for the duration of the track.
  selectAudioDac();
  digitalWrite(PIN_AUDIO_ENABLE, LOW);
  audioOutput->SetGain(1.20f);
  audioPlaying = audioDecoder->begin(audioId3, audioOutput);
  if (!audioPlaying) {
    Serial.printf("AUDIO|ERROR|DECODER|%s|%lu\n", path,
                  (unsigned long)fileSize);
    audioFile->close();
    digitalWrite(PIN_AUDIO_ENABLE, HIGH);
    selectHapticBus();
  } else {
    Serial.printf("AUDIO|PLAY|%s|%lu\n", path,
                  (unsigned long)fileSize);
  }
  return audioPlaying;
#else
  (void)path;
  return false;
#endif
}

static void startTrack(const char *path, const char *macName,
                       unsigned long durationMs) {
  if (!playLocalAudio(path)) {
    // Keep the visual duration deterministic while reporting a missing file;
    // no computer-side playback is required or attempted.
    remoteAudioPlaying = true;
    remoteAudioUntil = millis() + durationMs;
    Serial.printf("AUDIO|MISSING|%s|%s\n", macName, path);
  }
}

static void startGlorioso() {
  stopAudioOnly(false);
  hapticConfirm();
  overlay = OVERLAY_GLORIOSO;
  overlayStarted = millis();
  startTrack(AUDIO_GLORIOSO, "GLORIOSO", 13200UL);
}

static void startPapoilas() {
  stopAudioOnly(false);
  const uint8_t sequence[] = {7, 0x83, 7};
  playHapticSequence(sequence, sizeof(sequence));
  overlay = OVERLAY_PAPOILAS;
  overlayStarted = millis();
  startTrack(AUDIO_PAPOILAS, "PAPOILAS", 12200UL);
}

static void startGoal(bool demo) {
  stopAudioOnly(false);
  hapticGoal();
  goalIsDemo = demo;
  overlay = OVERLAY_GOAL;
  overlayStarted = millis();
  startTrack(AUDIO_GOAL, "GOAL", 18800UL);
}

static void startHymn() {
  stopAudioOnly(false);
  hapticConfirm();
  overlay = OVERLAY_NONE;
  currentPage = PAGE_HYMN;
  hymnStartedAt = millis();
  hymnPlaying = true;
  startTrack(AUDIO_HINO, "HINO", HINO_DURATION_MS);
  hymnPlaying = true;
  hymnStartedAt = millis();
}

static void finishPlayback() {
#if LOCAL_SPEAKER_ENABLED
  if (audioDecoder) audioDecoder->stop();
  if (audioFile) audioFile->close();
  digitalWrite(PIN_AUDIO_ENABLE, HIGH);
  selectHapticBus();
#endif
  audioPlaying = false;
  remoteAudioPlaying = false;
  remoteAudioUntil = 0;
  if (hymnPlaying) hymnPlaying = false;
  else if (overlay != OVERLAY_NONE) overlay = OVERLAY_NONE;
  audioOwnsNetworkMutex = false;
}

static void serviceAudio() {
#if LOCAL_SPEAKER_ENABLED
  if (audioPlaying && audioDecoder) {
    if (!audioDecoder->isRunning() || !audioDecoder->loop()) finishPlayback();
  }
#endif
  if (remoteAudioPlaying && (long)(millis() - remoteAudioUntil) >= 0) {
    finishPlayback();
  }
}

static bool inside(int x, int y, int left, int top, int width, int height) {
  return x >= left && x < left + width && y >= top && y < top + height;
}

static void beginTransition(Page target, int direction) {
  if (target == currentPage || transitionActive) return;
  if (hymnPlaying || overlay != OVERLAY_NONE || audioPlaying || remoteAudioPlaying) {
    stopAudioOnly(true);
  }
  overlay = OVERLAY_NONE;
  // Drawing two complete animated pages at once made the original slide take
  // too long on this ESP32. Switching the prepared page in one buffered frame
  // feels immediate and never exposes a partially drawn screen.
  (void)direction;
  currentPage = target;
  transitionActive = false;
  hapticSoftTap();
  renderFrame();
  lastFrame = millis();
}

static void handleSwipe(int dx) {
  if (dx < 0 && currentPage < PAGE_COUNT - 1) {
    beginTransition((Page)(currentPage + 1), 1);
  } else if (dx > 0 && currentPage > PAGE_HOME) {
    beginTransition((Page)(currentPage - 1), -1);
  } else {
    if (overlay != OVERLAY_NONE) {
      stopAudioOnly(true);
      overlay = OVERLAY_NONE;
    }
    hapticSoftTap();
  }
}

static void handleTap(int x, int y) {
  Serial.printf("TOUCH|%d|%d\n", x, y);
  if (overlay != OVERLAY_NONE) {
    if (millis() - overlayStarted > 650) {
      stopAudioOnly(true);
      overlay = OVERLAY_NONE;
      hapticSoftTap();
    }
    return;
  }

  if (currentPage == PAGE_HOME) {
    if (inside(x, y, 104, 46, 112, 122)) startGoal(true);
    else if (inside(x, y, 8, 175, 150, 50)) startGlorioso();
    else if (inside(x, y, 162, 175, 150, 50)) startPapoilas();
  } else if (currentPage == PAGE_HYMN && inside(x, y, 38, 47, 244, 111)) {
    if (!hymnPlaying) startHymn();
  }
}

static bool readTouchPoint(int &x, int &y) {
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  if (!tft.getTouch(&rawX, &rawY, 600)) return false;
  x = map(rawY, 0, tft.height(), 0, tft.width());
  y = map(tft.width() - rawX, 0, tft.width(), 0, tft.height());
  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);
  return true;
}

static void pollTouch() {
  if (millis() - lastTouchPoll < 20) return;
  lastTouchPoll = millis();
  int x = 0;
  int y = 0;
  bool down = readTouchPoint(x, y);
  if (down) {
    if (!touchWasDown) {
      touchStartX = touchLastX = x;
      touchStartY = touchLastY = y;
    } else {
      touchLastX = x;
      touchLastY = y;
    }
  } else if (touchWasDown && !transitionActive && millis() - lastTouchEvent > 120) {
    int dx = touchLastX - touchStartX;
    int dy = touchLastY - touchStartY;
    lastTouchEvent = millis();
    if (abs(dx) > 48 && abs(dx) > abs(dy) * 5 / 4) handleSwipe(dx);
    else if (abs(dx) < 24 && abs(dy) < 24) handleTap(touchStartX, touchStartY);
  }
  touchWasDown = down;
}

static String fieldAt(const String &line, int index) {
  int start = 0;
  for (int current = 0; current <= index; ++current) {
    int end = line.indexOf('|', start);
    if (current == index) {
      if (end < 0) return line.substring(start);
      return line.substring(start, end);
    }
    if (end < 0) return "";
    start = end + 1;
  }
  return "";
}

static void parseTable(const String &payload) {
  tableCount = 0;
  int start = 0;
  while (start < (int)payload.length() && tableCount < MAX_TABLE_TEAMS) {
    int end = payload.indexOf(';', start);
    if (end < 0) end = payload.length();
    String entry = payload.substring(start, end);
    int split = entry.lastIndexOf('~');
    if (split > 0) {
      tableTeam[tableCount] = entry.substring(0, split);
      tableTeam[tableCount].trim();
      tablePoints[tableCount] = constrain(entry.substring(split + 1).toInt(), 0, 255);
      ++tableCount;
    }
    start = end + 1;
  }
  Serial.printf("DATA|TABLE|%u\n", tableCount);
}

static void saveCachedSchedule() {
  if (!preferencesReady) return;
  String payload;
  payload.reserve(180);
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  payload = nextOpponent + "|" + nextDate + "|" + nextTimePortugal + "|" +
            nextTimeSpain + "|" + activeEventId + "|" + activeLeague + "|" +
            String((long)activeEventEpoch) + "|" + competitionText + "|" +
            String(nextIsChampions ? 1 : 0);
  if (dataMutex) xSemaphoreGive(dataMutex);
  if (payload != preferences.getString("onlineSched", "")) {
    preferences.putString("onlineSched", payload);
  }
}

static void saveCachedTable() {
  if (!preferencesReady) return;
  String payload;
  payload.reserve(520);
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (uint8_t i = 0; i < tableCount; ++i) {
    if (i) payload += ';';
    payload += tableTeam[i] + "~" + String(tablePoints[i]);
  }
  if (dataMutex) xSemaphoreGive(dataMutex);
  if (payload.length() && payload != preferences.getString("onlineTable", "")) {
    preferences.putString("onlineTable", payload);
  }
}

static void loadCachedOnlineData() {
  if (!preferencesReady) return;
  String schedule = preferences.getString("onlineSched", "");
  if (schedule.length()) {
    if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
    nextOpponent = fieldAt(schedule, 0);
    nextDate = fieldAt(schedule, 1);
    nextTimePortugal = fieldAt(schedule, 2);
    nextTimeSpain = fieldAt(schedule, 3);
    activeEventId = fieldAt(schedule, 4);
    activeLeague = fieldAt(schedule, 5);
    activeEventEpoch = (time_t)fieldAt(schedule, 6).toInt();
    competitionText = fieldAt(schedule, 7);
    nextIsChampions = fieldAt(schedule, 8).toInt() != 0;
    championsMatch = nextIsChampions;
    if (dataMutex) xSemaphoreGive(dataMutex);
    Serial.println("CACHE|SCHEDULE|READY");
  }
  String table = preferences.getString("onlineTable", "");
  if (table.length()) {
    if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
    parseTable(table);
    if (dataMutex) xSemaphoreGive(dataMutex);
    Serial.println("CACHE|TABLE|READY");
  }
}

static void handleSerialLine(String line) {
  line.trim();
  if (!line.length()) return;
  String command = fieldAt(line, 0);
  command.toUpperCase();

  if (command == "GOAL") {
    String incomingScore = fieldAt(line, 1);
    String incomingScorer = fieldAt(line, 2);
    String incomingHistory = fieldAt(line, 3);
    if (incomingScore.length()) scoreText = incomingScore;
    if (incomingScorer.length()) scorerText = incomingScorer;
    if (incomingHistory.length()) goalHistory = incomingHistory;
    matchActive = true;
    currentPage = PAGE_MATCH;
    startGoal(false);
  } else if (command == "MATCH") {
    String incomingScore = fieldAt(line, 1);
    String incomingHistory = fieldAt(line, 2);
    if (incomingScore.length()) scoreText = incomingScore;
    goalHistory = incomingHistory;
    matchActive = true;
    Serial.printf("DATA|MATCH|%u\n", goalHistory.length());
  } else if (command == "NEXT") {
    nextOpponent = fieldAt(line, 1);
    nextDate = fieldAt(line, 2);
    nextTimePortugal = fieldAt(line, 3);
    nextTimeSpain = fieldAt(line, 4);
    String incomingEventId = fieldAt(line, 5);
    String incomingLeague = fieldAt(line, 6);
    String incomingEpoch = fieldAt(line, 7);
    String incomingCompetition = fieldAt(line, 8);
    if (incomingEventId.length()) activeEventId = incomingEventId;
    if (incomingLeague.length()) activeLeague = incomingLeague;
    if (incomingEpoch.length()) activeEventEpoch = (time_t)incomingEpoch.toInt();
    if (incomingCompetition.length()) competitionText = incomingCompetition;
    nextIsChampions = activeLeague == "uefa.champions";
    championsMatch = nextIsChampions;
    matchActive = false;
    saveCachedSchedule();
    Serial.println("DATA|NEXT|READY");
  } else if (command == "TABLE") {
    parseTable(fieldAt(line, 1));
    saveCachedTable();
  } else if (command == "MATCH_END" || command == "IDLE") {
    matchActive = false;
  } else if (command == "STOP") {
    stopAudioOnly(true);
    overlay = OVERLAY_NONE;
  } else if (command == "LEFT" || command == "GLORIOSO") {
    startGlorioso();
  } else if (command == "RIGHT" || command == "PAPOILAS") {
    startPapoilas();
  } else if (command == "DEMO") {
    startGoal(true);
  } else if (command == "HINO") {
    startHymn();
  } else if (command == "PAGE") {
    int page = constrain(fieldAt(line, 1).toInt(), 0, PAGE_COUNT - 1);
    currentPage = (Page)page;
    overlay = OVERLAY_NONE;
  } else if (command == "HAPTIC") {
    if (!hapticReady) hapticReady = initHaptics();
    hapticConfirm();
    Serial.printf("HAPTIC|TEST|%s\n", hapticReady ? "READY" : "MISSING");
  } else if (command == "TRANSFER") {
    String action = fieldAt(line, 1);
    action.toUpperCase();
    transferSession = action == "BEGIN";
    Serial.printf("TRANSFER|%s\n", transferSession ? "READY" : "DONE");
  } else if (command == "FILECRC") {
    String path = fieldAt(line, 1);
    if (!sdReady || !path.startsWith("/Benfica/") || path.indexOf("..") >= 0) {
      Serial.println("FILECRC|ERROR|INVALID");
      return;
    }
    File checkFile = SD.open(path, FILE_READ);
    if (!checkFile) {
      Serial.printf("FILECRC|ERROR|OPEN|%s\n", path.c_str());
      return;
    }
    uint32_t crc = 0xFFFFFFFFUL;
    uint8_t checkBuffer[512];
    while (checkFile.available()) {
      size_t count = checkFile.read(checkBuffer, sizeof(checkBuffer));
      for (size_t i = 0; i < count; ++i) {
        crc ^= checkBuffer[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
          crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
      }
    }
    uint32_t checkedSize = checkFile.size();
    checkFile.close();
    Serial.printf("FILECRC|OK|%s|%lu|%08lX\n", path.c_str(),
                  (unsigned long)checkedSize, (unsigned long)(crc ^ 0xFFFFFFFFUL));
  } else if (command == "UPLOAD") {
    String path = fieldAt(line, 1);
    uint32_t size = strtoul(fieldAt(line, 2).c_str(), nullptr, 10);
    if (!sdReady || !path.startsWith("/Benfica/") || path.indexOf("..") >= 0 || !size) {
      Serial.println("UPLOAD|ERROR|INVALID");
      return;
    }
    stopAudioOnly(false);
    SD.mkdir("/Benfica");
    if (SD.exists(path)) SD.remove(path);
    uploadFile = SD.open(path, FILE_WRITE);
    if (!uploadFile) {
      Serial.println("UPLOAD|ERROR|OPEN");
      return;
    }
    uploadPath = path;
    uploadExpected = size;
    uploadReceived = 0;
    uploadActive = true;
    Serial.printf("UPLOAD|READY|%s|%lu\n", path.c_str(),
                  (unsigned long)size);
  } else if (command == "UISTATUS") {
    Serial.printf("UI|READY|PAGES=4|TABLE=%u|LYRICS=%u|SD=%s|HAPTIC=%s|WIFI=%s|RSSI=%d|REASON=%u|NET=%u|HEAP=%lu|NEXT=%s|AUDIO=%s\n",
                  tableCount, lyricCount,
                  sdReady ? "READY" : "MISSING",
                  hapticReady ? "READY" : "MISSING",
                  WiFi.status() == WL_CONNECTED ? "READY" : "OFFLINE",
                  WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                  lastWifiDisconnectReason, networkStage,
                  (unsigned long)ESP.getFreeHeap(),
                  nextOpponent.c_str(),
#if LOCAL_SPEAKER_ENABLED
                  "LOCAL");
#else
                  "MAC");
#endif
  } else if (command == "REFRESH") {
    forceNetworkRefresh = true;
    Serial.println("ONLINE|REFRESH|REQUESTED");
  }
}

static void pollSerial() {
  if (uploadActive) {
    uint8_t buffer[1024];
    while (Serial.available() && uploadReceived < uploadExpected) {
      size_t wanted = min((uint32_t)sizeof(buffer), uploadExpected - uploadReceived);
      wanted = min(wanted, (size_t)Serial.available());
      size_t received = Serial.read(buffer, wanted);
      if (!received) break;
      size_t written = uploadFile.write(buffer, received);
      uploadReceived += written;
      if (written != received) {
        uploadFile.close();
        uploadActive = false;
        Serial.println("UPLOAD|ERROR|WRITE");
        return;
      }
    }
    if (uploadReceived >= uploadExpected) {
      uploadFile.flush();
      uploadFile.close();
      uploadActive = false;
      Serial.printf("UPLOAD|DONE|%s|%lu\n", uploadPath.c_str(),
                    (unsigned long)uploadReceived);
    }
    return;
  }
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n') {
      handleSerialLine(serialLine);
      serialLine = "";
    } else if (ch != '\r' && serialLine.length() < 1200) {
      serialLine += ch;
    }
  }
}

static bool parseLrcTimestamp(const String &line, uint32_t &atMs, int &textStart) {
  if (!line.startsWith("[") || line.length() < 9) return false;
  int colon = line.indexOf(':');
  int close = line.indexOf(']');
  if (colon < 2 || close < colon + 3) return false;
  int minutes = line.substring(1, colon).toInt();
  float seconds = line.substring(colon + 1, close).toFloat();
  atMs = minutes * 60000UL + (uint32_t)(seconds * 1000.0f);
  textStart = close + 1;
  return true;
}

static void loadLyrics() {
  lyricCount = 0;
  for (const BuiltinLyricLine &line : BUILTIN_LYRICS) {
    if (lyricCount >= MAX_LYRIC_LINES) break;
    lyrics[lyricCount].atMs = line.atMs;
    lyrics[lyricCount].text = line.text;
    ++lyricCount;
  }
  Serial.printf("LYRICS|BUILTIN|%u\n", lyricCount);
}

static void initColors() {
  C_BG = tft.color565(8, 8, 11);
  C_BG_2 = tft.color565(22, 15, 18);
  C_SURFACE = tft.color565(29, 24, 28);
  C_SURFACE_2 = tft.color565(45, 35, 40);
  C_RED = tft.color565(218, 0, 30);
  C_RED_DARK = tft.color565(91, 0, 14);
  C_GOLD = tft.color565(235, 187, 61);
  C_WHITE = tft.color565(250, 248, 245);
  C_MUTED = tft.color565(180, 169, 176);
  C_BORDER = tft.color565(84, 67, 75);
  C_GREEN = tft.color565(47, 163, 92);
  C_BLACK = TFT_BLACK;
}

static void drawBootScreen(const String &status, int progress) {
  tft.fillScreen(C_BLACK);
  for (int row = 0; row < SCREEN_H; ++row) {
    uint8_t amount = (uint32_t)row * 255 / (SCREEN_H - 1);
    tft.drawFastHLine(0, row, SCREEN_W, mix565(C_RED_DARK, C_BLACK, amount));
  }
  tft.fillCircle(160, 81, 52, C_WHITE);
  tft.setSwapBytes(true);
  tft.pushImage(112, 33, BENFICA_CREST_WIDTH, BENFICA_CREST_HEIGHT,
                BENFICA_CREST, BENFICA_IMAGE_KEY);
  tft.setFreeFont(&Graduate18);
  tft.setTextColor(C_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("SL BENFICA", 160, 153);
  tft.fillRoundRect(54, 184, 212, 7, 3, C_SURFACE_2);
  tft.fillRoundRect(54, 184, constrain(progress, 0, 100) * 212 / 100,
                    7, 3, C_RED);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_MUTED);
  tft.drawString(status, 160, 211);
}

static void drawWifiSetupScreen(const String &line1, const String &line2) {
  tft.fillScreen(C_BLACK);
  for (int row = 0; row < SCREEN_H; ++row) {
    uint8_t amount = (uint32_t)row * 255 / (SCREEN_H - 1);
    tft.drawFastHLine(0, row, SCREEN_W, mix565(C_RED_DARK, C_BLACK, amount));
  }
  tft.fillRoundRect(20, 22, 280, 196, 20, C_SURFACE);
  tft.drawRoundRect(20, 22, 280, 196, 20, C_BORDER);
  tft.setFreeFont(&Graduate26);
  tft.setTextColor(C_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("WIFI", 160, 58);
  tft.setFreeFont(&Graduate18);
  tft.setTextColor(C_RED);
  tft.drawString(line1, 160, 104);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_WHITE);
  tft.drawString(line2, 160, 139);
  tft.setFreeFont(nullptr);
  tft.setTextFont(1);
  tft.setTextColor(C_MUTED);
  tft.drawString("A CONFIGURACAO FICA GUARDADA NO APARELHO", 160, 186);
}

static String wifiPortalHtml(const String &message = "") {
  String html =
    "<!doctype html><html lang='pt'><head><meta name='viewport' "
    "content='width=device-width,initial-scale=1'><style>"
    "body{font-family:system-ui;background:#111;color:#fff;max-width:430px;"
    "margin:40px auto;padding:20px}h1{color:#da001e}input,button{box-sizing:"
    "border-box;width:100%;padding:14px;margin:8px 0;border-radius:10px;"
    "border:1px solid #555;font-size:17px}button{background:#da001e;color:"
    "white;font-weight:700;border:0}</style></head><body><h1>Benfica CYD</h1>";
  if (message.length()) html += "<p>" + message + "</p>";
  html +=
    "<p>Escolha a rede Wi-Fi da casa. Os dados ficam guardados apenas no "
    "dispositivo.</p><form method='post' action='/save'><input name='ssid' "
    "placeholder='Nome da rede Wi-Fi' required><input name='pass' "
    "type='password' placeholder='Palavra-passe'><button>Guardar e ligar"
    "</button></form></body></html>";
  return html;
}

static bool connectStation(const String &ssid, const String &password,
                           unsigned long timeoutMs) {
  if (!ssid.length()) return false;
  wifiSsid = ssid;
  wifiPassword = password;
  WiFi.mode(WIFI_STA);
  esp_wifi_set_country_code("ES", true);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    delay(120);
  }
  wifiReady = WiFi.status() == WL_CONNECTED;
  return wifiReady;
}

static bool configureWifi() {
  preferencesReady = preferences.begin("benfica-cyd", false);
  loadCachedOnlineData();
  String savedSsid = preferences.getString("ssid", "");
  String savedPassword = preferences.getString("pass", "");
  if (savedSsid.length()) {
    drawWifiSetupScreen("A LIGAR", savedSsid);
    if (connectStation(savedSsid, savedPassword, 14000UL)) {
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      Serial.printf("WIFI|READY|%s|%s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
  }

  bool submitted = false;
  String submittedSsid;
  String submittedPassword;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WIFI_AP_NAME);
  IPAddress portalIp = WiFi.softAPIP();
  dnsServer.start(WIFI_DNS_PORT, "*", portalIp);

  setupServer.on("/", HTTP_GET, [&]() {
    setupServer.send(200, "text/html; charset=utf-8", wifiPortalHtml());
  });
  setupServer.on("/save", HTTP_POST, [&]() {
    submittedSsid = setupServer.arg("ssid");
    submittedPassword = setupServer.arg("pass");
    submittedSsid.trim();
    if (!submittedSsid.length()) {
      setupServer.send(400, "text/html; charset=utf-8",
                       wifiPortalHtml("Falta o nome da rede."));
      return;
    }
    submitted = true;
    setupServer.send(200, "text/html; charset=utf-8",
                     wifiPortalHtml("Dados recebidos. Pode voltar ao Benfica CYD."));
  });
  setupServer.onNotFound([&]() {
    setupServer.sendHeader("Location", "http://192.168.4.1", true);
    setupServer.send(302, "text/plain", "");
  });
  setupServer.begin();
  drawWifiSetupScreen(WIFI_AP_NAME, "ABRA 192.168.4.1 NO TELEMOVEL");

  while (!wifiReady) {
    dnsServer.processNextRequest();
    setupServer.handleClient();
    if (submitted) {
      drawWifiSetupScreen("A LIGAR", submittedSsid);
      if (connectStation(submittedSsid, submittedPassword, 18000UL)) {
        preferences.putString("ssid", submittedSsid);
        preferences.putString("pass", submittedPassword);
        break;
      }
      submitted = false;
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(WIFI_AP_NAME);
      drawWifiSetupScreen("PALAVRA-PASSE INCORRETA", "VOLTE A 192.168.4.1");
    }
    delay(8);
  }

  setupServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  wifiReady = WiFi.status() == WL_CONNECTED;
  if (wifiReady) {
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    Serial.printf("WIFI|READY|%s|%s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
  }
  return wifiReady;
}

static int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (int)doe - 719468LL;
}

static time_t epochFromIso(const String &iso) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d",
             &year, &month, &day, &hour, &minute, &second) < 5) return 0;
  return (time_t)(daysFromCivil(year, month, day) * 86400LL +
                  hour * 3600LL + minute * 60LL + second);
}

static int weekdayUtc(int year, int month, int day) {
  int64_t days = daysFromCivil(year, month, day);
  return (int)((days + 4) % 7 + 7) % 7;
}

static bool europeDst(time_t utc) {
  struct tm current;
  gmtime_r(&utc, &current);
  int year = current.tm_year + 1900;
  int marchSunday = 31 - weekdayUtc(year, 3, 31);
  int octoberSunday = 31 - weekdayUtc(year, 10, 31);
  time_t start = daysFromCivil(year, 3, marchSunday) * 86400LL + 3600;
  time_t end = daysFromCivil(year, 10, octoberSunday) * 86400LL + 3600;
  return utc >= start && utc < end;
}

static void formatEventTime(time_t utc, String &dateLabel,
                            String &portugalLabel, String &spainLabel) {
  static const char *months[] = {
    "JAN", "FEV", "MAR", "ABR", "MAI", "JUN",
    "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"
  };
  bool summer = europeDst(utc);
  time_t portugal = utc + (summer ? 3600 : 0);
  time_t spain = portugal + 3600;
  struct tm pt;
  struct tm es;
  gmtime_r(&portugal, &pt);
  gmtime_r(&spain, &es);
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%02d %s %04d",
           pt.tm_mday, months[pt.tm_mon], pt.tm_year + 1900);
  dateLabel = buffer;
  snprintf(buffer, sizeof(buffer), "%02d:%02d PT", pt.tm_hour, pt.tm_min);
  portugalLabel = buffer;
  snprintf(buffer, sizeof(buffer), "%02d:%02d ES", es.tm_hour, es.tm_min);
  spainLabel = buffer;
}

static bool openEspnResponseOnce(WiFiClient &client, const String &url) {
  int schemeEnd = url.indexOf("://");
  int hostStart = schemeEnd >= 0 ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', hostStart);
  String host = pathStart >= 0 ? url.substring(hostStart, pathStart)
                               : url.substring(hostStart);
  String path = pathStart >= 0 ? url.substring(pathStart) : "/";
  if (!host.length()) return false;
  client.stop();
  client.setTimeout(7);
  if (!client.connect(host.c_str(), 80, 4000)) {
    Serial.printf("HTTP|ERROR|RAW_CONNECT|%s\n", host.c_str());
    return false;
  }
  client.setNoDelay(true);
  String request = "GET " + path +
      " HTTP/1.0\r\nHost: " + host + "\r\n"
      "User-Agent: Mozilla/5.0 Benfica-CYD\r\n"
      "Accept: application/json\r\nAccept-Language: en\r\n"
      "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
  size_t sent = client.write((const uint8_t *)request.c_str(), request.length());
  if (sent != request.length()) {
    Serial.printf("HTTP|ERROR|RAW_WRITE|SENT=%u|EXPECTED=%u\n",
                  (unsigned)sent, (unsigned)request.length());
    client.stop();
    return false;
  }

  unsigned long started = millis();
  while (client.available() <= 0 && client.connected() &&
         millis() - started < 7000UL) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  int availableBytes = client.available();
  if (availableBytes <= 0) {
    Serial.printf("HTTP|ERROR|RAW_TIMEOUT|CONNECTED=%d|AVAILABLE=%d\n",
                  client.connected(), availableBytes);
    client.stop();
    return false;
  }
  String statusLine;
  for (uint8_t line = 0; line < 3 && !statusLine.length(); ++line) {
    statusLine = client.readStringUntil('\n');
    statusLine.trim();
  }
  if (!statusLine.startsWith("HTTP/1.1 200") &&
      !statusLine.startsWith("HTTP/1.0 200")) {
    Serial.printf("HTTP|ERROR|RAW_STATUS|%s\n",
                  statusLine.substring(0, 64).c_str());
    client.stop();
    return false;
  }
  int contentLength = -1;
  bool chunked = false;
  while (client.connected() || client.available() > 0) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (!header.length()) {
      Serial.printf("HTTP|READY|LEN=%d|CHUNKED=%d\n", contentLength, chunked);
      return true;
    }
    String normalizedHeader = header;
    normalizedHeader.toLowerCase();
    if (normalizedHeader.startsWith("content-length:")) {
      contentLength = header.substring(15).toInt();
    } else if (normalizedHeader.startsWith("transfer-encoding:") &&
               normalizedHeader.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }
  Serial.printf("HTTP|ERROR|RAW_HEADERS|%s\n", host.c_str());
  client.stop();
  return false;
}

static bool openEspnResponse(WiFiClient &client, const String &url) {
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    if (openEspnResponseOnce(client, url)) return true;
    client.stop();
    if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(450));
  }
  return false;
}

static bool fetchFilteredJson(const String &url, JsonDocument &document,
                              JsonDocument &filter) {
  if (WiFi.status() != WL_CONNECTED) {
    wifiReady = false;
    return false;
  }
  WiFiClient client;
  if (!openEspnResponse(client, url)) return false;
  CooperativeStream stream(client);
  DeserializationError error = deserializeJson(
      document, stream, DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(32));
  client.stop();
  if (error) {
    Serial.printf("JSON|ERROR|%s\n", error.c_str());
    return false;
  }
  wifiReady = true;
  return true;
}

static bool fetchCoreNextEvent(const String &league, int seasonYear,
                               const String &dateRange, String &eventId,
                               String &eventDate, String &eventName) {
  if (WiFi.status() != WL_CONNECTED) {
    wifiReady = false;
    return false;
  }
  String listUrl = "http://sports.core.api.espn.com/v2/sports/soccer/leagues/" +
      league + "/seasons/" + String(seasonYear) +
      "/types/1/teams/1929/events?lang=en&region=us&limit=100&dates=" +
      dateRange;
  WiFiClient client;
  if (!openEspnResponse(client, listUrl)) return false;
  CooperativeStream stream(client);
  if (!stream.find("\"items\":[")) {
    client.stop();
    return false;
  }
  int firstItem = stream.peek();
  if (firstItem == ']') {
    client.stop();
    eventId = "";
    Serial.printf("ONLINE|NO_EVENT|%s\n", league.c_str());
    return true;
  }
  String eventUrl;
  bool foundReference = findJsonText(stream, "\"$ref\":\"", eventUrl, 220);
  client.stop();
  if (!foundReference || !eventUrl.length()) {
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(120));
  WiFiClient eventClient;
  if (!openEspnResponse(eventClient, eventUrl)) return false;
  CooperativeStream eventStream(eventClient);
  bool ok = findJsonText(eventStream, "\"id\":\"", eventId, 24) &&
            findJsonText(eventStream, "\"date\":\"", eventDate, 32) &&
            findJsonText(eventStream, "\"name\":\"", eventName, 96);
  eventClient.stop();
  if (!ok) {
    Serial.printf("JSON|ERROR|CORE_EVENT_FIELDS|%s\n", league.c_str());
    client.stop();
    return false;
  }
  wifiReady = true;
  return true;
}

static String teamName(JsonVariantConst team) {
  const char *name = team["shortDisplayName"] | nullptr;
  if (!name || !*name) name = team["displayName"] | nullptr;
  if (!name || !*name) name = team["abbreviation"] | "RIVAL";
  return String(name);
}

static bool readCompetitors(JsonArrayConst competitors, String &scoreline,
                            String &opponentName, int &benficaScore) {
  String benfica = "0";
  String opponentScore = "0";
  bool found = false;
  for (JsonObjectConst competitor : competitors) {
    String id = competitor["team"]["id"] | "";
    if (id == "1929") {
      benfica = String((const char *)(competitor["score"] | "0"));
      benficaScore = benfica.toInt();
      found = true;
    } else {
      opponentName = teamName(competitor["team"]);
      opponentScore = String((const char *)(competitor["score"] | "0"));
    }
  }
  if (found) scoreline = "SLB " + benfica + "-" + opponentScore + " " + opponentName;
  return found;
}

static bool refreshSchedule() {
  time_t now = time(nullptr);
  if (now < 1700000000 && activeEventEpoch > 1700000000)
    now = activeEventEpoch - 86400;
  if (now < 1700000000) return false;
  time_t rangeEnd = now + 45L * 86400L;
  struct tm startTime;
  struct tm endTime;
  gmtime_r(&now, &startTime);
  gmtime_r(&rangeEnd, &endTime);
  char rangeBuffer[24];
  snprintf(rangeBuffer, sizeof(rangeBuffer), "%04d%02d%02d-%04d%02d%02d",
           startTime.tm_year + 1900, startTime.tm_mon + 1, startTime.tm_mday,
           endTime.tm_year + 1900, endTime.tm_mon + 1, endTime.tm_mday);
  int seasonYear = startTime.tm_year + 1900;
  if (startTime.tm_mon < 6) --seasonYear;

  static const char *leagues[] = {
      "por.1", "uefa.europa", "uefa.champions"
  };
  String chosenId;
  String chosenDate;
  String eventName;
  String league;
  time_t chosenEpoch = 0;
  bool allLeaguesReady = true;
  for (const char *candidateLeague : leagues) {
    String candidateId;
    String candidateDate;
    String candidateName;
    if (!fetchCoreNextEvent(candidateLeague, seasonYear, rangeBuffer,
                            candidateId, candidateDate, candidateName)) {
      allLeaguesReady = false;
      vTaskDelay(pdMS_TO_TICKS(180));
      continue;
    }
    if (!candidateId.length()) {
      vTaskDelay(pdMS_TO_TICKS(180));
      continue;
    }
    time_t candidateEpoch = epochFromIso(candidateDate);
    if (candidateEpoch >= now - 4L * 3600L &&
        (!chosenEpoch || candidateEpoch < chosenEpoch)) {
      chosenEpoch = candidateEpoch;
      chosenId = candidateId;
      chosenDate = candidateDate;
      eventName = candidateName;
      league = candidateLeague;
    }
    vTaskDelay(pdMS_TO_TICKS(180));
  }
  if (!allLeaguesReady) {
    Serial.println("ONLINE|SCHEDULE|KEEP_CACHE");
    return false;
  }
  if (!chosenEpoch) return false;
  String opponent = eventName;
  if (opponent.startsWith("Benfica at ")) opponent.remove(0, 11);
  else if (opponent.endsWith(" at Benfica"))
    opponent.remove(opponent.length() - 11);
  else if (opponent.startsWith("Benfica vs ")) opponent.remove(0, 11);
  else if (opponent.endsWith(" vs Benfica"))
    opponent.remove(opponent.length() - 11);
  if (!opponent.length()) opponent = "RIVAL";
  if (!league.length()) league = "por.1";
  String dateLabel;
  String portugalLabel;
  String spainLabel;
  formatEventTime(chosenEpoch, dateLabel, portugalLabel, spainLabel);

  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  activeEventId = chosenId;
  activeLeague = league;
  activeEventEpoch = chosenEpoch;
  nextOpponent = opponent;
  nextDate = dateLabel;
  nextTimePortugal = portugalLabel;
  nextTimeSpain = spainLabel;
  nextIsChampions = league == "uefa.champions";
  championsMatch = nextIsChampions;
  competitionText = championsMatch ? "CHAMPIONS" :
                    (league == "uefa.europa" ? "EUROPA" : "LIGA PORTUGUESA");
  if (dataMutex) xSemaphoreGive(dataMutex);
  Serial.printf("ONLINE|SCHEDULE|%s|%s|pre\n", activeEventId.c_str(),
                activeLeague.c_str());
  saveCachedSchedule();
  return true;
}

static bool refreshGoalHistory(const String &eventId) {
  JsonDocument filter;
  filter["header"]["competitions"][0]["details"][0]["scoringPlay"] = true;
  filter["header"]["competitions"][0]["details"][0]["team"]["id"] = true;
  filter["header"]["competitions"][0]["details"][0]["clock"]["displayValue"] = true;
  filter["header"]["competitions"][0]["details"][0]["participants"][0]["athlete"]["shortName"] = true;
  filter["header"]["competitions"][0]["details"][0]["participants"][0]["athlete"]["displayName"] = true;
  JsonDocument document;
  String url = "http://site.web.api.espn.com/apis/site/v2/sports/soccer/all/summary?event=" + eventId;
  if (!fetchFilteredJson(url, document, filter)) return false;

  String history;
  String latest = "GOLO DO BENFICA";
  for (JsonObjectConst detail :
       document["header"]["competitions"][0]["details"].as<JsonArrayConst>()) {
    bool scoring = detail["scoringPlay"] | false;
    String teamId = detail["team"]["id"] | "";
    if (!scoring || teamId != "1929") continue;
    String minute = detail["clock"]["displayValue"] | "--'";
    const char *name = detail["participants"][0]["athlete"]["shortName"] | nullptr;
    if (!name || !*name) name = detail["participants"][0]["athlete"]["displayName"] | "BENFICA";
    latest = String(name);
    if (history.length()) history += ';';
    history += minute + "  " + latest;
  }
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  goalHistory = history;
  scorerText = latest;
  if (dataMutex) xSemaphoreGive(dataMutex);
  return true;
}

static bool refreshActiveEvent() {
  String eventId = activeEventId;
  String league = activeLeague;
  if (!eventId.length() || !league.length()) return false;
  String dateDigits = nextDate;
  struct tm utc;
  gmtime_r(&activeEventEpoch, &utc);
  char dateBuffer[12];
  snprintf(dateBuffer, sizeof(dateBuffer), "%04d%02d%02d",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);

  JsonDocument filter;
  filter["events"][0]["id"] = true;
  filter["events"][0]["status"]["type"]["state"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["id"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["displayName"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["shortDisplayName"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["abbreviation"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["score"] = true;
  JsonDocument document;
  String url = "http://site.web.api.espn.com/apis/site/v2/sports/soccer/" + league +
               "/scoreboard?dates=" + dateBuffer;
  if (!fetchFilteredJson(url, document, filter)) return false;

  JsonObjectConst selected;
  for (JsonObjectConst event : document["events"].as<JsonArrayConst>()) {
    if (String((const char *)(event["id"] | "")) == eventId) {
      selected = event;
      break;
    }
  }
  if (!selected) return false;
  String state = selected["status"]["type"]["state"] | "pre";
  String localScore;
  String opponent;
  int benficaScore = 0;
  if (!readCompetitors(selected["competitions"][0]["competitors"].as<JsonArrayConst>(),
                       localScore, opponent, benficaScore)) return false;

  bool newGoal = state == "in" && lastNetworkBenficaScore >= 0 &&
                 benficaScore > lastNetworkBenficaScore;
  bool needsHistory = state == "in" && benficaScore > 0 &&
                      (newGoal || lastNetworkBenficaScore < 0 || goalHistory.length() == 0);
  if (needsHistory) refreshGoalHistory(eventId);

  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  scoreText = localScore;
  matchActive = state == "in";
  if (dataMutex) xSemaphoreGive(dataMutex);
  if (newGoal) pendingNetworkGoal = true;
  lastNetworkBenficaScore = benficaScore;
  if (state == "post") {
    lastNetworkBenficaScore = -1;
    activeEventId = "";
  }
  Serial.printf("ONLINE|EVENT|%s|%d\n", state.c_str(), benficaScore);
  return true;
}

static bool refreshStandings() {
  String url = "http://site.web.api.espn.com/apis/v2/sports/soccer/por.1/standings";
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  if (!openEspnResponse(client, url)) return false;

  CooperativeStream stream(client);
  if (!stream.find("\"entries\":[")) {
    Serial.println("JSON|ERROR|NO_TABLE_ENTRIES");
    client.stop();
    return false;
  }

  String names[MAX_TABLE_TEAMS];
  uint8_t points[MAX_TABLE_TEAMS] = {0};
  uint8_t found = 0;
  while (found < MAX_TABLE_TEAMS) {
    String name;
    int rank = 0;
    int pts = 0;
    const char *step = "team";
    bool ok = stream.find("\"team\":{");
    if (ok) {
      step = "name";
      ok = findJsonText(stream, "\"shortDisplayName\":\"", name, 48);
    }
    if (ok) {
      step = "points";
      ok = stream.find("\"name\":\"points\"");
    }
    if (ok) {
      step = "points_value";
      ok = stream.find("\"value\":") && readJsonInteger(stream, pts);
    }
    if (ok) {
      step = "rank";
      ok = stream.find("\"name\":\"rank\"");
    }
    if (ok) {
      step = "rank_value";
      ok = stream.find("\"value\":") && readJsonInteger(stream, rank);
    }
    if (!ok) {
      Serial.printf("JSON|ERROR|TABLE_FIELDS|INDEX=%u|STEP=%s\n", found, step);
      client.stop();
      return false;
    }
    if (rank < 1 || rank > MAX_TABLE_TEAMS) rank = found + 1;
    names[rank - 1] = name;
    points[rank - 1] = constrain(pts, 0, 255);
    ++found;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  client.stop();
  if (!found) return false;
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  tableCount = found;
  for (uint8_t i = 0; i < found; ++i) {
    tableTeam[i] = names[i];
    tablePoints[i] = points[i];
  }
  if (dataMutex) xSemaphoreGive(dataMutex);
  Serial.printf("ONLINE|TABLE|%u\n", found);
  saveCachedTable();
  return true;
}

static void networkTask(void *parameter) {
  (void)parameter;
  networkStage = 1;
  unsigned long lastSchedule = millis();
  unsigned long lastTable = millis();
  unsigned long lastEvent = 0;
  unsigned long lastScheduleAttempt = 0;
  unsigned long lastTableAttempt = 0;
  unsigned long lastEventAttempt = 0;
  unsigned long lastReconnectAttempt = 0;
  bool first = false;

  // Give the radio a short settling period after boot. Cached fixture and
  // table data are already visible, so this never delays the interface.
  vTaskDelay(pdMS_TO_TICKS(1800));

  for (;;) {
    if (forceNetworkRefresh) {
      forceNetworkRefresh = false;
      lastSchedule = lastTable = lastEvent = 0;
      lastScheduleAttempt = lastTableAttempt = lastEventAttempt = 0;
      first = true;
    }
    if (uploadActive || transferSession || audioPlaying) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      wifiReady = false;
      networkStage = 8;
      unsigned long reconnectNow = millis();
      if (wifiSsid.length() &&
          (!lastReconnectAttempt || reconnectNow - lastReconnectAttempt >= 15000UL)) {
        lastReconnectAttempt = reconnectNow;
        // The saved station is already configured by connectStation().
        // Repeating begin() during an automatic reconnect can interrupt an
        // in-flight association and was the source of reconnect loops.
        WiFi.reconnect();
        Serial.println("WIFI|RECONNECTING");
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    wifiReady = true;
    unsigned long nowMs = millis();
    bool scheduleDue = first || nowMs - lastSchedule >= SCHEDULE_REFRESH_MS ||
                       !activeEventId.length();
    if (scheduleDue && (first || nowMs - lastScheduleAttempt >= 30000UL)) {
      lastScheduleAttempt = nowMs;
      networkStage = 4;
      if (refreshSchedule()) lastSchedule = nowMs;
      networkStage = 5;
      vTaskDelay(pdMS_TO_TICKS(700));
    }
    if ((first || nowMs - lastTable >= TABLE_REFRESH_MS) &&
        (first || nowMs - lastTableAttempt >= 30000UL)) {
      lastTableAttempt = nowMs;
      networkStage = 2;
      if (refreshStandings()) lastTable = nowMs;
      networkStage = 3;
      vTaskDelay(pdMS_TO_TICKS(700));
    }

    time_t now = time(nullptr);
    long untilKickoff = activeEventEpoch && now > 1700000000
                          ? (long)(activeEventEpoch - now) : 999999L;
    bool matchWindow = untilKickoff <= 300L && untilKickoff > -14400L;
    unsigned long interval = (matchActive || matchWindow)
                               ? LIVE_REFRESH_MS : NEAR_MATCH_REFRESH_MS;
    bool eventCloseEnough = matchActive || matchWindow;
    if (activeEventId.length() && eventCloseEnough &&
        (first || nowMs - lastEvent >= interval) &&
        (first || nowMs - lastEventAttempt >= 3000UL)) {
      lastEventAttempt = nowMs;
      networkStage = 6;
      if (refreshActiveEvent()) lastEvent = nowMs;
    }
    networkStage = 7;
    first = false;
    vTaskDelay(pdMS_TO_TICKS(120));
  }
}

void setup() {
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("WIFI|DISCONNECTED|REASON=%u\n", lastWifiDisconnectReason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    lastWifiDisconnectReason = 0;
    Serial.println("WIFI|GOT_IP");
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  serialLine.reserve(1240);
  dataMutex = xSemaphoreCreateMutex();
  networkAudioMutex = xSemaphoreCreateMutex();

  pinMode(PIN_TFT_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_TFT_BACKLIGHT, HIGH);
  pinMode(PIN_AUDIO_ENABLE, OUTPUT);
  // AUDIO_EN is active-low on this Freenove board. Keep the amplifier muted
  // until actual samples are ready, which removes idle hiss and startup noise.
  digitalWrite(PIN_AUDIO_ENABLE, HIGH);

  tft.init();
  tft.setRotation(1);
  uint16_t calibration[5] = {286, 3534, 283, 3600, 6};
  tft.setTouch(calibration);
  initColors();

  // Reserve the only large contiguous block before SD, audio and Wi-Fi make
  // smaller heap allocations. This guarantees the full-screen framebuffer.
  canvas.setColorDepth(8);
  canvas.setSwapBytes(true);
  canvasReady = canvas.createSprite(SCREEN_W, SCREEN_H) != nullptr;
  if (!canvasReady) {
    drawBootScreen("ERRO DE MEMORIA", 100);
    Serial.println("UI|ERROR|NO_FRAMEBUFFER");
    return;
  }

  drawBootScreen("A INICIAR", 22);
  hapticReady = initHaptics();

  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdReady = SD.begin(PIN_SD_CS, sdSpi, 10000000, "/sd", 5, false);
  drawBootScreen(sdReady ? "MICROSD PRONTA" : "MODO SEM MICROSD", 61);

#if LOCAL_SPEAKER_ENABLED
  audioFile = new AudioFileSourceSD();
  audioId3 = new AudioFileSourceID3(audioFile);
  // Twelve DMA buffers keep the 32 kHz mono stream fed during a screen push,
  // while leaving a little more heap available to the Wi-Fi stack.
  audioOutput = new PersistentDacOutput(0, 12);
  audioDecoder = new AudioGeneratorMP3(audioDecoderMemory,
                                        sizeof(audioDecoderMemory));
  // Feed the same mono mix to both internal DAC channels. The onboard
  // amplifier listens to GPIO25, so no voice or instrument is lost when a
  // source MP3 happens to be panned towards the other stereo channel.
  audioOutput->SetOutputModeMono(true);
  audioOutput->SetGain(1.20f);
  // Reserve the DMA buffers before Wi-Fi and JSON allocations begin. The
  // persistent output's stop() only clears the buffers; it does not free them.
  audioDacInstalled = audioOutput->begin();
  if (!audioDacInstalled) Serial.println("AUDIO|ERROR|I2S_INIT");
  audioOutput->stop();
  selectHapticBus();
#endif
  loadLyrics();
  configureWifi();

  drawBootScreen("INTERFACE PRONTA", 100);
  delay(260);
  hapticSoftTap();
  renderFrame();
  // A static stack cannot fragment the remaining heap and is created only
  // after the framebuffer is safely allocated.
  // A socket can spend several seconds waiting before its timeout fires.
  // That work is isolated on CPU 0, while UI/audio remain on
  // CPU 1, so remove only CPU 0's idle-task watchdog to avoid false reboots.
#if ONLINE_REFRESH_ENABLED
  disableCore0WDT();
  networkTaskHandle = xTaskCreateStaticPinnedToCore(
      networkTask, "benfica-online", NETWORK_STACK_BYTES, nullptr, 1,
      networkTaskStack, &networkTaskControl, 0);
  Serial.printf("ONLINE|TASK|%s\n", networkTaskHandle ? "READY" : "ERROR");
#else
  networkStage = 9;
  Serial.println("ONLINE|TASK|PAUSED");
#endif

  Serial.println("READY|BENFICA_CYD|2");
  Serial.printf("DISPLAY|%d|%d|DOUBLE_BUFFER_8BIT\n", tft.width(), tft.height());
  Serial.printf("SD|%s\n", sdReady ? "READY" : "MISSING");
  Serial.printf("HAPTIC|%s\n", hapticReady ? "READY" : "MISSING");
  Serial.printf("LYRICS|%u\n", lyricCount);
  Serial.printf("FILES|GOAL=%d|GLORIOSO=%d|PAPOILAS=%d|HINO=%d\n",
                sdReady && SD.exists(AUDIO_GOAL),
                sdReady && SD.exists(AUDIO_GLORIOSO),
                sdReady && SD.exists(AUDIO_PAPOILAS),
                sdReady && SD.exists(AUDIO_HINO));
}

void loop() {
  // During a USB-to-SD transfer, give the serial receiver and filesystem
  // exclusive attention. Touch, drawing, audio and web requests stay paused.
  if (transferSession || uploadActive) {
    pollSerial();
    delay(1);
    return;
  }
  serviceAudio();
  if (pendingNetworkGoal) {
    pendingNetworkGoal = false;
    currentPage = PAGE_MATCH;
    startGoal(false);
  }
  pollSerial();
  serviceAudio();
  pollTouch();
  serviceAudio();

  unsigned long now = millis();
  unsigned long interval = 83;
  if (transitionActive || overlay == OVERLAY_GOAL) interval = 48;
  if (currentPage == PAGE_MATCH && overlay == OVERLAY_NONE) interval = 180;
  if (currentPage == PAGE_TABLE && overlay == OVERLAY_NONE) interval = 250;
  if (now - lastFrame >= interval && canvasReady) {
    lastFrame = now;
    renderFrame();
  }
  serviceAudio();
  delay(1);
}
