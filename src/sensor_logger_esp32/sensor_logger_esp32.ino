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
#define DEFAULT_LOG_INTERVAL_S    1200   // default: 20 minute logging interval
#define DEFAULT_NUM_OF_LOG_FILES    16   // default: 16 log chunks
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

File fsUploadFile;
SemaphoreHandle_t fsMutex;

uint32_t logInterval = DEFAULT_LOG_INTERVAL_S;
uint8_t numOfLogFiles = DEFAULT_NUM_OF_LOG_FILES;

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

  fsMutex = xSemaphoreCreateMutex();
  if (!fsMutex) {
    Serial.println("Failed to create FS mutex!");
    ESP.restart();
  }

  loadLogSettings();

  tempSensors.begin();

  startLittleFS();
  listFilesSerial();

  if (!startWiFi()) {
    startConfigPortal();   // blocking until WiFi configured
  }

  startSNTP();
  waitForTime();

  startServer();
}

/*
  ============================================================================
    Main loop
  ============================================================================
*/

void loop() {
  unsigned long now = millis();

  if (now - lastLogTime > (logInterval * 1000UL)) {
    lastLogTime = now;

    time_t ts = time(nullptr);  // RTC time (Unix UTC)

    float temp = tempSensors.getTempCByIndex(0);
    temp = 20+(temp-20)*.1 + 5* (ts%(3600*2))/(3600.0*2);
    temp = round(temp * 100) / 100.0;

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
  if(!root) {
    Serial.println("Failed to open root directory");
    return;
  }
  File f = root.openNextFile();
  while (f) {
    Serial.printf("  %s (%d bytes)\n", f.name(), f.size());
    f = root.openNextFile();
  }
}

/*
  ============================================================================
    Rolling log handling (FIFO: newest always log_0)
  ============================================================================
*/

/*
  On boot:
  - Remove files beyond numOfLogFiles
  - Nothing else required
*/
void enforceLogFileLimit() {
  Serial.println("Checking log file limits...");
  if (xSemaphoreTake(fsMutex, portMAX_DELAY)) {
    // Delete files exceeding numOfLogFiles
    for (int i = numOfLogFiles; i < 60; i++) {
      String path = "/log_" + String(i) + ".csv";
      if (LittleFS.exists(path)) {
        Serial.printf("Removing old file: %s\n", path.c_str());
        LittleFS.remove(path);
      }
    }
    xSemaphoreGive(fsMutex);
  }
}


/*
  Rotate logs:

  log_0 -> log_1
  log_1 -> log_2
  ...
  log_(MAX-2) -> log_(MAX-1)
  log_(MAX-1) gets deleted
*/
void rotateLogs() {
  Serial.println("Rotating log files (FIFO mode)");

  if (xSemaphoreTake(fsMutex, portMAX_DELAY)) {
    // Delete oldest file if it exists
    String oldest = "/log_" + String(numOfLogFiles - 1) + ".csv";
    if (LittleFS.exists(oldest)) {
      LittleFS.remove(oldest);
    }

  // Shift files backwards (from high to low)
    for (int i = numOfLogFiles - 2; i >= 0; i--) {

      String oldName = "/log_" + String(i) + ".csv";
      String newName = "/log_" + String(i + 1) + ".csv";

      if (LittleFS.exists(oldName)) {
        LittleFS.rename(oldName, newName);
      }
    }

    xSemaphoreGive(fsMutex);
  }
}


/*
  Always write to log_0.
  When it exceeds CHUNK_SIZE_BYTES:
  -> rotate
  -> create fresh log_0
*/
void writeLog(time_t ts, float temp) {
  if (!xSemaphoreTake(fsMutex, portMAX_DELAY))
    return;

  String path = "/log_0.csv";

  if (LittleFS.exists(path)) {

    File f = LittleFS.open(path, "r");
    if (f) {
      if (f.size() >= CHUNK_SIZE_BYTES) {
        f.close();
        xSemaphoreGive(fsMutex);   // Mutex freigeben vor Rotation
        rotateLogs();
        if (!xSemaphoreTake(fsMutex, portMAX_DELAY))
          return;
      } else {
        f.close();
      }
    } else {
      Serial.printf("Failed to open log file for size check: %s\n", path.c_str());
    }
  }

  File f = LittleFS.open(path, "a");
  if (f) {

    f.printf("%lu;%.2f;%.2f;%.2f;%.2f;%.2f;%.2f\n",
             (uint32_t)ts,
             temp,
             2 * temp,
             45.0 - 1.2 * temp,
             2 * (45.0 - temp),
             2.0 + 1.1 * temp,
             43.0 - temp);

    f.close();
  }else {
    Serial.printf("Failed to open log file for writing: %s\n", path.c_str());
  }

  xSemaphoreGive(fsMutex);
}

/*
  ============================================================================
    Log settings handling
  ============================================================================
*/
void validateLogSettings() {
  if (logInterval < 1 || logInterval > 1000000UL) logInterval = DEFAULT_LOG_INTERVAL_S;
  if (numOfLogFiles < 2 || numOfLogFiles > 48) numOfLogFiles = DEFAULT_NUM_OF_LOG_FILES;
  enforceLogFileLimit();
}

void loadLogSettings() {
  prefs.begin("logcfg", true); // read-only
  logInterval = prefs.getUInt("interval", DEFAULT_LOG_INTERVAL_S);
  numOfLogFiles  = prefs.getUChar("numFiles", DEFAULT_NUM_OF_LOG_FILES);
  prefs.end();
  validateLogSettings();
}

void saveLogSettings() {
  prefs.begin("logcfg", false); // write
  prefs.putUInt("interval", logInterval);
  prefs.putUChar("numFiles", numOfLogFiles);
  prefs.end();
}

String loadHtmlTemplate(const char* path, uint32_t interval, uint8_t numFiles) {
  File file = LittleFS.open(path, "r");
  if(!file) return "Template not found";

  String html = file.readString();
  file.close();

  html.replace("%INTERVAL%", String(logInterval));
  html.replace("%NUMFILES%", String(numOfLogFiles));

  return html;
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

  server.on("/config.html", HTTP_GET, [](AsyncWebServerRequest *request){
    String page = loadHtmlTemplate("/config.html.template", logInterval, numOfLogFiles);
    request->send(200, "text/html", page);
  });

  server.on("/savesettings", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.printf("Savesettings requested\n");
    if(request->hasParam("interval") && request->hasParam("numChunks")){
      logInterval = request->getParam("interval")->value().toInt();
      numOfLogFiles  = request->getParam("numChunks")->value().toInt();
      validateLogSettings();
      saveLogSettings();
      Serial.printf("Saved Log Settings: interval=%lus, numOfFiles=%u\n", logInterval, numOfLogFiles);
    }
    else {
      Serial.println("Savesettings: Parameters missing!");
    }
    request->redirect("/");
  });

  server.on(
    "/upload.html",
    HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->send(200, "text/plain", "");
    },
    [](AsyncWebServerRequest *request,
      String filename,
      size_t index,
      uint8_t *data,
      size_t len,
      bool final) {

      // File upload handler

      if (!index) {
        // Upload start

        if (!filename.startsWith("/"))
          filename = "/" + filename;

        // If uploading non-gz file → remove old gz version
        if (!filename.endsWith(".gz")) {
          String gz = filename + ".gz";
          if (LittleFS.exists(gz)) {
            LittleFS.remove(gz);
          }
        }

        Serial.printf("UploadStart: %s\n", filename.c_str());

        fsUploadFile = LittleFS.open(filename, "w");
      }

      // Write received chunk
      if (len) {
        if (fsUploadFile) {
          fsUploadFile.write(data, len);
        }
      }

      // Upload finished
      if (final) {

        if (fsUploadFile) {
          fsUploadFile.close();
          Serial.printf("UploadEnd: %s (%u bytes)\n",
                        filename.c_str(),
                        index + len);

          request->redirect("/success.html");
        } else {
          request->send(500, "text/plain",
                        "500: couldn't create file");
        }
      }
    });

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
    Download handler (streams all chunks into one CSV but in very small stream bits)
  ============================================================================
*/

void handleDownload(AsyncWebServerRequest *request) {

  struct DownloadState {
    int index;
    File file;
  };

  DownloadState *state = new DownloadState();
  state->index = numOfLogFiles - 1;

  AsyncWebServerResponse *response =
    request->beginChunkedResponse(
      "text/csv",
      [state](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {

        if (!xSemaphoreTake(fsMutex, portMAX_DELAY))
          return 0;

        const size_t MAX_PER_CALL = 2048;

        while (true) {

          if (!state->file) {

            if (state->index < 0) {
              xSemaphoreGive(fsMutex);
              delete state;
              return 0;   // fertig
            }

            String path = "/log_" + String(state->index--) + ".csv";

            if (LittleFS.exists(path)) {
              state->file = LittleFS.open(path, "r");
            }

            continue;
          }

          size_t toRead = min(MAX_PER_CALL, maxLen);
          size_t bytesRead = state->file.read(buffer, toRead);

          if (bytesRead > 0) {
            xSemaphoreGive(fsMutex);
            return bytesRead;
          }

          // Datei fertig
          state->file.close();
        }
      });

  response->addHeader(
    "Content-Disposition",
    "attachment; filename=\"data.csv\"");

  request->send(response);
}