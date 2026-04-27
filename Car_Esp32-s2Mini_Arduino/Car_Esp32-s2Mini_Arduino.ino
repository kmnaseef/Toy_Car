#include <WiFi.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include <HTTPUpdateServer.h> 
#include "engine_pcm.h"       // Your Engine Start sound array

// --- Pin Configuration (ESP32-WROVER Compatible) ---
// Avoided pins: 6-11 (Flash), 16-17 (PSRAM), 34-39 (Input Only)
const int enaPin = 4;       
const int motorAPin1 = 13;  
const int motorAPin2 = 14;  
const int motorBPin1 = 26;   
const int motorBPin2 = 27;   
const int enbPin = 25;      

// --- I2S Audio Configuration (Safe Output Pins) ---
#define I2S_DOUT 19
#define I2S_BCLK 5
#define I2S_LRC  18

// Global variables
volatile bool isHonking = false;

// --- Safety Watchdog Variables ---
unsigned long lastCommandTime = 0;
bool isSafetyStopped = false;

// --- Steering Pulse Variables ---
enum SteerState { STEER_IDLE, STEER_PULSE_ON, STEER_PULSE_OFF };
SteerState currentSteerState = STEER_IDLE;
String currentSteerDir = "stop";
unsigned long steerTimer = 0;
int steerActiveTime = 5;       // Starts at 5ms
const int steerRestTime = 100; // Always rests for 100ms
const int steerPWM = 255;      // Give it max torque during the pulse

WebServer server(80);
HTTPUpdateServer httpUpdater;  

// --- HTML/JS Web Page ---
const char* htmlPage = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<style>
  * { -webkit-tap-highlight-color: transparent; -webkit-user-select: none; user-select: none; outline: none; box-sizing: border-box; }
  body { font-family: sans-serif; margin: 0; display: flex; height: 100vh; overflow: hidden; background: #121212; color: white; touch-action: none; }
  
  .pane { flex: 1; display: flex; flex-direction: column; align-items: center; justify-content: center; border-right: 1px solid #333; position: relative; padding-top: 50px;}
  .pane:last-child { border-right: none; }
  
  h3 { color: #666; position: absolute; top: 20px; letter-spacing: 2px; text-transform: uppercase; margin: 0; pointer-events: none;}
  
  .swipe-pad {
    width: 80%;
    height: 45%;
    background: #222;
    border-radius: 20px;
    border: 2px dashed #444;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #555;
    font-weight: bold;
    text-align: center;
    padding: 20px;
    margin-bottom: 20px;
    cursor: pointer;
  }
  
  #drivePad {
    height: 85%;
    margin-bottom: 0;
  }
  
  .active-swipe { background: #333; border-color: #2196F3; color: #2196F3; }

  .horn-btn {
    width: 80%;
    height: 80px;
    background: #B71C1C;
    color: white;
    border: none;
    border-radius: 20px;
    font-size: 20px;
    font-weight: bold;
    letter-spacing: 2px;
    box-shadow: 0 5px 15px rgba(183, 28, 28, 0.4);
  }
  .horn-btn:active { background: #D32F2F; transform: scale(0.95); box-shadow: 0 2px 5px rgba(183, 28, 28, 0.4); }

</style></head>
<body>
  <div class="pane">
    <h3>Drive</h3>
    <div class="swipe-pad" id="drivePad">SWIPE<br>UP / DOWN</div>
  </div>

  <div class="pane">
    <h3>Steering</h3>
    <div class="swipe-pad" id="steerPad">PULSE<br>STEER</div>
    <button class="horn-btn" id="hornBtn">HORN</button>
  </div>

  <script>
    // --- HEARTBEAT LOGIC ---
    setInterval(() => { fetch('/ping').catch(e=>{}); }, 300);

    // --- SWIPE TO DRIVE LOGIC ---
    const drivePad = document.getElementById('drivePad');
    let startY = 0;
    let isDriving = false;
    let lastDrivePWM = 0;
    let lastSendTimeDrive = 0;
    const maxDriveSwipeDistance = 150; 

    function updateDrive(dy) {
      let val = Math.floor((dy / maxDriveSwipeDistance) * 100);
      if (val > 100) val = 100;
      if (val < -100) val = -100;

      let pwm = 0;
      
      if (val > 20) {
        pwm = Math.floor(102 + ((val - 20) / 80) * 153);
      } else if (val < -20) {
        pwm = Math.floor(-102 + ((val + 20) / 80) * 153);
      } else {
        pwm = 0; 
      }

      let now = Date.now();
      if (pwm !== lastDrivePWM && (now - lastSendTimeDrive > 40 || pwm === 0)) {
        fetch(`/drive?v=${pwm}`).catch(e => {});
        lastDrivePWM = pwm;
        lastSendTimeDrive = now;
      }
    }

    drivePad.addEventListener('pointerdown', e => { 
      isDriving = true; 
      startY = e.clientY; 
      drivePad.classList.add('active-swipe');
    });

    drivePad.addEventListener('pointermove', e => {
      if (!isDriving) return;
      let dy = startY - e.clientY; 
      updateDrive(dy);
    });

    const stopDrive = () => {
      if (!isDriving) return;
      isDriving = false;
      drivePad.classList.remove('active-swipe');
      updateDrive(0); 
    };

    drivePad.addEventListener('pointerup', stopDrive);
    drivePad.addEventListener('pointerleave', stopDrive);
    drivePad.addEventListener('pointercancel', stopDrive);

    // --- PULSE STEERING LOGIC ---
    const steerPad = document.getElementById('steerPad');
    let startX = 0;
    let isSwiping = false;
    let currentSteer = 'stop';

    function updateSteer(dx) {
      let dir = 'stop';
      
      if (dx < -15) dir = 'left';
      else if (dx > 15) dir = 'right';

      if (dir !== currentSteer) {
        fetch(`/steer?dir=${dir}`).catch(e=>{});
        currentSteer = dir;
      }
    }

    steerPad.addEventListener('pointerdown', e => { 
      isSwiping = true; 
      startX = e.clientX; 
      steerPad.classList.add('active-swipe');
    });

    steerPad.addEventListener('pointermove', e => {
      if (!isSwiping) return;
      let dx = e.clientX - startX;
      updateSteer(dx);
    });

    const stopSteer = () => {
      if (!isSwiping) return;
      isSwiping = false;
      steerPad.classList.remove('active-swipe');
      if (currentSteer !== 'stop') {
        fetch(`/steer?dir=stop`).catch(e=>{});
        currentSteer = 'stop';
      }
    };

    steerPad.addEventListener('pointerup', stopSteer);
    steerPad.addEventListener('pointerleave', stopSteer);
    steerPad.addEventListener('pointercancel', stopSteer);

    // --- HORN LOGIC ---
    const hornBtn = document.getElementById('hornBtn');
    hornBtn.addEventListener('pointerdown', () => { fetch('/horn?state=on').catch(e=>{}); });
    const stopHorn = () => { fetch('/horn?state=off').catch(e=>{}); };
    hornBtn.addEventListener('pointerup', stopHorn);
    hornBtn.addEventListener('pointerleave', stopHorn);
    hornBtn.addEventListener('pointercancel', stopHorn);

    // --- TAB VISIBILITY SAFETY ---
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) {
        stopDrive();
        stopSteer();
        stopHorn();
      }
    });

    document.addEventListener('contextmenu', e => e.preventDefault());
  </script>
</body></html>
)rawliteral";

// --- Audio Task (Handles Engine Start & Dual-Tone Sawtooth Horn with Fade Envelope) ---
void audioTask(void *pvParameters) {
  int engineIndex = 0;
  bool engineFinished = false; 
  int16_t samples[128]; 

  // Audio settings
  const float sampleRate = 44100.0f;
  const float freq1 = 440.0f; // Updated frequency 1
  const float freq2 = 540.0f; // Updated frequency 2
  
  // Phase increments normalized between 0.0 and 1.0 per sample
  const float phaseInc1 = freq1 / sampleRate;
  const float phaseInc2 = freq2 / sampleRate;
  
  float phase1 = 0.0f;
  float phase2 = 0.0f;

  // Fade control variables
  float fadeMultiplier = 0.0f;
  
  // 150ms fade-out duration = 0.15 seconds (Takes ~6,615 samples to fade to zero)
  const float fadeDurationSecs = 0.15f; 
  const float fadeDecrement = 1.0f / (sampleRate * fadeDurationSecs); 
  
  while(1) {
    for (int i = 0; i < 128; i++) {
      
      // 1. Play the engine start sound exactly once
      if (!engineFinished) {
        int16_t sample = engine_raw[engineIndex] | (engine_raw[engineIndex + 1] << 8);
        samples[i] = sample;
        engineIndex += 2;
        
        if (engineIndex >= engine_raw_len - 2) {
          engineFinished = true;
        }
      } 
      // 2. Play the mathematically generated dual-tone sawtooth horn
      else {
        // Handle volume tracking and fade envelope
        if (isHonking) {
          // Instantly snap to max volume
          fadeMultiplier = 1.0f; 
        } else if (fadeMultiplier > 0.0f) {
          // Linearly fade out to zero volume
          fadeMultiplier -= fadeDecrement; 
          if (fadeMultiplier < 0.0f) fadeMultiplier = 0.0f; // Clamp to zero
        }

        // If the horn is active, or fading out, generate the wave
        if (fadeMultiplier > 0.0f) {
          // A sawtooth wave equation: value goes from -1.0 to 1.0 based on phase
          float wave1 = 2.0f * phase1 - 1.0f;
          float wave2 = 2.0f * phase2 - 1.0f;

          // Multiply by amplitude (10000 each) and the current fade envelope state
          int16_t sample = (int16_t)((10000.0f * wave1 + 10000.0f * wave2) * fadeMultiplier);
          samples[i] = sample;

          // Advance phases and wrap around at 1.0
          phase1 += phaseInc1;
          if (phase1 >= 1.0f) phase1 -= 1.0f;

          phase2 += phaseInc2;
          if (phase2 >= 1.0f) phase2 -= 1.0f;
        } 
        // 3. Output pure silence when not honking and fade has finished
        else {
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

// Helper to update the watchdog timer
void resetWatchdog() {
  lastCommandTime = millis();
  isSafetyStopped = false;
}

void setup() {
  pinMode(enaPin, OUTPUT); pinMode(motorAPin1, OUTPUT); pinMode(motorAPin2, OUTPUT);
  pinMode(enbPin, OUTPUT); pinMode(motorBPin1, OUTPUT); pinMode(motorBPin2, OUTPUT);
  
  digitalWrite(motorAPin1, LOW); digitalWrite(motorAPin2, LOW);
  digitalWrite(motorBPin1, LOW); digitalWrite(motorBPin2, LOW);
  analogWrite(enaPin, 0); analogWrite(enbPin, 0);

  // --- Initialize I2S ---
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
  i2s_pin_config_t pin_config = { .bck_io_num = I2S_BCLK, .ws_io_num = I2S_LRC, .data_out_num = I2S_DOUT, .data_in_num = I2S_PIN_NO_CHANGE };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 4096, NULL, 3, NULL, 1);

  WiFi.softAP("ESP32-RC-Car", "12345678");

  server.on("/", []() { server.send(200, "text/html", htmlPage); });

  server.on("/ping", []() {
    resetWatchdog();
    server.send(200);
  });

  server.on("/horn", []() {
    resetWatchdog();
    if (server.hasArg("state")) { isHonking = (server.arg("state") == "on"); }
    server.send(200);
  });

  server.on("/drive", []() { 
    resetWatchdog();
    if (server.hasArg("v")) {
      int v = server.arg("v").toInt();
      if (v > 0) { 
        digitalWrite(motorAPin1, HIGH); digitalWrite(motorAPin2, LOW); analogWrite(enaPin, v);
      } else if (v < 0) { 
        digitalWrite(motorAPin1, LOW); digitalWrite(motorAPin2, HIGH); analogWrite(enaPin, abs(v)); 
      } else { 
        digitalWrite(motorAPin1, LOW); digitalWrite(motorAPin2, LOW); analogWrite(enaPin, 0);
      }
    }
    server.send(200); 
  });

  // --- PULSE STEERING ENDPOINT ---
  server.on("/steer", []() { 
    resetWatchdog();
    if (server.hasArg("dir")) {
      String dir = server.arg("dir");
      
      if (dir != currentSteerDir) {
        currentSteerDir = dir;
        
        if (dir == "stop") {
          currentSteerState = STEER_IDLE;
          digitalWrite(motorBPin1, LOW); digitalWrite(motorBPin2, LOW); analogWrite(enbPin, 0);
        } else {
          currentSteerState = STEER_PULSE_ON;
          steerActiveTime = 5; 
          steerTimer = millis();
          
          if (dir == "left") {
            digitalWrite(motorBPin1, HIGH); digitalWrite(motorBPin2, LOW); analogWrite(enbPin, steerPWM);
          } else if (dir == "right") {
            digitalWrite(motorBPin1, LOW); digitalWrite(motorBPin2, HIGH); analogWrite(enbPin, steerPWM);
          }
        }
      }
    }
    server.send(200); 
  });

  // --- ATTACH OTA UPDATER TO SERVER ---
  httpUpdater.setup(&server);

  server.begin();
  lastCommandTime = millis();
}

void loop() {
  server.handleClient();
  unsigned long currentMillis = millis();

  // --- THE PULSE STEERING STATE MACHINE ---
  if (currentSteerState == STEER_PULSE_ON) {
    if (currentMillis - steerTimer >= steerActiveTime) {
      currentSteerState = STEER_PULSE_OFF; 
      steerTimer = currentMillis;
      digitalWrite(motorBPin1, LOW); digitalWrite(motorBPin2, LOW); analogWrite(enbPin, 0); 
    }
  } 
  else if (currentSteerState == STEER_PULSE_OFF) {
    if (currentMillis - steerTimer >= steerRestTime) {
      if (currentSteerDir != "stop") {
        steerActiveTime += 5; 
        currentSteerState = STEER_PULSE_ON; 
        steerTimer = currentMillis;
        
        if (currentSteerDir == "left") {
          digitalWrite(motorBPin1, HIGH); digitalWrite(motorBPin2, LOW); analogWrite(enbPin, steerPWM);
        } else if (currentSteerDir == "right") {
          digitalWrite(motorBPin1, LOW); digitalWrite(motorBPin2, HIGH); analogWrite(enbPin, steerPWM);
        }
      } else {
        currentSteerState = STEER_IDLE; 
      }
    }
  }

  // --- SAFETY WATCHDOG TIMER ---
  if (!isSafetyStopped && (currentMillis - lastCommandTime > 1000)) {
    digitalWrite(motorAPin1, LOW); digitalWrite(motorAPin2, LOW); analogWrite(enaPin, 0);
    digitalWrite(motorBPin1, LOW); digitalWrite(motorBPin2, LOW); analogWrite(enbPin, 0);
    
    isHonking = false;
    currentSteerDir = "stop";
    currentSteerState = STEER_IDLE;
    
    isSafetyStopped = true;
  }
}