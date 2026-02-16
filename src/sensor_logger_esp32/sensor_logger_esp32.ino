#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>

/*
  =========================
  Konfiguration
  =========================
*/

// Setup-Taster: beim Boot gedrückt halten → WLAN Setup AP starten
#define SETUP_PIN 9     // GPIO anpassen falls nötig

#define ONE_HOUR 3600000UL
#define WIFI_TIMEOUT 15000UL

/*
  =========================
  Dummy Temperatursensor
  =========================
*/

class TempSensor {
public:
  TempSensor() {}
  void setWaitForConversion(bool) {}
  void begin() {}
  uint8_t getDeviceCount() { return 1; }
  void requestTemperatures() {}
  float getTempCByIndex(uint8_t) {
    return random(200, 250) / 10.0;
  }
};

TempSensor tempSensors;

/*
  =========================
  Globale Objekte
  =========================
*/

WebServer server(80);
WiFiUDP UDP;
Preferences prefs;

IPAddress timeServerIP;
const char* ntpServerName = "time.nist.gov";

const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];

/*
  =========================
  Zeitsteuerung
  =========================
*/

const unsigned long intervalNTP = ONE_HOUR;
unsigned long prevNTP = 0;
unsigned long lastNTPResponse = millis();

const unsigned long intervalTemp = 60000;
unsigned long prevTemp = 0;
bool tmpRequested = false;
const unsigned long DS_delay = 750;

uint32_t timeUNIX = 0;

/*
  =========================
  Setup
  =========================
*/

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- ESP32-C3 Sensorlogger ---");

  pinMode(SETUP_PIN, INPUT_PULLUP);

  tempSensors.begin();

  startLittleFS();
  listFilesSerial();

  if (!startWiFi()) {
    startConfigPortal();
  }

  startServer();
  startUDP();

  WiFi.hostByName(ntpServerName, timeServerIP);
  sendNTPpacket(timeServerIP);
}

/*
  =========================
  Loop
  =========================
*/

void loop() {
  unsigned long now = millis();

  if (now - prevNTP > intervalNTP) {
    prevNTP = now;
    sendNTPpacket(timeServerIP);
  }

  uint32_t t = getTime();
  if (t) {
    timeUNIX = t;
    lastNTPResponse = millis();
  }

  if (timeUNIX && now - prevTemp > intervalTemp) {
    tempSensors.requestTemperatures();
    tmpRequested = true;
    prevTemp = now;
  }

  if (tmpRequested && now - prevTemp > DS_delay) {
    tmpRequested = false;
    float temp = tempSensors.getTempCByIndex(0);
    temp = round(temp * 100) / 100.0;

    uint32_t ts = timeUNIX + (millis() - lastNTPResponse) / 1000;

    File f = LittleFS.open("/data.csv", "a");
    f.printf("%lu;%.2f;;;;;\n", ts, temp);
    f.close();
  }

  server.handleClient();
}

/*
  =========================
  WLAN Management
  =========================
*/

bool startWiFi() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid == "" || digitalRead(SETUP_PIN) == LOW) return false;

  Serial.print("Connecting to: ");
  Serial.println(ssid);

  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

/*
  Setup Access Point + WLAN Scan + Mini-Konfig-Portal
*/

void startConfigPortal() {
  WiFi.softAP("Sensorlogger-Setup");

  Serial.println("\nSETUP MODE");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, []() {
    String html =
      "<h2>WLAN Setup</h2>"
      "<form action='/save' method='POST'>"
      "SSID:<br><select name='s'>";

    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
      html += "<option>" + WiFi.SSID(i) + "</option>";

    html +=
      "</select><br>Passwort:<br>"
      "<input name='p' type='password'><br><br>"
      "<button>Speichern</button></form>";

    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    prefs.begin("wifi", false);
    prefs.putString("ssid", server.arg("s"));
    prefs.putString("pass", server.arg("p"));
    prefs.end();

    server.send(200, "text/plain", "Gespeichert. Reboot...");
    delay(1500);
    ESP.restart();
  });

  server.begin();

  while (true) server.handleClient();
}

/*
  =========================
  LittleFS
  =========================
*/

void startLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed!");
    ESP.restart();
  }
}

void listFilesSerial() {
  Serial.println("\nLittleFS Inhalt:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
    file = root.openNextFile();
  }
}

/*
  =========================
  HTTP Server
  =========================
*/

void startServer() {

  // HTML Datei-Liste
  server.on("/list", HTTP_GET, []() {
    String html = "<h2>Dateien</h2><ul>";

    File root = LittleFS.open("/");
    File file = root.openNextFile();

    while (file) {
      html += "<li><a href='" + String(file.name()) + "'>" +
              String(file.name()) + "</a> (" + String(file.size()) + ")</li>";
      file = root.openNextFile();
    }

    html += "</ul>";
    server.send(200, "text/html", html);
  });

  server.onNotFound(handleNotFound);
  server.begin();
}

/*
  =========================
  File Handling
  =========================
*/

void handleNotFound() {
  if (!handleFileRead(server.uri()))
    server.send(404, "text/plain", "404");
}

bool handleFileRead(String path) {
  if (path.endsWith("/")) path += "index.html";

  String contentType = getContentType(path);
  String gzPath = path + ".gz";

  if (LittleFS.exists(gzPath)) path = gzPath;
  if (!LittleFS.exists(path)) return false;

  File file = LittleFS.open(path, "r");
  server.streamFile(file, contentType);
  file.close();
  return true;
}

/*
  =========================
  UDP + NTP
  =========================
*/

void startUDP() {
  UDP.begin(123);
}

unsigned long getTime() {
  if (!UDP.parsePacket()) return 0;

  UDP.read(packetBuffer, NTP_PACKET_SIZE);

  uint32_t ntp = (packetBuffer[40] << 24) |
                 (packetBuffer[41] << 16) |
                 (packetBuffer[42] << 8) |
                 packetBuffer[43];

  return ntp - 2208988800UL;
}

void sendNTPpacket(IPAddress& address) {
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;

  UDP.beginPacket(address, 123);
  UDP.write(packetBuffer, NTP_PACKET_SIZE);
  UDP.endPacket();
}

/*
  =========================
  Helper
  =========================
*/

String getContentType(String filename) {
  if (filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css")) return "text/css";
  if (filename.endsWith(".js")) return "application/javascript";
  if (filename.endsWith(".png")) return "image/png";
  if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}
