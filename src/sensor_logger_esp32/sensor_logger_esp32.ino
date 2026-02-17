#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>

/*
  ============================================================================
    Configuration
  ============================================================================
*/

#define SETUP_PIN          9       // LOW = force WiFi setup portal
#define WIFI_TIMEOUT       15000   // ms
#define LOG_INTERVAL_MS    60000   // 1 minute logging interval
#define LOG_INTERVAL_MS    5000   // 5s logging interval

#define MAX_CHUNKS         16
#define CHUNK_SIZE_BYTES  32768   // 32 KB per log chunk

/*
  ============================================================================
    Dummy temperature sensor (replace later with real sensor)
  ============================================================================
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
  ============================================================================
    Global objects
  ============================================================================
*/

AsyncWebServer server(80);
Preferences prefs;

/*
  ============================================================================
    Logging state
  ============================================================================
*/

unsigned long lastLogTime = 0;
int currentChunk = 0;

/*
  ============================================================================
    Setup
  ============================================================================
*/

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n--- ESP32-C3 Sensor Logger (Async + RTC + Rolling Logs) ---");

  pinMode(SETUP_PIN, INPUT_PULLUP);

  tempSensors.begin();

  startLittleFS();
  listFilesSerial();

  if (!startWiFi()) {
    startConfigPortal();   // blocking until WiFi configured
  }

  startSNTP();
  waitForTime();

  detectCurrentChunk();

  startServer();
}

/*
  ============================================================================
    Main loop
  ============================================================================
*/

void loop() {
  unsigned long now = millis();

  if (now - lastLogTime > LOG_INTERVAL_MS) {
    lastLogTime = now;

    float temp = tempSensors.getTempCByIndex(0);
    temp = round(temp * 100) / 100.0;

    time_t ts = time(nullptr);  // RTC time (Unix UTC)

    writeLog(ts, temp);

    Serial.printf("LOG: %lu ; %.2f\n", (uint32_t)ts, temp);
  }
}

/*
  ============================================================================
    WiFi handling
  ============================================================================
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
  ============================================================================
    WiFi setup portal
  ============================================================================
*/

static const char WIFI_SETUP_HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>WiFi Setup</title>
<style>
body{font-family:Arial;text-align:center}
form{display:inline-block;text-align:left}
button{border:0;border-radius:.3rem;background:#1fa3ec;color:#fff;
line-height:2.4rem;font-size:1.1rem;width:100%}
input,select{width:100%;padding:6px;margin:0 0 8px;
font-size:1em;box-sizing:border-box;border:1px solid #ddd;border-radius:.2rem}
</style></head><body>

<h2>WiFi Setup</h2>

<form action="/save" method="POST">
SSID:<br>
<select name="s">
{{OPTIONS}}
</select>

Password:<br>
<input name="p" type="password">

<button>Save</button>
</form>

</body></html>
)rawliteral";

void startConfigPortal() {
  WiFi.disconnect(true);
  delay(200);

  WiFi.mode(WIFI_AP_STA);
  delay(200);

  WiFi.softAP("Sensorlogger-Setup");
  delay(300);

  Serial.println("\nSETUP MODE");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String options;

    Serial.println("Network scan:");
    int n = WiFi.scanNetworks();

    Serial.printf("Scan result: %d\n", n);

    for (int i = 0; i < n; i++) {
      options += "<option>";
      options += WiFi.SSID(i);
      options += "</option>";
      Serial.printf("%s (chan: %d)\n", WiFi.SSID(i).c_str(), WiFi.channel(i));
    }

    String html = FPSTR(WIFI_SETUP_HTML_PAGE);
    html.replace("{{OPTIONS}}", options);

    request->send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {

    prefs.begin("wifi", false);
    prefs.putString("ssid", request->arg("s"));
    prefs.putString("pass", request->arg("p"));
    prefs.end();

    request->send(200, "text/plain", "Saved. Rebooting...");
    delay(1500);
    ESP.restart();
  });

  server.begin();

  while (true) delay(100);
}

/*
  ============================================================================
    SNTP + RTC
  ============================================================================
*/

void startSNTP() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("SNTP started (UTC)");
}

void waitForTime() {
  Serial.print("Waiting for time sync");
  time_t now;

  do {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  } while (now < 1700000000);

  Serial.println("\nTime synchronized.");
}

/*
  ============================================================================
    LittleFS
  ============================================================================
*/

void startLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    ESP.restart();
  }
}

void listFilesSerial() {
  Serial.println("\nLittleFS content:");
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    Serial.printf("  %s (%d bytes)\n", f.name(), f.size());
    f = root.openNextFile();
  }
}

/*
  ============================================================================
    Rolling log handling
  ============================================================================
*/

void detectCurrentChunk() {
  currentChunk = 0;

  for (int i = 0; i < MAX_CHUNKS; i++) {
    String path = "/log_" + String(i) + ".csv";
    if (!LittleFS.exists(path)) break;

    File f = LittleFS.open(path, "r");
    if (f.size() < CHUNK_SIZE_BYTES) {
      currentChunk = i;
      f.close();
      return;
    }
    f.close();
  }

  currentChunk = MAX_CHUNKS - 1;
}

void rotateLogs() {
  Serial.println("Rotating log chunks");

  if (LittleFS.exists("/log_0.csv")) LittleFS.remove("/log_0.csv");

  for (int i = 1; i < MAX_CHUNKS; i++) {
    String oldName = "/log_" + String(i) + ".csv";
    String newName = "/log_" + String(i - 1) + ".csv";
    if (LittleFS.exists(oldName)) LittleFS.rename(oldName, newName);
  }

  currentChunk = MAX_CHUNKS - 1;
}

void writeLog(time_t ts, float temp) {
  String path = "/log_" + String(currentChunk) + ".csv";

  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    if (f.size() > CHUNK_SIZE_BYTES) {
      f.close();
      rotateLogs();
      path = "/log_" + String(currentChunk) + ".csv";
    } else {
      f.close();
    }
  }

  File f = LittleFS.open(path, "a");
  f.printf("%lu;%.2f;%.2f;%.2f;%.2f;%.2f;%.2f\n", (uint32_t)ts, temp, 2*temp, 45.0-1.2*temp, 2*(45.0-temp), 2.0+1.1*temp, 43.0-temp);
  f.close();
}

/*
  ============================================================================
    Async Web Server
  ============================================================================
*/

void startServer() {

  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request) {

    String html = "<h2>Files</h2><ul>";
    File root = LittleFS.open("/");
    File f = root.openNextFile();

    while (f) {
      html += "<li>" + String(f.name()) +
              " (" + String(f.size()) + ")</li>";
      f = root.openNextFile();
    }

    html += "</ul>";
    request->send(200, "text/html", html);
  });

  server.on("/data.csv", HTTP_GET, handleDownload);

  server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest *request) {

    String path = request->url();
    if (path.endsWith("/")) path += "index.html";

    String gzPath = path + ".gz";

    if (LittleFS.exists(gzPath)) {
      request->send(LittleFS, gzPath, String(), true);
      return;
    }

    request->send(404, "text/plain", "404");
  });

  server.begin();
  Serial.println("Async HTTP server started.");
}

/*
  ============================================================================
    Download handler (streams all chunks into one CSV)
  ============================================================================
*/

void handleDownload(AsyncWebServerRequest *request) {

  AsyncResponseStream *response =
      request->beginResponseStream("text/csv");

  response->addHeader("Content-Disposition",
                       "attachment; filename=\"data.csv\"");

  for (int i = 0; i < MAX_CHUNKS; i++) {
    String path = "/log_" + String(i) + ".csv";
    if (!LittleFS.exists(path)) continue;

    File f = LittleFS.open(path, "r");
    while (f.available()) response->write(f.read());
    f.close();
  }

  request->send(response);
}
