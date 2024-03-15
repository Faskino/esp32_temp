// https:// github.com/mobizt/Firebase-ESP-Client/tree/main/examples/Firestore/CreateDocuments
#include <secrets.h>
#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "time.h"
#include "sntp.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#define SEND_DATA_INTERVAL_FIRESTORE 900000 // 15 minutes in milliseconds
#define SEND_DATA_INTERVAL_RTDB 20000

// Firebase initialization
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long sendDataPrevMillisFirestore = 0;
unsigned long sendDataPrevMillisRTDB = 0;
bool signupOK = false;

const String meter1 = "temp1";
const String meter2 = "temp2";
// Temp sensor setup
OneWire oneWire1(21);
OneWire oneWire2(22);
DallasTemperature sensor1(&oneWire1);
DallasTemperature sensor2(&oneWire2);

// Time setup
const char *ntpServer1 = "pool.ntp.org";
const char *ntpServer2 = "time.nist.gov";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;
const char *time_zone = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// Saving data
Preferences preferences;

FirebaseJson query;
// After getting ssids from firestore, write them into memory
void writeToPreferences(const char *ssid, const char *password, const int count)
{
  preferences.begin("wifi", false);
  preferences.putString("ssid" + count, String(ssid));
  preferences.putString("password" + count, String(password));
  preferences.end();
}
// get ssids from Firestore
void getWifiFromDb()
{
  query.set("from/collectionId", "wifi_cred");
  const int wifi_credentials = Firebase.Firestore.runQuery(&fbdo, FIREBASE_PROJECT_ID, "(default)", "/", &query);
  if (wifi_credentials)
  {
    DynamicJsonDocument doc(2048);

    DeserializationError error = deserializeJson(doc, fbdo.payload());
    JsonArray array = doc.as<JsonArray>();

    int count = 0;
    for (JsonVariant value : array)
    {
      const char *ssid = value["document"]["fields"]["ssid"]["stringValue"];
      const char *password = value["document"]["fields"]["password"]["stringValue"];
      writeToPreferences(ssid, password, count);
      count++;
    }
  }
  else
  {
    Serial.println(fbdo.errorReason());
  }
}

// Connect to wifi using saved credentials
void connectToWifi()
{
  int index = 0;
  preferences.begin("wifi", true);
  while (true)
  {
    String ssid = preferences.getString("ssid" + index, "");
    String password = preferences.getString("password" + index, "");

    if (ssid == "")
    {
      break; // Exit the loop
    }

    Serial.print("Trying SSID: ");
    Serial.println(ssid);

    // Attempt to connect to WiFi
    WiFi.begin(ssid.c_str(), password.c_str());

    // Wait for connection or timeout
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 5)
    {
      delay(1000);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("Connected!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      break;
    }
    else
    {
      Serial.println("Connection failed!");
      WiFi.disconnect();
    }
    index++;
  }
  preferences.end();
}
// Get timestamp
String getFormattedTimestamp()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("No time available (yet)");
    return "";
  }

  const int TIMEZONE_OFFSET = 1;
  timeinfo.tm_hour -= 2;
  mktime(&timeinfo);

  char timestamp[21];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  return String(timestamp);
}
// Write to RealTime firebase Database
void writeToRTDB(float temp, String meter)
{
  if (Firebase.RTDB.setFloat(&fbdo, meter + "/temperature", temp))
  {
    Serial.println("Ok");
  }
  else
  {
    Serial.println("Failed to write temperature to the database");
    Serial.println("Reason: " + fbdo.errorReason());
  }
}
// Write temperatures + timestamp into Firestore
void writeToFirestore(float temp1, float temp2)
{
  String timestamp = getFormattedTimestamp();
  FirebaseJson content;
  String doc_path = "teploty/";

  content.set("fields/teplota1/doubleValue", temp1);
  content.set("fields/teplota2/doubleValue", temp2);
  Serial.println(timestamp);
  content.set("fields/timestamp/timestampValue", timestamp);

  if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "(default)", doc_path.c_str(), content.raw()))
  {
    Serial.println("Document set successfully");
  }
  else
  {
    Serial.println(fbdo.errorReason());
  }
}
// Get temperatures from sensors
float getTemp(int pin)
{
  float temp;
  switch (pin)
  {
  case 1:
    sensor1.requestTemperatures();
    temp = sensor1.getTempCByIndex(0);
    return temp;
    break;
  case 2:
    sensor2.requestTemperatures();
    temp = sensor2.getTempCByIndex(0);
    return temp;
    break;
  }
}
void setup()
{
  Serial.begin(9600);

  connectToWifi();
  getWifiFromDb();

  sensor1.begin();
  sensor2.begin();

  sntp_servermode_dhcp(1);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  // Signup
  if (Firebase.signUp(&config, &auth, "", ""))
  {
    Serial.println("Signed up successfully");
    signupOK = true;
  }
  else
  {
    Serial.printf("Sign-up failed: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Setting sensor places
  Firebase.RTDB.set(&fbdo, meter1 + "/place", "Obyvacka");
  Firebase.RTDB.set(&fbdo, meter2 + "/place", "Kuchyna");
}

void loop()
{

  float temperatureC1 = getTemp(1);
  float temperatureC2 = getTemp(2);

  if (Firebase.ready() && signupOK)
  {
    unsigned long currentMillis = millis();

    // Checking for time when to write to Firestore (every 4 hours)
    if (currentMillis - sendDataPrevMillisFirestore >= SEND_DATA_INTERVAL_FIRESTORE || sendDataPrevMillisFirestore == 0)
    {
      sendDataPrevMillisFirestore = currentMillis;
      writeToFirestore(temperatureC1, temperatureC2);
    }
    //... to Realtime Database (every 20 seconds)
    if (currentMillis - sendDataPrevMillisRTDB >= SEND_DATA_INTERVAL_RTDB || sendDataPrevMillisRTDB == 0)
    {
      sendDataPrevMillisRTDB = currentMillis;
      writeToRTDB(temperatureC1, "temp1");
      writeToRTDB(temperatureC2, "temp2");
    }
  }
  else
  {
    Serial.println("Reason: " + fbdo.errorReason());
  }
}