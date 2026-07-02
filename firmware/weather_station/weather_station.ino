#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <math.h>

// =======================
// CONFIG
// =======================

// Wi-Fi
const char* WIFI_SSID = "your-wifi-ssid";
const char* WIFI_PASSWORD = "your-wifi-password";

// API
const char* WEBHOOK_URL = "https://your-host:port/weather";
const char* API_KEY = "your-api-key";

// NTP
const long UTC_OFFSET_SEC = 5 * 3600;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", UTC_OFFSET_SEC, 60000);

// BME280
Adafruit_BME280 bme;
const float STATION_ALTITUDE_M = 110.0f;

// Анемометр
const int PIN_WIND = A0;
const float VREF = 3.3f;
const int ADC_RES = 1023;
const float CAL_FACTOR = 1000.0f * 157.0f / 206.0f / 8.0f;

// Интервалы
const unsigned long SAMPLE_MS = 1000UL;
const unsigned long SEND_PERIOD_MS = 5UL * 60UL * 1000UL;
const unsigned long WIFI_RETRY_MS = 15000UL;
const unsigned long BME_RETRY_MS = 30000UL;
const unsigned long NTP_RETRY_MS = 60000UL;
const unsigned long FLUSH_PERIOD_MS = 15000UL;
const unsigned long HTTP_TIMEOUT_MS = 12000UL;

// Очередь
const char* QUEUE_FILE = "/queue.txt";
const char* QUEUE_TMP = "/queue.tmp";
const char* QUEUE_BAK = "/queue.bak";

// Не даём LittleFS забиться бесконечно.
// Если очередь превысит лимит, будут удаляться самые старые строки.
const size_t MAX_QUEUE_BYTES = 96UL * 1024UL;

// Сколько строк очереди пробуем отправить за один проход.
const uint8_t MAX_FLUSH_LINES_PER_PASS = 10;

// Если BME отвалился, но раньше были валидные значения:
// true  — отправляем последние валидные значения + bme_ok=0 + bme_stale=1.
// false — отправляем NaN + bme_ok=0.
const bool SEND_STALE_BME_VALUES = true;

// =======================
// STATE
// =======================

bool fsReady = false;

unsigned long lastSampleTime = 0;
unsigned long lastSendTime = 0;
unsigned long lastWiFiTry = 0;
unsigned long lastBmeTry = 0;
unsigned long lastNtpTry = 0;
unsigned long lastFlushTime = 0;

// Накопители ветра
float sumSpeed = 0.0f;
uint16_t sampleCount = 0;
float maxSpeed = 0.0f;

// BME state
bool bmePresent = false;
bool hasLastBme = false;
float lastTemp = NAN;
float lastHum = NAN;
float lastPres = NAN;
unsigned long lastBmeOkMs = 0;

// Time state
bool timeOk = false;
time_t lastEpoch = 0;
unsigned long lastEpochMs = 0;

// =======================
// UTILS
// =======================

String urlEncode(const char* str) {
  String encoded;
  encoded.reserve(strlen(str) * 3);

  char c;
  char buf[4];
  while ((c = *str++)) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

String formatFloatOrNaN(bool ok, float value, uint8_t decimals) {
  if (!ok || isnan(value)) {
    return "NaN";
  }
  return String(value, decimals);
}

bool validRange(float value, float minValue, float maxValue) {
  return !isnan(value) && value >= minValue && value <= maxValue;
}

void initI2C() {
  Wire.begin();
  Wire.setClock(50000);
#ifdef ESP8266
  Wire.setClockStretchLimit(150000);
#endif
}

// =======================
// WIFI
// =======================

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
#ifdef ESP8266
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiTry = millis();
  Serial.println("Wi-Fi start");
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (now - lastWiFiTry >= WIFI_RETRY_MS) {
    lastWiFiTry = now;
    Serial.println("Wi-Fi reconnect...");
    WiFi.reconnect();
  }
}

// =======================
// TIME
// =======================

void maintainTime() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (!timeOk || now - lastNtpTry >= NTP_RETRY_MS) {
    lastNtpTry = now;
    if (timeClient.update()) {
      lastEpoch = timeClient.getEpochTime();
      lastEpochMs = now;
      timeOk = true;
      Serial.println("NTP synced");
    }
  }
}

time_t getBestEpoch() {
  if (!timeOk) {
    return 0;
  }

  unsigned long deltaSec = (millis() - lastEpochMs) / 1000UL;
  return lastEpoch + deltaSec;
}

String makeTimestamp() {
  char timestamp[25];

  if (!timeOk) {
    snprintf(timestamp, sizeof(timestamp), "01-01-1970-00-00-00");
    return urlEncode(timestamp);
  }

  time_t rawTime = getBestEpoch();
  struct tm* t = gmtime(&rawTime);
  if (t == nullptr) {
    snprintf(timestamp, sizeof(timestamp), "01-01-1970-00-00-00");
    return urlEncode(timestamp);
  }

  snprintf(
    timestamp,
    sizeof(timestamp),
    "%02d-%02d-%04d-%02d-%02d-%02d",
    t->tm_mday,
    t->tm_mon + 1,
    t->tm_year + 1900,
    t->tm_hour,
    t->tm_min,
    t->tm_sec
  );
  return urlEncode(timestamp);
}

// =======================
// BME280
// =======================

bool initBME() {
  lastBmeTry = millis();
  initI2C();

  const uint8_t addresses[] = {0x76, 0x77};
  for (uint8_t i = 0; i < 2; i++) {
    if (bme.begin(addresses[i])) {
      bme.setSampling(
        Adafruit_BME280::MODE_FORCED,
        Adafruit_BME280::SAMPLING_X2,
        Adafruit_BME280::SAMPLING_X2,
        Adafruit_BME280::SAMPLING_X2,
        Adafruit_BME280::FILTER_OFF,
        Adafruit_BME280::STANDBY_MS_1000
      );
      bmePresent = true;
      Serial.print("BME280 online at 0x");
      Serial.println(addresses[i], HEX);
      return true;
    }
    yield();
  }

  bmePresent = false;
  Serial.println("BME280 offline");
  return false;
}

void maintainBME() {
  if (bmePresent) {
    return;
  }

  unsigned long now = millis();
  if (now - lastBmeTry >= BME_RETRY_MS) {
    initBME();
  }
}

bool readBME(float& temp, float& hum, float& presMbar) {
  maintainBME();
  if (!bmePresent) {
    return false;
  }

  bool measurementOk = bme.takeForcedMeasurement();
  if (!measurementOk) {
    Serial.println("BME280 measurement failed");
    bmePresent = false;
    lastBmeTry = millis();
    initI2C();
    return false;
  }

  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float pressurePa = bme.readPressure();
  float p = bme.seaLevelForAltitude(STATION_ALTITUDE_M, pressurePa) / 100.0f;

  bool valuesOk = validRange(t, -80.0f, 85.0f)
    && validRange(h, 0.0f, 100.0f)
    && validRange(p, 800.0f, 1100.0f);

  if (!valuesOk) {
    Serial.println("BME280 invalid values");
    bmePresent = false;
    lastBmeTry = millis();
    initI2C();
    return false;
  }

  temp = t;
  hum = h;
  presMbar = p;
  lastTemp = t;
  lastHum = h;
  lastPres = p;
  hasLastBme = true;
  lastBmeOkMs = millis();
  return true;
}

// =======================
// QUEUE
// =======================

void recoverQueue() {
  if (!fsReady) {
    return;
  }

  if (!LittleFS.exists(QUEUE_FILE) && LittleFS.exists(QUEUE_BAK)) {
    LittleFS.rename(QUEUE_BAK, QUEUE_FILE);
    Serial.println("Queue recovered from backup");
  }

  if (LittleFS.exists(QUEUE_TMP)) {
    LittleFS.remove(QUEUE_TMP);
  }
}

size_t fileSize(const char* path) {
  if (!fsReady || !LittleFS.exists(path)) {
    return 0;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    return 0;
  }

  size_t size = f.size();
  f.close();
  return size;
}

bool replaceQueueWithTmp() {
  if (!fsReady) {
    return false;
  }

  LittleFS.remove(QUEUE_BAK);
  if (LittleFS.exists(QUEUE_FILE)) {
    if (!LittleFS.rename(QUEUE_FILE, QUEUE_BAK)) {
      Serial.println("Queue backup rename failed");
      return false;
    }
  }

  if (!LittleFS.rename(QUEUE_TMP, QUEUE_FILE)) {
    Serial.println("Queue tmp rename failed");
    if (!LittleFS.exists(QUEUE_FILE) && LittleFS.exists(QUEUE_BAK)) {
      LittleFS.rename(QUEUE_BAK, QUEUE_FILE);
    }
    return false;
  }

  LittleFS.remove(QUEUE_BAK);
  return true;
}

bool trimQueueTo(size_t maxBytes) {
  if (!fsReady || !LittleFS.exists(QUEUE_FILE)) {
    return true;
  }

  size_t currentSize = fileSize(QUEUE_FILE);
  if (currentSize <= maxBytes) {
    return true;
  }

  size_t bytesToSkip = currentSize - maxBytes;
  size_t skipped = 0;

  File in = LittleFS.open(QUEUE_FILE, "r");
  if (!in) {
    return false;
  }

  File out = LittleFS.open(QUEUE_TMP, "w");
  if (!out) {
    in.close();
    return false;
  }

  while (in.available()) {
    String line = in.readStringUntil('\n');
    size_t lineBytes = line.length() + 1;

    if (skipped < bytesToSkip) {
      skipped += lineBytes;
      yield();
      continue;
    }

    line.trim();
    if (line.length() > 0) {
      out.println(line);
    }
    yield();
  }

  in.close();
  out.close();

  bool ok = replaceQueueWithTmp();
  if (ok) {
    Serial.println("Queue trimmed: oldest records removed");
  }
  return ok;
}

bool ensureQueueSpace(size_t payloadBytes) {
  if (!fsReady) {
    return false;
  }

  if (payloadBytes >= MAX_QUEUE_BYTES) {
    Serial.println("Payload is bigger than queue limit");
    return false;
  }

  size_t currentSize = fileSize(QUEUE_FILE);
  if (currentSize + payloadBytes <= MAX_QUEUE_BYTES) {
    return true;
  }

  return trimQueueTo(MAX_QUEUE_BYTES - payloadBytes);
}

bool enqueuePayload(const String& payload) {
  if (!fsReady) {
    Serial.println("LittleFS unavailable, cannot enqueue");
    return false;
  }

  size_t payloadBytes = payload.length() + 2;
  if (!ensureQueueSpace(payloadBytes)) {
    Serial.println("No queue space");
    return false;
  }

  File f = LittleFS.open(QUEUE_FILE, "a");
  if (!f) {
    Serial.println("Queue open failed");
    return false;
  }

  size_t written = f.println(payload);
  f.flush();
  f.close();

  if (written == 0) {
    Serial.println("Queue write failed");
    return false;
  }

  Serial.println("Payload enqueued");
  return true;
}

// =======================
// HTTP
// =======================

bool sendPayload(const String& payload) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  bool beginOk = http.begin(client, WEBHOOK_URL);
  if (!beginOk) {
    Serial.println("HTTP begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(payload);
  http.end();

  Serial.printf("HTTP code: %d\n", code);
  if (code <= 0) {
    Serial.println("HTTP transport error");
    return false;
  }

  return code >= 200 && code < 300;
}

void flushQueueLimited(uint8_t maxLines) {
  if (!fsReady) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!LittleFS.exists(QUEUE_FILE)) {
    return;
  }

  File in = LittleFS.open(QUEUE_FILE, "r");
  if (!in) {
    return;
  }

  File out = LittleFS.open(QUEUE_TMP, "w");
  if (!out) {
    in.close();
    return;
  }

  uint8_t attempted = 0;
  uint8_t sent = 0;
  bool stopSending = false;

  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      yield();
      continue;
    }

    if (!stopSending && attempted < maxLines) {
      attempted++;
      if (sendPayload(line)) {
        sent++;
      } else {
        out.println(line);
        stopSending = true;
      }
    } else {
      out.println(line);
    }

    yield();
  }

  in.close();
  out.close();
  replaceQueueWithTmp();
  Serial.printf("Queue flush: attempted=%u, sent=%u\n", attempted, sent);
}

// =======================
// WEATHER PAYLOAD
// =======================

void sampleWind() {
  int raw = analogRead(PIN_WIND);
  if (raw < 0) {
    return;
  }

  float voltage = (float)raw / (float)ADC_RES * VREF;
  float speed = voltage * CAL_FACTOR;
  if (isnan(speed) || speed < 0.0f) {
    return;
  }

  sumSpeed += speed;
  sampleCount++;
  if (speed > maxSpeed) {
    maxSpeed = speed;
  }
}

String buildPayload() {
  float avgSpeed = sampleCount > 0 ? sumSpeed / sampleCount : 0.0f;
  float peakSpeed = maxSpeed;

  sumSpeed = 0.0f;
  sampleCount = 0;
  maxSpeed = 0.0f;

  float temp = NAN;
  float hum = NAN;
  float pres = NAN;

  bool bmeOkNow = readBME(temp, hum, pres);
  bool bmeStale = false;

  if (!bmeOkNow && SEND_STALE_BME_VALUES && hasLastBme) {
    temp = lastTemp;
    hum = lastHum;
    pres = lastPres;
    bmeStale = true;
  }

  bool bmeValuesUsable = bmeOkNow || bmeStale;
  unsigned long bmeAgeSec = 0;
  if (hasLastBme) {
    bmeAgeSec = (millis() - lastBmeOkMs) / 1000UL;
  }

  String payload;
  payload.reserve(300);

  payload += "temp=";
  payload += formatFloatOrNaN(bmeValuesUsable, temp, 2);
  payload += "&rh=";
  payload += formatFloatOrNaN(bmeValuesUsable, hum, 2);
  payload += "&mbar=";
  payload += formatFloatOrNaN(bmeValuesUsable, pres, 2);
  payload += "&wind=";
  payload += String(avgSpeed, 2);
  payload += "&gust=";
  payload += String(peakSpeed, 2);
  payload += "&time=";
  payload += makeTimestamp();
  payload += "&time_ok=";
  payload += (timeOk ? "1" : "0");
  payload += "&uptime_ms=";
  payload += String(millis());
  payload += "&bme_ok=";
  payload += (bmeOkNow ? "1" : "0");
  payload += "&bme_stale=";
  payload += (bmeStale ? "1" : "0");
  payload += "&bme_age_sec=";
  payload += (hasLastBme ? String(bmeAgeSec) : String(-1));
  payload += "&wifi_rssi=";
  payload += (WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String(-999));
  payload += "&key=";
  payload += API_KEY;

  return payload;
}

// =======================
// SETUP / LOOP
// =======================

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("Weather station boot");

  pinMode(PIN_WIND, INPUT);
  initI2C();

  fsReady = LittleFS.begin();
  if (!fsReady) {
    Serial.println("LittleFS mount failed. Queue disabled.");
  } else {
    recoverQueue();
    Serial.println("LittleFS mounted");
  }

  setupWiFi();
  timeClient.begin();
  initBME();

  unsigned long now = millis();
  lastSampleTime = now;
  lastSendTime = now;
  lastFlushTime = now;
}

void loop() {
  unsigned long now = millis();

  maintainWiFi();
  maintainTime();
  maintainBME();

  if (now - lastSampleTime >= SAMPLE_MS) {
    lastSampleTime = now;
    sampleWind();
  }

  if (now - lastSendTime >= SEND_PERIOD_MS) {
    lastSendTime = now;
    String payload = buildPayload();

    bool queued = enqueuePayload(payload);
    if (!queued && WiFi.status() == WL_CONNECTED) {
      sendPayload(payload);
    }
  }

  if (now - lastFlushTime >= FLUSH_PERIOD_MS) {
    lastFlushTime = now;
    flushQueueLimited(MAX_FLUSH_LINES_PER_PASS);
  }

  yield();
}
