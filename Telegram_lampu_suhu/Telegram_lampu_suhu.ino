#ifdef ESP32
  #include <WiFi.h>
  #include <WebServer.h>
#endif
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <time.h>

// ==================== KONFIGURASI ====================
const char* ssid     = "";
const char* password = "";

#define BOTtoken "8701088647:AAEJAEzCDlkpBf_RGbQy-xL8uvfGAKmHoQM"
#define CHAT_ID  "1250404612"

const char* ntpServer      = "pool.ntp.org";
const long  gmtOffset      = 7 * 3600;
const int   daylightOffset = 0;

// ==================== PIN ====================
int  relayPin[4] = {23, 19, 18, 5};
bool relay[4]    = {false, false, false, false};

#define DHTPIN  4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

float suhu = 0, kelembapan = 0;

unsigned long lastDHTRead       = 0;
unsigned long lastTimeBotRan    = 0;
unsigned long lastSensorSend    = 0;
int           botRequestDelay   = 1000;

// ==================== VARIASI DISCO ====================
bool          variasi1Active     = false;
bool          variasi2Active     = false;
unsigned long lastVariasi1Switch = 0;
unsigned long lastVariasi2Switch = 0;
bool          variasiState       = false;
int           variasiStep        = 0;
const int     variasiDelay1      = 120;
const int     variasiDelay2      = 80;

// ==================== MQTT ====================
const char*  mqtt_broker = "broker.hivemq.com";
const int    mqtt_port   = 1883;
const char*  mqtt_topic  = "smarthome/relay";
const char*  mqtt_sensor = "smarthome/sensor";

WiFiClient        espClient;
PubSubClient      mqtt(espClient);
WiFiClientSecure  secureClient;
UniversalTelegramBot bot(BOTtoken, secureClient);

// ==================== WEB SERVER ====================
WebServer server(80);

// ==================== NTP ====================
String getTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--:--:--";
  char buf[22];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);
  return String(buf);
}

// ==================== NOTIF TELEGRAM ====================
void sendTelegramNotif(String message) {
  String notif = message + "\n🕐 " + getTime() + " WIB";
  bot.sendMessage(CHAT_ID, notif, "");
}

// ==================== RELAY ====================
void setRelay(int index, bool state) {
  relay[index] = state;
  digitalWrite(relayPin[index], state ? LOW : HIGH);
  Serial.printf("Relay %d -> %s\n", index + 1, state ? "ON" : "OFF");
}

void setAllRelay(bool state) {
  for (int r = 0; r < 4; r++) setRelay(r, state);
}

void stopVariasi() {
  variasi1Active     = false;
  variasi2Active     = false;
  variasiStep        = 0;
  variasiState       = false;
  lastVariasi1Switch = 0;
  lastVariasi2Switch = 0;
}

// ==================== CORS ====================
void setCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ==================== GET /state ====================
void handleState() {
  setCORSHeaders();
  String json = "{";
  json += "\"relay1\":"      + String(relay[0] ? "true" : "false") + ",";
  json += "\"relay2\":"      + String(relay[1] ? "true" : "false") + ",";
  json += "\"relay3\":"      + String(relay[2] ? "true" : "false") + ",";
  json += "\"relay4\":"      + String(relay[3] ? "true" : "false") + ",";
  json += "\"temperature\":" + (isnan(suhu)       ? String("null") : String(suhu, 1))       + ",";
  json += "\"humidity\":"    + (isnan(kelembapan)  ? String("null") : String(kelembapan, 1)) + ",";
  json += "\"time\":\""      + getTime() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// ==================== POST /relay ====================
void handleRelay() {
  setCORSHeaders();
  if (server.hasArg("plain")) {
    StaticJsonDocument<100> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      int  relayNum = doc["relay"];
      bool state    = doc["state"];
      if (relayNum >= 1 && relayNum <= 4) {
        stopVariasi();
        setRelay(relayNum - 1, state);
        String cmd = "relay" + String(relayNum) + (state ? "_on" : "_off");
        mqtt.publish(mqtt_topic, cmd.c_str());
        sendTelegramNotif("[WEB] Lampu " + String(relayNum) + (state ? " ON 💡" : " OFF 🔌"));
      }
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      server.send(400, "application/json", "{\"status\":\"error\"}");
    }
  }
}

// ==================== POST /all ====================
void handleAll() {
  setCORSHeaders();
  if (server.hasArg("plain")) {
    StaticJsonDocument<50> doc;
    deserializeJson(doc, server.arg("plain"));
    bool state = doc["state"];
    stopVariasi();
    setAllRelay(state);
    mqtt.publish(mqtt_topic, state ? "all_on" : "all_off");
    sendTelegramNotif(state ? "[WEB] Semua lampu ON ⚡" : "[WEB] Semua lampu OFF 🔴");
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  }
}

// ==================== OPTIONS ====================
void handleOptions() {
  setCORSHeaders();
  server.send(204);
}

// ==================== MQTT CALLBACK ====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println("MQTT masuk: " + msg);

  if      (msg == "relay1_on")   { stopVariasi(); setRelay(0, true);  sendTelegramNotif("[WEB] Lampu 1 ON 💡"); }
  else if (msg == "relay1_off")  { stopVariasi(); setRelay(0, false); sendTelegramNotif("[WEB] Lampu 1 OFF 🔌"); }
  else if (msg == "relay2_on")   { stopVariasi(); setRelay(1, true);  sendTelegramNotif("[WEB] Lampu 2 ON 💡"); }
  else if (msg == "relay2_off")  { stopVariasi(); setRelay(1, false); sendTelegramNotif("[WEB] Lampu 2 OFF 🔌"); }
  else if (msg == "relay3_on")   { stopVariasi(); setRelay(2, true);  sendTelegramNotif("[WEB] Lampu 3 ON 💡"); }
  else if (msg == "relay3_off")  { stopVariasi(); setRelay(2, false); sendTelegramNotif("[WEB] Lampu 3 OFF 🔌"); }
  else if (msg == "relay4_on")   { stopVariasi(); setRelay(3, true);  sendTelegramNotif("[WEB] Lampu 4 ON 💡"); }
  else if (msg == "relay4_off")  { stopVariasi(); setRelay(3, false); sendTelegramNotif("[WEB] Lampu 4 OFF 🔌"); }
  else if (msg == "all_on")      { stopVariasi(); setAllRelay(true);  sendTelegramNotif("[WEB] Semua lampu ON ⚡"); }
  else if (msg == "all_off")     { stopVariasi(); setAllRelay(false); sendTelegramNotif("[WEB] Semua lampu OFF 🔴"); }
  else if (msg == "variasi_off") { stopVariasi(); setAllRelay(false); sendTelegramNotif("[WEB] Variasi dimatikan ⏹"); }
  else if (msg == "variasi1") {
    stopVariasi();
    variasi1Active = true;
    sendTelegramNotif("[WEB] Variasi 1 aktif 🎉");
  }
  else if (msg == "variasi2") {
    stopVariasi();
    variasi2Active = true;
    sendTelegramNotif("[WEB] Variasi 2 aktif 🎉");
  }
  else if (msg == "get_sensor") {
    String data = "Suhu: " + String(suhu, 1) + "C | Kelembapan: " + String(kelembapan, 1) + "%";
    mqtt.publish(mqtt_sensor, data.c_str());
  }
}

// ==================== MQTT RECONNECT ====================
void mqttReconnect() {
  while (!mqtt.connected()) {
    Serial.print("Connecting MQTT...");
    String clientId = "ESP32-" + String(random(0xffff), HEX);
    if (mqtt.connect(clientId.c_str())) {
      Serial.println("Connected!");
      mqtt.subscribe(mqtt_topic);
    } else {
      Serial.println("Gagal, coba lagi 3 detik...");
      delay(3000);
    }
  }
}

// ==================== KIRIM MENU RELAY ====================
void sendRelayMenu(String chat_id) {
  String msg = "Pilih lampu yang ingin dikontrol:\n";
  for (int i = 0; i < 4; i++)
    msg += "Lampu " + String(i+1) + ": " + (relay[i] ? "🟢 ON" : "⚫ OFF") + "\n";

  String kb = "[[";
  kb += "{\"text\":\"💡 Lampu 1 " + String(relay[0]?"✅":"") + " ON\",\"callback_data\":\"r1_on\"},";
  kb += "{\"text\":\"🔌 Lampu 1 OFF\",\"callback_data\":\"r1_off\"}],[";
  kb += "{\"text\":\"💡 Lampu 2 " + String(relay[1]?"✅":"") + " ON\",\"callback_data\":\"r2_on\"},";
  kb += "{\"text\":\"🔌 Lampu 2 OFF\",\"callback_data\":\"r2_off\"}],[";
  kb += "{\"text\":\"💡 Lampu 3 " + String(relay[2]?"✅":"") + " ON\",\"callback_data\":\"r3_on\"},";
  kb += "{\"text\":\"🔌 Lampu 3 OFF\",\"callback_data\":\"r3_off\"}],[";
  kb += "{\"text\":\"💡 Lampu 4 " + String(relay[3]?"✅":"") + " ON\",\"callback_data\":\"r4_on\"},";
  kb += "{\"text\":\"🔌 Lampu 4 OFF\",\"callback_data\":\"r4_off\"}],[";
  kb += "{\"text\":\"⚡ Semua ON\",\"callback_data\":\"all_on\"},";
  kb += "{\"text\":\"🔴 Semua OFF\",\"callback_data\":\"all_off\"}]]";
  bot.sendMessageWithInlineKeyboard(chat_id, msg, "", kb);
}

// ==================== KIRIM STATUS ====================
void sendStatus(String chat_id) {
  String msg = "📋 Status Lampu:\n";
  for (int r = 0; r < 4; r++)
    msg += (relay[r] ? "🟢" : "⚫") + String(" Lampu ") + String(r+1) + ": " + (relay[r] ? "ON" : "OFF") + "\n";
  msg += "🕐 " + getTime() + " WIB";
  bot.sendMessage(chat_id, msg, "");
}

// ==================== HANDLE CALLBACK QUERY ====================
void handleCallbackQuery(String chat_id, String callback_id, String data) {
  bot.answerCallbackQuery(callback_id, "");
  int  relayIdx = -1;
  bool state    = false;
  if      (data == "r1_on")  { relayIdx = 0; state = true; }
  else if (data == "r1_off") { relayIdx = 0; state = false; }
  else if (data == "r2_on")  { relayIdx = 1; state = true; }
  else if (data == "r2_off") { relayIdx = 1; state = false; }
  else if (data == "r3_on")  { relayIdx = 2; state = true; }
  else if (data == "r3_off") { relayIdx = 2; state = false; }
  else if (data == "r4_on")  { relayIdx = 3; state = true; }
  else if (data == "r4_off") { relayIdx = 3; state = false; }
  else if (data == "all_on") {
    stopVariasi(); setAllRelay(true);
    mqtt.publish(mqtt_topic, "all_on");
    sendTelegramNotif("[TG] Semua lampu ON ⚡");
    sendRelayMenu(chat_id); return;
  }
  else if (data == "all_off") {
    stopVariasi(); setAllRelay(false);
    mqtt.publish(mqtt_topic, "all_off");
    sendTelegramNotif("[TG] Semua lampu OFF 🔴");
    sendRelayMenu(chat_id); return;
  }
  if (relayIdx >= 0) {
    stopVariasi();
    setRelay(relayIdx, state);
    String cmd = "relay" + String(relayIdx+1) + (state ? "_on" : "_off");
    mqtt.publish(mqtt_topic, cmd.c_str());
    sendTelegramNotif("[TG] Lampu " + String(relayIdx+1) + (state ? " ON 💡" : " OFF 🔌"));
    sendRelayMenu(chat_id);
  }
}

// ==================== PARSER NATURAL ====================
void parseNaturalCommand(String chat_id, String rawText) {
  String text = rawText;
  text.toLowerCase();
  int lampu = -1;
  if      (text.indexOf("satu")  >= 0 || text.indexOf(" 1") >= 0) lampu = 0;
  else if (text.indexOf("dua")   >= 0 || text.indexOf(" 2") >= 0) lampu = 1;
  else if (text.indexOf("tiga")  >= 0 || text.indexOf(" 3") >= 0) lampu = 2;
  else if (text.indexOf("empat") >= 0 || text.indexOf(" 4") >= 0) lampu = 3;

  if (text.indexOf("variasi") >= 0 && (text.indexOf("mati") >= 0 || text.indexOf("off") >= 0 || text.indexOf("stop") >= 0)) {
    stopVariasi(); setAllRelay(false);
    mqtt.publish(mqtt_topic, "variasi_off");
    bot.sendMessage(chat_id, "⏹ Variasi dimatikan", ""); return;
  }
  if (text.indexOf("variasi") >= 0) {
    if (text.indexOf("1") >= 0 || text.indexOf("satu") >= 0) {
      stopVariasi();
      variasi1Active = true;
      mqtt.publish(mqtt_topic, "variasi1");
      bot.sendMessage(chat_id, "🎉 Variasi 1 aktif! Lampu 1&3 vs 2&4 berkedip", "");
    } else if (text.indexOf("2") >= 0 || text.indexOf("dua") >= 0) {
      stopVariasi();
      variasi2Active = true;
      mqtt.publish(mqtt_topic, "variasi2");
      bot.sendMessage(chat_id, "🎉 Variasi 2 aktif! Running Light 1→2→3→4", "");
    }
    return;
  }
  if (text.indexOf("suhu") >= 0 || text.indexOf("temperatur") >= 0) {
    bot.sendMessage(chat_id, "🌡 Suhu: " + String(suhu, 1) + " °C", ""); return;
  }
  if (text.indexOf("lembap") >= 0 || text.indexOf("humid") >= 0) {
    bot.sendMessage(chat_id, "💧 Kelembapan: " + String(kelembapan, 1) + " %", ""); return;
  }
  bool isOn  = text.indexOf("nyala") >= 0 || text.indexOf("hidup") >= 0 || text.indexOf("on") >= 0;
  bool isOff = text.indexOf("mati")  >= 0 || text.indexOf("off")   >= 0 || text.indexOf("padam") >= 0;
  if (!isOn && !isOff) {
    bot.sendMessage(chat_id, "Perintah tidak dikenali. Ketik /start untuk menu.", ""); return;
  }
  if (lampu == -1) {
    stopVariasi(); setAllRelay(isOn);
    mqtt.publish(mqtt_topic, isOn ? "all_on" : "all_off");
    sendTelegramNotif("[TG] Semua lampu " + String(isOn ? "ON ⚡" : "OFF 🔴"));
  } else {
    stopVariasi(); setRelay(lampu, isOn);
    String cmd = "relay" + String(lampu+1) + (isOn ? "_on" : "_off");
    mqtt.publish(mqtt_topic, cmd.c_str());
    sendTelegramNotif("[TG] Lampu " + String(lampu+1) + (isOn ? " ON 💡" : " OFF 🔌"));
  }
}

// ==================== HANDLE TELEGRAM ====================
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id     = String(bot.messages[i].chat_id);
    String text        = bot.messages[i].text;
    String name        = bot.messages[i].from_name;
    String type        = bot.messages[i].type;
    String callback_id = String(bot.messages[i].message_id);

    if (chat_id != CHAT_ID) { bot.sendMessage(chat_id, "⛔ Unauthorized", ""); continue; }
    if (type == "callback_query") { handleCallbackQuery(chat_id, callback_id, text); continue; }

    if (text == "/start") {
      String msg  = "👋 Halo " + name + "!\n\n";
      msg += "/lampu  - Kontrol relay 🎛\n";
      msg += "/allon  - Semua ON ⚡\n";
      msg += "/alloff - Semua OFF 🔴\n";
      msg += "/status - Status lampu 📋\n";
      msg += "/sensor - Data sensor 🌡\n";
      msg += "/waktu  - Waktu sekarang 🕐\n";
      msg += "/ip     - IP ESP32 🌐\n\n";
      msg += "/lampu1_on /lampu1_off\n";
      msg += "/lampu2_on /lampu2_off\n";
      msg += "/lampu3_on /lampu3_off\n";
      msg += "/lampu4_on /lampu4_off\n\n";
      msg += "Atau ketik bebas:\n";
      msg += "- Nyalakan lampu 1\n- Matikan semua\n- Berapa suhu?\n- Variasi 1";
      bot.sendMessage(chat_id, msg, "");
    }
    else if (text == "/lampu")  { sendRelayMenu(chat_id); }
    else if (text == "/allon")  { stopVariasi(); setAllRelay(true);  mqtt.publish(mqtt_topic,"all_on");  sendTelegramNotif("[TG] Semua lampu ON ⚡"); }
    else if (text == "/alloff") { stopVariasi(); setAllRelay(false); mqtt.publish(mqtt_topic,"all_off"); sendTelegramNotif("[TG] Semua lampu OFF 🔴"); }
    else if (text == "/status") { sendStatus(chat_id); }
    else if (text == "/sensor") {
      String msg = "🌡 Suhu: " + String(suhu, 1) + " °C\n💧 Kelembapan: " + String(kelembapan, 1) + " %\n🕐 " + getTime() + " WIB";
      bot.sendMessage(chat_id, msg, "");
    }
    else if (text == "/waktu") { bot.sendMessage(chat_id, "🕐 " + getTime() + " WIB", ""); }
    else if (text == "/ip")    { bot.sendMessage(chat_id, "🌐 IP: " + WiFi.localIP().toString(), ""); }
    else if (text == "/lampu1_on")  { stopVariasi(); setRelay(0, true);  mqtt.publish(mqtt_topic,"relay1_on");  sendTelegramNotif("[TG] Lampu 1 ON 💡"); }
    else if (text == "/lampu1_off") { stopVariasi(); setRelay(0, false); mqtt.publish(mqtt_topic,"relay1_off"); sendTelegramNotif("[TG] Lampu 1 OFF 🔌"); }
    else if (text == "/lampu2_on")  { stopVariasi(); setRelay(1, true);  mqtt.publish(mqtt_topic,"relay2_on");  sendTelegramNotif("[TG] Lampu 2 ON 💡"); }
    else if (text == "/lampu2_off") { stopVariasi(); setRelay(1, false); mqtt.publish(mqtt_topic,"relay2_off"); sendTelegramNotif("[TG] Lampu 2 OFF 🔌"); }
    else if (text == "/lampu3_on")  { stopVariasi(); setRelay(2, true);  mqtt.publish(mqtt_topic,"relay3_on");  sendTelegramNotif("[TG] Lampu 3 ON 💡"); }
    else if (text == "/lampu3_off") { stopVariasi(); setRelay(2, false); mqtt.publish(mqtt_topic,"relay3_off"); sendTelegramNotif("[TG] Lampu 3 OFF 🔌"); }
    else if (text == "/lampu4_on")  { stopVariasi(); setRelay(3, true);  mqtt.publish(mqtt_topic,"relay4_on");  sendTelegramNotif("[TG] Lampu 4 ON 💡"); }
    else if (text == "/lampu4_off") { stopVariasi(); setRelay(3, false); mqtt.publish(mqtt_topic,"relay4_off"); sendTelegramNotif("[TG] Lampu 4 OFF 🔌"); }
    else { parseNaturalCommand(chat_id, text); }
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(100);

  for (int r = 0; r < 4; r++) {
    pinMode(relayPin[r], OUTPUT);
    digitalWrite(relayPin[r], HIGH);
  }
  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  secureClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());

  configTime(gmtOffset, daylightOffset, ntpServer);
  Serial.print("Sinkronisasi NTP");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) { Serial.print("."); delay(1000); retry++; }
  Serial.println("\nWaktu: " + getTime());

  mqtt.setServer(mqtt_broker, mqtt_port);
  mqtt.setCallback(mqttCallback);

  server.on("/state", HTTP_GET,     handleState);
  server.on("/relay", HTTP_POST,    handleRelay);
  server.on("/all",   HTTP_POST,    handleAll);
  server.on("/state", HTTP_OPTIONS, handleOptions);
  server.on("/relay", HTTP_OPTIONS, handleOptions);
  server.on("/all",   HTTP_OPTIONS, handleOptions);
  server.begin();
  Serial.println("Web server started!");

  sendTelegramNotif("✅ ESP32 Online!\n🌐 IP: " + WiFi.localIP().toString());
}

// ==================== LOOP ====================
void loop() {
  server.handleClient();

  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();

  // Variasi 1 — Lampu 1&3 vs 2&4 (timer sendiri)
  if (variasi1Active && millis() - lastVariasi1Switch > variasiDelay1) {
    variasiState = !variasiState;
    setRelay(0, variasiState);  setRelay(1, !variasiState);
    setRelay(2, variasiState);  setRelay(3, !variasiState);
    lastVariasi1Switch = millis();
  }

  // Variasi 2 — Running Light 1→2→3→4 (timer sendiri)
  if (variasi2Active && millis() - lastVariasi2Switch > variasiDelay2) {
    setAllRelay(false);
    setRelay(variasiStep % 4, true);
    variasiStep++;
    if (variasiStep >= 4) variasiStep = 0;
    lastVariasi2Switch = millis();
  }

  if (millis() - lastDHTRead > 2000) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) { kelembapan = h; suhu = t; }
    lastDHTRead = millis();
  }

  if (millis() - lastSensorSend > 5000) {
    String data = "Suhu: " + String(suhu, 1) + "C | Kelembapan: " + String(kelembapan, 1) + "%";
    mqtt.publish(mqtt_sensor, data.c_str());
    lastSensorSend = millis();
  }

  if (millis() - lastTimeBotRan > 1000) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }
}