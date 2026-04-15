#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <WebServer.h>

// ----------- CONFIG HUAWEI IOT -----------
const char* mqtt_server = "-";
const int mqtt_port = 8883;

const char* device_id = "-";
const char* device_secret = "-";

// ----------- OBJECTS -----------
WiFiClientSecure espClient;
PubSubClient client(espClient);
Preferences preferences;
WebServer server(80);

String ssid;
String password;

// ----------- CERTIFICATE -----------
const char* root_ca = \
"-----BEGIN CERTIFICATE-----\n" \
"...\n" \
"-----END CERTIFICATE-----\n";

// ----------- SAVE WIFI -----------
void saveWiFi(String s, String p) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", s);
  preferences.putString("pass", p);
  preferences.end();
}

// ----------- WIFI -----------
void loadWiFi() {
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  preferences.end();
}

// ----------- CONFIG (AP) -----------
void startConfigPortal() {

  WiFi.softAP("VentiLabs_Config");

  server.on("/config", HTTP_POST, []() {

    if (server.hasArg("plain")) {
      String body = server.arg("plain");

      int ssidIndex = body.indexOf("ssid");
      int passIndex = body.indexOf("password");

      // parsing simple
      String newSSID = body.substring(body.indexOf(":\"")+2, body.indexOf("\","));
      String newPASS = body.substring(body.lastIndexOf(":\"")+2, body.lastIndexOf("\""));

      saveWiFi(newSSID, newPASS);

      server.send(200, "application/json", "{\"status\":\"saved\"}");

      delay(2000);
      ESP.restart();
    }
  });

  server.begin();

  while (true) {
    server.handleClient();
  }
}

// ----------- CONNECT WIFI -----------
void connectWiFi() {

  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    startConfigPortal();
  }
}

// ----------- MQTT -----------
void connectMQTT() {

  espClient.setCACert(root_ca);

  client.setServer(mqtt_server, mqtt_port);

  while (!client.connected()) {

    if (client.connect(device_id, device_id, device_secret)) {
      Serial.println("MQTT conectado");
    } else {
      delay(2000);
    }
  }
}

// ----------- SEND TEST DATA -----------
void sendData() {

  float gas = random(80, 150);
  float temp = 24;
  float hum = 50;
  int fan = gas / 2;

  String topic = "$oc/devices/" + String(device_id) + "/sys/properties/report";

  String payload = "{ \"services\": [{ \"service_id\": \"VentiLabs\", \"properties\": {";
  payload += "\"gas\":" + String(gas) + ",";
  payload += "\"temp\":" + String(temp) + ",";
  payload += "\"humidity\":" + String(hum) + ",";
  payload += "\"fan\":" + String(fan);
  payload += "}}]}";

  client.publish(topic.c_str(), payload.c_str());
}

// ----------- SETUP -----------
void setup() {

  Serial.begin(115200);

  loadWiFi();

  if (ssid == "") {
    startConfigPortal();
  }

  connectWiFi();
  connectMQTT();
}

// ----------- LOOP -----------
void loop() {

  if (!client.connected()) {
    connectMQTT();
  }

  client.loop();

  sendData();

  delay(5000);
}