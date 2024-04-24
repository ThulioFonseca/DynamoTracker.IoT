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

#define RXD2 16
#define TXD2 17

HardwareSerial neogps(1);
TinyGPSPlus gps;
WiFiClient client;
HTTPClient http;

String networkSsid;
String networkPassword;

// Dados de autenticação do Azure Service Bus
String clientId;
String clientSecret;
String resource;
String uri;

// URL do endpoint do Azure Service Bus
String serviceBusUrl;

String jwtToken;
unsigned long tokenExpirationTime = 0;

void fileSystemInit()
{
  if (!LittleFS.begin())
  {
    Serial.println("Falha ao inicializar o sistema de arquivos LittleFS");
    return;
  }
  Serial.println("Sistema de arquivos LittleFS inicializado");
  Serial.println();

  File secrets = LittleFS.open("/secrets.json", "r");

  if (secrets)
  {
    Serial.println("Arquivo de secrets encontrado");

    size_t size = secrets.size();
    std::unique_ptr<char[]> buf(new char[size]);
    secrets.readBytes(buf.get(), size);

    JsonDocument doc;
    deserializeJson(doc, buf.get());

    clientId = doc["Authentication"]["ClientId"].as<String>();
    clientSecret = doc["Authentication"]["ClientSecret"].as<String>();
    resource = doc["Authentication"]["Resource"].as<String>();
    uri = doc["Authentication"]["Uri"].as<String>();

    serviceBusUrl = doc["ServiceBus"]["ConnectionString"].as<String>();

    networkSsid = doc["Network"]["Ssid"].as<String>();
    networkPassword = doc["Network"]["Password"].as<String>();

    secrets.close();
  }
  else
  {
    Serial.println("Arquivo de secrets não encontrado");
  }
}
void getJwtToken()
{
  http.begin(uri);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String payload = "grant_type=client_credentials&client_id=" + String(clientId) +
                   "&client_secret=" + String(clientSecret) + "&resource=" + String(resource);

  int httpResponseCode = http.POST(payload);

  if (httpResponseCode >= 200)
  {
    String response = http.getString();
    JsonDocument doc;
    deserializeJson(doc, response);

    jwtToken = doc["access_token"].as<String>();

    Serial.println("Token obtido: " + jwtToken);
    Serial.println();

    // Atualiza o tempo de expiração do token
    unsigned long expiresIn = doc["expires_in"].as<unsigned long>();
    tokenExpirationTime = millis() + expiresIn * 1000; // Converte para milissegundos
  }
  else
  {
    Serial.println("Erro ao obter token: " + String(httpResponseCode));
    Serial.println(http.getString());
    Serial.println();
  }

  http.end();
}
void sendToAzureServiceBus(JsonDocument &json)
{
  // Verifica se o token está expirado ou prestes a expirar em breve
  if (millis() >= tokenExpirationTime - 60000 || millis() >= tokenExpirationTime)
  {
    Serial.println("Obtendo novo token...");
    getJwtToken(); // Renova o token 1 minuto antes da expiração
  }

  // Serializa o JSON
  String jsonString;
  serializeJson(json, jsonString);

  // Inicia a conexão com o endpoint do Azure Service Bus
  http.begin(serviceBusUrl);
  http.addHeader("Authorization", "Bearer " + jwtToken);

  // Constrói o corpo da mensagem JSON
  Serial.print("Mensagem JSON: ");
  Serial.println(jsonString);

  // Envia a requisição HTTP manualmente
  Serial.println("Enviando requisição HTTP...");
  int httpResponseCode = http.POST(jsonString);

  if (httpResponseCode >= 200)
  {
    // Verifica a resposta do servidor
    Serial.print("Resposta do Servidor: ");
    Serial.println(httpResponseCode);
    String response = http.getString();
    Serial.println(response);
  }
  else
  {
    Serial.print("Erro ao enviar requisição HTTP: ");
    Serial.println(httpResponseCode);
    String response = http.getString();
  }

  http.end();
}
void SpinProgress(int counter, String term)
{
  const char *progressChars = "|/-\\";
  Serial.print(progressChars[counter % 4] + term);
  delay(100);
  Serial.print("\r\x1B[1C\b");
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
  // Informações do Hardware
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
  neogps.begin(9600, SERIAL_8N1, RXD2, TXD2);

  fileSystemInit();
  ConnectToWiFi();
}
void loop()
{
  unsigned long startTime = millis();
  boolean newData = false;
  int count = 0;

  // Verifica continuamente se há novos dados por 1 segundo
  while (millis() - startTime < 1000)
  {
    while (neogps.available())
    {
      if (gps.encode(neogps.read()))
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
