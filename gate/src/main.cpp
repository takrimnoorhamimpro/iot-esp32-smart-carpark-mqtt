// Smart Carpark - GATE CONTROLLER - ESP32, Wokwi + PlatformIO
//
// This board is the physical gate. It has no booking data. It only:
//   - sees a car arrive (entry HC-SR04) or want to leave (exit HC-SR04),
//   - reads the driver's code on a 4x4 keypad,
//   - asks the bay controller (the "brain", ../bays) if the car may enter,
//   - opens the servo barrier on a yes, and never closes it on a car.
//
// Driver at the entry:
//   0-9  type the 4-digit booking code      *  clear      #  confirm      A  no booking, take a ticket
//   3 wrong codes lock the keypad for 30 s.
//
// MQTT (PREFIX must be the same as in ../bays and the web pages)
//   publish    PREFIX/gate/req        "verify:REQ:CODE" | "walkin:REQ" | "passed:REQ" | "abort:REQ" | "left"
//   subscribe  PREFIX/gate/res        "REQ:ok:BAY" | "REQ:invalid" | "REQ:used" | "REQ:full" | "REQ:busy"
//   subscribe  PREFIX/sim/gs/entry    virtual entry-sensor distance in cm (simulator page)
//   subscribe  PREFIX/sim/gs/exit     virtual exit-sensor distance in cm
//   subscribe  PREFIX/sim/key         one key press typed from the simulator page (0-9 * # A)
//   subscribe  PREFIX/sim/reset       back to idle, barrier down
//   publish    PREFIX/status/gate        "open" | "closed"                  (retained)
//   publish    PREFIX/status/gateresult  "ok" | "wrong" | "used" | "full" | "locked" | "offline" | "timeout"
//   publish    PREFIX/status/gatelcd     "line1|line2" what the LCD shows   (retained)
//   publish    PREFIX/status/gateonline  "online" | "offline"               (retained, last will)

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// ===================== CONFIG =====================
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const bool  USE_TLS   = false;                 // Wokwi: false (port 1883) keeps the simulation fast. A real install MUST use true (port 8883 + TLS).
const String PREFIX = "takrim2026/carpark/k7q2x9m4";   // same as ../bays and the web pages

const int PIN_TRIG_ENTRY = 4,  PIN_ECHO_ENTRY = 34;    // 34/35 are input-only pins: fine for ECHO
const int PIN_TRIG_EXIT  = 5,  PIN_ECHO_EXIT  = 35;
const int PIN_SERVO  = 18;
const int PIN_BUZZER = 19;
const int BUZZ_CH    = 15;
// On real hardware ECHO is 5 V: put a voltage divider (1k + 2k) before the ESP32 pin.

const byte ROWS = 4, COLS = 4;
byte rowPins[ROWS] = {13, 14, 27, 26};
byte colPins[COLS] = {25, 33, 32, 23};
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

const int GATE_OPEN_ANGLE   = 90;
const int GATE_CLOSED_ANGLE = 0;
const int MAX_CM        = 400;
const int NEAR_CM       = 60;      // closer than this = a car is at the sensor
const int CLEAR_CM      = 90;      // farther than this = the sensor is clear (hysteresis)
const int REAL_CHANGE_CM = 4;      // a real sensor must move this much before it overrides a virtual value
const unsigned long SENSOR_PERIOD_MS = 40;     // one sensor per tick
const unsigned long CAR_SEEN_MS   = 600;       // a car must stand at the sensor this long before we react
const unsigned long CAR_GONE_MS   = 1200;      // and be gone this long before we say it left
const unsigned long CODE_IDLE_MS  = 60000;     // driver gave up typing
const unsigned long REPLY_MS      = 4000;      // how long we wait for the brain
const unsigned long CLOSE_DELAY_MS = 1200;     // after the car cleared the sensor
const unsigned long EXIT_MAX_MS   = 30000;
const unsigned long LOCK_MS       = 30000;
const int MAX_TRIES = 3;
const int CODE_LEN  = 4;
// ==================================================

WiFiClient net;
WiFiClientSecure netTls;
PubSubClient mqtt;
Servo barrier;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Keypad pad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

enum State { ST_IDLE, ST_WAIT_CODE, ST_CHECKING, ST_OPEN_ENTRY, ST_OPEN_EXIT, ST_DENIED, ST_LOCKED };
State state = ST_IDLE;

// sensors
int dist[2] = {MAX_CM, MAX_CM};          // 0 = entry, 1 = exit (effective: real or virtual)
int lastReal[2] = {-1, -1};
int ring[2][3];
uint8_t ringPos[2] = {0, 0};
bool ringInit[2] = {false, false};
bool present[2] = {false, false};        // debounced "a car is there"
unsigned long changeAt[2] = {0, 0};      // when the raw reading last disagreed with `present`
uint8_t sensorIdx = 0;
unsigned long lastSensorTick = 0;

// state data
String code;                             // what the driver has typed
String reqId;                            // the request waiting for an answer / the car that was let in
unsigned long stateSince = 0;
unsigned long lastKeyAt = 0;
unsigned long lockUntil = 0;
int tries = 0;
bool barrierUp = false;
bool sawClear = false;                   // ST_OPEN_ENTRY/EXIT: the car has left the sensor
bool virtualActive[2] = {false, false};     // web simulator owns a sensor once it sends sim/gs/*
unsigned long clearAt = 0;
uint16_t reqSeq = 0;
unsigned long buzzOffAt = 0;
unsigned long lastMqttTry = 0, lastWifiTry = 0;
String lcd0, lcd1;
String lastResult;

// ---------- helpers ----------
String topic(const String& suffix) { return PREFIX + "/" + suffix; }

void pub(const String& suffix, const String& value, bool retain) {
  if (mqtt.connected()) mqtt.publish(topic(suffix).c_str(), value.c_str(), retain);
}

String cleanId(const String& s) {
  String o;
  for (unsigned int i = 0; i < s.length() && o.length() < 16; i++) if (isalnum((unsigned char)s[i])) o += s[i];
  return o;
}

bool parseUInt(const String& s, int& out) {
  if (s.length() == 0 || s.length() > 5) return false;
  for (unsigned int i = 0; i < s.length(); i++) if (!isdigit((unsigned char)s[i])) return false;
  out = s.toInt();
  return true;
}

String newReq() { return String(++reqSeq, 36) + String((uint16_t)esp_random(), 36); }

void beep(unsigned long ms) {
  ledcWriteTone(BUZZ_CH, 2000);
  buzzOffAt = millis() + ms;
}

void show(const String& a, const String& b) {
  if (a == lcd0 && b == lcd1) return;
  lcd0 = a; lcd1 = b;
  String x = a, y = b;
  while (x.length() < 16) x += ' ';
  while (y.length() < 16) y += ' ';
  lcd.setCursor(0, 0); lcd.print(x.substring(0, 16));
  lcd.setCursor(0, 1); lcd.print(y.substring(0, 16));
  pub("status/gatelcd", a + "|" + b, true);
}

void result(const String& r) {
  lastResult = r;
  pub("status/gateresult", r, false);
  Serial.println("Result: " + r);
}

void setState(State s) { state = s; stateSince = millis(); }

void openBarrier() {
  barrier.write(GATE_OPEN_ANGLE);
  barrierUp = true;
  pub("status/gate", "open", true);
  Serial.println("Barrier open");
}

void closeBarrier() {
  barrier.write(GATE_CLOSED_ANGLE);
  barrierUp = false;
  pub("status/gate", "closed", true);
  Serial.println("Barrier closed");
}

String masked() {
  String m;
  for (int i = 0; i < CODE_LEN; i++) m += (i < (int)code.length()) ? '*' : '_';
  return m;
}

void showPrompt() { show("Code: " + masked(), "#=OK *=clr A=none"); }

void goIdle() {
  code = "";
  tries = 0;
  setState(ST_IDLE);
  show("Smart Carpark", "Welcome");
}

// ---------- sensors ----------
int readCm(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long us = pulseIn(echo, HIGH, 25000UL);
  if (us == 0) return MAX_CM;
  return constrain((int)(us / 58), 2, MAX_CM);
}

int median3(int a, int b, int c) {
  if (a > b) { int t = a; a = b; b = t; }
  if (b > c) { int t = b; b = c; c = t; }
  if (a > b) { int t = a; a = b; b = t; }
  return b;
}

void sampleNextSensor() {
  int i = sensorIdx;
  sensorIdx = (sensorIdx + 1) % 2;
  int r = (i == 0) ? readCm(PIN_TRIG_ENTRY, PIN_ECHO_ENTRY) : readCm(PIN_TRIG_EXIT, PIN_ECHO_EXIT);
  if (!ringInit[i]) { ring[i][0] = ring[i][1] = ring[i][2] = r; ringInit[i] = true; }
  else { ring[i][ringPos[i]] = r; ringPos[i] = (ringPos[i] + 1) % 3; }
  int m = median3(ring[i][0], ring[i][1], ring[i][2]);
  if (lastReal[i] < 0 || abs(m - lastReal[i]) >= REAL_CHANGE_CM) {   // the real sensor changed: it wins over a virtual value
    lastReal[i] = m;
    dist[i] = m;
  }
}

// turns the distance into a debounced "car present" flag
void updatePresence() {
  for (int i = 0; i < 2; i++) {
    if (virtualActive[i]) continue;  // virtual sensor changes are already exact MQTT events
    bool raw = present[i] ? (dist[i] < CLEAR_CM) : (dist[i] < NEAR_CM);
    if (raw == present[i]) { changeAt[i] = millis(); continue; }
    unsigned long need = raw ? CAR_SEEN_MS : CAR_GONE_MS;
    if (millis() - changeAt[i] >= need) { present[i] = raw; changeAt[i] = millis(); }
  }
}

// ---------- entry ----------
void deny(const String& line0, const String& line1, const String& res) {
  show(line0, line1);
  result(res);
  beep(400);
}

void submitCode() {
  if (!mqtt.connected()) { deny("System offline", "Ask the office", "offline"); return; }
  reqId = newReq();
  pub("gate/req", "verify:" + reqId + ":" + code, false);
  show("Checking...", "");
  setState(ST_CHECKING);
}

void submitWalkIn() {
  if (!mqtt.connected()) { deny("System offline", "Ask the office", "offline"); return; }
  reqId = newReq();
  pub("gate/req", "walkin:" + reqId, false);
  show("Checking...", "");
  setState(ST_CHECKING);
}

void startLock() {
  lockUntil = millis() + LOCK_MS;
  code = "";
  setState(ST_LOCKED);
  result("locked");
  beep(600);
}

void onBrainReply(const String& msg) {                // "REQ:ok:BAY" | "REQ:invalid" | ...
  if (state != ST_CHECKING) return;
  int c = msg.indexOf(':');
  if (c < 0 || msg.substring(0, c) != reqId) return;  // an answer to an old request
  String rest = msg.substring(c + 1);
  if (rest.startsWith("ok")) {
    int bay = 0;
    int c2 = rest.indexOf(':');
    if (c2 >= 0) parseUInt(rest.substring(c2 + 1), bay);
    if (!present[0]) { pub("gate/req", "abort:" + reqId, false); goIdle(); return; }   // the car already left
    show(bay > 0 ? "Code OK" : "Welcome", bay > 0 ? "Go to Bay " + String(bay) : "Take any bay");
    result("ok");
    tries = 0;
    openBarrier();
    beep(80);
    sawClear = false;
    setState(ST_OPEN_ENTRY);
  } else if (rest == "full") {
    deny("Carpark FULL", "Booked cars only", "full");
    setState(ST_DENIED);
  } else if (rest == "busy") {
    deny("Gate busy", "Please wait", "timeout");
    code = "";
    setState(ST_WAIT_CODE);
  } else {                                            // invalid | used
    tries++;
    String why = (rest == "used") ? "Code already used" : "Wrong code";
    if (tries >= MAX_TRIES) { show("Too many tries", "Locked 30 s"); startLock(); }
    else { deny(why, String(MAX_TRIES - tries) + " tries left", rest == "used" ? "used" : "wrong"); code = ""; setState(ST_WAIT_CODE); }
  }
}

void handleKey(char k) {
  lastKeyAt = millis();
  if (state != ST_WAIT_CODE) return;                     // keys only matter while a car waits for the code
  if (k >= '0' && k <= '9') {
    if ((int)code.length() < CODE_LEN) code += k;
    showPrompt();
    beep(30);
  } else if (k == '*') {
    code = "";
    showPrompt();
  } else if (k == '#') {
    if ((int)code.length() == CODE_LEN) submitCode();
    else { show("Need " + String(CODE_LEN) + " digits", "Code: " + masked()); beep(150); }
  } else if (k == 'A') {
    submitWalkIn();
  }
}

// Web simulator sensor events are handled immediately instead of waiting for simulated millis().
// This keeps the gate visually correct even when Wokwi runs at 10-20% real-time speed.
void applyVirtualSensor(int i, int cm) {
  cm = constrain(cm, 0, MAX_CM);
  virtualActive[i] = true;
  dist[i] = cm;
  bool nowPresent = cm < NEAR_CM;
  bool wasPresent = present[i];
  present[i] = nowPresent;
  changeAt[i] = millis();

  if (nowPresent == wasPresent) return;

  if (i == 0) { // ENTRY
    if (nowPresent && state == ST_IDLE) {
      code = ""; tries = 0; lastKeyAt = millis();
      showPrompt();
      setState(ST_WAIT_CODE);
    } else if (!nowPresent) {
      if (state == ST_CHECKING) { pub("gate/req", "abort:" + reqId, false); goIdle(); }
      else if (state == ST_WAIT_CODE || state == ST_DENIED) goIdle();
      else if (state == ST_OPEN_ENTRY) {
        pub("gate/req", "passed:" + reqId, false);
        closeBarrier();
        goIdle();
      }
    }
  } else { // EXIT
    if (nowPresent && state == ST_IDLE) {
      show("Goodbye", "Drive safe");
      openBarrier();
      beep(80);
      setState(ST_OPEN_EXIT);
    } else if (!nowPresent && state == ST_OPEN_EXIT) {
      pub("gate/req", "left", false);
      closeBarrier();
      goIdle();
    }
  }
}

// ---------- MQTT ----------
void onMessage(char* topicName, byte* payload, unsigned int length) {
  if (length > 48) return;
  String t = String(topicName);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (t == topic("gate/res")) { onBrainReply(msg); return; }
  String base = PREFIX + "/sim/";
  if (!t.startsWith(base)) return;
  String sub = t.substring(base.length());
  if (sub == "gs/entry" || sub == "gs/exit") {
    int cm = 0;
    if (parseUInt(msg, cm)) applyVirtualSensor(sub == "gs/entry" ? 0 : 1, cm);
  } else if (sub == "key") {
    if (msg.length() == 1) handleKey(msg[0]);
  } else if (sub == "reset") {
    if (barrierUp) closeBarrier();
    dist[0] = dist[1] = MAX_CM;
    present[0] = present[1] = false;
    virtualActive[0] = virtualActive[1] = false;
    goIdle();
  }
}

void connectMqtt() {
  String clientId = "esp32-gate-" + String((uint32_t)esp_random(), HEX);
  String willTopic = topic("status/gateonline");
  Serial.print("MQTT connecting... ");
  if (mqtt.connect(clientId.c_str(), NULL, NULL, willTopic.c_str(), 0, true, "offline")) {
    Serial.println("ok");
    mqtt.subscribe(topic("gate/res").c_str());
    mqtt.subscribe(topic("sim/gs/entry").c_str());
    mqtt.subscribe(topic("sim/gs/exit").c_str());
    mqtt.subscribe(topic("sim/key").c_str());
    mqtt.subscribe(topic("sim/reset").c_str());
    mqtt.publish(willTopic.c_str(), "online", true);
    pub("status/gate", barrierUp ? "open" : "closed", true);
    pub("status/gatelcd", lcd0 + "|" + lcd1, true);
  } else {
    Serial.printf("failed, rc=%d\n", mqtt.state());
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG_ENTRY, OUTPUT); digitalWrite(PIN_TRIG_ENTRY, LOW); pinMode(PIN_ECHO_ENTRY, INPUT);
  pinMode(PIN_TRIG_EXIT, OUTPUT);  digitalWrite(PIN_TRIG_EXIT, LOW);  pinMode(PIN_ECHO_EXIT, INPUT);

  ESP32PWM::allocateTimer(0);
  barrier.setPeriodHertz(50);
  barrier.attach(PIN_SERVO, 500, 2400);
  barrier.write(GATE_CLOSED_ANGLE);

  ledcSetup(BUZZ_CH, 2000, 8);
  ledcAttachPin(PIN_BUZZER, BUZZ_CH);
  ledcWriteTone(BUZZ_CH, 0);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  show("Smart Carpark", "Connecting WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(250); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi connected" : "\nWiFi not connected, gate is offline");

  if (USE_TLS) { netTls.setInsecure(); mqtt.setClient(netTls); }   // a real install pins the broker CA instead
  else mqtt.setClient(net);
  mqtt.setServer(MQTT_HOST, USE_TLS ? 8883 : 1883);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(60);          // Wokwi runs slower than real time: a short keepalive makes the broker drop us
  goIdle();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 10000) { lastWifiTry = millis(); WiFi.reconnect(); }
  } else if (!mqtt.connected() && millis() - lastMqttTry > 3000) {
    lastMqttTry = millis();
    connectMqtt();
  }
  mqtt.loop();

  // Heartbeat: a small message every 2 s keeps the broker connection alive even when the simulation runs slowly.
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat >= 2000) { lastBeat = millis(); pub("status/gatehb", String(millis() / 1000), false); }

  if (millis() - lastSensorTick >= SENSOR_PERIOD_MS) { lastSensorTick = millis(); sampleNextSensor(); }
  updatePresence();

  char k = pad.getKey();                               // the real keypad
  if (k) handleKey(k);

  if (buzzOffAt && millis() >= buzzOffAt) { ledcWriteTone(BUZZ_CH, 0); buzzOffAt = 0; }

  switch (state) {
    case ST_IDLE:
      if (present[0]) {                                // a car is waiting at the entry
        code = ""; tries = 0; lastKeyAt = millis();
        showPrompt();
        setState(ST_WAIT_CODE);
      } else if (present[1]) {                         // a car wants to leave
        show("Goodbye", "Drive safe");
        openBarrier();
        beep(80);
        sawClear = false;
        setState(ST_OPEN_EXIT);
      }
      break;

    case ST_WAIT_CODE:
      if (!present[0]) goIdle();                       // the car gave up and left
      else if (millis() - lastKeyAt > CODE_IDLE_MS) { show("Timed out", "Move on"); result("timeout"); setState(ST_DENIED); }
      break;

    case ST_CHECKING:
      if (millis() - stateSince > REPLY_MS) {          // the brain did not answer
        deny("No answer", "Try again", "timeout");
        code = "";
        setState(ST_WAIT_CODE);
      }
      break;

    case ST_DENIED:                                       // wait until that car has gone, then start over
      if (!present[0]) goIdle();
      break;

    case ST_LOCKED: {
      long left = (long)(lockUntil - millis());
      if (left <= 0) { tries = 0; code = ""; if (present[0]) { showPrompt(); setState(ST_WAIT_CODE); } else goIdle(); }
      else show("Keypad locked", String((left + 999) / 1000) + " s");
      break;
    }

    case ST_OPEN_ENTRY:                                   // the barrier is up: wait for the car to clear the sensor, then count it
      if (!present[0]) {
        if (!sawClear) { sawClear = true; clearAt = millis(); pub("gate/req", "passed:" + reqId, false); }
        else if (millis() - clearAt > CLOSE_DELAY_MS) { closeBarrier(); goIdle(); }
      } else if (sawClear) { sawClear = false; }       // a car is on the sensor again: never close the barrier on it
      break;

    case ST_OPEN_EXIT:
      if (!present[1]) {
        if (!sawClear) { sawClear = true; clearAt = millis(); pub("gate/req", "left", false); }
        else if (millis() - clearAt > CLOSE_DELAY_MS) { closeBarrier(); goIdle(); }
      } else if (sawClear) { sawClear = false; }
      break;
  }
}