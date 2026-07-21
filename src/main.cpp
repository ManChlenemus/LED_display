#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <FastLED.h>
#include <time.h>

#include "../fonts/fonts.h"
#include "secrets.h"

Preferences preferences;
uint8_t currentFontIndex = 0;

#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
constexpr uint8_t LED_PIN = 4;
constexpr uint8_t WIDTH = 36;
constexpr uint8_t HEIGHT = 24;
constexpr uint16_t NUM_LEDS = WIDTH * HEIGHT;

constexpr uint16_t FRAME_INTERVAL_MS = 33;
CRGB leds[NUM_LEDS];
uint8_t brightness = 80;
bool panelEnabled = true;
uint32_t lastFrameTime = 0;

const char* PREF_NAMESPACE = "settings";
const char* PREF_KEY_EFFECT = "effectIdx";

const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* AP_SSID = "LED_MATRIX";
const char* AP_PASSWORD = "12345678";

WebServer server(80);

const char* DEVICE_KEY = SECRET_DEVICE_KEY;
const char* PROXY_HOST = SECRET_PROXY_HOST;
constexpr uint32_t PROXY_CHECK_INTERVAL_MS = 2000;
constexpr uint32_t PROXY_ERROR_BACKOFF_MS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 3000;
constexpr uint32_t IMAGE_HTTP_TIMEOUT_MS = 5000;
uint32_t lastProxyCheckTime = 0;
uint32_t nextProxyAllowedTime = 0;

bool telegramPollingEnabled = false;
constexpr uint32_t IMAGE_CHECK_INTERVAL_MS = 2000;
uint32_t currentImageId = 0;
uint32_t lastImageCheckTime = 0;
bool imageModeEnabled = false;
bool imageCheckRequested = false;

uint8_t baseHue = 0;

uint8_t dotX = 0;
uint8_t dotY = 0;
int8_t dotVelocityX = 1;
int8_t dotVelocityY = 1;

constexpr uint8_t RAIN_DROP_COUNT = 18;
uint8_t rainX[RAIN_DROP_COUNT];
int8_t rainY[RAIN_DROP_COUNT];
uint8_t rainSpeed[RAIN_DROP_COUNT];

uint8_t wavePhase = 0;

uint8_t confettiHue = 0;

uint32_t fireTime = 0;

uint32_t plasmaTime = 0;

uint8_t solidR = 0;
uint8_t solidG = 229;
uint8_t solidB = 255;

uint8_t textR = 0;
uint8_t textG = 66;
uint8_t textB = 169;

String scrollingText = "Hello, world!";
int16_t textPositionX = WIDTH;

struct EffectDescriptor {
  const char* name;
  void (*reset)();
  void (*update)();
  void (*render)();
};

uint16_t XY(uint8_t x, uint8_t y) {
  if (y % 2 == 0) {
    return y * WIDTH + x;
  }
  return y * WIDTH + (WIDTH - 1 - x);
}

void drawPixel(uint8_t x, uint8_t y, CRGB color) {
  if (x >= WIDTH || y >= HEIGHT) {
    return;
  }

  leds[XY(x, y)] = color;
}

String utf8ToCp1251(const String& utf8) {
  String res = "";
  for (size_t i = 0; i < utf8.length(); i++) {
    uint8_t c = utf8[i];
    if (c < 128) {
      res += (char)c;
    } else if (c == 0xD0 && i + 1 < utf8.length()) {
      uint8_t c2 = utf8[++i];
      if (c2 == 0x81)
        res += (char)168;
      else if (c2 >= 0x90 && c2 <= 0xBF)
        res += (char)(c2 + 48);
    } else if (c == 0xD1 && i + 1 < utf8.length()) {
      uint8_t c2 = utf8[++i];
      if (c2 == 0x91)
        res += (char)184;
      else if (c2 >= 0x80 && c2 <= 0x8F)
        res += (char)(c2 + 112);
    }
  }
  return res;
}

uint8_t drawChar(uint8_t code, int16_t startX, int16_t startY, CRGB color) {
  Glyph g = getGlyph(code, currentFontIndex);
  if (g.width == 0 || g.data == nullptr) return 6;

  int byteIndex = 0;
  for (uint8_t col = 0; col < g.width; col++) {
    uint8_t b0 = pgm_read_byte(&(g.data[byteIndex++]));
    uint8_t b1 = pgm_read_byte(&(g.data[byteIndex++]));
    uint8_t b2 = pgm_read_byte(&(g.data[byteIndex++]));

    int16_t x = startX + col;
    if (x < 0 || x >= WIDTH) continue;

    for (uint8_t row = 0; row < 8; row++)
      if (b0 & (1 << row)) drawPixel(x, startY + row, color);
    for (uint8_t row = 8; row < 16; row++)
      if (b1 & (1 << (row - 8))) drawPixel(x, startY + row, color);
    for (uint8_t row = 16; row < 24; row++)
      if (b2 & (1 << (row - 16))) drawPixel(x, startY + row, color);
  }
  return g.width;
}

void resetRainbow() { baseHue = 0; }

void updateRainbow() { baseHue++; }

void renderRainbow() {
  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      uint8_t pixelHue = baseHue + x * 4 + y * 8;
      drawPixel(x, y, CHSV(pixelHue, 255, 255));
    }
  }

  FastLED.show();
}

void resetDot() {
  dotX = 0;
  dotY = 0;
  dotVelocityX = 1;
  dotVelocityY = 1;
}

void updateDot() {
  int16_t nextX = dotX + dotVelocityX;
  int16_t nextY = dotY + dotVelocityY;

  if (nextX < 0 || nextX >= WIDTH) {
    dotVelocityX = -dotVelocityX;
    nextX = dotX + dotVelocityX;
  }

  if (nextY < 0 || nextY >= HEIGHT) {
    dotVelocityY = -dotVelocityY;
    nextY = dotY + dotVelocityY;
  }

  dotX = nextX;
  dotY = nextY;
}

void renderDot() {
  fadeToBlackBy(leds, NUM_LEDS, 40);
  drawPixel(dotX, dotY, CRGB::Red);
  FastLED.show();
}

void resetRain() {
  for (uint8_t i = 0; i < RAIN_DROP_COUNT; i++) {
    rainX[i] = random(WIDTH);
    rainY[i] = -random(0, HEIGHT);
    rainSpeed[i] = random(1, 3);
  }

  FastLED.clear();
}

void updateRain() {
  for (uint8_t i = 0; i < RAIN_DROP_COUNT; i++) {
    rainY[i] += rainSpeed[i];

    if (rainY[i] >= HEIGHT) {
      rainX[i] = random(WIDTH);
      rainY[i] = -random(0, 8);
      rainSpeed[i] = random(1, 3);
    }
  }
}

void renderRain() {
  fadeToBlackBy(leds, NUM_LEDS, 70);

  for (uint8_t i = 0; i < RAIN_DROP_COUNT; i++) {
    if (rainY[i] >= 0 && rainY[i] < HEIGHT) {
      drawPixel(rainX[i], rainY[i], CRGB(80, 120, 255));
    }

    int8_t tailY = rainY[i] - 1;

    if (tailY >= 0 && tailY < HEIGHT) {
      drawPixel(rainX[i], tailY, CRGB(20, 40, 120));
    }
  }

  FastLED.show();
}

void resetWave() {
  wavePhase = 0;
  FastLED.clear();
}

void updateWave() { wavePhase += 3; }

void renderWave() {
  fadeToBlackBy(leds, NUM_LEDS, 40);

  for (uint8_t x = 0; x < WIDTH; x++) {
    uint8_t angle = wavePhase + x * 12;
    uint8_t waveY = beatsin8(20, 0, HEIGHT - 1, 0, angle);

    uint8_t hue = wavePhase + x * 4;

    drawPixel(x, waveY, CHSV(hue, 255, 255));

    if (waveY > 0) {
      drawPixel(x, waveY - 1, CHSV(hue, 200, 100));
    }

    if (waveY < HEIGHT - 1) {
      drawPixel(x, waveY + 1, CHSV(hue, 200, 100));
    }
  }

  FastLED.show();
}

void resetConfetti() {
  confettiHue = 0;
  FastLED.clear();
}

void updateConfetti() { confettiHue += 2; }

void renderConfetti() {
  fadeToBlackBy(leds, NUM_LEDS, 15);

  for (uint8_t i = 0; i < 3; i++) {
    uint8_t x = random(WIDTH);
    uint8_t y = random(HEIGHT);
    uint8_t hue = confettiHue + random(0, 64);
    leds[XY(x, y)] += CHSV(hue, 200, 255);
  }

  FastLED.show();
}

void resetFire() {
  fireTime = 0;
  FastLED.clear();
}

void updateFire() { fireTime += 15; }

void renderFire() {
  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      uint8_t noise = inoise8(x * 20, y * 30 - fireTime, fireTime / 3);

      uint8_t cooling = (HEIGHT - 1 - y) * (255 / HEIGHT);

      uint8_t heat = qsub8(noise, cooling);

      drawPixel(x, y, HeatColor(heat));
    }
  }

  FastLED.show();
}

void resetPlasma() {
  plasmaTime = 0;
  FastLED.clear();
}

void updatePlasma() { plasmaTime += 5; }

void renderPlasma() {
  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      uint8_t noise = inoise8(x * 15, y * 15, plasmaTime);
      uint8_t hue = noise + (plasmaTime / 4);
      drawPixel(x, y, CHSV(hue, 255, 255));
    }
  }

  FastLED.show();
}

void resetSolid() { FastLED.clear(); }

void updateSolid() {}

void renderSolid() {
  fill_solid(leds, NUM_LEDS, CRGB(solidR, solidG, solidB));
  FastLED.show();
}

void resetText() {
  textPositionX = WIDTH;
  FastLED.clear();
}

void updateText() {
  textPositionX--;

  int totalWidth = 0;
  for (uint16_t i = 0; i < scrollingText.length(); i++) {
    uint8_t code = scrollingText[i];
    Glyph g = getGlyph(code, currentFontIndex);
    totalWidth += (g.width > 0 ? g.width : 6) + 1;
  }

  if (textPositionX < -totalWidth) {
    textPositionX = WIDTH;
  }
}

void renderText() {
  FastLED.clear();
  int16_t cursorX = textPositionX;
  CRGB textColor = CRGB(textR, textG, textB);

  for (uint16_t i = 0; i < scrollingText.length(); i++) {
    uint8_t code = scrollingText[i];
    uint8_t w = drawChar(code, cursorX, 0, textColor);
    cursorX += w + 1;
  }

  FastLED.show();
}

uint32_t spectrogramTime = 0;

void resetSpectrogram() {
  spectrogramTime = 0;
  FastLED.clear();
}

void updateSpectrogram() {
  spectrogramTime += 15;
}

void renderSpectrogram() {
  fadeToBlackBy(leds, NUM_LEDS, 80);
  
  for (uint8_t x = 0; x < WIDTH; x++) {
    uint8_t noise = inoise8(x * 35, spectrogramTime);
    uint8_t colHeight = dim8_video(noise); 
    colHeight = map(colHeight, 0, 255, 0, HEIGHT);
    
    for (uint8_t y = 0; y < colHeight; y++) {
      uint8_t hue = map(y, 0, HEIGHT, 96, 0);
      drawPixel(x, y, CHSV(hue, 255, 200));
    }
  }
  FastLED.show();
}

void resetClock() {
  FastLED.clear();
}

void updateClock() {}

void renderClock() {
  FastLED.clear();
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) {
    int16_t x = 2;
    x += drawChar('-', x, 0, CRGB::Red) + 1;
    x += drawChar('-', x, 0, CRGB::Red) + 1;
    drawPixel(x+1, 10, CRGB::Red); drawPixel(x+1, 14, CRGB::Red); x += 3;
    x += drawChar('-', x, 0, CRGB::Red) + 1;
    drawChar('-', x, 0, CRGB::Red);
    FastLED.show();
    return;
  }

  char timeStr[6];
  strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  
  int totalWidth = 0;
  for (int i = 0; i < 5; i++) {
    if (timeStr[i] == ':') {
      totalWidth += 3;
    } else {
      Glyph g = getGlyph(timeStr[i], currentFontIndex);
      totalWidth += g.width + 1;
    }
  }
  if (totalWidth > 0) totalWidth--;
  
  int16_t x = (WIDTH - totalWidth) / 2;
  if (x < 0) x = 0;
  
  CRGB color = CHSV((millis() / 50) % 255, 255, 255);
  
  for (int i = 0; i < 5; i++) {
    if (timeStr[i] == ':') {
      if (millis() % 1000 < 500) {
        drawPixel(x+1, 10, color);
        drawPixel(x+1, 14, color);
      }
      x += 3;
    } else {
      x += drawChar(timeStr[i], x, 0, color) + 1;
    }
  }
  
  FastLED.show();
}

EffectDescriptor effects[] = {
  { "rainbow", resetRainbow, updateRainbow, renderRainbow },
  { "dot", resetDot, updateDot, renderDot },
  { "rain", resetRain, updateRain, renderRain },
  { "wave", resetWave, updateWave, renderWave },
  { "confetti", resetConfetti, updateConfetti, renderConfetti },
  { "fire", resetFire, updateFire, renderFire },
  { "plasma", resetPlasma, updatePlasma, renderPlasma },
  { "solid", resetSolid, updateSolid, renderSolid },
  { "text", resetText, updateText, renderText },
  { "spectrogram", resetSpectrogram, updateSpectrogram, renderSpectrogram },
  { "clock", resetClock, updateClock, renderClock }
};

constexpr uint8_t EFFECT_COUNT = sizeof(effects) / sizeof(effects[0]);

uint8_t currentEffectIndex = 0;

void saveEffect() {
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putUChar(PREF_KEY_EFFECT, currentEffectIndex);
  preferences.end();
}

void loadEffect() {
  preferences.begin(PREF_NAMESPACE, true);
  if (!preferences.isKey(PREF_KEY_EFFECT)) {
    preferences.end();
    currentEffectIndex = 0;
    saveEffect();
    return;
  }

  uint8_t savedEffectIndex = preferences.getUChar(PREF_KEY_EFFECT, 0);
  preferences.end();

  if (savedEffectIndex < EFFECT_COUNT) {
    currentEffectIndex = savedEffectIndex;
  } else {
    currentEffectIndex = 0;
    saveEffect();
  }
}

void setEffect(uint8_t newEffectIndex, bool shouldSave = true) {
  if (newEffectIndex >= EFFECT_COUNT) {
    return;
  }

  imageModeEnabled = false;
  currentEffectIndex = newEffectIndex;

  FastLED.clear();
  effects[currentEffectIndex].reset();
  FastLED.show();

  if (shouldSave) {
    saveEffect();
  }
}

void nextEffect() {
  uint8_t nextIndex = currentEffectIndex + 1;

  if (nextIndex >= EFFECT_COUNT) {
    nextIndex = 0;
  }

  setEffect(nextIndex);
}

void previousEffect() {
  if (currentEffectIndex == 0) {
    setEffect(EFFECT_COUNT - 1);
  } else {
    setEffect(currentEffectIndex - 1);
  }
}

void updateCurrentEffect() { effects[currentEffectIndex].update(); }

void renderCurrentEffect() { effects[currentEffectIndex].render(); }

void setBrightness(int16_t value) {
  if (value < 0) {
    value = 0;
  }

  if (value > 255) {
    value = 255;
  }

  brightness = value;
  FastLED.setBrightness(brightness);

  FastLED.show();
}

void changeBrightness(int16_t delta) { setBrightness(brightness + delta); }

String escapeJson(String text) {
  text.replace("\\", "\\\\");
  text.replace("\"", "\\\"");
  text.replace("\n", "\\n");
  text.replace("\r", "");
  return text;
}

String extractJsonStringValue(const String& json, const String& key) {
  String pattern = "\"" + key + "\":\"";
  int startPos = json.indexOf(pattern);

  if (startPos < 0) {
    return "";
  }

  startPos += pattern.length();

  int endPos = json.indexOf("\"", startPos);

  if (endPos < 0) {
    return "";
  }

  return json.substring(startPos, endPos);
}

int extractJsonIntValue(const String& json, const String& key) {
  String pattern = "\"" + key + "\":";
  int startPos = json.indexOf(pattern);

  if (startPos < 0) {
    return -1;
  }

  startPos += pattern.length();

  int endPos = json.indexOf(",", startPos);

  if (endPos < 0) {
    endPos = json.indexOf("}", startPos);
  }

  if (endPos < 0) {
    return -1;
  }

  return json.substring(startPos, endPos).toInt();
}

int8_t findEffectByName(String name) {
  name.toLowerCase();

  for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
    String effectName = effects[i].name;
    effectName.toLowerCase();

    if (name == effectName) {
      return i;
    }
  }

  return -1;
}

String getStatusText() {
  String status;

  status += "Panel: ";
  status += panelEnabled ? "on" : "off";
  status += "\n";

  status += "Image mode: ";
  status += imageModeEnabled ? "on" : "off";
  status += "\n";

  status += "Current image id: ";
  status += currentImageId;
  status += "\n";

  status += "Telegram polling: ";
  status += telegramPollingEnabled ? "on" : "off";
  status += "\n";

  status += "Effect index: ";
  status += currentEffectIndex;
  status += "\n";

  status += "Effect name: ";
  status += effects[currentEffectIndex].name;
  status += "\n";

  status += "Brightness: ";
  status += brightness;
  status += "\n";

  status += "AP SSID: ";
  status += AP_SSID;
  status += "\n";

  status += "AP IP: ";
  status += WiFi.softAPIP().toString();
  status += "\n";

  status += "STA WiFi: ";
  status += WiFi.status() == WL_CONNECTED ? "connected" : "not connected";
  status += "\n";

  if (WiFi.status() == WL_CONNECTED) {
    status += "STA IP: ";
    status += WiFi.localIP().toString();
    status += "\n";
  }

  status += "Proxy host: ";
  status += PROXY_HOST;
  status += "\n";

  status += "Free heap: ";
  status += ESP.getFreeHeap();
  status += "\n";

  status += "FPS: ";
  status += FastLED.getFPS();

  return status;
}

String getEffectsText() {
  String text;

  text += "Effects:\n";

  for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
    text += i;
    text += ": ";
    text += effects[i].name;

    if (i == currentEffectIndex && !imageModeEnabled) {
      text += " <- current";
    }

    text += "\n";
  }

  return text;
}

String getHelpText() {
  String help;

  help += "Commands:\n";
  help += "/help\n";
  help += "/status\n";
  help += "/effects\n";
  help += "/on\n";
  help += "/off\n";
  help += "/image\n";
  help += "/tg on\n";
  help += "/tg off\n";
  help += "/tg status\n";
  help += "/next\n";
  help += "/prev\n";
  help += "/effect rainbow\n";
  help += "/effect dot\n";
  help += "/effect rain\n";
  help += "/effect wave\n";
  help += "/effect confetti\n";
  help += "/effect fire\n";
  help += "/effect text\n";
  help += "/effect spectrogram\n";
  help += "/effect clock\n";
  help += "/effect next\n";
  help += "/effect prev\n";
  help += "/effect 0\n";
  help += "/brightness 80\n";
  help += "/brightness +10\n";
  help += "/brightness -10";

  return help;
}

void printHelp() {
  Serial.println();
  Serial.println(getHelpText());
}

void printStatus() {
  Serial.println();
  Serial.println(F("=== Status ==="));
  Serial.println(getStatusText());
  Serial.println(F("=============="));
}

String processCommand(String command) {
  command.trim();
  command.toLowerCase();

  if (command.length() == 0) {
    return "Empty command";
  }

  Serial.print(F("> "));
  Serial.println(command);

  if (command == "/help") {
    String help = getHelpText();
    Serial.println(help);
    return help;
  } else if (command == "/status") {
    String status = getStatusText();
    Serial.println(status);
    return status;
  } else if (command == "/effects") {
    String effectsText = getEffectsText();
    Serial.println(effectsText);
    return effectsText;
  } else if (command == "/off") {
    panelEnabled = false;
    FastLED.clear();
    FastLED.show();
    Serial.println(F("Panel off"));
    return "Panel off";
  } else if (command == "/on") {
    panelEnabled = true;

    if (!imageModeEnabled) {
      setEffect(currentEffectIndex);
    }

    Serial.println(F("Panel on"));
    return "Panel on";
  } else if (command == "/image") {
    panelEnabled = true;
    currentImageId = 0;
    lastImageCheckTime = 0;
    imageCheckRequested = true;

    Serial.println(F("Image mode requested"));
    return "Image mode requested";
  } else if (command.startsWith("/color ")) {
    String hex = command.substring(7);
    hex.trim();

    if (hex.startsWith("#")) {
      hex = hex.substring(1);
    }

    if (hex.length() == 6) {
      long number = strtol(hex.c_str(), NULL, 16);

      solidR = (number >> 16) & 0xFF;
      solidG = (number >> 8) & 0xFF;
      solidB = number & 0xFF;

      int8_t effectIndex = findEffectByName("solid");
      if (effectIndex >= 0) {
        setEffect(effectIndex);
        panelEnabled = true;
      }

      String reply = "Color changed to #" + hex;
      Serial.println(reply);
      return reply;
    }

    String reply = "Invalid HEX color format";
    Serial.println(reply);
    return reply;
  } else if (command.startsWith("/text ")) {
    String newText = command.substring(6);
    newText.trim();

    scrollingText = utf8ToCp1251(newText);

    int8_t effectIndex = findEffectByName("text");
    if (effectIndex >= 0) {
      setEffect(effectIndex);
      panelEnabled = true;
    }

    Serial.println("Text set (CP1251)");
    return "Text set";
  } else if (command.startsWith("/textcolor ")) {
    String hex = command.substring(11);
    hex.trim();

    if (hex.startsWith("#")) hex = hex.substring(1);

    if (hex.length() == 6) {
      long number = strtol(hex.c_str(), NULL, 16);

      textR = (number >> 16) & 0xFF;
      textG = (number >> 8) & 0xFF;
      textB = number & 0xFF;

      String reply = "Text color changed to #" + hex;
      Serial.println(reply);
      return reply;
    }

    return "Invalid HEX text color";
  } else if (command.startsWith("/font ")) {
    int fontIdx = command.substring(6).toInt();

    if (fontIdx >= 0 && fontIdx < CUSTOM_FONT_COUNT) {
      currentFontIndex = fontIdx;
      Serial.println("Font changed to index: " + String(currentFontIndex));
      return "Font changed";
    } else {
      return "Invalid font index";
    }
  } else if (command == "/tg on") {
    telegramPollingEnabled = true;
    lastProxyCheckTime = 0;
    nextProxyAllowedTime = 0;

    Serial.println(F("Telegram polling on"));
    return "Telegram polling on";
  } else if (command == "/tg off") {
    telegramPollingEnabled = false;

    Serial.println(F("Telegram polling off"));
    return "Telegram polling off";
  } else if (command == "/tg status") {
    String reply = "Telegram polling: ";
    reply += telegramPollingEnabled ? "on" : "off";

    Serial.println(reply);
    return reply;
  } else if (command == "/next") {
    nextEffect();
    panelEnabled = true;

    String reply = "Effect changed to: ";
    reply += effects[currentEffectIndex].name;

    Serial.println(reply);
    return reply;
  } else if (command == "/prev") {
    previousEffect();
    panelEnabled = true;

    String reply = "Effect changed to: ";
    reply += effects[currentEffectIndex].name;

    Serial.println(reply);
    return reply;
  } else if (command.startsWith("/effect ")) {
    String effectName = command.substring(8);
    effectName.trim();

    if (effectName == "next") {
      nextEffect();
      panelEnabled = true;

      String reply = "Effect changed to: ";
      reply += effects[currentEffectIndex].name;

      Serial.println(reply);
      return reply;
    }

    if (effectName == "prev") {
      previousEffect();
      panelEnabled = true;

      String reply = "Effect changed to: ";
      reply += effects[currentEffectIndex].name;

      Serial.println(reply);
      return reply;
    }

    bool isNumeric = true;

    for (uint16_t i = 0; i < effectName.length(); i++) {
      if (!isDigit(effectName[i])) {
        isNumeric = false;
        break;
      }
    }

    if (isNumeric) {
      uint8_t effectIndex = effectName.toInt();

      if (effectIndex < EFFECT_COUNT) {
        setEffect(effectIndex);
        panelEnabled = true;

        String reply = "Effect changed to: ";
        reply += effects[currentEffectIndex].name;

        Serial.println(reply);
        return reply;
      }

      String reply = "Unknown effect index: ";
      reply += effectIndex;

      Serial.println(reply);
      return reply;
    }

    int8_t effectIndex = findEffectByName(effectName);

    if (effectIndex >= 0) {
      setEffect(effectIndex);
      panelEnabled = true;

      String reply = "Effect changed to: ";
      reply += effects[currentEffectIndex].name;

      Serial.println(reply);
      return reply;
    }

    String reply = "Unknown effect: ";
    reply += effectName;

    Serial.println(reply);
    return reply;
  } else if (command.startsWith("/brightness ")) {
    String valueText = command.substring(12);
    valueText.trim();

    if (valueText.startsWith("+") || valueText.startsWith("-")) {
      changeBrightness(valueText.toInt());
    } else {
      setBrightness(valueText.toInt());
    }

    String reply = "Brightness: ";
    reply += brightness;

    Serial.println(reply);
    return reply;
  }

  String reply = "Unknown command: ";
  reply += command;
  reply += "\n\n";
  reply += getHelpText();

  Serial.println(reply);
  return reply;
}

void handleSerial() {
  static String serialBuffer = "";

  while (Serial.available() > 0) {
    char incomingChar = Serial.read();

    if (incomingChar == '\n' || incomingChar == '\r') {
      if (serialBuffer.length() > 0) {
        processCommand(serialBuffer);
        serialBuffer = "";
      }

      continue;
    }

    serialBuffer += incomingChar;

    if (serialBuffer.length() > 120) {
      Serial.println(F("Serial command too long, buffer cleared"));
      serialBuffer = "";
    }
  }
}

void setupWiFi() {
  Serial.println();
  Serial.println(F("Starting WiFi..."));

  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.print(F("AP SSID: "));
  Serial.println(AP_SSID);

  Serial.print(F("AP IP: "));
  Serial.println(WiFi.softAPIP());

  Serial.print(F("Connecting to STA WiFi: "));
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t startTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(250);
    Serial.print(F("."));
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("STA connected"));

    Serial.print(F("STA IP: "));
    Serial.println(WiFi.localIP());

    configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println(F("NTP configured for UTC+3"));
  } else {
    Serial.println(F("STA connection failed"));
    Serial.println(F("Use AP mode instead"));
  }
}

void sendProxyResult(int commandId, String result) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Proxy result skipped: WiFi not connected"));
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String(PROXY_HOST) + "/esp/result";

  if (!http.begin(client, url)) {
    Serial.println(F("Proxy result begin failed"));
    return;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");

  String body = "{";
  body += "\"key\":\"";
  body += DEVICE_KEY;
  body += "\",";
  body += "\"command_id\":";
  body += commandId;
  body += ",";
  body += "\"result\":\"";
  body += escapeJson(result);
  body += "\"";
  body += "}";

  int httpCode = http.POST(body);

  if (httpCode == 200) {
    Serial.println(F("Proxy result sent"));
  } else {
    Serial.print(F("Proxy result HTTP code: "));
    Serial.print(httpCode);

    Serial.print(F(" | error: "));
    Serial.println(http.errorToString(httpCode));

    String response = http.getString();

    if (response.length() > 0) {
      Serial.print(F("Proxy result response: "));
      Serial.println(response);
    }
  }

  http.end();
}

void handleProxy() {
  if (!telegramPollingEnabled) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  uint32_t now = millis();

  if (now < nextProxyAllowedTime) {
    return;
  }

  if (now - lastProxyCheckTime < PROXY_CHECK_INTERVAL_MS) {
    return;
  }

  lastProxyCheckTime = now;

  WiFiClient client;
  HTTPClient http;

  String url = String(PROXY_HOST) + "/esp/command?key=" + DEVICE_KEY;

  if (!http.begin(client, url)) {
    Serial.println(F("Proxy command begin failed"));
    nextProxyAllowedTime = now + PROXY_ERROR_BACKOFF_MS;
    return;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.print(F("Proxy command HTTP code: "));
    Serial.print(httpCode);

    Serial.print(F(" | error: "));
    Serial.println(http.errorToString(httpCode));

    String errorPayload = http.getString();

    if (errorPayload.length() > 0) {
      Serial.print(F("Proxy error payload: "));
      Serial.println(errorPayload);
    }

    http.end();

    nextProxyAllowedTime = now + PROXY_ERROR_BACKOFF_MS;
    return;
  }

  String payload = http.getString();
  http.end();

  if (payload.indexOf("\"has_command\":true") < 0) {
    return;
  }

  int commandId = extractJsonIntValue(payload, "command_id");
  String command = extractJsonStringValue(payload, "text");

  command.trim();
  command.toLowerCase();

  if (commandId < 0 || command.length() == 0) {
    Serial.print(F("Proxy payload parse failed: "));
    Serial.println(payload);
    return;
  }

  Serial.println();
  Serial.println(F("=== Proxy command ==="));

  Serial.print(F("Command id: "));
  Serial.println(commandId);

  Serial.print(F("Command text: "));
  Serial.println(command);

  String result = processCommand(command);

  Serial.print(F("Command result: "));
  Serial.println(result);

  sendProxyResult(commandId, result);

  Serial.println(F("====================="));
}

bool downloadImageFromProxy(uint32_t newImageId) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String(PROXY_HOST) + "/esp/image/data?key=" + DEVICE_KEY;
  Serial.print(F("Image data URL: "));
  Serial.println(url);

  if (!http.begin(client, url)) {
    Serial.println(F("Image data begin failed"));
    return false;
  }

  http.setTimeout(IMAGE_HTTP_TIMEOUT_MS);
  http.setReuse(false);

  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.print(F("Image HTTP code: "));
    Serial.println(httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();

  if (contentLength != NUM_LEDS * 3) {
    Serial.print(F("Invalid image size: "));
    Serial.println(contentLength);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  stream->setTimeout(IMAGE_HTTP_TIMEOUT_MS);

  static uint8_t imgBuffer[WIDTH * HEIGHT * 3];

  Serial.println(F("Downloading image to RAM..."));

  size_t totalRead = stream->readBytes(imgBuffer, contentLength);

  if (totalRead != contentLength) {
    Serial.print(F("Download incomplete. Read bytes: "));
    Serial.println(totalRead);
    http.end();
    return false;
  }

  http.end();
  client.stop();

  uint32_t waitStart = millis();
  while (millis() - waitStart < 50) {
    yield();
  }

  FastLED.clear();

  uint16_t pixelIndex = 0;
  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      uint8_t r = imgBuffer[pixelIndex++];
      uint8_t g = imgBuffer[pixelIndex++];
      uint8_t b = imgBuffer[pixelIndex++];
      drawPixel(x, y, CRGB(r, g, b));
    }
  }

  currentImageId = newImageId;
  imageModeEnabled = true;
  panelEnabled = true;

  FastLED.show();

  Serial.print(F("Image loaded successfully, id: "));
  Serial.println(currentImageId);

  return true;
}

void handleImageProxy() {
  if (!imageCheckRequested) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Image check skipped: WiFi not connected"));
    imageCheckRequested = false;
    return;
  }

  uint32_t now = millis();

  if (now - lastImageCheckTime < IMAGE_CHECK_INTERVAL_MS) {
    return;
  }

  lastImageCheckTime = now;

  WiFiClient client;
  HTTPClient http;

  String url = String(PROXY_HOST) + "/esp/image/meta?key=" + DEVICE_KEY;

  Serial.print(F("Image meta URL: "));
  Serial.println(url);

  if (!http.begin(client, url)) {
    Serial.println(F("Image meta begin failed"));
    imageCheckRequested = false;
    return;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.print(F("Image meta HTTP code: "));
    Serial.println(httpCode);

    http.end();
    imageCheckRequested = false;
    return;
  }

  String payload = http.getString();
  http.end();

  Serial.print(F("Image meta payload: "));
  Serial.println(payload);

  if (payload.indexOf("\"has_image\":true") < 0) {
    Serial.println(F("No image on server"));
    imageCheckRequested = false;
    return;
  }

  int imageId = extractJsonIntValue(payload, "image_id");
  int width = extractJsonIntValue(payload, "width");
  int height = extractJsonIntValue(payload, "height");
  int bytes = extractJsonIntValue(payload, "bytes");

  if (imageId <= 0) {
    Serial.println(F("Invalid image id"));
    imageCheckRequested = false;
    return;
  }

  if ((uint32_t)imageId == currentImageId) {
    Serial.println(F("Image already loaded"));
    imageCheckRequested = false;
    return;
  }

  if (width != WIDTH || height != HEIGHT || bytes != NUM_LEDS * 3) {
    Serial.print(F("Invalid image meta: "));
    Serial.println(payload);
    imageCheckRequested = false;
    return;
  }

  Serial.print(F("New image detected, id: "));
  Serial.println(imageId);

  bool loaded = downloadImageFromProxy(imageId);

  if (!loaded) {
    Serial.println(F("Image download failed"));
  }

  imageCheckRequested = false;
}

void setupOTA() {
  ArduinoOTA.setHostname("led-matrix");

  ArduinoOTA.onStart([]() {
    Serial.println(F("OTA start"));

    panelEnabled = false;
    FastLED.clear();
    FastLED.show();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println();
    Serial.println(F("OTA end"));
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA progress: %u%%\r", (progress * 100) / total);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error[%u]: ", error);

    if (error == OTA_AUTH_ERROR) {
      Serial.println(F("Auth failed"));
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println(F("Begin failed"));
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println(F("Connect failed"));
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println(F("Receive failed"));
    } else if (error == OTA_END_ERROR) {
      Serial.println(F("End failed"));
    }
  });

  ArduinoOTA.begin();

  Serial.println(F("OTA ready"));
}

const char indexHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>LED Matrix Control</title>
  <style>
    :root {
      --bg: #121212;
      --card: #1e1e24;
      --accent: #00e5ff;
      --text: #e0e0e0;
      --text-muted: #888;
    }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      margin: 0;
      padding: 16px;
      background: var(--bg);
      color: var(--text);
      -webkit-font-smoothing: antialiased;
    }
    .container {
      max-width: 500px;
      margin: 0 auto;
    }
    h1 {
      text-align: center;
      color: var(--accent);
      font-weight: 600;
      letter-spacing: 1px;
      margin-bottom: 24px;
    }
    .card {
      background: var(--card);
      border-radius: 16px;
      padding: 20px;
      margin-bottom: 20px;
      box-shadow: 0 4px 20px rgba(0,0,0,0.4);
    }
    .card-title {
      font-size: 1.1rem;
      color: var(--text-muted);
      margin-top: 0;
      margin-bottom: 16px;
      text-transform: uppercase;
      letter-spacing: 1px;
    }
    .grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    .grid-3 { display: grid; grid-template-columns: repeat(auto-fit, minmax(100px, 1fr)); gap: 12px; }
    
    button {
      background: #2a2a35;
      color: var(--text);
      border: 1px solid #3a3a45;
      padding: 14px;
      font-size: 15px;
      border-radius: 10px;
      cursor: pointer;
      transition: all 0.2s ease;
      font-weight: 500;
    }
    button:hover { background: #353542; border-color: var(--accent); }
    button:active { transform: scale(0.97); }
    
    /* Active State for Buttons */
    button.active {
      background: rgba(0, 229, 255, 0.15);
      color: var(--accent);
      border-color: var(--accent);
      box-shadow: 0 0 12px rgba(0, 229, 255, 0.2);
    }

    input[type=range] {
      width: 100%;
      margin: 10px 0;
      accent-color: var(--accent);
    }
    .slider-val { text-align: center; font-size: 1.2rem; font-weight: bold; color: var(--accent); margin-bottom: 8px;}
    
    /* Dashboard Table */
    .status-row {
      display: flex;
      justify-content: space-between;
      padding: 8px 0;
      border-bottom: 1px solid #2a2a35;
      font-size: 14px;
    }
    .status-row:last-child { border-bottom: none; }
    .status-label { color: var(--text-muted); }
    .status-value { font-weight: 600; color: #fff; }
    
    .custom-cmd { display: flex; gap: 8px; }
    .custom-cmd input {
      flex: 1;
      padding: 12px;
      border-radius: 10px;
      border: 1px solid #3a3a45;
      background: #121212;
      color: #fff;
      font-size: 16px;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>LED MATRIX</h1>

    <!-- Power & Mode -->
    <div class="card">
      <h2 class="card-title">Power & Mode</h2>
      <div class="grid-2">
        <button id="btn-on" onclick="sendCommand('/on')">POWER ON</button>
        <button id="btn-off" onclick="sendCommand('/off')">POWER OFF</button>
      </div>
      <div style="margin-top: 12px;">
        <button style="width: 100%; border-color:#ff0055; color:#ff0055" onclick="sendCommand('/image')">SHOW LATEST IMAGE</button>
      </div>
    </div>

    <!-- Brightness -->
    <div class="card">
      <h2 class="card-title">Brightness</h2>
      <div class="slider-val" id="br-val">32</div>
      <input type="range" id="brightnessSlider" min="0" max="255" value="32" 
             oninput="document.getElementById('br-val').innerText = this.value" 
             onchange="sendCommand('/brightness ' + this.value)">
    </div>

    <!-- Solid Color -->
    <div class="card">
      <h2 class="card-title">Solid Color</h2>
      <div style="display: flex; align-items: center; justify-content: space-between; background: #121212; padding: 12px; border-radius: 10px; border: 1px solid #3a3a45;">
        <span id="colorHex" style="font-size: 1.1rem; font-weight: bold; color: var(--accent); letter-spacing: 1px;">#00E5FF</span>
        <input type="color" id="colorPicker" value="#00e5ff" style="width: 60px; height: 42px; border: 1px solid #3a3a45; border-radius: 6px; cursor: pointer; background: none; padding: 0;" onchange="sendColor(this.value)">
      </div>
    </div>

    <!-- Scrolling Text -->
    <div class="card">
      <h2 class="card-title">Scrolling Text</h2>
      
      <div class="custom-cmd" style="margin-bottom: 12px;">
        <select id="fontSelect" onchange="sendCommand('/font ' + this.value)" style="width: 100%; padding: 12px; background: #121212; color: #fff; border: 1px solid #3a3a45; border-radius: 10px; font-size: 15px; cursor: pointer; appearance: auto;">
          <optgroup label="HSE Sans">
            <option value="0">HSE Sans Black</option>
            <option value="1">HSE Sans Bold</option>
            <option value="2">HSE Sans Italic</option>
            <option value="3">HSE Sans Regular</option>
            <option value="4">HSE Sans SemiBold</option>
            <option value="5">HSE Sans Thin</option>
          </optgroup>
          <optgroup label="HSE Slab">
            <option value="6">HSE Slab Black</option>
            <option value="7">HSE Slab Italic</option>
            <option value="8">HSE Slab Regular</option>
          </optgroup>
        </select>
      </div>

      <div style="display: flex; align-items: center; justify-content: space-between; background: #121212; padding: 12px; border-radius: 10px; border: 1px solid #3a3a45; margin-bottom: 12px;">
        <span style="font-size: 1rem; color: var(--text-muted);">Text Color</span>
        <input type="color" id="textColorPicker" value="#0042A9" style="width: 60px; height: 42px; border: 1px solid #3a3a45; border-radius: 6px; cursor: pointer; background: none; padding: 0;" onchange="sendCommand('/textcolor ' + this.value)">
      </div>

      <div class="custom-cmd">
        <input id="textInput" type="text" placeholder="Введите текст..." onkeypress="if(event.key === 'Enter') sendScrollText()">
        <button onclick="sendScrollText()">SEND</button>
      </div>
    </div>

    <!-- Effects -->
    <div class="card">
      <h2 class="card-title">Effects</h2>
      <div class="grid-2" style="margin-bottom: 12px;">
        <button onclick="sendCommand('/prev')">&larr; Prev</button>
        <button onclick="sendCommand('/next')">Next &rarr;</button>
      </div>
      <div class="grid-3">
        <button class="eff-btn" data-eff="rainbow" onclick="sendCommand('/effect rainbow')">Rainbow</button>
        <button class="eff-btn" data-eff="dot" onclick="sendCommand('/effect dot')">Dot</button>
        <button class="eff-btn" data-eff="rain" onclick="sendCommand('/effect rain')">Rain</button>
        <button class="eff-btn" data-eff="wave" onclick="sendCommand('/effect wave')">Wave</button>
        <button class="eff-btn" data-eff="confetti" onclick="sendCommand('/effect confetti')">Confetti</button>
        <button class="eff-btn" data-eff="fire" onclick="sendCommand('/effect fire')">Fire</button>
        <button class="eff-btn" data-eff="text" onclick="sendCommand('/effect text')">Text</button>
        <button class="eff-btn" data-eff="spectrogram" onclick="sendCommand('/effect spectrogram')">Spectrogram</button>
        <button class="eff-btn" data-eff="clock" onclick="sendCommand('/effect clock')">Clock</button>
      </div>
    </div>

    <!-- Telegram -->
    <div class="card">
      <h2 class="card-title">Telegram Polling</h2>
      <div class="grid-2">
        <button id="btn-tg-on" onclick="sendCommand('/tg on')">TG ON</button>
        <button id="btn-tg-off" onclick="sendCommand('/tg off')">TG OFF</button>
      </div>
    </div>

    <!-- System Status -->
    <div class="card">
      <h2 class="card-title">System Status</h2>
      <div id="dashboard">
        <div class="status-row"><span class="status-label">Loading data...</span></div>
      </div>
      <button style="width: 100%; margin-top: 16px;" onclick="loadStatus()">Refresh Status</button>
    </div>

    <!-- Custom Command -->
    <div class="card">
      <h2 class="card-title">Terminal</h2>
      <div class="custom-cmd">
        <input id="commandInput" type="text" placeholder="/effect fire" onkeypress="if(event.key === 'Enter') sendCustomCommand()">
        <button onclick="sendCustomCommand()">SEND</button>
      </div>
    </div>

  </div>

  <script>
    async function sendCommand(command) {
      try {
        await fetch('/cmd?value=' + encodeURIComponent(command));
        await loadStatus();
      } catch (e) {
        console.error('Network error', e);
      }
    }

    function sendCustomCommand() {
      const input = document.getElementById('commandInput');
      if(input.value) {
        sendCommand(input.value);
        input.value = '';
      }
    }

    function sendColor(hex) {
      document.getElementById('colorHex').innerText = hex.toUpperCase();
      sendCommand('/color ' + hex);
    }

    async function loadStatus() {
      try {
        const response = await fetch('/status');
        const data = await response.json();

        // Update Slider if not currently dragging
        const slider = document.getElementById('brightnessSlider');
        const valText = document.getElementById('br-val');
        if (document.activeElement !== slider) {
          slider.value = data.brightness;
          valText.innerText = data.brightness;
        }

        // Highlight Power Buttons
        document.getElementById('btn-on').classList.toggle('active', data.panel === 'on');
        document.getElementById('btn-off').classList.toggle('active', data.panel === 'off');

        // Highlight Telegram Buttons
        document.getElementById('btn-tg-on').classList.toggle('active', data.tgPolling === 'on');
        document.getElementById('btn-tg-off').classList.toggle('active', data.tgPolling === 'off');

        // Highlight Effect Buttons
        document.querySelectorAll('.eff-btn').forEach(btn => {
          if (btn.dataset.eff === data.effect && data.imageMode === 'off' && data.panel === 'on') {
            btn.classList.add('active');
          } else {
            btn.classList.remove('active');
          }
        });

        // Update Dashboard
        document.getElementById('dashboard').innerHTML = `
          <div class="status-row"><span class="status-label">Mode</span><span class="status-value">${data.imageMode === 'on' ? 'IMAGE' : 'EFFECT'}</span></div>
          <div class="status-row"><span class="status-label">Effect</span><span class="status-value">${data.effect.toUpperCase()}</span></div>
          <div class="status-row"><span class="status-label">Image ID</span><span class="status-value">${data.imageId}</span></div>
          <div class="status-row"><span class="status-label">Wi-Fi</span><span class="status-value">${data.staConnected ? 'Connected' : 'Disconnected'}</span></div>
          <div class="status-row"><span class="status-label">IP Address</span><span class="status-value">${data.staIp}</span></div>
          <div class="status-row"><span class="status-label">AP IP</span><span class="status-value">${data.apIp}</span></div>
          <div class="status-row"><span class="status-label">Free RAM</span><span class="status-value">${Math.round(data.freeHeap/1024)} KB</span></div>
          <div class="status-row"><span class="status-label">FPS</span><span class="status-value" style="color:var(--accent);">${data.fps}</span></div>
        `;
      } catch (e) {
        document.getElementById('dashboard').innerHTML = '<div class="status-row" style="color:#ff4444;">Device is offline / unreachable</div>';
      }
    }

    // Load initial data
    loadStatus();
    // Auto-refresh every 5 seconds
    setInterval(loadStatus, 5000);

    function sendScrollText() {
      const input = document.getElementById('textInput');
      if(input.value) {
        sendCommand('/text ' + input.value);
        input.value = '';
      }
    }
  </script>
</body>
</html>
)rawliteral";

void handleRoot() { server.send_P(200, "text/html", indexHtml); }

void handleCommand() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "Missing value");
    return;
  }

  String command = server.arg("value");
  String reply = processCommand(command);

  server.send(200, "text/plain", reply);
}

void handleStatus() {
  String response = "{";

  response += "\"panel\":\"";
  response += panelEnabled ? "on" : "off";
  response += "\",";

  response += "\"imageMode\":\"";
  response += imageModeEnabled ? "on" : "off";
  response += "\",";

  response += "\"imageId\":";
  response += currentImageId;
  response += ",";

  response += "\"tgPolling\":\"";
  response += telegramPollingEnabled ? "on" : "off";
  response += "\",";

  response += "\"effect\":\"";
  response += effects[currentEffectIndex].name;
  response += "\",";

  response += "\"effectIndex\":";
  response += currentEffectIndex;
  response += ",";

  response += "\"brightness\":";
  response += brightness;
  response += ",";

  response += "\"freeHeap\":";
  response += ESP.getFreeHeap();
  response += ",";

  response += "\"fps\":";
  response += FastLED.getFPS();
  response += ",";

  response += "\"apIp\":\"";
  response += WiFi.softAPIP().toString();
  response += "\",";

  response += "\"staConnected\":";
  response += (WiFi.status() == WL_CONNECTED ? "true" : "false");
  response += ",";

  response += "\"staIp\":\"";
  response += WiFi.localIP().toString();
  response += "\"";

  response += "}";

  server.send(200, "application/json", response);
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/cmd", handleCommand);
  server.on("/status", handleStatus);

  server.begin();

  Serial.println(F("Web server started"));
}

TaskHandle_t renderTaskHandle;

void renderTask(void* pvParameters) {
  for (;;) {
    if (!panelEnabled) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (imageModeEnabled) {
      static uint32_t lastImageShowTime = 0;
      if (millis() - lastImageShowTime > 1000) {
        lastImageShowTime = millis();
        FastLED.show();
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    uint32_t now = millis();
    if (now - lastFrameTime >= FRAME_INTERVAL_MS) {
      lastFrameTime = now;
      renderCurrentEffect();
      updateCurrentEffect();
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(200);

  randomSeed(ESP.getCycleCount());

  loadEffect();

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(brightness);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 40000);
  FastLED.setDither(1);

  FastLED.clear();
  FastLED.show();

  setupWiFi();
  setupWebServer();
  setupOTA();

  setEffect(currentEffectIndex, false);

  xTaskCreatePinnedToCore(
    renderTask,
    "RenderTask",
    8192,
    NULL,
    1,
    &renderTaskHandle,
    0
  );

  Serial.println();
  Serial.println(F("Matrix controller started"));
  printHelp();
  printStatus();
}

void loop() {
  handleSerial();
  server.handleClient();
  ArduinoOTA.handle();

  handleImageProxy();
  handleProxy();

  vTaskDelay(pdMS_TO_TICKS(10));
}