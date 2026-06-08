#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_VL53L0X.h>

// ===================== MCP ADDRESSES =====================
#define MCP1_ADDR  0x27
#define MCP2_ADDR  0x23

// ===================== MCP REGISTERS =====================
#define IODIRA  0x00
#define IODIRB  0x01
#define GPIOA   0x12
#define GPIOB   0x13

// ===================== PORT STATE TRACKING =====================
static uint8_t mcp1A = 0x00, mcp1B = 0x00;
static uint8_t mcp2A = 0x00, mcp2B = 0x00; 

// ===================== MCP HELPERS =====================
void mcpWrite(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

void mcpInit(uint8_t addr) {
  mcpWrite(addr, IODIRA, 0x00);
  mcpWrite(addr, IODIRB, 0x00);
  mcpWrite(addr, GPIOA,  0x00);
  mcpWrite(addr, GPIOB,  0x00);
}

void mcpSetPin(uint8_t addr, uint8_t pin, bool value) {
  uint8_t* portState;

  if (addr == MCP1_ADDR)
    portState = (pin < 8) ? &mcp1A : &mcp1B;
  else
    portState = (pin < 8) ? &mcp2A : &mcp2B;

  uint8_t bit = (pin < 8) ? pin : pin - 8;
  uint8_t reg = (pin < 8) ? GPIOA : GPIOB;

  value ? *portState |= (1 << bit) : *portState &= ~(1 << bit);
  mcpWrite(addr, reg, *portState);
}

void allOff() {
  mcp1A = mcp1B = mcp2A = mcp2B = 0x00;
  mcpWrite(MCP1_ADDR, GPIOA, 0x00);
  mcpWrite(MCP1_ADDR, GPIOB, 0x00);
  mcpWrite(MCP2_ADDR, GPIOA, 0x00);
  mcpWrite(MCP2_ADDR, GPIOB, 0x00);
}

// ===================== OLED =====================
// SH1106 on same I2C bus (SDA=17, SCL=18), address 0x3C
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

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

// ===================== TOF SENSORS =====================
Adafruit_VL53L0X lox1, lox2, lox3, lox4, lox5, lox6, lox7, lox8;

#define XSHUT_1   4
#define XSHUT_2   6
#define XSHUT_3   8
#define XSHUT_4   9
#define XSHUT_5  10
#define XSHUT_6  11
#define XSHUT_7  12
#define XSHUT_8  13

// ===================== THRESHOLDS =====================
// Lowered by 2cm from previous to avoid cross-space pickup on tabletop model
#define OCCUPIED_CM        8.0
#define FREE_CM            13.0
#define BLINK_INTERVAL     400
#define ACK_TIMEOUT_MS     1200
#define HEARTBEAT_INTERVAL 30000
#define SENSOR_GAP         60
#define SENSOR_INTERVAL    120

// ===================== PARKING SPACE =====================
struct ParkingSpace {
  const char*        id;
  Adafruit_VL53L0X*  sensor;
  uint8_t            addr;
  uint8_t            pinR, pinG, pinB;
  OverrideState      overrideState;
  bool               occupied;
  bool               lastOccupied;
  bool               bootAnnounced;
  float              distanceCM;
  unsigned long      lastSensorRead;
  unsigned long      lastBlink;
  bool               blinkState;
  bool               awaitingAck;
  unsigned long      lastSendTime;
  uint8_t            retryCount;
};

ParkingSpace spaces[] = {
  {"A1", &lox1, MCP2_ADDR,  0,  1,  2, OVERRIDE_NONE, false, false, false, 999.0, 0, 0, false, false, 0, 0},
  {"A2", &lox2, MCP2_ADDR,  3,  4,  5, OVERRIDE_NONE, false, false, false, 999.0, 0, 0, false, false, 0, 0},
  {"A3", &lox3, MCP2_ADDR,  6,  7,  8, OVERRIDE_NONE, false, false, false, 999.0, 0, 0, false, false, 0, 0},
  {"A5", &lox4, MCP2_ADDR, 12, 13, 14, OVERRIDE_NONE, false, false, false, 999.0, 0, 0, false, false, 0, 0},
  {"A6", &lox5, MCP1_ADDR,  0,  1,  2, OVERRIDE_NONE, false, false, false, 999.0, 0, 0, false, false, 0, 0},
  {"A7", &lox6, MCP1_ADDR,  3,  4,  5, OVERRIDE_NONE, false, false, false, 999.0, 0, 0, false, false, 0, 0},
  {"A8", &lox7, MCP1_ADDR,  6,  7,  8, OVERRIDE_NONE, false, false, false, 999.0, 0, 0, false, false, 0, 0},
  {"A9", &lox8, MCP1_ADDR,  9, 10, 11, OVERRIDE_NONE, false, false, false, 999.0, 0, 0, false, false, 0, 0},
};

#define NUM_SPACES (sizeof(spaces)/sizeof(spaces[0]))

// ===================== OLED UPDATE =====================
unsigned long lastDisplayUpdate = 0;
#define DISPLAY_INTERVAL 1000  // refresh every 1 second

void updateDisplay() {
  uint8_t totalOccupied  = 0;
  uint8_t totalReserved  = 0;
  uint8_t totalAvailable = 0;
  uint8_t totalMaint     = 0;

  for (auto& s : spaces) {
    if (s.overrideState == OVERRIDE_MAINTENANCE) {
      totalMaint++;
    } else if (s.overrideState == OVERRIDE_RESERVED ||
               s.overrideState == OVERRIDE_VIOLATION) {
      totalReserved++;
    } else if (s.occupied) {
      totalOccupied++;
    } else {
      totalAvailable++;
    }
  }

  display.clearBuffer();

  // Header
  display.setFont(u8g2_font_helvB08_tr);
  display.drawStr(20, 11, "PARKING STATUS");
  display.drawHLine(0, 14, 128);

  char buf[32];

  // Available 
  display.setFont(u8g2_font_helvB12_tr);
  display.drawStr(0, 30, "AVAIL:");
  sprintf(buf, "%d", totalAvailable);
  display.drawStr(80, 30, buf);

  // Occupied
  display.setFont(u8g2_font_6x10_tr);
  sprintf(buf, "Occupied : %d", totalOccupied);
  display.drawStr(0, 44, buf);

  // Reserved
  sprintf(buf, "Reserved : %d", totalReserved);
  display.drawStr(0, 56, buf);

  // Maintenance count in corner if any
  if (totalMaint > 0) {
    sprintf(buf, "Maint:%d", totalMaint);
    display.drawStr(88, 56, buf);
  }

  display.sendBuffer();
}

// ===================== LED =====================
void setLED(ParkingSpace& s, bool r, bool g, bool b) {
  mcpSetPin(s.addr, s.pinR, r);
  mcpSetPin(s.addr, s.pinG, g);
  mcpSetPin(s.addr, s.pinB, b);
}

void updateLED(ParkingSpace& s, unsigned long now) {

  if (s.overrideState == OVERRIDE_MAINTENANCE) {
    setLED(s, false, false, false);
    return;
  }

  if (s.overrideState == OVERRIDE_VIOLATION) {
    if (now - s.lastBlink >= BLINK_INTERVAL) {
      s.lastBlink  = now;
      s.blinkState = !s.blinkState;
      setLED(s, s.blinkState, false, !s.blinkState);
    }
    return;
  }

  if (s.overrideState == OVERRIDE_RESERVED) {
    setLED(s, false, false, true);
    return;
  }

  s.occupied ? setLED(s, true, false, false)
             : setLED(s, false, true, false);
}

// ===================== TOF READ =====================
float readVL53CM(Adafruit_VL53L0X& lox) {
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);
  if (measure.RangeStatus == 4) return 999.0;
  uint16_t mm = measure.RangeMilliMeter;
  if (mm < 20 || mm > 2000) return 999.0;
  return mm / 10.0;
}

// ===================== ESP-NOW SEND =====================
void sendStatus(ParkingSpace& s, const char* reason) {
  NodeMessage msg{};
  strncpy(msg.node_id, s.id, sizeof(msg.node_id) - 1);
  strcpy(msg.sensor_status, s.occupied ? "OCCUPIED" : "FREE");
  msg.distance_cm = s.distanceCM;
  msg.timestamp   = millis();

  esp_err_t r = esp_now_send(GATEWAY_MAC, (uint8_t*)&msg, sizeof(msg));

  Serial.printf("📡 %s → %s (%.1fcm) [%s] send=%s\n",
                s.id, msg.sensor_status, msg.distance_cm, reason,
                r == ESP_OK ? "OK" : "FAIL");

  s.awaitingAck  = true;
  s.lastSendTime = millis();
}

void startReliableSend(ParkingSpace& s, const char* reason) {
  s.retryCount = 0;
  sendStatus(s, reason);
}

// ===================== ESP-NOW RECEIVE =====================
void onReceive(const uint8_t*, const uint8_t* data, int len) {

  if (len == sizeof(DecisionMessage)) {
    DecisionMessage msg{};
    memcpy(&msg, data, sizeof(msg));

    for (auto& s : spaces) {
      if (!strcmp(msg.node_id, s.id)) {
        if      (!strcmp(msg.decision, "RESERVED"))    s.overrideState = OVERRIDE_RESERVED;
        else if (!strcmp(msg.decision, "VIOLATION"))   s.overrideState = OVERRIDE_VIOLATION;
        else if (!strcmp(msg.decision, "MAINTENANCE")) s.overrideState = OVERRIDE_MAINTENANCE;
        else                                           s.overrideState = OVERRIDE_NONE;
        Serial.printf("📩 DECISION %s → %s\n", s.id, msg.decision);
      }
    }
    return;
  }

  if (len == sizeof(AckMessage)) {
    AckMessage ack{};
    memcpy(&ack, data, sizeof(ack));

    for (auto& s : spaces) {
      if (!strcmp(ack.node_id, s.id) && s.awaitingAck) {
        s.awaitingAck = false;
        s.retryCount  = 0;
        Serial.printf("✅ ACK RX %s\n", s.id);
      }
    }
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(17, 18);
  mcpInit(MCP1_ADDR);
  mcpInit(MCP2_ADDR);
  allOff();

  // OLED init
  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_helvB08_tr);
  display.drawStr(15, 35, "INITIALISING...");
  display.sendBuffer();
  delay(500);

  // Pull all XSHUT low
  uint8_t xshutPins[] = {XSHUT_1, XSHUT_2, XSHUT_3, XSHUT_4,
                          XSHUT_5, XSHUT_6, XSHUT_7, XSHUT_8};
  for (uint8_t i = 0; i < 8; i++) {
    pinMode(xshutPins[i], OUTPUT);
    digitalWrite(xshutPins[i], LOW);
  }
  delay(10);

  // Bring up each sensor one at a time and assign unique address
  digitalWrite(XSHUT_1, HIGH); delay(10);
  lox1.begin(0x29); lox1.setAddress(0x30);

  digitalWrite(XSHUT_2, HIGH); delay(10);
  lox2.begin(0x29); lox2.setAddress(0x31);

  digitalWrite(XSHUT_3, HIGH); delay(10);
  lox3.begin(0x29); lox3.setAddress(0x32);

  digitalWrite(XSHUT_4, HIGH); delay(10);
  lox4.begin(0x29); lox4.setAddress(0x33);

  digitalWrite(XSHUT_5, HIGH); delay(10);
  lox5.begin(0x29); lox5.setAddress(0x34);

  digitalWrite(XSHUT_6, HIGH); delay(10);
  lox6.begin(0x29); lox6.setAddress(0x35);

  digitalWrite(XSHUT_7, HIGH); delay(10);
  lox7.begin(0x29); lox7.setAddress(0x36);

  digitalWrite(XSHUT_8, HIGH); delay(10);
  lox8.begin(0x29); lox8.setAddress(0x37);

  // All green on startup
  for (auto& s : spaces)
    setLED(s, false, true, false);

  // Initial display
  updateDisplay();

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

  Serial.println("✅ INDOOR NODE READY");
}

// ===================== LOOP =====================
uint8_t currentIndex   = 0;
unsigned long lastFire      = 0;
unsigned long lastHeartbeat = 0;

void loop() {

  unsigned long now = millis();

  // Update LEDs every loop
  for (auto& s : spaces)
    updateLED(s, now);

  // Round-robin sensor reads
  if (now - lastFire >= SENSOR_GAP) {
    lastFire = now;

    ParkingSpace& s = spaces[currentIndex];

    if (now - s.lastSensorRead >= SENSOR_INTERVAL) {
      s.lastSensorRead = now;

      float d = readVL53CM(*s.sensor);
      s.distanceCM = d;

      if      (d < OCCUPIED_CM) s.occupied = true;
      else if (d > FREE_CM)     s.occupied = false;

      if (!s.bootAnnounced) {
        s.bootAnnounced = true;
        s.lastOccupied  = s.occupied;
        startReliableSend(s, "boot");
      }

      if (s.occupied != s.lastOccupied && !s.awaitingAck) {
        s.lastOccupied = s.occupied;
        startReliableSend(s, "change");
      }
    }

    currentIndex++;
    if (currentIndex >= NUM_SPACES) currentIndex = 0;
  }

  // Retry logic
  for (auto& s : spaces) {
    if (!s.awaitingAck) continue;

    if (millis() - s.lastSendTime >= ACK_TIMEOUT_MS) {
      if (s.retryCount < 10) {
        s.retryCount++;
        Serial.printf("🔁 RETRY %s (%u/10)\n", s.id, s.retryCount);
        sendStatus(s, "retry");
      } else {
        Serial.printf("⚠️ GIVING UP %s\n", s.id);
        s.awaitingAck = false;
        s.retryCount  = 0;
      }
    }
  }

  // Heartbeat
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    for (auto& s : spaces)
      if (!s.awaitingAck)
        startReliableSend(s, "heartbeat");
  }

  // OLED refresh every second
  if (now - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = now;
    updateDisplay();
  }
}