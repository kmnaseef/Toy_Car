#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/i2s.h>
#include "engine_pcm.h"

// ===================== Motor Pins =====================
const int enaPin = 4;
const int motorAPin1 = 13;
const int motorAPin2 = 14;

const int motorBPin1 = 26;
const int motorBPin2 = 27;
const int enbPin = 25;

// ===================== I2S Audio Pins =====================
#define I2S_DOUT 19
#define I2S_BCLK 5
#define I2S_LRC  18

// ===================== WiFi / OTA =====================
const char* apSsid = "ESP32-RC-Car";
const char* apPassword = "12345678";

WebServer server(80);
HTTPUpdateServer httpUpdater;

bool apActive = false;
unsigned long apStartTime = 0;
const unsigned long AP_TIMEOUT = 15000;

// ===================== ESP-NOW =====================
const int ESPNOW_CHANNEL = 1;

// ===================== Control Data =====================
int throttle = 0;
int throttleH = 0;
int steer = 0;
int steerV = 0;
int btn4State = 0;
int btn5State = 0;

volatile bool isHonking = false;

// ===================== Tuning =====================
const int steerPWM = 200;     // Increased steering power
const int maxDrivePWM = 255;
const int minDrivePWM = 100;

// ===================== Safety =====================
unsigned long lastCommandTime = 0;
bool isSafetyStopped = false;
const unsigned long FAILSAFE_TIMEOUT = 250; // 25% less than 1000 ms

// ===================== Audio Task =====================
void audioTask(void *pvParameters) {
  int engineIndex = 0;
  bool engineFinished = false;
  int16_t samples[128];

  const float sampleRate = 44100.0f;
  const float freq1 = 440.0f;
  const float freq2 = 540.0f;

  const float phaseInc1 = freq1 / sampleRate;
  const float phaseInc2 = freq2 / sampleRate;

  float phase1 = 0.0f;
  float phase2 = 0.0f;
  float fadeMultiplier = 0.0f;

  const float fadeDurationSecs = 0.15f;
  const float fadeDecrement = 1.0f / (sampleRate * fadeDurationSecs);

  while (1) {
    for (int i = 0; i < 128; i++) {
      if (!engineFinished) {
        int16_t sample = engine_raw[engineIndex] | (engine_raw[engineIndex + 1] << 8);
        samples[i] = sample;
        engineIndex += 2;

        if (engineIndex >= engine_raw_len - 2) {
          engineFinished = true;
        }
      } else {
        if (isHonking) {
          fadeMultiplier = 1.0f;
        } else if (fadeMultiplier > 0.0f) {
          fadeMultiplier -= fadeDecrement;
          if (fadeMultiplier < 0.0f) fadeMultiplier = 0.0f;
        }

        if (fadeMultiplier > 0.0f) {
          float wave1 = 2.0f * phase1 - 1.0f;
          float wave2 = 2.0f * phase2 - 1.0f;

          int16_t sample = (int16_t)((10000.0f * wave1 + 10000.0f * wave2) * fadeMultiplier);
          samples[i] = sample;

          phase1 += phaseInc1;
          if (phase1 >= 1.0f) phase1 -= 1.0f;

          phase2 += phaseInc2;
          if (phase2 >= 1.0f) phase2 -= 1.0f;
        } else {
          samples[i] = 0;
          phase1 = 0.0f;
          phase2 = 0.0f;
        }
      }
    }

    size_t bytes_written;
    i2s_write(I2S_NUM_0, samples, sizeof(samples), &bytes_written, portMAX_DELAY);
  }
}

// ===================== Motor Helpers =====================
int levelToPwm(int value) {
  int level = abs(value);

  if (level <= 0) return 0;
  if (level > 4) level = 4;

  return map(level, 1, 4, minDrivePWM, maxDrivePWM);
}

void stopAll() {
  digitalWrite(motorAPin1, LOW);
  digitalWrite(motorAPin2, LOW);
  analogWrite(enaPin, 0);

  digitalWrite(motorBPin1, LOW);
  digitalWrite(motorBPin2, LOW);
  analogWrite(enbPin, 0);

  isHonking = false;
  isSafetyStopped = true;
}

void applyDrive(int value) {
  int pwm = levelToPwm(value);

  if (value > 0) {
    digitalWrite(motorAPin1, HIGH);
    digitalWrite(motorAPin2, LOW);
    analogWrite(enaPin, pwm);
  } else if (value < 0) {
    digitalWrite(motorAPin1, LOW);
    digitalWrite(motorAPin2, HIGH);
    analogWrite(enaPin, pwm);
  } else {
    digitalWrite(motorAPin1, LOW);
    digitalWrite(motorAPin2, LOW);
    analogWrite(enaPin, 0);
  }
}

void applySteer(int value) {
  // Left/right reversed here
  if (value > 0) {
    digitalWrite(motorBPin1, LOW);
    digitalWrite(motorBPin2, HIGH);
    analogWrite(enbPin, steerPWM);
  } else if (value < 0) {
    digitalWrite(motorBPin1, HIGH);
    digitalWrite(motorBPin2, LOW);
    analogWrite(enbPin, steerPWM);
  } else {
    digitalWrite(motorBPin1, LOW);
    digitalWrite(motorBPin2, LOW);
    analogWrite(enbPin, 0);
  }
}

void applyCommand() {
  applyDrive(throttle);
  applySteer(steer);

  // CHANGED: Horn is now triggered by btn5State instead of btn4State
  isHonking = btn5State == 1;

  lastCommandTime = millis();
  isSafetyStopped = false;
}

// ===================== ESP-NOW Receive =====================
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  char csvData[32];

  if (len >= sizeof(csvData)) {
    len = sizeof(csvData) - 1;
  }

  memcpy(csvData, incomingData, len);
  csvData[len] = '\0';

  int parsed = sscanf(
    csvData,
    "%d,%d,%d,%d,%d,%d",
    &throttle,
    &throttleH,
    &steer,
    &steerV,
    &btn4State,
    &btn5State
  );

  if (parsed == 6) {
    Serial.print("RX: ");
    Serial.println(csvData);
    applyCommand();
  } else {
    Serial.print("Parse error: ");
    Serial.println(csvData);
  }
}

// ===================== OTA AP =====================
void handleRoot() {
  String html = "<html><body>";
  html += "<h2>ESP32 RC Car</h2>";

  html += "<p><b>STA MAC for ESP-NOW:</b> ";
  html += WiFi.macAddress();
  html += "</p>";

  html += "<p><b>AP MAC:</b> ";
  html += WiFi.softAPmacAddress();
  html += "</p>";

  html += "<p><b>ESP-NOW Channel:</b> ";
  html += String(ESPNOW_CHANNEL);
  html += "</p>";

  html += "<p><b>Connected Devices:</b> ";
  html += String(WiFi.softAPgetStationNum());
  html += "</p>";

  html += "<p><a href='/update'>OTA Update</a></p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void startOtaAp() {
  WiFi.softAP(apSsid, apPassword, ESPNOW_CHANNEL);

  server.on("/", handleRoot);
  httpUpdater.setup(&server);
  server.begin();

  apActive = true;
  apStartTime = millis();

  Serial.println("OTA AP started");
  Serial.print("AP SSID: ");
  Serial.println(apSsid);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Open: http://192.168.4.1/");
  Serial.println("OTA:  http://192.168.4.1/update");
}

void stopOtaAp() {
  if (!apActive) return;

  server.stop();
  WiFi.softAPdisconnect(true);

  apActive = false;

  Serial.println("OTA AP stopped");
}

void handleOtaApTimeout() {
  if (!apActive) return;

  int connectedDevices = WiFi.softAPgetStationNum();

  if (millis() - apStartTime >= AP_TIMEOUT && connectedDevices == 0) {
    stopOtaAp();
  }
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("--- ESP32 ESP-NOW RC Car Receiver Booting ---");

  pinMode(enaPin, OUTPUT);
  pinMode(motorAPin1, OUTPUT);
  pinMode(motorAPin2, OUTPUT);

  pinMode(enbPin, OUTPUT);
  pinMode(motorBPin1, OUTPUT);
  pinMode(motorBPin2, OUTPUT);

  stopAll();

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 4096, NULL, 3, NULL, 1);

  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("ESP32 STA MAC for transmitter: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESP32 AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());
  Serial.print("ESP-NOW channel: ");
  Serial.println(ESPNOW_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: esp_now_init() failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW receiver ready");

  startOtaAp();

  lastCommandTime = millis();
}

// ===================== Loop =====================
void loop() {
  if (apActive) {
    server.handleClient();
    handleOtaApTimeout();
  }

  if (!isSafetyStopped && millis() - lastCommandTime > FAILSAFE_TIMEOUT) {
    Serial.println("FAILSAFE STOP");
    stopAll();
  }

  delay(10);
}