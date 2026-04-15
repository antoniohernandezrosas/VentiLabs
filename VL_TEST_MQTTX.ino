#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ===== WIFI =====
const char* ssid = "-";
const char* password = "-";

// ===== IOTDA =====
const char* mqtt_server = "-";
const int mqtt_port = 8883;

const char* device_id = "-";
const char* device_secret = "-";

const char* client_id = "-";

// ===== CERTIFICADO =====
static const char* root_ca =
"-----BEGIN CERTIFICATE-----\n"

"-----END CERTIFICATE-----\n";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== CONEXIÓN =====
void connectMQTT() {

  client.setServer(mqtt_server, mqtt_port);

  while (!client.connected()) {

    Serial.println("Intentando conexión...");

    bool ok = client.connect(
      client_id,
      device_id,
      device_secret,
      NULL,
      0,
      false,
      NULL,
      true   // clean session
    );

    if (ok) {
      Serial.println("Conectado a IoTDA");
    } else {
      Serial.print("Error rc=");
      Serial.println(client.state());
      delay(3000);
    }
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi conectado");
  espClient.setCACert(root_ca);
  espClient.setTimeout(15000);

  connectMQTT();
}

// ===== LOOP =====
void loop() {
  client.loop();
}