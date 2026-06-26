// Paired LoveLamp — ESP32 D1 Mini + TTP223 + WS2812 ring (12 LEDs)
//
// Wiring:
//   TTP223 VCC  -> 3.3V    TTP223 SIG -> IO23 (D7)
//   LED VCC     -> 5V       LED DIN    -> IO17 (D3)
//   Shared GND

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <driver/gpio.h>
#include <Preferences.h>

#include "secrets.h"

#ifndef LAMP_ID
#define LAMP_ID "A"
#endif

static const int TOUCH_PIN = 23;
static const int LED_PIN = 17;
static const int NUM_LEDS = 12;
static const int LED_BRIGHTNESS = 128;

static const uint32_t AUTO_OFF_MS = 1800000UL;
static const uint32_t ACK_TIMEOUT_MS = 5000UL;
static const uint32_t TAP_GAP_MS = 700;
static const uint32_t ON_FAIL_DARK_MS = 400;

static const char *TOPIC_EVENT = "lovelamp/event";
static const char *TOPIC_ACK = "lovelamp/ack";
static const char *PORTAL_SSID = "Z+S";

static const CRGB CYCLE_COLORS[] = {
  CRGB(255, 255, 255),  // white
  CRGB::Blue,
  CRGB(255, 105, 180),  // hot pink
  CRGB::Red,
};
static const uint8_t CYCLE_LEN = sizeof(CYCLE_COLORS) / sizeof(CYCLE_COLORS[0]);
static const uint8_t CYCLE_OFF = CYCLE_LEN;

CRGB leds[NUM_LEDS];
WiFiManager wm;
WiFiClientSecure wifiTls;
PubSubClient mqtt(wifiTls);
Preferences prefs;

bool lampOn = false;
CRGB activeColor = CRGB::Black;
uint32_t offAt = 0;
uint32_t msgId = 0;

bool waitingForAck = false;
uint32_t pendingMsgId = 0;
uint32_t ackWaitStart = 0;
bool ackReceived = false;

enum class AnimState {
  None,
  OnFail,
  ErrorPulse,
};

AnimState animState = AnimState::None;
uint8_t animStep = 0;
uint32_t animStepAt = 0;

// --- Touch tracking ---
bool touchPrev = false;
uint32_t lastRelease = 0;
uint8_t tapCount = 0;
uint8_t cycleIndex = 0;
bool portalActive = false;
bool bootReadyFlashPending = false;

bool isTouched() { return digitalRead(TOUCH_PIN) == HIGH; }

void setRing(CRGB color) {
  activeColor = color;
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
}

void ringOff() {
  lampOn = false;
  offAt = 0;
  activeColor = CRGB::Black;
  cycleIndex = 0;
  FastLED.clear(true);
}

void startHourTimer() {
  offAt = millis() + AUTO_OFF_MS;
  lampOn = true;
}

// --- Blocking LED animations (boot / setup only) ---
void delayWithMqtt(uint32_t ms) {
  uint32_t end = millis() + ms;
  while ((int32_t)(end - millis()) > 0) {
    mqtt.loop();
    delay(10);
  }
}

void pulseColorTwice(CRGB color) {
  for (int i = 0; i < 2; i++) {
    setRing(color);
    delayWithMqtt(180);
    FastLED.clear(true);
    FastLED.show();
    delayWithMqtt(180);
  }
}

void pulseColorOnce(CRGB color) {
  setRing(color);
  delayWithMqtt(250);
  FastLED.clear(true);
  FastLED.show();
}

void breatheBlueFrame() {
  float phase = (millis() % 2000) / 2000.0f;
  float level = (sinf(phase * 2.0f * PI) + 1.0f) * 0.5f;
  uint8_t b = (uint8_t)(40 + level * 180);
  fill_solid(leds, NUM_LEDS, CRGB(0, 0, b));
  FastLED.show();
}

// --- Non-blocking animations during main loop ---
void startAnim(AnimState state) {
  animState = state;
  animStep = 0;
  animStepAt = millis();
  if (state == AnimState::OnFail) {
    ringOff();
  }
}

void tickAnimation() {
  if (animState == AnimState::None) return;

  uint32_t now = millis();
  switch (animState) {
    case AnimState::OnFail:
      if (animStep == 0 && (int32_t)(now - animStepAt) >= (int32_t)ON_FAIL_DARK_MS) {
        setRing(CRGB::Red);
        animStep = 1;
        animStepAt = now;
      } else if (animStep == 1 && (int32_t)(now - animStepAt) >= 200) {
        FastLED.clear(true);
        FastLED.show();
        animStep = 2;
        animStepAt = now;
      } else if (animStep == 2) {
        animState = AnimState::None;
        ringOff();
      }
      break;

    case AnimState::ErrorPulse:
      if (animStep == 0 && (int32_t)(now - animStepAt) >= 250) {
        setRing(CRGB::Red);
        animStep = 1;
        animStepAt = now;
      } else if (animStep == 1 && (int32_t)(now - animStepAt) >= 250) {
        animState = AnimState::None;
        FastLED.clear(true);
        FastLED.show();
      }
      break;

    default:
      break;
  }
}

bool animBusy() { return animState != AnimState::None; }

// --- MQTT ---
void loadPersistedState() {
  prefs.begin("lovelamp", false);
  msgId = prefs.getUInt("msg_id", 0);
  cycleIndex = prefs.getUChar("cycle_idx", 0);
  if (cycleIndex > CYCLE_OFF) cycleIndex = 0;
}

void saveMsgId() {
  prefs.putUInt("msg_id", msgId);
}

void saveCycleIndex() {
  prefs.putUChar("cycle_idx", cycleIndex);
}

bool mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqtt.connected()) return true;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);

  String clientId = String("lovelamp-") + LAMP_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    mqtt.subscribe(TOPIC_EVENT);
    mqtt.subscribe(TOPIC_ACK);
    Serial.println("MQTT connected");
    return true;
  }
  Serial.print("MQTT failed: ");
  Serial.println(mqtt.state());
  return false;
}

void tryBootReadyFlash() {
  if (!bootReadyFlashPending) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqttConnect()) return;
  bootReadyFlashPending = false;
  pulseColorTwice(CRGB::Green);
}

bool publishEvent(const char *cmd, CRGB color = CRGB::Black, uint8_t nextCycleIndex = 0) {
  if (!mqtt.connected() && !mqttConnect()) return false;

  JsonDocument doc;
  doc["sender"] = LAMP_ID;
  doc["msg_id"] = msgId;
  doc["cmd"] = cmd;
  doc["cycle_index"] = nextCycleIndex;
  if (strcmp(cmd, "on") == 0) {
    doc["r"] = color.r;
    doc["g"] = color.g;
    doc["b"] = color.b;
  }

  char payload[192];
  serializeJson(doc, payload, sizeof(payload));
  return mqtt.publish(TOPIC_EVENT, payload);
}

bool publishAck(const char *ackFor, uint32_t id) {
  if (!mqtt.connected() && !mqttConnect()) return false;

  JsonDocument doc;
  doc["sender"] = LAMP_ID;
  doc["ack_for"] = ackFor;
  doc["msg_id"] = id;

  char payload[128];
  serializeJson(doc, payload, sizeof(payload));
  return mqtt.publish(TOPIC_ACK, payload);
}

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;

  if (strcmp(topic, TOPIC_ACK) == 0) {
    const char *ackFor = doc["ack_for"] | "";
    uint32_t id = doc["msg_id"] | 0U;
    if (strcmp(ackFor, LAMP_ID) == 0 && waitingForAck && id == pendingMsgId) {
      ackReceived = true;
    }
    return;
  }

  if (strcmp(topic, TOPIC_EVENT) != 0) return;

  const char *sender = doc["sender"] | "";
  if (strcmp(sender, LAMP_ID) == 0) return;

  const char *cmd = doc["cmd"] | "";
  if (strcmp(cmd, "off") == 0) {
    ringOff();
    saveCycleIndex();
    return;
  }

  if (strcmp(cmd, "on") == 0) {
    CRGB color(doc["r"] | 0, doc["g"] | 0, doc["b"] | 0);
    cycleIndex = doc["cycle_index"] | 0U;
    if (cycleIndex > CYCLE_OFF) cycleIndex = 0;
    saveCycleIndex();
    setRing(color);
    startHourTimer();
    uint32_t id = doc["msg_id"] | 0U;
    publishAck(sender, id);
  }
}

void beginAckWait(uint32_t id) {
  waitingForAck = true;
  pendingMsgId = id;
  ackWaitStart = millis();
  ackReceived = false;
}

void clearAckWait() {
  waitingForAck = false;
  ackReceived = false;
}

void tickAckWait() {
  if (!waitingForAck) return;

  if (ackReceived) {
    clearAckWait();
    return;
  }

  if ((int32_t)(millis() - ackWaitStart) >= (int32_t)ACK_TIMEOUT_MS) {
    clearAckWait();
    startAnim(AnimState::OnFail);
  }
}

// --- Lamp actions ---
bool wifiIsConfigured() { return wm.getWiFiIsSaved(); }

void showNoWifiError() {
  startAnim(AnimState::ErrorPulse);
}

void turnOn(CRGB color, uint8_t nextCycleIndex) {
  if (!wifiIsConfigured()) {
    showNoWifiError();
    return;
  }
  if (!mqttConnect()) {
    showNoWifiError();
    return;
  }

  msgId++;
  saveMsgId();
  setRing(color);
  startHourTimer();

  if (!publishEvent("on", color, nextCycleIndex)) {
    ringOff();
    startAnim(AnimState::OnFail);
    return;
  }

  cycleIndex = nextCycleIndex;
  saveCycleIndex();
  beginAckWait(msgId);
}

void turnOff() {
  ringOff();
  saveCycleIndex();
  if (!wifiIsConfigured()) return;

  msgId++;
  saveMsgId();
  if (!publishEvent("off", CRGB::Black, 0)) {
    startAnim(AnimState::ErrorPulse);
  }
}

void tickAutoOff() {
  if (!lampOn || offAt == 0) return;
  if ((int32_t)(millis() - offAt) >= 0) {
    ringOff();
  }
}

// --- Gestures ---

void startWifiPortal() {
  Serial.println("WiFi setup portal starting");
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(300);
  wm.setCaptivePortalEnable(true);
  wm.startConfigPortal(PORTAL_SSID);
  portalActive = true;
  tapCount = 0;
}

void cancelWifiPortal() {
  Serial.println("WiFi setup portal cancelled");
  wm.stopConfigPortal();
  portalActive = false;
  tapCount = 0;
  FastLED.clear(true);
  FastLED.show();
}

void cycleTap() {
  if (animBusy() || waitingForAck) return;

  if (!lampOn) {
    if (cycleIndex >= CYCLE_LEN) cycleIndex = 0;
    uint8_t next = (cycleIndex + 1 >= CYCLE_LEN) ? CYCLE_OFF : cycleIndex + 1;
    turnOn(CYCLE_COLORS[cycleIndex], next);
    return;
  }

  if (cycleIndex >= CYCLE_LEN) {
    turnOff();
    return;
  }

  uint8_t next = (cycleIndex + 1 >= CYCLE_LEN) ? CYCLE_OFF : cycleIndex + 1;
  turnOn(CYCLE_COLORS[cycleIndex], next);
}

void processButtonRelease() {
  uint32_t now = millis();
  if (tapCount == 0 || (now - lastRelease) > TAP_GAP_MS) {
    tapCount = 1;
  } else {
    tapCount++;
  }
  lastRelease = now;

  if (tapCount >= 3) {
    tapCount = 0;
    if (portalActive) {
      cancelWifiPortal();
    } else {
      startWifiPortal();
    }
  }
}

void tickWifiPortal() {
  if (!portalActive) return;

  breatheBlueFrame();
  wm.process();

  if (wm.getConfigPortalActive()) return;

  portalActive = false;
  if (wm.getWiFiIsSaved()) {
    pulseColorOnce(CRGB::Green);
    delay(300);
    ESP.restart();
  } else {
    FastLED.clear(true);
    FastLED.show();
  }
}

void tickTouch() {
  bool touched = isTouched();
  uint32_t now = millis();

  if (!touched && touchPrev) {
    processButtonRelease();
  }

  touchPrev = touched;

  if (portalActive) return;

  if (tapCount >= 1 && tapCount < 3 && (now - lastRelease) > TAP_GAP_MS) {
    cycleTap();
    tapCount = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TOUCH_PIN, INPUT);
  gpio_set_drive_capability((gpio_num_t)LED_PIN, GPIO_DRIVE_CAP_3);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  FastLED.clear(true);

  loadPersistedState();

  wm.setWiFiAutoReconnect(true);
  wm.setConnectRetries(3);

  if (wifiIsConfigured()) {
    WiFi.mode(WIFI_STA);
    wm.autoConnect(PORTAL_SSID);
    bootReadyFlashPending = true;
  } else {
    pulseColorTwice(CRGB::Red);
  }

  wifiTls.setInsecure();
  mqtt.setCallback(onMqttMessage);

  tryBootReadyFlash();

  Serial.print("LoveLamp ");
  Serial.print(LAMP_ID);
  Serial.println(" ready");
}

void loop() {
  if (portalActive) {
    tickTouch();
    tickWifiPortal();
    delay(10);
    return;
  }

  mqtt.loop();
  if (wifiIsConfigured()) {
    if (!mqtt.connected()) {
      mqttConnect();
    }
    tryBootReadyFlash();
  }

  tickTouch();
  tickAutoOff();
  tickAckWait();
  tickAnimation();

  delay(10);
}
