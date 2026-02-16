#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>

/*
  =========================
  Konfiguration
  =========================
*/

#define SETUP_PIN 9          // Setup-Taster (LOW = Setup-Modus)
#define ONE_MINUTE 60000UL
#define WIFI_TIMEOUT 15000UL

/*
  =========================
  Dummy Temperatursensor
  =========================
*/

class TempSensor {
public:
  void begin() {}
  void setWaitForConversion(bool) {}
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
Preferences prefs;

/*
  =========================
  Zeitsteuerung
  =========================
*/

const unsigned long intervalTemp = ONE_MINUTE;
unsigned long prevTemp = 0;
bool tmpRequested = false;
const unsigned long DS_delay = 750;

/*
  =========================
  Setup
  =========================
*/

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- ESP32-C3 Sensorlogger (RTC + SNTP) ---");

  pinMode(SETUP_PIN, INPUT_PULLUP);

  tempSensors.begin();

  startLittleFS();
  listFilesSerial();

  if (!startWiFi()) {
    startConfigPortal();   // blockierend bis WLAN gesetzt
  }

  startSNTP();             // <<< echte RTC + SNTP
  waitForTime();           // <<< warten bis Zeit gültig

  startServer();
}

/*
  =========================
  Loop
  =========================
*/

void loop() {
  unsigned long now = millis();

  if (now - prevTemp > intervalTemp) {
    tempSensors.requestTemperatures();
    tmpRequested = true;
    prevTemp = now;
  }

  if (tmpRequested && now - prevTemp > DS_delay) {
    tmpRequested = false;

    float temp = tempSensors.getTempCByIndex(0);
    temp = round(temp * 100) / 100.0;

    // Unix-Time direkt aus RTC (GMT/UTC)
    time_t ts = time(nullptr);

    File f = LittleFS.open("/data.csv", "a");
    f.printf("%lu;%.2f;;;;;\n", (uint32_t)ts, temp);
    f.close();

    Serial.printf("LOG: %lu ; %.2f\n", (uint32_t)ts, temp);
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

  // Setup erzwingen per Taster oder wenn keine Daten vorhanden
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
  =========================
  Setup-Portal (AP + WLAN Scan)
  =========================
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

  // Blockierend im Setup-Modus
  while (true) server.handleClient();
}

/*
  =========================
  RTC + SNTP
  =========================
*/

void startSNTP() {
  // GMT / UTC → offset = 0, daylight = 0
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("SNTP gestartet (UTC / GMT)");
}

void waitForTime() {
  Serial.print("Warte auf Zeit-Sync");
  time_t now;
  do {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  } while (now < 1700000000);   // grobe Plausibilitätsgrenze (~2023)

  Serial.println("\nZeit synchronisiert!");
  Serial.print("Unix-Time: ");
  Serial.println((uint32_t)now);
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

  // gzip bevorzugen
  if (LittleFS.exists(gzPath)) path = gzPath;
  if (!LittleFS.exists(path)) return false;

  File file = LittleFS.open(path, "r");
  server.streamFile(file, contentType);
  file.close();
  return true;
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
