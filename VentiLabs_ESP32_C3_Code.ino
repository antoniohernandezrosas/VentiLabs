/*********************************************************************
  VentiLabs – AI Powered Ventilation System
*********************************************************************/

#include <Arduino.h>
#include <DHT.h>
#include <WiFi.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

/* ===================== PINOUT ===================== */
#define MQ7_PIN     0
#define MQ4_PIN     1
#define MQ8_PIN     2
#define MQ3_PIN     3
#define MQ135_PIN   4

#define DHT_PIN     5
#define DHT_TYPE  DHT11

#define BTN_PAIR    6
#define LED_STATUS  7

#define TRIAC_CH1   8
#define TRIAC_CH2   9
#define RELAY_EMERG 10

#define LED_R       20
#define LED_G       21

/* ===================== BLE UUID ===================== */
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

/* ===================== OBJECTS ===================== */
DHT dht(DHT_PIN, DHT_TYPE);
Preferences prefs;
NimBLECharacteristic *txChar;

/* ===================== CONSTANTS ===================== */
const float MIN_FAN = 20.0;
const float MAX_FAN = 100.0;

float SAFE_PPM     = 100.0;
float ALERT_PPM    = 300.0;
float CRITICAL_PPM = 400.0;

/* Derivative control (Mathematic innovation) */
const float KP = 0.25;
const float KD = 0.15;

/* ===================== SYSTEM ===================== */
#define N_SENS 5

float ppm[N_SENS];
float prevPPM[N_SENS];
float dPPM[N_SENS];
float baseline[N_SENS];

bool sensorIdle[N_SENS];
uint16_t idleCounter[N_SENS];

float pwmFinal = MIN_FAN;
float systemFatigue = 0;

/* ===================== STATES ===================== */
enum SystemState { NORMAL, PREVENTIVE, ALERT, CRITICAL, FAILSAFE };
SystemState state = NORMAL;

/* ===================== TIME ===================== */
unsigned long lastSample = 0;
unsigned long lastPWM = 0;
unsigned long lastSerial = 0;
unsigned long lastSelfTest = 0;

/* ===================== CONECTIVITY ===================== */
bool bleActive = false;
bool wifiConnected = false;
bool lastBtn = HIGH;
unsigned long ledTimer = 0;
bool ledBlink = false;

/* ===================== FUNCIONS ===================== */

// ---------- DIMMER AC ----------
void driveTriac(uint8_t pin, float percent) {
  percent = constrain(percent, 0, 100);
  digitalWrite(pin, percent > 50); // Simplified (CPU Save)
}

// ---------- GAS READ ----------
float readGas(uint8_t pin, float t, float h) {
  int raw = analogRead(pin);
  float ppm = map(raw, 0, 4095, 0, 1000);
  ppm *= (1.0 + (t - 25.0) * 0.01);
  ppm *= (1.0 + (h - 50.0) * 0.005);
  return constrain(ppm, 0, 1000);
}

// ---------- RGB LED ----------
void setAirLED(float value) {
  if (state == NORMAL && value < SAFE_PPM * 0.8) {
    analogWrite(LED_R, 0);
    analogWrite(LED_G, 0);
    return;
  }

  int r = 0, g = 0;
  if (value < SAFE_PPM) g = 180;
  else if (value < ALERT_PPM) {
    r = map(value, SAFE_PPM, ALERT_PPM, 0, 180);
    g = 180;
  } else {
    r = 180;
    g = map(value, ALERT_PPM, CRITICAL_PPM, 180, 0);
  }

  analogWrite(LED_R, r);
  analogWrite(LED_G, g);
}

// ---------- PREDICTION WITHOUT AI ----------
bool predictRisk(float ppm, float dppm) {
  return (ppm + dppm * 8.0) > ALERT_PPM;
}

/* ===================== BLE RX ===================== */
class RXCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) {
    String msg = c->getValue().c_str();
    if (msg.startsWith("WIFI:")) {
      msg.remove(0, 5);
      int p = msg.indexOf(',');
      if (p > 0) {
        prefs.putString("ssid", msg.substring(0, p));
        prefs.putString("pass", msg.substring(p + 1));
        WiFi.begin(msg.substring(0, p).c_str(),
                   msg.substring(p + 1).c_str());
      }
    }
  }
};

// ---------- BLE ----------
void startBLE() {
  if (bleActive) return;
  NimBLEDevice::init("VentiLabs");
  auto server = NimBLEDevice::createServer();
  auto service = server->createService(SERVICE_UUID);

  txChar = service->createCharacteristic(CHAR_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  auto rxChar = service->createCharacteristic(CHAR_RX_UUID, NIMBLE_PROPERTY::WRITE);
  rxChar->setCallbacks(new RXCallbacks());

  service->start();
  NimBLEDevice::getAdvertising()->start();
  bleActive = true;
}

/* ===================== SETUP ===================== */
void setup() {
  Serial.begin(115200);

  pinMode(TRIAC_CH1, OUTPUT);
  pinMode(TRIAC_CH2, OUTPUT);
  pinMode(RELAY_EMERG, OUTPUT);
  pinMode(BTN_PAIR, INPUT_PULLUP);
  pinMode(LED_STATUS, OUTPUT);

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);

  dht.begin();
  prefs.begin("wifi", false);

  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  if (ssid.length()) WiFi.begin(ssid.c_str(), pass.c_str());
}

/* ===================== LOOP ===================== */
void loop() {

  /* ---- BLE BUTTON ---- */
  bool btn = digitalRead(BTN_PAIR);
  if (lastBtn == HIGH && btn == LOW) startBLE();
  lastBtn = btn;

  /* ---- Blue LED ---- */
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (bleActive && !wifiConnected) {
    if (millis() - ledTimer > 300) {
      ledTimer = millis();
      ledBlink = !ledBlink;
      digitalWrite(LED_STATUS, ledBlink);
    }
  } else if (wifiConnected) digitalWrite(LED_STATUS, HIGH);
  else digitalWrite(LED_STATUS, LOW);

  /* ---- ADAPTATIVE READ ---- */
  unsigned long interval = (state == NORMAL) ? 4000 :
                           (state == PREVENTIVE) ? 1000 :
                           (state == ALERT) ? 300 : 150;
  if (millis() - lastSample < interval) return;
  float dt = (millis() - lastSample) / 1000.0;
  lastSample = millis();

  /* ---- DHT ---- */
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) { t = 25; h = 50; }

  uint8_t pins[N_SENS] = { MQ7_PIN, MQ4_PIN, MQ8_PIN, MQ3_PIN, MQ135_PIN };

  float VAI = 0;
  int quorum = 0;

  for (int i = 0; i < N_SENS; i++) {
    if (sensorIdle[i]) continue;
    ppm[i] = readGas(pins[i], t, h);
    baseline[i] = baseline[i] * 0.999 + ppm[i] * 0.001;

    float val = ppm[i] - baseline[i];
    dPPM[i] = (val - prevPPM[i]) / dt;
    prevPPM[i] = val;

    if (fabs(dPPM[i]) < 0.01 && ppm[i] < SAFE_PPM) idleCounter[i]++;
    else idleCounter[i] = 0;

    if (idleCounter[i] > 50) sensorIdle[i] = true;
    else sensorIdle[i] = false;

    VAI = max(VAI, ppm[i] + fabs(dPPM[i]) * 20);
    quorum++;
  }

  /* ---- WATCHDOG ---- */
  if (quorum < 2) state = FAILSAFE;
  else if (VAI < SAFE_PPM) state = NORMAL;
  else if (VAI < ALERT_PPM) state = PREVENTIVE;
  else if (VAI < CRITICAL_PPM) state = ALERT;
  else state = CRITICAL;

  /* ---- PWM ---- */
  float pwmReq = MIN_FAN + KP * (VAI - SAFE_PPM) + KD * fabs(dPPM[4]);
  pwmReq = constrain(pwmReq, MIN_FAN, MAX_FAN);

  if (millis() - lastPWM > 500) {
    lastPWM = millis();
    if (state == CRITICAL || state == FAILSAFE) {
      pwmFinal = 100;
      digitalWrite(RELAY_EMERG, HIGH);
    } else {
      pwmFinal = pwmReq;
      digitalWrite(RELAY_EMERG, LOW);
    }
  }

  systemFatigue += pwmFinal * dt * 0.01;
  if (systemFatigue > 1000) pwmFinal *= 0.9;

  driveTriac(TRIAC_CH1, pwmFinal);
  driveTriac(TRIAC_CH2, pwmFinal);

  setAirLED(VAI);

  /* ---- SELF TEST ---- */
  if (millis() - lastSelfTest > 21600000UL) {
    digitalWrite(RELAY_EMERG, HIGH);
    delay(100);
    digitalWrite(RELAY_EMERG, LOW);
    lastSelfTest = millis();
  }

  /* ---- SERIAL ---- */
  if (millis() - lastSerial > 1000) {
    lastSerial = millis();
    Serial.println("---- VentiLabs ----");
    Serial.print("Estado: "); Serial.println(state);
    Serial.print("VAI: "); Serial.println(VAI);
    Serial.print("PWM: "); Serial.println(pwmFinal);
  }
}
