// Smart Carpark - BAY CONTROLLER ("the brain") - ESP32, Wokwi + PlatformIO
//
// This board owns everything about the bays and the bookings:
//   4 x HC-SR04 (one per bay), red/green LED per bay, 16x2 LCD.
// The barrier, keypad and entry/exit sensors are on the separate GATE CONTROLLER (../gate).
// The two boards only talk over MQTT. The gate asks, this board decides.
//
// Gate <-> brain (PREFIX/gate/...)
//   gate -> brain  PREFIX/gate/req   "verify:REQ:CODE"  driver typed a booking code
//                                    "walkin:REQ"       driver pressed A (no booking)
//                                    "passed:REQ"       the granted car drove through the barrier
//                                    "abort:REQ"        the granted car never came
//                                    "left"             a car drove out
//   brain -> gate  PREFIX/gate/res   "REQ:ok:BAY"  (BAY 0 = walk-in, any free bay)
//                                    "REQ:invalid" | "REQ:used" | "REQ:full" | "REQ:busy"
//
// Other topics
//   subscribe  PREFIX/sim/slot/1..4   virtual bay distance in cm (simulator page, not retained)
//   subscribe  PREFIX/sim/reset       cars inside = 0
//   subscribe  PREFIX/sim/book        "BAY:CODE"  book a bay (app)      e.g. "3:4821"
//   subscribe  PREFIX/sim/cancel      "CODE"      cancel a booking (app)
//   subscribe  PREFIX/sim/hold        seconds a booking is held
//   publish    PREFIX/status/slot/N     "occupied" | "free"        (retained)
//   publish    PREFIX/status/available  free and not booked bays   (retained)
//   publish    PREFIX/status/inside     cars inside                (retained)
//   publish    PREFIX/status/suggest    bay to guide to, 0 = none  (retained)
//   publish    PREFIX/status/event      "entry-ok:ID" | "exit-ok" | "book-ok|book-denied|book-expired|book-done|book-cancelled:CODE"
//   publish    PREFIX/status/reserve/N  "none" | "CODE:secondsLeft:arrived(0/1)"   (retained)
//   publish    PREFIX/status/hold       booking hold time in seconds (retained)
//   publish    PREFIX/status/online     "online" | "offline"       (retained, last will)

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ===================== CONFIG =====================
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";   // or "broker.emqx.io"
const bool  USE_TLS   = false;                 // Wokwi: false (port 1883) keeps the simulation fast. A real install MUST use true (port 8883 + TLS).
// Public broker: anyone who knows this prefix can talk to your carpark. Keep it long and random,
// and use the same value in the web page.
const String PREFIX = "takrim2026/carpark/k7q2x9m4";

#define NUM_SLOTS 4
const int MAX_CM          = 400;   // HC-SR04 range limit, also "no echo"
const int OCC_ENTER_CM    = 40;    // below this a bay becomes occupied
const int OCC_EXIT_CM     = 55;    // above this a bay becomes free again (hysteresis, stops flicker)
const int REAL_CHANGE_CM  = 4;     // a sensor must move this much before it overrides a virtual value
const unsigned long SENSOR_PERIOD_MS = 50;   // one sensor per tick, so a full sweep = 200 ms

// HC-SR04 (TRIG outputs, ECHO inputs; 34/35/36/39 are input-only pins)
const int PIN_TRIG[NUM_SLOTS] = {4, 5, 16, 17};
const int PIN_ECHO[NUM_SLOTS] = {34, 35, 36, 39};
// On real hardware ECHO is 5 V: put a voltage divider (1k + 2k) before the ESP32 pin.

const int PIN_GREEN[NUM_SLOTS] = {13, 23, 14, 27};
const int PIN_RED[NUM_SLOTS]   = {26, 25, 33, 32};
const unsigned long BLINK_MS         = 300;
const unsigned long GRANT_MS         = 40000;   // a car that was let in has this long to drive through the barrier
const unsigned long LCD_MSG_MS = 4000;       // how long the result of a code check stays on the LCD
const unsigned long DEFAULT_HOLD_S = 120;     // how long a booking is held (the simulator page can change it)
// ==================================================

WiFiClient net;
WiFiClientSecure netTls;
PubSubClient mqtt;
LiquidCrystal_I2C lcd(0x27, 16, 2);

int distanceCm[NUM_SLOTS] = {200, 200, 200, 200};   // effective distance (real or virtual)
bool occupied[NUM_SLOTS]  = {false, false, false, false};
int8_t publishedState[NUM_SLOTS] = {-1, -1, -1, -1};

// sensor filtering
int ring[NUM_SLOTS][3];
uint8_t ringPos[NUM_SLOTS] = {0, 0, 0, 0};
bool ringInit[NUM_SLOTS]   = {false, false, false, false};
int lastReal[NUM_SLOTS]    = {-1, -1, -1, -1};
unsigned long lastSensorTick = 0;
uint8_t sensorIdx = 0;

// bookings: a booked bay is held for holdSec. Parked in time = done, too late = it is released.
unsigned long holdSec = DEFAULT_HOLD_S;
bool reserved[NUM_SLOTS]         = {false, false, false, false};
bool resArrived[NUM_SLOTS]       = {false, false, false, false};   // the booked car already came through the gate
String resCode[NUM_SLOTS];
unsigned long resUntil[NUM_SLOTS] = {0, 0, 0, 0};
unsigned long resPubAt[NUM_SLOTS] = {0, 0, 0, 0};

int freeNow = NUM_SLOTS;
int inside = 0;                    // cars that passed the entry gate and have not left
int pendingParks = 0;              // cars that entered and have not parked yet (they need guidance)
int lastFreePublished = -1;
int lastSuggestPublished = -1;
int suggestedBay = -1;             // 0-based first free bay, -1 = none
bool guiding = false;              // a car is inside and still looking for a bay
bool wasGuiding = false;
bool blinkOn = false;
unsigned long lastBlink = 0;

// the one car the gate was told it may let in (the gate handles a single car at a time)
struct Grant { bool active; String req; int bay; String code; unsigned long until; } grant = {false, "", -1, "", 0};
unsigned long lastMqttTry = 0;
unsigned long lastWifiTry = 0;
unsigned long lcdMsgUntil = 0;     // while this is in the future the LCD shows lcdMsg0/lcdMsg1
String lcdMsg0, lcdMsg1;


// ---------- helpers ----------
String topic(const String& suffix) { return PREFIX + "/" + suffix; }

void pub(const String& suffix, const String& value, bool retain) {
  if (mqtt.connected()) mqtt.publish(topic(suffix).c_str(), value.c_str(), retain);
}

String cleanId(const String& s) {          // ids from the network: letters/digits only, max 16
  String o;
  for (unsigned int i = 0; i < s.length() && o.length() < 16; i++) {
    char c = s[i];
    if (isalnum((unsigned char)c)) o += c;
  }
  return o;
}

String cleanCode(const String& s) {       // booking codes are letters/digits, upper case
  String c = cleanId(s);
  c.toUpperCase();
  return c;
}

String withId(const char* name, const String& id) {
  return id.length() ? String(name) + ":" + id : String(name);
}

bool parseUInt(const String& s, int& out) {   // digits only, so garbage never becomes "0 cm"
  if (s.length() == 0 || s.length() > 5) return false;
  for (unsigned int i = 0; i < s.length(); i++) if (!isdigit((unsigned char)s[i])) return false;
  out = s.toInt();
  return true;
}

int reservedCount() {
  int n = 0;
  for (int i = 0; i < NUM_SLOTS; i++) if (reserved[i]) n++;
  return n;
}

int availCount() {                          // bays that are free AND nobody booked
  int n = 0;
  for (int i = 0; i < NUM_SLOTS; i++) if (!occupied[i] && !reserved[i]) n++;
  return n;
}

int findReservation(const String& code) {
  if (code.length() == 0) return -1;
  for (int i = 0; i < NUM_SLOTS; i++) if (reserved[i] && resCode[i] == code) return i;
  return -1;
}

// "full" is for cars without a booking: booked bays and bays already promised to a car do not count
bool isFull() { return inside >= NUM_SLOTS || availCount() - pendingParks <= 0; }

void lcdLine(uint8_t row, const String& text) {
  String s = text;
  while (s.length() < 16) s += ' ';
  lcd.setCursor(0, row);
  lcd.print(s.substring(0, 16));
}

void updateLcd() {
  if (lcdMsgUntil && (long)(lcdMsgUntil - millis()) > 0) {   // a code check result is on screen
    lcdLine(0, lcdMsg0);
    lcdLine(1, lcdMsg1);
    return;
  }
  lcdLine(0, "Free:" + String(availCount()) + " R:" + String(reservedCount()) + " In:" + String(inside));
  if (isFull())            lcdLine(1, "FULL - NO ENTRY");
  else if (suggestedBay >= 0 && guiding) lcdLine(1, "Go to Bay " + String(suggestedBay + 1));
  else                          lcdLine(1, "Ready");
}

// shows the result of checking a booking code for a few seconds, then the normal screen comes back
void showCodeResult(const String& code, const String& result) {
  lcdMsg0 = "Code " + code;
  lcdMsg1 = result;
  lcdMsgUntil = millis() + LCD_MSG_MS;
  if (lcdMsgUntil == 0) lcdMsgUntil = 1;
  updateLcd();
}

// ---------- sensors ----------
int readCm(int i) {
  digitalWrite(PIN_TRIG[i], LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG[i], HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG[i], LOW);
  unsigned long us = pulseIn(PIN_ECHO[i], HIGH, 25000UL);   // 25 ms is about 4.3 m
  if (us == 0) return MAX_CM;                               // no echo = nothing in range
  return constrain((int)(us / 58), 2, MAX_CM);
}

int median3(int a, int b, int c) {
  if (a > b) { int t = a; a = b; b = t; }
  if (b > c) { int t = b; b = c; c = t; }
  if (a > b) { int t = a; a = b; b = t; }
  return b;
}

void refreshSlots();

void sampleNextSensor() {
  int i = sensorIdx;
  sensorIdx = (sensorIdx + 1) % NUM_SLOTS;
  int r = readCm(i);
  if (!ringInit[i]) {
    ring[i][0] = ring[i][1] = ring[i][2] = r;
    ringInit[i] = true;
  } else {
    ring[i][ringPos[i]] = r;
    ringPos[i] = (ringPos[i] + 1) % 3;
  }
  int m = median3(ring[i][0], ring[i][1], ring[i][2]);      // kills single bad readings
  if (lastReal[i] < 0 || abs(m - lastReal[i]) >= REAL_CHANGE_CM) {
    lastReal[i] = m;
    if (distanceCm[i] != m) {                                // the sensor changed: it wins
      distanceCm[i] = m;
      refreshSlots();
    }
  }
}

// ---------- slots ----------
// green = free, red = occupied, green + red together = booked and waiting for its car
void applySlotLeds() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    digitalWrite(PIN_RED[i], (occupied[i] || reserved[i]) ? HIGH : LOW);
    digitalWrite(PIN_GREEN[i], occupied[i] ? LOW : HIGH);
  }
}

// ---------- bookings ----------
void pubReserve(int i) {
  resPubAt[i] = millis();
  String t = "status/reserve/" + String(i + 1);
  if (!reserved[i]) { pub(t, "none", true); return; }
  long left = (long)(resUntil[i] - millis()) / 1000;
  if (left < 0) left = 0;
  pub(t, resCode[i] + ":" + String(left) + ":" + (resArrived[i] ? "1" : "0"), true);
}

// ends a booking. why = "book-done" | "book-expired" | "book-cancelled"
void endReservation(int i, const char* why) {
  String code = resCode[i];
  bool wasInside = resArrived[i];
  reserved[i] = false;
  resArrived[i] = false;
  resCode[i] = "";
  pub("status/event", String(why) + ":" + code, false);   // the reason first, so the app can show it
  pubReserve(i);
  // a booked car that is already inside but lost its bay now looks for any free bay like everyone else
  if (wasInside && strcmp(why, "book-done") != 0) pendingParks++;
  Serial.printf("Booking %s for bay %d ended: %s\n", code.c_str(), i + 1, why);
}

// Guidance: if a car is inside but not parked yet, point it to the first free bay.
void updateGuidance() {
  suggestedBay = -1;
  // a booked car that is inside goes to its own bay
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (reserved[i] && resArrived[i] && !occupied[i]) { suggestedBay = i; break; }
  }
  bool bookedCar = (suggestedBay >= 0);
  // any other car goes to the first free bay that nobody booked
  if (!bookedCar) {
    for (int i = 0; i < NUM_SLOTS; i++) {
      if (!occupied[i] && !reserved[i]) { suggestedBay = i; break; }
    }
  }
  int parked = NUM_SLOTS - freeNow;
  // Only a car that just came in is guided. A parked car backing out of its bay is not.
  pendingParks = constrain(pendingParks, 0, max(0, inside - parked));
  guiding = (suggestedBay >= 0) && (bookedCar || pendingParks > 0);
  int value = guiding ? suggestedBay + 1 : 0;
  if (value != lastSuggestPublished) {
    lastSuggestPublished = value;
    pub("status/suggest", String(value), true);
  }
}

void refreshSlots() {
  int freeCount = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    bool was = occupied[i];
    if (distanceCm[i] < OCC_ENTER_CM) occupied[i] = true;
    else if (distanceCm[i] > OCC_EXIT_CM) occupied[i] = false;   // in between: keep old state
    if (!was && occupied[i]) {
      if (reserved[i]) endReservation(i, "book-done");            // parked in a booked bay: booking complete
      else if (pendingParks > 0) pendingParks--;                  // the arriving car has parked
    }
    if (!occupied[i]) freeCount++;
    if (publishedState[i] != (int8_t)occupied[i]) {
      publishedState[i] = occupied[i];
      pub("status/slot/" + String(i + 1), occupied[i] ? "occupied" : "free", true);
      Serial.printf("Slot %d -> %s (%d cm)\n", i + 1, occupied[i] ? "occupied" : "free", distanceCm[i]);
    }
  }
  freeNow = freeCount;
  applySlotLeds();
  int avail = availCount();                                       // free and not booked
  if (avail != lastFreePublished) {
    lastFreePublished = avail;
    pub("status/available", String(avail), true);
  }
  updateGuidance();
  updateLcd();
}

// ---------- gate ----------
void publishInside() { pub("status/inside", String(inside), true); }

void replyGate(const String& req, const String& result) { pub("gate/res", req + ":" + result, false); }

// The gate controller sends what the driver did. We decide; the gate only obeys.
// code empty = walk-in (no booking).
void handleVerify(const String& req, const String& code) {
  if (grant.active && (long)(millis() - grant.until) < 0 && grant.req != req) {
    replyGate(req, "busy");                              // another car was already let in and has not driven through yet
    return;
  }
  grant.active = false;
  if (code.length() > 0) {
    int b = findReservation(code);
    if (b >= 0 && !resArrived[b]) {
      grant = {true, req, b, code, millis() + GRANT_MS};
      showCodeResult(code, "VERIFIED Bay " + String(b + 1));
      Serial.printf("Code %s verified: bay %d\n", code.c_str(), b + 1);
      replyGate(req, "ok:" + String(b + 1));
    } else if (b >= 0) {
      showCodeResult(code, "ALREADY USED");
      Serial.printf("Code %s already used\n", code.c_str());
      replyGate(req, "used");
    } else {
      showCodeResult(code, "INVALID/EXPIRED");
      Serial.printf("Code %s invalid or expired\n", code.c_str());
      replyGate(req, "invalid");
    }
    return;
  }
  if (isFull()) {                                        // walk-in: booked bays and bays promised to other cars do not count
    Serial.println("Walk-in denied: carpark full");
    replyGate(req, "full");
    return;
  }
  grant = {true, req, -1, "", millis() + GRANT_MS};
  Serial.println("Walk-in granted");
  replyGate(req, "ok:0");
}

// the granted car really drove through the barrier: now it counts as inside
void handlePassed(const String& req) {
  if (!grant.active || grant.req != req) return;
  grant.active = false;
  inside++;
  int b = grant.bay;
  if (b >= 0 && reserved[b] && resCode[b] == grant.code && !resArrived[b]) {
    resArrived[b] = true;
    pubReserve(b);
    Serial.printf("Booked car for bay %d is in\n", b + 1);
  } else {
    pendingParks++;                                      // walk-in, or the booking ran out while it was driving in
  }
  publishInside();
  updateGuidance();
  pub("status/event", withId("entry-ok", req), false);
  refreshSlots();
  Serial.printf("Entry ok, cars inside: %d\n", inside);
}

void handleAbort(const String& req) {
  if (grant.active && grant.req == req) grant.active = false;
}

void handleExit() {
  if (inside > 0) inside--;
  publishInside();
  updateGuidance();
  pub("status/event", "exit-ok", false);
  updateLcd();
  Serial.printf("Exit ok, cars inside: %d\n", inside);
}

void handleBook(int n, const String& code) {
  int i = n - 1;
  bool ok = code.length() >= 3
         && findReservation(code) < 0
         && !occupied[i] && !reserved[i]
         && availCount() - pendingParks > 0;           // a bay already promised to an arriving car is not for booking
  if (!ok) {
    Serial.printf("Booking %s for bay %d denied\n", code.c_str(), n);
    pub("status/event", "book-denied:" + code, false);
    return;
  }
  reserved[i] = true;
  resArrived[i] = false;
  resCode[i] = code;
  resUntil[i] = millis() + holdSec * 1000UL;
  pubReserve(i);
  pub("status/event", "book-ok:" + code, false);
  Serial.printf("Booking %s: bay %d held for %lu s\n", code.c_str(), n, holdSec);
  refreshSlots();                                      // LEDs, free count, guidance, LCD
}

void handleCancel(const String& code) {
  int i = findReservation(code);
  if (i < 0) return;
  endReservation(i, "book-cancelled");
  refreshSlots();
}

// once a second: release bookings whose time ran out, refresh the countdown for the apps
void checkReservations() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();
  bool changed = false;
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (!reserved[i]) continue;
    if ((long)(millis() - resUntil[i]) >= 0) {
      endReservation(i, "book-expired");
      changed = true;
    } else if (millis() - resPubAt[i] >= 5000UL) {
      pubReserve(i);
    }
  }
  if (changed) refreshSlots();
}

// ---------- MQTT ----------
void onMessage(char* topicName, byte* payload, unsigned int length) {
  if (length > 64) return;                       // nothing we accept is this long
  String t = String(topicName);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (t == topic("gate/req")) {                  // "verify:REQ:CODE" | "walkin:REQ" | "passed:REQ" | "abort:REQ" | "left"
    int c = msg.indexOf(':');
    String cmd  = (c < 0) ? msg : msg.substring(0, c);
    String rest = (c < 0) ? String("") : msg.substring(c + 1);
    int c2 = rest.indexOf(':');
    String req  = cleanId(c2 < 0 ? rest : rest.substring(0, c2));
    String code = (c2 < 0) ? String("") : cleanCode(rest.substring(c2 + 1));
    if (cmd == "verify" && req.length() && code.length()) handleVerify(req, code);
    else if (cmd == "walkin" && req.length()) handleVerify(req, "");
    else if (cmd == "passed" && req.length()) handlePassed(req);
    else if (cmd == "abort" && req.length()) handleAbort(req);
    else if (cmd == "left") handleExit();
    return;
  }

  String base = PREFIX + "/sim/";
  if (!t.startsWith(base)) return;
  String sub = t.substring(base.length());

  if (sub.startsWith("slot/")) {
    int n = 0, cm = 0;
    if (parseUInt(sub.substring(5), n) && n >= 1 && n <= NUM_SLOTS && parseUInt(msg, cm)) {
      distanceCm[n - 1] = constrain(cm, 0, MAX_CM);
      refreshSlots();
    }
  } else if (sub == "book") {                    // "BAY:CODE"
    int c = msg.indexOf(':');
    int n = 0;
    if (c > 0 && parseUInt(msg.substring(0, c), n) && n >= 1 && n <= NUM_SLOTS) {
      handleBook(n, cleanCode(msg.substring(c + 1)));
    }
  } else if (sub == "cancel") {
    handleCancel(cleanCode(msg));
  } else if (sub == "hold") {
    int s = 0;
    if (parseUInt(msg, s)) {
      holdSec = constrain(s, 10, 3600);          // applies to new bookings
      pub("status/hold", String(holdSec), true);
    }
  } else if (sub == "reset") {
    inside = 0;
    pendingParks = 0;
    grant.active = false;
    for (int i = 0; i < NUM_SLOTS; i++) {        // cars are gone, so booked cars have to arrive again
      if (reserved[i] && resArrived[i]) { resArrived[i] = false; pubReserve(i); }
    }
    publishInside();
    updateGuidance();
    updateLcd();
    Serial.println("Reset: cars inside = 0");
  }
}

void connectMqtt() {
  String clientId = "esp32-carpark-" + String((uint32_t)esp_random(), HEX);
  String willTopic = topic("status/online");
  Serial.print("MQTT connecting... ");
  if (mqtt.connect(clientId.c_str(), NULL, NULL, willTopic.c_str(), 0, true, "offline")) {
    Serial.println("ok");
    mqtt.subscribe(topic("sim/#").c_str());
    mqtt.subscribe(topic("gate/req").c_str());
    mqtt.publish(willTopic.c_str(), "online", true);
    for (int i = 0; i < NUM_SLOTS; i++) publishedState[i] = -1;
    lastFreePublished = -1;
    lastSuggestPublished = -1;
    pub("status/hold", String(holdSec), true);
    for (int i = 0; i < NUM_SLOTS; i++) pubReserve(i);   // also clears old retained bookings
    publishInside();
    refreshSlots();
  } else {
    Serial.printf("failed, rc=%d\n", mqtt.state());
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(PIN_GREEN[i], OUTPUT);
    pinMode(PIN_RED[i], OUTPUT);
    pinMode(PIN_TRIG[i], OUTPUT);
    digitalWrite(PIN_TRIG[i], LOW);
    pinMode(PIN_ECHO[i], INPUT);
  }

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdLine(0, "Smart Carpark");
  lcdLine(1, "Connecting WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi connected" : "\nWiFi not connected, running offline");

  if (USE_TLS) {
    netTls.setInsecure();      // encrypts the link; a real product should pin the broker CA instead
    mqtt.setClient(netTls);
  } else {
    mqtt.setClient(net);
  }
  mqtt.setServer(MQTT_HOST, USE_TLS ? 8883 : 1883);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(60);          // Wokwi runs slower than real time: a short keepalive makes the broker drop us

  refreshSlots();            // the carpark works locally even without WiFi or MQTT
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 10000) {
      lastWifiTry = millis();
      WiFi.reconnect();
    }
  } else if (!mqtt.connected() && millis() - lastMqttTry > 3000) {
    lastMqttTry = millis();
    connectMqtt();
  }
  mqtt.loop();

  // Heartbeat: a small message every 2 s keeps the broker connection alive even when the simulation runs slowly.
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat >= 2000) { lastBeat = millis(); pub("status/bayhb", String(millis() / 1000), false); }

  if (millis() - lastSensorTick >= SENSOR_PERIOD_MS) {
    lastSensorTick = millis();
    sampleNextSensor();
  }

  checkReservations();
  if (lcdMsgUntil && (long)(millis() - lcdMsgUntil) >= 0) { lcdMsgUntil = 0; updateLcd(); }   // result time is over

  if (grant.active && (long)(millis() - grant.until) >= 0) grant.active = false;   // the car never drove through

  // blink the green LED of the suggested bay while a car is looking for a place
  if (guiding) {
    if (millis() - lastBlink >= BLINK_MS) {
      lastBlink = millis();
      blinkOn = !blinkOn;
      applySlotLeds();
      if (!blinkOn && suggestedBay >= 0) {
        digitalWrite(PIN_GREEN[suggestedBay], LOW);
        if (reserved[suggestedBay]) digitalWrite(PIN_RED[suggestedBay], LOW);
      }
    }
    wasGuiding = true;
  } else if (wasGuiding) {
    applySlotLeds();
    wasGuiding = false;
  }
}