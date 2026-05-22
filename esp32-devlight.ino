// DevLight — ESP32 三色灯做成 Claude Code + Codex 开发状态指示灯
// 灯色:绿常亮=完成/空闲;黄闪=开发中;红闪=等确认;三灯慢闪=离线/未配网
// 接线:- ->GND, G->GPIO25, Y->GPIO26, R->GPIO27(共阴,高电平点亮)
// HTTP:GET /set?agent=<claude|codex>&state=<idle|working|confirm>;GET / 看状态
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "devlight_types.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS ""
#endif
#ifndef DEVICE_NAME
  #define DEVICE_NAME ""
#endif

const int PIN_G = 25, PIN_Y = 26, PIN_R = 27;

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
bool portalMode = false;
unsigned long lastWifiCheck = 0;

SubState claudeState = ST_IDLE;
SubState codexState  = ST_IDLE;
const char* subName(SubState s){ return s==ST_CONFIRM?"confirm":s==ST_WORKING?"working":"idle"; }

// 主机名 = devlight-<设备名>;优先 NVS,其次 secrets 的 DEVICE_NAME,最后芯片 MAC 后4位hex(多盏灯不撞名)
String deviceHostname() {
  String name = prefs.getString("name", DEVICE_NAME);
  if (name.length() == 0) {
    uint64_t mac = ESP.getEfuseMac();
    char suf[5]; snprintf(suf, sizeof(suf), "%04x", (uint16_t)(mac & 0xFFFF));
    name = String(suf);
  }
  return String("devlight-") + name;
}

Agg aggregate() {
  if (claudeState==ST_CONFIRM || codexState==ST_CONFIRM) return AGG_RED;
  if (claudeState==ST_WORKING || codexState==ST_WORKING) return AGG_YELLOW;
  return AGG_GREEN;
}
const char* aggName(Agg a){ return a==AGG_RED?"red":a==AGG_YELLOW?"yellow":"green"; }

bool parseState(const String& v, SubState& out) {
  if (v=="idle"){out=ST_IDLE;return true;}
  if (v=="working"){out=ST_WORKING;return true;}
  if (v=="confirm"){out=ST_CONFIRM;return true;}
  return false;
}

void handleRoot() {
  String body = String("claude=") + subName(claudeState) +
                "\ncodex=" + subName(codexState) +
                "\nagg=" + aggName(aggregate()) + "\n";
  server.send(200, "text/plain", body);
}

void handleSet() {
  String agent = server.arg("agent");
  String state = server.arg("state");
  SubState parsed;
  if ((agent!="claude" && agent!="codex") || !parseState(state, parsed)) {
    server.send(400, "text/plain", "bad agent/state\n");
    return;
  }
  if (agent=="claude") claudeState = parsed; else codexState = parsed;
  Serial.printf("set %s=%s -> agg=%s\n", agent.c_str(), state.c_str(), aggName(aggregate()));
  server.send(200, "text/plain", "ok\n");
}

// ---------- captive portal 配网 ----------
void handlePortalRoot() {
  server.send(200, "text/html",
    "<h3>DevLight 配网</h3>"
    "<form action='/save' method='POST'>"
    "WiFi 名称(SSID):<br><input name='ssid'><br>"
    "WiFi 密码:<br><input name='pass' type='password'><br>"
    "设备名(如 desk,可留空):<br><input name='name'><br><br>"
    "<button type='submit'>保存并重启</button></form>");
}
void handlePortalSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String name = server.arg("name");
  if (ssid.length()==0) { server.send(400,"text/plain","ssid required\n"); return; }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("name", name);
  server.send(200, "text/html", "<p>已保存,正在重启...</p>");
  delay(1500);
  ESP.restart();
}
void startPortal() {
  portalMode = true;
  Serial.println("Starting captive portal AP: DevLight-Setup");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("DevLight-Setup");
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(apIP);
  dnsServer.start(53, "*", apIP);
  server.on("/", handlePortalRoot);
  server.on("/save", HTTP_POST, handlePortalSave);
  server.onNotFound(handlePortalRoot);
  server.begin();
}

void connectWifi() {
  String ssid = prefs.getString("ssid", WIFI_SSID);
  String pass = prefs.getString("pass", WIFI_PASS);
  if (ssid.length()==0) { Serial.println("No WiFi creds -> portal."); return; }
  String host = deviceHostname();
  Serial.printf("Connecting to '%s' as %s ...\n", ssid.c_str(), host.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(host.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(250); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    if (MDNS.begin(host.c_str())) { MDNS.addService("http","tcp",80); Serial.printf("mDNS: %s.local\n", host.c_str()); }
  } else {
    Serial.println("WiFi connect FAILED");
  }
}

void renderLeds() {
  if (WiFi.status() != WL_CONNECTED) {        // 离线/配网:三灯一起慢闪 ~0.5Hz
    bool slowOn = (millis() / 1000) % 2 == 0;
    digitalWrite(PIN_G, slowOn?HIGH:LOW);
    digitalWrite(PIN_Y, slowOn?HIGH:LOW);
    digitalWrite(PIN_R, slowOn?HIGH:LOW);
    return;
  }
  Agg a = aggregate();
  bool blinkOn = (millis() / 250) % 2 == 0;   // 2Hz
  digitalWrite(PIN_G, a==AGG_GREEN ? HIGH : LOW);
  digitalWrite(PIN_Y, (a==AGG_YELLOW && blinkOn) ? HIGH : LOW);
  digitalWrite(PIN_R, (a==AGG_RED   && blinkOn) ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_G, OUTPUT); pinMode(PIN_Y, OUTPUT); pinMode(PIN_R, OUTPUT);
  prefs.begin("wifi", false);
  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    server.on("/", handleRoot);
    server.on("/set", handleSet);
    server.begin();
  } else {
    startPortal();
  }
}

void loop() {
  if (portalMode) { dnsServer.processNextRequest(); server.handleClient(); renderLeds(); return; }
  server.handleClient();
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi lost, reconnecting..."); WiFi.reconnect(); }
  }
  renderLeds();
}
