#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Preferences.h>

#define RELAY_PIN 0
#define LED_PIN 2

#define AP_SSID "ESP-Relay"
#define WEB_PORT 80
#define CONFIG_NS "relay_cfg"
#define MAX_SCHEDULES 6
#define WIFI_CONNECT_ATTEMPTS 30
#define WIFI_CONNECT_DELAY_MS 500
#define WIFI_CHECK_INTERVAL_MS 30000
#define LED_BLINK_INTERVAL_MS 500
#define NTP_SYNC_INTERVAL_MS 3600000UL
#define MIN_INTERVAL_MIN 1
#define MAX_INTERVAL_MIN 1440

#define NTP_MIN_VALID_EPOCH 1700000000L
#define SCHEDULE_TYPE_TIME 1
#define SCHEDULE_TYPE_INTERVAL 2

struct Schedule {
  bool enabled;
  uint8_t type;
  uint8_t hour;
  uint8_t minute;
  uint16_t intervalMin;
  bool turnOn;
};

Preferences prefs;
ESP8266WebServer server(WEB_PORT);

String wifiSSID;
String wifiPassword;
Schedule schedules[MAX_SCHEDULES];

bool relayState = false;
bool apModeActive = false;
bool ntpSynced = false;
bool timeScheduleReady = false;
unsigned long lastWiFiCheck = 0;
unsigned long lastNtpSync = 0;
int lastExecutedMinute[MAX_SCHEDULES];
unsigned long lastIntervalTrigger[MAX_SCHEDULES];

const char PAGE_STYLE[] PROGMEM =
  "body{font-family:sans-serif;margin:0;padding:16px;background:#f4f4f4;color:#222;}"
  ".wrap{max-width:480px;margin:0 auto;background:#fff;border:1px solid #ccc;padding:16px;}"
  "h1,h2{font-size:18px;margin:0 0 12px;}"
  "p,label,td{font-size:14px;}"
  ".tabs{display:flex;border-bottom:1px solid #ccc;margin:0 0 16px;}"
  ".tab{flex:1;text-align:center;padding:8px 12px;text-decoration:none;color:#666;"
  "font-size:14px;border:1px solid #ccc;border-bottom:none;background:#f0f0f0;margin-right:-1px;}"
  ".tab:last-child{margin-right:0;}"
  ".tab.active{color:#222;background:#fff;font-weight:bold;}"
  ".status{padding:10px;border:1px solid #ccc;margin-bottom:12px;}"
  ".status.on{background:#e8f5e9;border-color:#4caf50;}"
  ".status.off{background:#ffebee;border-color:#f44336;}"
  ".btn{display:inline-block;padding:8px 16px;margin:4px 4px 4px 0;border:1px solid #666;"
  "background:#eee;color:#222;text-decoration:none;font-size:14px;}"
  ".btn-primary{background:#1976d2;color:#fff;border-color:#1565c0;}"
  ".btn-danger{background:#c62828;color:#fff;border-color:#b71c1c;}"
  "input,select{width:100%;box-sizing:border-box;padding:6px;margin:4px 0 10px;}"
  "table{width:100%;border-collapse:collapse;margin-top:8px;}"
  "td,th{border:1px solid #ddd;padding:6px;text-align:left;}"
  "th{background:#f0f0f0;}"
  ".hint{color:#666;font-size:12px;margin:4px 0;line-height:1.4;}"
  ".warn{color:#b71c1c;font-size:12px;margin:4px 0;}"
  ".sch{width:100%;border-collapse:collapse;margin-top:6px;font-size:13px;}"
  ".sch td,.sch th{border:1px solid #ddd;padding:4px 6px;vertical-align:middle;}"
  ".sch th{background:#f0f0f0;font-weight:normal;}"
  ".sch input[type=number],.sch select{width:auto;margin:0;padding:3px 4px;font-size:13px;}"
  ".sch input[type=checkbox],.sch input[type=radio]{width:auto;margin:0 2px 0 0;}"
  ".sch label{margin:0 6px 0 0;white-space:nowrap;}"
  ".sch .num{width:2.8em;}"
  ".sch .iv{width:3.5em;}"
  ".hidden{display:none;}"
  ".divider{border:0;border-top:1px solid #ccc;margin:20px 0;}"
  ".block{border:1px solid #ddd;padding:12px;}"
  ".block-danger{border-color:#d9b3b3;background:#faf6f6;}"
  ".block h2{font-size:16px;margin:0 0 8px;}";

void setRelay(bool on) {
  relayState = on;
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
}

void updateLed() {
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, LOW);
    return;
  }

  static unsigned long lastToggle = 0;
  static bool ledOn = false;
  unsigned long now = millis();
  if (now - lastToggle >= LED_BLINK_INTERVAL_MS) {
    lastToggle = now;
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn ? LOW : HIGH);
  }
}

bool hasEnabledTimeSchedules() {
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (schedules[i].enabled && schedules[i].type == SCHEDULE_TYPE_TIME) {
      return true;
    }
  }
  return false;
}

void disarmTimeSchedules() {
  timeScheduleReady = false;
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    lastExecutedMinute[i] = -1;
  }
}

void armTimeSchedulesAfterNtp() {
  if (timeScheduleReady) return;
  timeScheduleReady = true;
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    lastExecutedMinute[i] = -1;
  }
  if (hasEnabledTimeSchedules()) {
    Serial.println(F("NTP 已同步，按时间任务开始检查"));
  }
}

bool isTimeValid() {
  return time(nullptr) >= NTP_MIN_VALID_EPOCH;
}

void loadConfig() {
  prefs.begin(CONFIG_NS, true);
  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPassword = prefs.getString("wifi_pwd", "");

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    String prefix = "sch" + String(i) + "_";
    schedules[i].enabled = prefs.getBool((prefix + "en").c_str(), false);
    schedules[i].type = prefs.getUChar((prefix + "type").c_str(), SCHEDULE_TYPE_INTERVAL);
    schedules[i].hour = prefs.getUChar((prefix + "h").c_str(), 0);
    schedules[i].minute = prefs.getUChar((prefix + "m").c_str(), 0);
    schedules[i].intervalMin = prefs.getUShort((prefix + "iv").c_str(), 60);
    schedules[i].turnOn = prefs.getBool((prefix + "on").c_str(), true);

    if (schedules[i].type != SCHEDULE_TYPE_TIME &&
        schedules[i].type != SCHEDULE_TYPE_INTERVAL) {
      schedules[i].type = SCHEDULE_TYPE_INTERVAL;
    }
    if (schedules[i].hour > 23) schedules[i].hour = 0;
    if (schedules[i].minute > 59) schedules[i].minute = 0;
    if (schedules[i].intervalMin < MIN_INTERVAL_MIN) schedules[i].intervalMin = MIN_INTERVAL_MIN;
    if (schedules[i].intervalMin > MAX_INTERVAL_MIN) schedules[i].intervalMin = MAX_INTERVAL_MIN;
  }
  prefs.end();
}

void saveConfig() {
  prefs.begin(CONFIG_NS, false);
  prefs.putString("wifi_ssid", wifiSSID);
  prefs.putString("wifi_pwd", wifiPassword);

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    String prefix = "sch" + String(i) + "_";
    prefs.putBool((prefix + "en").c_str(), schedules[i].enabled);
    prefs.putUChar((prefix + "type").c_str(), schedules[i].type);
    prefs.putUChar((prefix + "h").c_str(), schedules[i].hour);
    prefs.putUChar((prefix + "m").c_str(), schedules[i].minute);
    prefs.putUShort((prefix + "iv").c_str(), schedules[i].intervalMin);
    prefs.putBool((prefix + "on").c_str(), schedules[i].turnOn);
  }
  prefs.end();
}

void clearConfig() {
  prefs.begin(CONFIG_NS, false);
  prefs.clear();
  prefs.end();
}

void startAPMode() {
  if (apModeActive) return;
  WiFi.softAP(AP_SSID);
  apModeActive = true;
  Serial.println(F("AP 已开启: 192.168.4.1"));
}

void stopAPMode() {
  if (!apModeActive) return;
  WiFi.softAPdisconnect(true);
  apModeActive = false;
  Serial.println(F("AP 已关闭"));
}

bool connectWiFi() {
  if (wifiSSID.length() == 0) {
    Serial.println(F("未配置 WiFi"));
    return false;
  }

  Serial.print(F("连接 WiFi: "));
  Serial.println(wifiSSID);

  WiFi.mode(WIFI_AP_STA);
  if (apModeActive) {
    WiFi.softAP(AP_SSID);
  }

  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  for (int i = 0; i < WIFI_CONNECT_ATTEMPTS; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print(F("已连接, IP: "));
      Serial.println(WiFi.localIP());
      stopAPMode();
      return true;
    }
    updateLed();
    delay(WIFI_CONNECT_DELAY_MS);
  }

  Serial.println(F("WiFi 连接失败"));
  startAPMode();
  return false;
}

void syncNtp() {
  if (WiFi.status() != WL_CONNECTED) return;

  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  for (int i = 0; i < 20; i++) {
    if (isTimeValid()) {
      bool firstSync = !ntpSynced;
      ntpSynced = true;
      lastNtpSync = millis();
      Serial.println(F("NTP 同步完成"));
      if (firstSync || !timeScheduleReady) {
        armTimeSchedulesAfterNtp();
      }
      return;
    }
    delay(200);
  }
  ntpSynced = false;
  timeScheduleReady = false;
  Serial.println(F("NTP 同步失败"));
}

String ipAddressText() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  if (apModeActive) {
    return F("192.168.4.1 (AP)");
  }
  return F("-");
}

String wifiStatusText() {
  if (WiFi.status() == WL_CONNECTED) {
    return String(F("已连接 (")) + WiFi.SSID() + F(")");
  }
  if (wifiSSID.length() == 0) {
    return F("未配置");
  }
  return F("未连接");
}

String currentTimeText() {
  if (!ntpSynced) return F("未同步");
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (!t) return F("-");
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

void appendPageHead(String& html, const char* title, uint8_t activeTab) {
  html += F("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>");
  html += title;
  html += F("</title><style>");
  html += FPSTR(PAGE_STYLE);
  html += F("</style></head><body><div class='wrap'>");
  html += F("<div class='tabs'>");
  html += F("<a href='/' class='tab");
  if (activeTab == 0) html += F(" active");
  html += F("'>控制</a>");
  html += F("<a href='/settings' class='tab");
  if (activeTab == 1) html += F(" active");
  html += F("'>设置</a></div>");
  html += F("<h1>");
  html += title;
  html += F("</h1>");
}

void appendPageFoot(String& html) {
  html += F("</div></body></html>");
}

String generateRootPage() {
  String html;
  html.reserve(1200);
  appendPageHead(html, "继电器控制", 0);

  html += F("<div class='status ");
  html += relayState ? "on" : "off";
  html += F("'>状态: ");
  html += relayState ? "开启" : "关闭";
  html += F("</div>");

  html += F("<a class='btn btn-primary' href='/relay?state=on'>开启</a>");
  html += F("<a class='btn' href='/relay?state=off'>关闭</a>");

  html += F("<table><tr><th>项目</th><th>值</th></tr>");
  html += F("<tr><td>IP</td><td>");
  html += htmlEscape(ipAddressText());
  html += F("</td></tr><tr><td>WiFi</td><td>");
  html += htmlEscape(wifiStatusText());
  html += F("</td></tr><tr><td>时间</td><td>");
  html += htmlEscape(currentTimeText());
  html += F("</td></tr>");
  if (WiFi.status() == WL_CONNECTED) {
    html += F("<tr><td>信号</td><td>");
    html += String(WiFi.RSSI());
    html += F(" dBm</td></tr>");
  }
  html += F("</table>");

  appendPageFoot(html);
  return html;
}

String generateSettingsPage() {
  String html;
  html.reserve(3800);
  appendPageHead(html, "设置", 1);

  html += F("<form method='POST' action='/save'>");
  html += F("<h2>WiFi</h2>");
  html += F("<label>SSID</label>");
  html += F("<input name='wifi_ssid' value=\"");
  html += htmlEscape(wifiSSID);
  html += F("\">");
  html += F("<label>密码</label>");
  html += F("<input type='password' name='wifi_pwd' value=\"");
  html += htmlEscape(wifiPassword);
  html += F("\">");

  html += F("<h2>定时任务</h2>");
  html += F("<p class='hint'>类型二选一。按时间需 NTP 已同步。当前时间: ");
  html += htmlEscape(currentTimeText());
  html += F("</p>");
  if (!ntpSynced) {
    html += F("<p class='warn'>NTP 未同步，按时间选项暂不可用。</p>");
  }

  html += F("<script>"
    "function toggleTask(i){"
    "var t=document.querySelector('input[name=sch'+i+'_type]:checked');"
    "var time=document.getElementById('time'+i);"
    "var iv=document.getElementById('iv'+i);"
    "if(!t||!time||!iv)return;"
    "if(t.value==='1'){time.classList.remove('hidden');iv.classList.add('hidden');}"
    "else{time.classList.add('hidden');iv.classList.remove('hidden');}"
    "}"
    "</script>");

  html += F("<table class='sch'><tr><th>启用</th><th>类型</th><th>参数</th><th>动作</th></tr>");

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    bool isTime = schedules[i].type == SCHEDULE_TYPE_TIME;

    html += F("<tr><td><input type='checkbox' name='sch");
    html += String(i);
    html += F("_en' value='1'");
    if (schedules[i].enabled) html += F(" checked");
    html += F("></td><td>");
    html += F("<label><input type='radio' name='sch");
    html += String(i);
    html += F("_type' value='1'");
    if (isTime) html += F(" checked");
    if (!ntpSynced) html += F(" disabled");
    html += F(" onclick='toggleTask(");
    html += String(i);
    html += F(")'>时间</label>");
    html += F("<label><input type='radio' name='sch");
    html += String(i);
    html += F("_type' value='2'");
    if (!isTime) html += F(" checked");
    html += F(" onclick='toggleTask(");
    html += String(i);
    html += F(")'>间隔</label></td><td>");

    html += F("<span id='time");
    html += String(i);
    html += F("' class='field-time");
    if (!isTime) html += F(" hidden");
    html += F("'><input class='num' type='number' min='0' max='23' name='sch");
    html += String(i);
    html += F("_h' value='");
    html += String(schedules[i].hour);
    html += F("'>:<input class='num' type='number' min='0' max='59' name='sch");
    html += String(i);
    html += F("_m' value='");
    html += String(schedules[i].minute);
    html += F("'></span>");

    html += F("<span id='iv");
    html += String(i);
    html += F("' class='field-interval");
    if (isTime) html += F(" hidden");
    html += F("'><input class='iv' type='number' min='");
    html += String(MIN_INTERVAL_MIN);
    html += F("' max='");
    html += String(MAX_INTERVAL_MIN);
    html += F("' name='sch");
    html += String(i);
    html += F("_iv' value='");
    html += String(schedules[i].intervalMin);
    html += F("'> 分</span></td><td>");

    html += F("<select name='sch");
    html += String(i);
    html += F("_on'><option value='1'");
    if (schedules[i].turnOn) html += F(" selected");
    html += F(">开</option><option value='0'");
    if (!schedules[i].turnOn) html += F(" selected");
    html += F(">关</option></select></td></tr>");
  }

  html += F("</table>");

  html += F("<button class='btn btn-primary' type='submit'>保存并重启</button>");
  html += F("</form>");

  html += F("<hr class='divider'>");
  html += F("<div class='block block-danger'>");
  html += F("<h2>恢复出厂设置</h2>");
  html += F("<p class='hint'>清除 WiFi、定时任务等全部配置，设备将重启。</p>");
  html += F("<a class='btn btn-danger' href='/reset' "
            "onclick=\"return confirm('确认恢复出厂设置？所有配置将被清除。')\">恢复出厂设置</a>");
  html += F("</div>");

  appendPageFoot(html);
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", generateRootPage());
}

void handleRelay() {
  String state = server.arg("state");
  if (state == "on") {
    setRelay(true);
    Serial.println(F("继电器: 开启"));
  } else if (state == "off") {
    setRelay(false);
    Serial.println(F("继电器: 关闭"));
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSettings() {
  server.send(200, "text/html; charset=UTF-8", generateSettingsPage());
}

void handleSave() {
  if (server.hasArg("wifi_ssid")) {
    wifiSSID = server.arg("wifi_ssid");
    wifiSSID.trim();
    if (wifiSSID.length() > 32) wifiSSID = wifiSSID.substring(0, 32);
  }
  if (server.hasArg("wifi_pwd")) {
    wifiPassword = server.arg("wifi_pwd");
    if (wifiPassword.length() > 64) wifiPassword = wifiPassword.substring(0, 64);
  }

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    String idx = String(i);
    schedules[i].enabled = server.hasArg("sch" + idx + "_en");

    String typeArg = server.arg("sch" + idx + "_type");
    if (typeArg == "1") {
      if (!ntpSynced) {
        server.send(400, "text/html; charset=UTF-8",
                    F("<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body>"
                      "<p>保存失败：按时间触发需先完成 NTP 同步。</p>"
                      "<p><a href='/settings'>返回设置</a></p></body></html>"));
        return;
      }
      schedules[i].type = SCHEDULE_TYPE_TIME;
    } else {
      schedules[i].type = SCHEDULE_TYPE_INTERVAL;
    }

    schedules[i].hour = constrain(server.arg("sch" + idx + "_h").toInt(), 0, 23);
    schedules[i].minute = constrain(server.arg("sch" + idx + "_m").toInt(), 0, 59);
    schedules[i].intervalMin = constrain(server.arg("sch" + idx + "_iv").toInt(),
                                         MIN_INTERVAL_MIN, MAX_INTERVAL_MIN);
    schedules[i].turnOn = server.arg("sch" + idx + "_on") != "0";

    if (schedules[i].enabled && schedules[i].type == SCHEDULE_TYPE_TIME && !ntpSynced) {
      String err = F("<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body>"
                     "<p>保存失败：任务 ");
      err += idx;
      err += F(" 使用按时间触发，但 NTP 尚未同步。</p>"
               "<p><a href='/settings'>返回设置</a></p></body></html>");
      server.send(400, "text/html; charset=UTF-8", err);
      return;
    }
  }

  saveConfig();
  Serial.println(F("配置已保存"));

  server.send(200, "text/html; charset=UTF-8",
              F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta http-equiv='refresh' "
                "content='3;url=/'></head><body><p>配置已保存，设备将重启。</p></body></html>"));
  delay(1000);
  ESP.restart();
}

void handleReset() {
  Serial.println(F("恢复出厂设置"));
  server.send(200, "text/html; charset=UTF-8",
              F("<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body>"
                "<p>配置已清除，设备将重启。</p></body></html>"));
  delay(500);
  clearConfig();
  delay(500);
  ESP.restart();
}

void logScheduleAction(int index, bool turnOn, const char* mode) {
  Serial.print(F("定时任务 "));
  Serial.print(index);
  Serial.print(F(" ("));
  Serial.print(mode);
  Serial.print(F("): "));
  Serial.println(turnOn ? F("开启") : F("关闭"));
}

void checkTimeSchedules() {
  // 掉电重启后须等 NTP 首次同步成功才启用按时间检查，避免未校时误触发
  if (!timeScheduleReady || !ntpSynced || WiFi.status() != WL_CONNECTED) return;
  if (!isTimeValid()) {
    ntpSynced = false;
    timeScheduleReady = false;
    return;
  }

  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (!t) return;

  int minuteOfDay = t->tm_hour * 60 + t->tm_min;

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (!schedules[i].enabled) continue;
    if (schedules[i].type != SCHEDULE_TYPE_TIME) continue;

    int targetMinute = schedules[i].hour * 60 + schedules[i].minute;
    if (minuteOfDay != targetMinute) continue;
    if (lastExecutedMinute[i] == minuteOfDay) continue;

    lastExecutedMinute[i] = minuteOfDay;
    setRelay(schedules[i].turnOn);
    logScheduleAction(i, schedules[i].turnOn, "time");
  }

  static int lastDayMinute = -1;
  if (minuteOfDay != lastDayMinute) {
    if (lastDayMinute >= 0 && minuteOfDay < lastDayMinute) {
      for (int j = 0; j < MAX_SCHEDULES; j++) {
        lastExecutedMinute[j] = -1;
      }
    }
    lastDayMinute = minuteOfDay;
  }
}

void checkIntervalSchedules() {
  unsigned long now = millis();

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (!schedules[i].enabled) continue;
    if (schedules[i].type != SCHEDULE_TYPE_INTERVAL) continue;

    unsigned long intervalMs = (unsigned long)schedules[i].intervalMin * 60000UL;
    if (lastIntervalTrigger[i] == 0) {
      lastIntervalTrigger[i] = now;
      continue;
    }
    if (now - lastIntervalTrigger[i] < intervalMs) continue;

    lastIntervalTrigger[i] = now;
    setRelay(schedules[i].turnOn);
    logScheduleAction(i, schedules[i].turnOn, "interval");
  }
}

void checkSchedules() {
  checkTimeSchedules();
  checkIntervalSchedules();
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/relay", handleRelay);
  server.on("/settings", handleSettings);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", handleReset);
  server.begin();
  Serial.println(F("Web 服务已启动"));
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  setRelay(false);
  digitalWrite(LED_PIN, HIGH);

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    lastExecutedMinute[i] = -1;
    lastIntervalTrigger[i] = 0;
  }
  disarmTimeSchedules();

  loadConfig();

  if (hasEnabledTimeSchedules()) {
    Serial.println(F("已加载按时间任务，等待 NTP 同步后生效"));
  }

  WiFi.mode(WIFI_AP_STA);
  startAPMode();

  if (wifiSSID.length() > 0) {
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      syncNtp();
    }
  } else {
    Serial.println(F("首次使用，请连接 AP 进行配网"));
  }

  setupWebServer();
}

void loop() {
  server.handleClient();

  static wl_status_t lastStatus = WL_DISCONNECTED;
  wl_status_t status = WiFi.status();

  if (status != lastStatus) {
    if (status == WL_CONNECTED) {
      Serial.println(F("WiFi 已连接"));
      stopAPMode();
      syncNtp();
    } else {
      Serial.println(F("WiFi 断开"));
      ntpSynced = false;
      disarmTimeSchedules();
      startAPMode();
    }
    lastStatus = status;
  }

  if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL_MS) {
    lastWiFiCheck = millis();
    if (wifiSSID.length() > 0 && WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
  }

  if (WiFi.status() == WL_CONNECTED && (!ntpSynced || millis() - lastNtpSync > NTP_SYNC_INTERVAL_MS)) {
    syncNtp();
  }

  checkSchedules();
  updateLed();
}
