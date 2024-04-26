#include <Wire.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "base64.h"
#include <FS.h>
#include <LittleFS.h>
#include <esp_chip_info.h>
#include <esp_cpu.h>
#include <esp_system.h>

#define RX_PIN 16
#define TX_PIN 17

HardwareSerial neoGps(1);
TinyGPSPlus gps;
WiFiClient wifiClient;
HTTPClient httpClient;

String networkSsid;
String networkPassword;

// Azure Authentication Credentials
String azureAuthClientId;
String azureAuthClientSecret;
String azureAuthResource;
String azureAuthUri;

// Azure Service Bus URL
String serviceBusUrl;

String jwtToken;
unsigned long tokenExpirationTime = 0;

void fileSystemInit()
{
  if (!LittleFS.begin())
  {
    Serial.println("Fail to mount LittleFS");
    return;
  }
  Serial.println("LittleFS mounted successfully");
  Serial.println();

  File secrets = LittleFS.open("/secrets.json", "r");

  if (secrets)
  {
    Serial.println("Secrets loaded successfully");

    size_t size = secrets.size();
    std::unique_ptr<char[]> buf(new char[size]);
    secrets.readBytes(buf.get(), size);

    JsonDocument doc;
    deserializeJson(doc, buf.get());

    azureAuthClientId = doc["Authentication"]["ClientId"].as<String>();
    azureAuthClientSecret = doc["Authentication"]["ClientSecret"].as<String>();
    azureAuthResource = doc["Authentication"]["Resource"].as<String>();
    azureAuthUri = doc["Authentication"]["Uri"].as<String>();

    serviceBusUrl = doc["ServiceBus"]["ConnectionString"].as<String>();

    networkSsid = doc["Network"]["Ssid"].as<String>();
    networkPassword = doc["Network"]["Password"].as<String>();

    secrets.close();
  }
  else
  {
    Serial.println("Secrets not found");
  }
}
void getJwtToken()
{
  httpClient.begin(azureAuthUri);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String payload = "grant_type=client_credentials&client_id=" + String(azureAuthClientId) +
                   "&client_secret=" + String(azureAuthClientSecret) + "&azureAuthResource=" + String(azureAuthResource);

  int httpResponseCode = httpClient.POST(payload);

  if (httpResponseCode >= 200)
  {
    String response = httpClient.getString();
    JsonDocument doc;
    deserializeJson(doc, response);

    jwtToken = doc["access_token"].as<String>();

    Serial.println("Token generated: " + jwtToken);
    Serial.println();

    // Update token expiration time
    unsigned long expiresIn = doc["expires_in"].as<unsigned long>();
    tokenExpirationTime = millis() + expiresIn * 1000; // Converte para milissegundos
  }
  else
  {
    Serial.println("Failed to get JWT token: " + String(httpResponseCode));
    Serial.println(httpClient.getString());
    Serial.println();
  }

  httpClient.end();
}
void sendToAzureServiceBus(JsonDocument &json)
{
  // Verify if token has expired or is about to expire
  if (millis() >= tokenExpirationTime - 60000 || millis() >= tokenExpirationTime)
  {
    Serial.println("Getting new token...");
    getJwtToken();
  }

  String message;
  serializeJson(json, message);

  // Start connection to Azure Service Bus
  httpClient.begin(serviceBusUrl);
  httpClient.addHeader("Authorization", "Bearer " + jwtToken);
  httpClient.addHeader("Content-Type", "application/json");

  Serial.print("Sending message: ");
  Serial.println(message);
  int httpResponseCode = httpClient.POST(message);

  if (httpResponseCode >= 200)
  {
    Serial.print("Server response: ");
  }
  else
  {
    Serial.print("Error sending message: ");
  }

  Serial.print(httpResponseCode);
  String response = httpClient.getString();
  Serial.println(response);
  Serial.println();

  httpClient.end();
}
void SpinProgress(int counter, String term)
{
  const char *progressChars = "|/-\\";
  Serial.print(progressChars[counter % 4] + term);
  delay(100);
  Serial.print("\r                                                   \r"); 
  //Serial.print("\r\x1B[1C\b");
}
void ConnectToWiFi()
{
  int count = 0;
  WiFi.begin(networkSsid, networkPassword);
  while (WiFi.status() != WL_CONNECTED)
  {
    SpinProgress(count, " Connecting to WiFi: " + String(networkSsid));
    count++;
  }
  Serial.print("\r                                         \r");
  Serial.println("Connected to " + String(networkSsid));
}
String gpsFormatedDateTimeWithTimeZone()
{
  char dateTimeBuffer[30];
  snprintf(dateTimeBuffer, sizeof(dateTimeBuffer), "%04d-%02d-%02dT%02d:%02d:%02d-03:00",
           gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour() - 3, gps.time.minute(), gps.time.second());
  return String(dateTimeBuffer);
}
JsonDocument CreateJson()
{
  // Hardware Information
  esp_chip_info_t chipInfo;
  esp_chip_info(&chipInfo);
  esp_chip_model_t chipModel = chipInfo.model;
  esp_reset_reason_t resetReason = esp_reset_reason();

  JsonDocument root;

  root["device"]["chipModel"] = chipModel;
  root["device"]["chipRevision"] = chipInfo.revision;
  root["device"]["chipId"] = ESP.getEfuseMac();
  root["device"]["flashChipId"] = chipInfo.features;
  root["device"]["flashChipSize"] = ESP.getFlashChipSize();
  root["device"]["freeHeap"] = ESP.getFreeHeap();
  root["device"]["numOfCores"] = chipInfo.cores;
  root["device"]["cpuFreqMHz"] = ESP.getCpuFreqMHz();
  root["device"]["sdkVersion"] = ESP.getSdkVersion();
  root["device"]["ipAdress"] = WiFi.localIP().toString();
  root["device"]["macAddress"] = WiFi.macAddress();
  root["device"]["ssid"] = WiFi.SSID();
  root["device"]["rssi"] = WiFi.RSSI();
  root["device"]["uptime"] = millis() / 1000;

  root["gpsData"]["latitude"] = gps.location.lat();
  root["gpsData"]["longitude"] = gps.location.lng();
  root["gpsData"]["altitudeInMeters"] = gps.altitude.meters();
  root["gpsData"]["speedInKmph"] = gps.speed.kmph();
  root["gpsData"]["course"] = gps.course.deg();
  root["gpsData"]["satellites"] = gps.satellites.value();
  root["gpsData"]["hdop"] = gps.hdop.value();
  root["gpsData"]["dateTime"] = gpsFormatedDateTimeWithTimeZone();

  return root;
}

void setup()
{
  Serial.begin(115200);
  neoGps.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  fileSystemInit();
  ConnectToWiFi();
}
void loop()
{
  unsigned long startTime = millis();
  boolean newData = false;
  int count = 0;

  // Get GPS data routine 
  while (millis() - startTime < 1000)
  {
    while (neoGps.available())
    {
      if (gps.encode(neoGps.read()))
      {
        newData = true;
        break;
      }
    }
    if (newData)
    {
      break;
    }
    SpinProgress(count, " Finding Satellites");
    count++;
  }

  if (newData && gps.location.isValid())
  {
    JsonDocument json = CreateJson();
    sendToAzureServiceBus(json);
    delay(5000);
  }
}
