#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

#define LED_PIN 4

const char *server_api = "https://sieuthitiendung.com/api/register";
// const char *server_api = "http://192.168.100.217:3000/api/register";

String mqtt_host = "";
int mqtt_port = 8883;
String mqtt_user = "";
String mqtt_pass = "";

WiFiClientSecure espClient;
PubSubClient client(espClient);

String macAddress;
unsigned long lastHeartbeat = 0;
const long heartbeatInterval = 120000; // 2 phút (2000ms * 60 * 2)

volatile bool ledRunning = false;
unsigned long lastToggle = 0;
unsigned long startTime = 0;
unsigned long durationMs = 0;

void doOTA(const char *firmwareUrl)
{
	espClient.stop();
	delay(500);

	WiFiClientSecure client;
	client.setInsecure();

	ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);

	t_httpUpdate_return ret = ESPhttpUpdate.update(
		client,
		firmwareUrl);

	switch (ret)
	{
	case HTTP_UPDATE_FAILED:
		Serial.printf(
			"❌ OTA Failed (%d): %s\n",
			ESPhttpUpdate.getLastError(),
			ESPhttpUpdate.getLastErrorString().c_str());
		break;

	case HTTP_UPDATE_NO_UPDATES:
		Serial.println("ℹ️ No Update");
		break;

	case HTTP_UPDATE_OK:
		Serial.println("✅ OTA Success → Reboot");
		break;
	}
}

bool getMqttConfig()
{
	if (WiFi.status() == WL_CONNECTED)
	{
		WiFiClientSecure server;
		server.setInsecure();
		// WiFiClient server;
		HTTPClient http;

		http.begin(server, server_api);
		http.addHeader("Content-Type", "application/json");
		http.setTimeout(5000);

		String ip = WiFi.localIP().toString();
		String jsonBody = "{\"mac\":\"" + macAddress + "\", \"ip\":\"" + ip + "\"}";

		int httpCode = http.POST(jsonBody);

		Serial.println("HTTP Response code: " + String(httpCode));

		if (httpCode == 200)
		{
			String payload = http.getString();
			DynamicJsonDocument doc(1024);
			DeserializationError error = deserializeJson(doc, payload);

			if (!error)
			{
				mqtt_host = doc["config"]["mqtt_host"].as<String>();
				mqtt_port = doc["config"]["mqtt_port"].as<int>();
				mqtt_user = doc["config"]["mqtt_user"].as<String>();
				mqtt_pass = doc["config"]["mqtt_pass"].as<String>();

				http.end();

				Serial.println("MQTT Config:");
				Serial.println("Host: " + mqtt_host);
				Serial.println("Port: " + String(mqtt_port));
				Serial.println("User: " + mqtt_user);
				Serial.println("Pass: " + mqtt_pass);

				return true;
			}
		}
		http.end();
	}
	return false;
}

void callback(char *topic, byte *payload, unsigned int length)
{
	String message;
	for (unsigned int i = 0; i < length; i++)
	{
		message += (char)payload[i];
	}

	StaticJsonDocument<256> doc;
	deserializeJson(doc, message);

	const char *action = doc["action"];

	if (strcmp(action, "ON") == 0 && !ledRunning)
	{
		Serial.println("action: ON");
		ledRunning = true;
		int duration = doc["duration"];
		durationMs = duration * 1000;
		startTime = millis();
		lastToggle = millis();
	}
	else if (strcmp(action, "OFF") == 0)
	{
		Serial.println("action: OFF");
		ledRunning = false;
		digitalWrite(LED_PIN, LOW);
	}
	else if (strcmp(action, "OTA") == 0)
	{
		const char *url = doc["url"];
		Serial.println("🚀 Start OTA update");
		doOTA(url);
	}
}

void reconnect()
{
	if (mqtt_host == "")
		return;

	while (!client.connected())
	{
		String clientId = "ESP8266-" + macAddress;
		String statusTopic = "device/" + macAddress + "/status";
		String cmdTopic = "device/" + macAddress + "/cmd";

		if (client.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str(),
						   statusTopic.c_str(), 1, true, "offline"))
		{
			client.publish(statusTopic.c_str(), "online", true);
			client.subscribe(cmdTopic.c_str());
		}
		else
		{
			delay(5000);
		}
	}
}

void setup()
{
	Serial.begin(9600);
	pinMode(LED_PIN, OUTPUT);

	WiFiManager wm;
	bool res = wm.autoConnect("FEED-FISH-AP");
	if (!res)
	{
		ESP.restart();
	}

	macAddress = WiFi.macAddress();
	espClient.setInsecure();

	for (int i = 0; i < 5; i++)
	{
		if (getMqttConfig())
			break;

		Serial.println("Failed to get MQTT config, retrying...");
		delay(2000);
	}

	if (mqtt_host != "")
	{
		client.setServer(mqtt_host.c_str(), mqtt_port);
		client.setCallback(callback);
	}
	else
	{
		ESP.restart();
	}
}

void loop()
{
	if (mqtt_host != "")
	{
		if (!client.connected())
		{
			reconnect();
		}
		client.loop();

		if (ledRunning)
		{
			if (millis() - lastToggle >= 1000)
			{
				lastToggle = millis();
				digitalWrite(LED_PIN, HIGH);
			}

			if (durationMs > 0 && millis() - startTime >= durationMs)
			{
				ledRunning = false;
				digitalWrite(LED_PIN, LOW);
			}
		}

		unsigned long now = millis();
		if (now - lastHeartbeat > heartbeatInterval)
		{
			lastHeartbeat = now;
			String statusTopic = "device/" + macAddress + "/status";
			client.publish(statusTopic.c_str(), "online");
		}
	}
}