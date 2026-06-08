#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

/* ===================== WIFI ===================== */
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

/* ===================== SERVER ===================== */
const char* UPDATE_URL =
  "https://smart-parking-backend-u60i.onrender.com/api/node/update";

const char* STATUS_URL =
  "https://smart-parking-backend-u60i.onrender.com/api/parking/status";

const char* NODES_URL =
  "https://smart-parking-backend-u60i.onrender.com/api/nodes";

/* ===================== NODE MACs ===================== */
uint8_t INDOOR_MAC[]  = {0xE0, 0x72, 0xA1, 0xF2, 0xED, 0x84};
uint8_t OUTDOOR_MAC[] = {0xE0, 0x72, 0xA1, 0xF2, 0xED, 0x84}; 

/* ===================== STRUCTS ===================== */
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

/* ===================== TIMING ===================== */
#define SERVER_POLL_INTERVAL_MS 800
#define POST_COOLDOWN_MS        50

unsigned long lastPoll = 0;
unsigned long lastPost = 0;

/* ===================== RX STATE ===================== */
volatile bool newPacket = false;
NodeMessage rxMsg{};

/* ===================== MULTI-NODE TRACKING ===================== */
struct NodeDecisionState {
  char node_id[4];
  char lastDecision[16];
  bool used;
};

#define MAX_NODES 12
NodeDecisionState nodes[MAX_NODES];

/* ===================== GLOBAL HTTP/JSON ===================== */
WiFiClientSecure httpsClient;
HTTPClient http;

StaticJsonDocument<256>  postDoc;
StaticJsonDocument<8192> getDoc;

/* ===================== MAC HELPER ===================== */
uint8_t* getMacForNode(const char* node_id) {
  if (!strcmp(node_id, "O1")) return OUTDOOR_MAC;
  return INDOOR_MAC;
}

/* ===================== HELPERS ===================== */
int findOrAddNode(const char* id) {
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].used && strcmp(nodes[i].node_id, id) == 0) return i;
  }
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].used) {
      nodes[i].used = true;
      strncpy(nodes[i].node_id, id, sizeof(nodes[i].node_id) - 1);
      nodes[i].node_id[sizeof(nodes[i].node_id) - 1] = '\0';
      strcpy(nodes[i].lastDecision, "");
      Serial.printf("🆕 REGISTERED NODE: %s\n", nodes[i].node_id);
      return i;
    }
  }
  Serial.println("⚠️ Node list full!");
  return -1;
}

bool anyNodesDiscovered() {
  for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used) return true;
  return false;
}

/* ===================== NODE BOOTSTRAP ===================== */
void preloadNodesFromServer() {
  Serial.println("📥 Preloading nodes from server...");

  http.begin(httpsClient, NODES_URL);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("⚠️ Node preload failed (%d)\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("⚠️ Failed to parse node list");
    return;
  }

  for (JsonVariant v : doc.as<JsonArray>()) {
    findOrAddNode(v.as<const char*>());
  }

  Serial.println("✅ Node preload complete");
}

/* ===================== ESPNOW RX CALLBACK ===================== */
void onReceive(const uint8_t*, const uint8_t* data, int len) {
  if (len != (int)sizeof(NodeMessage)) return;
  memcpy((void*)&rxMsg, data, sizeof(rxMsg));
  newPacket = true;
}

/* ===================== SEND DECISION ===================== */
void sendDecision(const char* node_id, const char* decision) {
  DecisionMessage msg{};
  strncpy(msg.node_id, node_id, sizeof(msg.node_id) - 1);
  strncpy(msg.decision, decision, sizeof(msg.decision) - 1);

  uint8_t* mac = getMacForNode(node_id);
  esp_err_t r = esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
  Serial.printf("📤 DECISION TX → %s=%s | send=%d\n", node_id, decision, (int)r);
}

/* ===================== SEND ACK ===================== */
void sendAck(const char* node_id) {
  AckMessage ack{};
  strncpy(ack.node_id, node_id, sizeof(ack.node_id) - 1);

  uint8_t* mac = getMacForNode(node_id);
  esp_err_t r = esp_now_send(mac, (uint8_t*)&ack, sizeof(ack));
  Serial.printf("✅ ACK TX → %s | send=%d\n", node_id, (int)r);
}

/* ===================== SETUP ===================== */
void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n=== SMART PARKING GATEWAY (NO SEQ MODE) ===");

  for (int i = 0; i < MAX_NODES; i++) nodes[i].used = false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("📶 Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi CONNECTED");
  Serial.println(WiFi.macAddress());

  httpsClient.setInsecure();

  preloadNodesFromServer();

  uint8_t channel = 6;
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onReceive);

  // Indoor peer
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, INDOOR_MAC, 6);
  peer.channel = channel;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(INDOOR_MAC)) {
    if (esp_now_add_peer(&peer) != ESP_OK)
      Serial.println("❌ Failed to add indoor peer");
  }

  // Outdoor peer
  esp_now_peer_info_t peer2{};
  memcpy(peer2.peer_addr, OUTDOOR_MAC, 6);
  peer2.channel = channel;
  peer2.encrypt = false;
  if (!esp_now_is_peer_exist(OUTDOOR_MAC)) {
    if (esp_now_add_peer(&peer2) != ESP_OK)
      Serial.println("❌ Failed to add outdoor peer");
  }

  Serial.println("✅ ESP-NOW READY");
  Serial.printf("📶 WiFi Channel: %d\n", WiFi.channel());
}

/* ===================== POST NODE UPDATE ===================== */
bool postUpdateToServer(const NodeMessage& m) {
  postDoc.clear();
  postDoc["node_id"]       = m.node_id;
  postDoc["sensor_status"] = m.sensor_status;
  postDoc["distance_cm"]   = m.distance_cm;
  postDoc["timestamp"]     = m.timestamp;

  String body;
  serializeJson(postDoc, body);

  http.begin(httpsClient, UPDATE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "keep-alive");
  http.setTimeout(8000);

  int code = http.POST(body);
  String response = (code > 0) ? http.getString() : "";
  http.end();

  Serial.printf("🌐 HTTPS %d | %s\n", code, response.c_str());
  return (code == 200 && response.indexOf("ok") >= 0);
}

/* ===================== GET STATUS + SEND CHANGES ===================== */
void pollServerAndSendDecisions() {
  if (!anyNodesDiscovered()) return;

  http.begin(httpsClient, STATUS_URL);
  int code = http.GET();

  if (code != 200) {
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  getDoc.clear();
  if (deserializeJson(getDoc, payload)) return;

  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].used) continue;

    const char* id = nodes[i].node_id;
    if (!getDoc.containsKey(id)) continue;

    const char* raw = getDoc[id]["final_status"] | "CLEAR";

    const char* decision = "CLEAR";
    if (!strcmp(raw, "RESERVED") || !strcmp(raw, "VIOLATION") || !strcmp(raw, "MAINTENANCE")) {
      decision = raw;
    }

    if (strcmp(decision, nodes[i].lastDecision) != 0) {
      sendDecision(id, decision);
      strncpy(nodes[i].lastDecision, decision, sizeof(nodes[i].lastDecision) - 1);
      nodes[i].lastDecision[sizeof(nodes[i].lastDecision) - 1] = '\0';
    }
  }
}

/* ===================== LOOP ===================== */
void loop() {

  if (newPacket) {
    newPacket = false;

    NodeMessage localMsg{};
    memcpy(&localMsg, (void*)&rxMsg, sizeof(NodeMessage));

    Serial.printf("📦 Packet RX from: %s status: %s\n",
                  localMsg.node_id, localMsg.sensor_status);

    findOrAddNode(localMsg.node_id);

    bool ok = postUpdateToServer(localMsg);
    Serial.printf("🌐 POST result: %s\n", ok ? "OK" : "FAIL");

    if (ok) sendAck(localMsg.node_id);
  }

  if (millis() - lastPoll >= SERVER_POLL_INTERVAL_MS) {
    lastPoll = millis();
    pollServerAndSendDecisions();
  }
}