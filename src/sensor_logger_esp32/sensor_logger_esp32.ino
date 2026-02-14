#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>

#define ONE_HOUR 3600000UL

// Dummy Klasse für den TempSensor
class TempSensor {
public:
  TempSensor();
  ~TempSensor();
  void setWaitForConversion(bool wait);
  void begin();
  uint8_t getDeviceCount();
  void requestTemperatures();
  float getTempCByIndex(uint8_t index);

private:
  bool m_wait;
};

TempSensor::TempSensor(): m_wait(true){}
TempSensor::~TempSensor(){}
void TempSensor::setWaitForConversion(bool wait) { m_wait = wait; }
void TempSensor::begin() {}
uint8_t TempSensor::getDeviceCount() { return 1; }
void TempSensor::requestTemperatures() {}
float TempSensor::getTempCByIndex(uint8_t index) {
  return random(200, 250) / 10.0;
}

TempSensor tempSensors;

WebServer server(80);
File fsUploadFile;

const char *OTAName = "ESP32-C3";
const char *OTAPassword = "esp32c3";

const char* mdnsName = "esp32c3";

WiFiUDP UDP;
IPAddress timeServerIP;
const char* ntpServerName = "time.nist.gov";

const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];

/*__________________________________________________________SETUP__________________________________________________________*/

void setup() {
  Serial.begin(115200);
  delay(500);

  tempSensors.setWaitForConversion(false);
  tempSensors.begin();

  if (tempSensors.getDeviceCount() == 0) {
    Serial.println("No temperature sensor found. Rebooting.");
    delay(3000);
    ESP.restart();
  }

  startWiFi();
  startOTA();
  startLittleFS();
  startMDNS();
  startServer();
  startUDP();

  WiFi.hostByName(ntpServerName, timeServerIP);
  sendNTPpacket(timeServerIP);
}

/*__________________________________________________________LOOP__________________________________________________________*/

const unsigned long intervalNTP = ONE_HOUR;
unsigned long prevNTP = 0;
unsigned long lastNTPResponse = millis();

const unsigned long intervalTemp = 60000;
unsigned long prevTemp = 0;
bool tmpRequested = false;
const unsigned long DS_delay = 750;

uint32_t timeUNIX = 0;

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - prevNTP > intervalNTP) {
    prevNTP = currentMillis;
    sendNTPpacket(timeServerIP);
  }

  uint32_t time = getTime();
  if (time) {
    timeUNIX = time;
    lastNTPResponse = millis();
  } else if ((millis() - lastNTPResponse) > 24UL * ONE_HOUR) {
    ESP.restart();
  }

  if (timeUNIX != 0) {
    if (currentMillis - prevTemp > intervalTemp) {
      tempSensors.requestTemperatures();
      tmpRequested = true;
      prevTemp = currentMillis;
    }

    if (currentMillis - prevTemp > DS_delay && tmpRequested) {
      tmpRequested = false;

      uint32_t actualTime = timeUNIX + (currentMillis - lastNTPResponse) / 1000;
      float temp = tempSensors.getTempCByIndex(0);
      temp = round(temp * 100.0) / 100.0;

      File tempLog = LittleFS.open("/data.csv", "a");
      tempLog.printf("%lu;%.2f;;;;;\n", actualTime, temp);
      tempLog.close();
    }
  }

  server.handleClient();
  ArduinoOTA.handle();
}

/*__________________________________________________________SETUP_FUNCTIONS__________________________________________________________*/

void startWiFi() {
  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setTimeout(180);

  if (!wm.autoConnect("Sensorlogger-AP")) {
    ESP.restart();
  }

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void startUDP() {
  UDP.begin(123);
}

void startOTA() {
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() { Serial.println("OTA Start"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA End"); });

  ArduinoOTA.begin();
}

void startLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
  }
}

void startMDNS() {
  if (!MDNS.begin(mdnsName)) {
    Serial.println("Error starting mDNS");
  }
}

void startServer() {
  server.on("/edit.html", HTTP_POST, []() {
    server.send(200, "text/plain", "");
  }, handleFileUpload);

  server.onNotFound(handleNotFound);
  server.begin();
}

/*__________________________________________________________SERVER_HANDLERS__________________________________________________________*/

void handleNotFound() {
  if (!handleFileRead(server.uri()))
    server.send(404, "text/plain", "404");
}

bool handleFileRead(String path) {
  if (path.endsWith("/")) path += "index.html";
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    server.streamFile(file, getContentType(path));
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    fsUploadFile = LittleFS.open("/" + upload.filename, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (fsUploadFile) fsUploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile) fsUploadFile.close();
  }
}

/*__________________________________________________________HELPER__________________________________________________________*/

String getContentType(String filename) {
  if (filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css")) return "text/css";
  if (filename.endsWith(".js")) return "application/javascript";
  return "text/plain";
}

unsigned long getTime() {
  if (UDP.parsePacket() == 0) return 0;

  UDP.read(packetBuffer, NTP_PACKET_SIZE);
  uint32_t NTPTime = (packetBuffer[40] << 24) |
                     (packetBuffer[41] << 16) |
                     (packetBuffer[42] << 8) |
                     packetBuffer[43];

  return NTPTime - 2208988800UL;
}

void sendNTPpacket(IPAddress& address) {
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;

  UDP.beginPacket(address, 123);
  UDP.write(packetBuffer, NTP_PACKET_SIZE);
  UDP.endPacket();
}
