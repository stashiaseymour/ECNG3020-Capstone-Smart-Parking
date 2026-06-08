#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_VL53L0X.h>
#include <DFRobot_BMM150.h>

// ===================== PINS =====================
#define STACK_RED     8
#define STACK_YELLOW  10
#define STACK_GREEN   12
#define STACK_BUZZER  14

#define ON  HIGH
#define OFF LOW

// ===================== OLED =====================
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// ===================== TOF =====================
Adafruit_VL53L0X lox;
#define XSHUT_PIN 4

// ===================== MAGNETIC =====================
DFRobot_BMM150_I2C mag(&Wire, 0x13);

float magBaseline       = 0.0;
float magneticThreshold = 8.0;   // Z axis only
unsigned long magConfirmStart = 0;
bool magConfirmed = false;

// ===================== ESP-NOW =====================
#define ESPNOW_CHANNEL 6
uint8_t GATEWAY_MAC[] = {0x10, 0xB4, 0x1D, 0xEB, 0x10, 0x18};

// ===================== MESSAGES =====================
typedef struct __attribute__((packed)) {
  char node_id[4];
  char sensor_status[10];
  float distance_cm;
  uint32_t timestamp;
} NodeMessage;

typedef struct __attribute__((packed)) {
  char node_id[8];
  char decision[16];
} DecisionMessage;

typedef struct __attribute__((packed)) {
  char node_id[4];
} AckMessage;

// ===================== OVERRIDE STATES =====================
enum OverrideState {
  OVERRIDE_NONE,
  OVERRIDE_RESERVED,
  OVERRIDE_VIOLATION,
  OVERRIDE_MAINTENANCE
};

// ===================== STATE =====================
OverrideState overrideState = OVERRIDE_NONE;
bool occupied        = false;
bool lastOccupied    = false;
bool bootAnnounced   = false;
float distanceCM     = 999.0;

bool awaitingAck     = false;
unsigned long lastSendTime = 0;
uint8_t retryCount   = 0;

unsigned long lastBlink     = 0;
bool blinkState             = false;
unsigned long lastHeartbeat = 0;

// TOF shoots horizontally across a ~3ft space from ~3ft height
// Car body at that height detected within 75cm, clear past 85cm
#define OCCUPIED_CM        75.0
#define FREE_CM            85.0
#define BLINK_INTERVAL     400
#define ACK_TIMEOUT_MS     1200
#define HEARTBEAT_INTERVAL 30000

// ===================== STACKLIGHT =====================
void allOff() {
  digitalWrite(STACK_RED,    OFF);
  digitalWrite(STACK_YELLOW, OFF);
  digitalWrite(STACK_GREEN,  OFF);
  digitalWrite(STACK_BUZZER, OFF);
}

void updateStacklight(unsigned long now) {
  allOff();

  if (overrideState == OVERRIDE_MAINTENANCE) {
    return;
  }

  if (overrideState == OVERRIDE_VIOLATION) {
    digitalWrite(STACK_BUZZER, ON);
    if (now - lastBlink >= BLINK_INTERVAL) {
      lastBlink  = now;
      blinkState = !blinkState;
    }
    blinkState ? digitalWrite(STACK_RED, ON) : digitalWrite(STACK_YELLOW, ON);
    return;
  }

  if (overrideState == OVERRIDE_RESERVED) {
    digitalWrite(STACK_YELLOW, ON);
    return;
  }

  // Normal
  occupied ? digitalWrite(STACK_RED,   ON)
           : digitalWrite(STACK_GREEN, ON);
}

// ===================== OLED =====================
void updateDisplay(float delta) {
  const char* statusStr;
  switch (overrideState) {
    case OVERRIDE_RESERVED:    statusStr = "RESERVED";    break;
    case OVERRIDE_VIOLATION:   statusStr = "VIOLATION";   break;
    case OVERRIDE_MAINTENANCE: statusStr = "MAINTENANCE"; break;
    default: statusStr = occupied ? "OCCUPIED" : "FREE";  break;
  }

  display.clearBuffer();

  display.setFont(u8g2_font_helvB08_tr);
  display.drawStr(30, 12, "SPACE  O1");
  display.drawHLine(0, 15, 128);

  display.setFont(u8g2_font_helvB12_tr);
  display.drawStr(0, 32, statusStr);

  display.setFont(u8g2_font_6x10_tr);
  char buf[32];
  // Show 999 as --- on display when out of range
  if (distanceCM >= 999.0) {
    sprintf(buf, "Dist: --- cm");
  } else {
    sprintf(buf, "Dist: %.1f cm", distanceCM);
  }
  display.drawStr(0, 46, buf);

  sprintf(buf, "Mag Z: %.1f uT", delta);
  display.drawStr(0, 58, buf);

  display.sendBuffer();
}

// ===================== TOF READ =====================
float readVL53CM() {
  if (!lox.isRangeComplete()) return distanceCM; // return last known if not ready
  uint16_t mm = lox.readRange();
  if (lox.Status != VL53L0X_ERROR_NONE) return 999.0;
  if (mm > 8000) return 999.0;  // sensor max reliable range
  return mm / 10.0;
}

// ===================== BASELINE =====================
void captureMagBaseline() {
  Serial.println("Capturing magnetic baseline (Z axis)...");

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tr);
  display.drawStr(10, 28, "Calibrating...");
  display.drawStr(10, 44, "Keep area clear!");
  display.sendBuffer();

  float sum = 0.0;
  for (int i = 0; i < 500; i++) {
    sBmm150MagData_t data = mag.getGeomagneticData();
    // Z axis only — most sensitive to vehicle chassis directly overhead
    sum += data.z;
    delay(20);
  }
  magBaseline = sum / 500.0;
  Serial.printf("Baseline Z: %.2f uT\n", magBaseline);
}

// ===================== ESP-NOW SEND =====================
void sendStatus(const char* reason) {
  NodeMessage msg{};
  strncpy(msg.node_id, "O1", sizeof(msg.node_id) - 1);
  strcpy(msg.sensor_status, occupied ? "OCCUPIED" : "FREE");
  msg.distance_cm = distanceCM >= 999.0 ? 0.0 : distanceCM;
  msg.timestamp   = millis();

  esp_err_t r = esp_now_send(GATEWAY_MAC, (uint8_t*)&msg, sizeof(msg));

  Serial.printf("📡 O1 → %s (%.1fcm) [%s] send=%s\n",
                msg.sensor_status, msg.distance_cm, reason,
                r == ESP_OK ? "OK" : "FAIL");

  awaitingAck  = true;
  lastSendTime = millis();
}

void startReliableSend(const char* reason) {
  retryCount = 0;
  sendStatus(reason);
}

// ===================== ESP-NOW RECEIVE =====================
void onReceive(const uint8_t*, const uint8_t* data, int len) {

  if (len == sizeof(DecisionMessage)) {
    DecisionMessage msg{};
    memcpy(&msg, data, sizeof(msg));

    if (!strcmp(msg.node_id, "O1")) {
      if      (!strcmp(msg.decision, "RESERVED"))    overrideState = OVERRIDE_RESERVED;
      else if (!strcmp(msg.decision, "VIOLATION"))   overrideState = OVERRIDE_VIOLATION;
      else if (!strcmp(msg.decision, "MAINTENANCE")) overrideState = OVERRIDE_MAINTENANCE;
      else                                           overrideState = OVERRIDE_NONE;
      Serial.printf("📩 DECISION O1 → %s\n", msg.decision);
    }
    return;
  }

  if (len == sizeof(AckMessage)) {
    AckMessage ack{};
    memcpy(&ack, data, sizeof(ack));

    if (!strcmp(ack.node_id, "O1") && awaitingAck) {
      awaitingAck = false;
      retryCount  = 0;
      Serial.println("✅ ACK RX O1");
    }
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Stacklight pins
  pinMode(STACK_RED,    OUTPUT); digitalWrite(STACK_RED,    OFF);
  pinMode(STACK_YELLOW, OUTPUT); digitalWrite(STACK_YELLOW, OFF);
  pinMode(STACK_GREEN,  OUTPUT); digitalWrite(STACK_GREEN,  OFF);
  pinMode(STACK_BUZZER, OUTPUT); digitalWrite(STACK_BUZZER, OFF);

  Wire.begin(17, 18);

  // OLED
  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_helvB08_tr);
  display.drawStr(20, 35, "INITIALISING...");
  display.sendBuffer();
  delay(500);

  // BMM150
  if (mag.begin() != 0) {
    Serial.println("BMM150 failed!");
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(0, 35, "BMM150 FAILED!");
    display.sendBuffer();
    while(1);
  }
  mag.setOperationMode(BMM150_POWERMODE_NORMAL);
  mag.setPresetMode(BMM150_PRESETMODE_HIGHACCURACY);
  delay(1000);

  captureMagBaseline();

  // VL53L0X
  pinMode(XSHUT_PIN, OUTPUT);
  digitalWrite(XSHUT_PIN, LOW);
  delay(10);
  digitalWrite(XSHUT_PIN, HIGH);
  delay(10);
  if (!lox.begin(0x29)) {
    Serial.println("VL53L0X failed!");
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(0, 35, "TOF FAILED!");
    display.sendBuffer();
    while(1);
  }
  lox.configSensor(Adafruit_VL53L0X::VL53L0X_SENSE_LONG_RANGE);
  lox.startRangeContinuous();

  // ESP-NOW
  WiFi.mode(WIFI_STA);
  Serial.println(WiFi.macAddress());
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_now_init();
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, GATEWAY_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  // Start green
  digitalWrite(STACK_GREEN, ON);

  Serial.println("✅ OUTDOOR NODE READY");
}

// ===================== LOOP =====================
unsigned long lastSensorRead = 0;
#define SENSOR_INTERVAL 200

void loop() {
  unsigned long now = millis();

  // Sensor read
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    // TOF
    float d = readVL53CM();
    distanceCM = d;

    // Magnetic — Z axis only
    sBmm150MagData_t mdata = mag.getGeomagneticData();
    float delta = abs(mdata.z - magBaseline);

    // Magnetic confirmation — 3s sustained change on Z axis
    if (delta > magneticThreshold) {
  if (magConfirmStart == 0) magConfirmStart = now;
  if (now - magConfirmStart > 3000) {
    magConfirmed = true;
  }
} else {
  // Only reset if delta has been below threshold; don't reset magConfirmStart instantly
  // to avoid noise causing confirmation timer to restart repeatedly
  magConfirmStart = 0;
  magConfirmed    = false;
}

    // Strict AND both ways:
// OCCUPIED only if BOTH distance close AND magnetic confirmed
// FREE if EITHER distance is clear OR magnetic has settled
// This prevents getting stuck in occupied
if (d < OCCUPIED_CM && magConfirmed) {
  occupied = true;
} else {
  occupied = false;
}

    // Boot announce
    if (!bootAnnounced) {
      bootAnnounced = true;
      lastOccupied  = occupied;
      startReliableSend("boot");
    }

    // Change detect
    if (occupied != lastOccupied && !awaitingAck) {
      lastOccupied = occupied;
      startReliableSend("change");
    }

    updateDisplay(delta);
  }

  // Stacklight
  updateStacklight(now);

  // Retry
  if (awaitingAck && millis() - lastSendTime >= ACK_TIMEOUT_MS) {
    if (retryCount < 10) {
      retryCount++;
      Serial.printf("🔁 RETRY O1 (%u/10)\n", retryCount);
      sendStatus("retry");
    } else {
      Serial.println("⚠️ GIVING UP O1");
      awaitingAck = false;
      retryCount  = 0;
    }
  }

  // Heartbeat
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    if (!awaitingAck) startReliableSend("heartbeat");
  }
}