#include <Arduino.h>
#include <TimeLib.h>
#include "WifiConfig.h"
#include <ESP8266WiFi.h>
#include "EmonConfig.h"
#include <ArduinoOTA.h> // Библиотека для OTA-прошивки
#include <ArduinoJson.h>

#include "DigitLedDisplay.h"

#define DIN D7 
#define CS D6  //LOAD/CS
#define CLK D5 

DigitLedDisplay ld = DigitLedDisplay(DIN, CS, CLK);

DynamicJsonDocument doc(1024);

#ifndef YOUR_WIFI_SSID
#define YOUR_WIFI_SSID "YOUR_WIFI_SSID"
#define YOUR_WIFI_PASSWD "YOUR_WIFI_PASSWD"
#endif // !YOUR_WIFI_SSID

// #define OTA_HOSNAME "osmos_pump_timer"
#define OTA_HOSNAME "show-power"

#ifndef EMON_APIKEY
#define EMON_APIKEY "XXXXXXXXXXXXX"
#endif // !EMON_APIKEY

const char *emon_get_power_node_id = "84";
char node_id_to_get[3]; //2+1 просто переменная которая умеет хранить 2 знака

#define EMON_DOMAIN "udom.ua"
#define EMON_PATH "emoncms"
#define EMON_GET_DATA_TIMEOUT 5000 //ms

#define ONBOARDLED D4     // Built in LED on ESP-12/ESP-07
#define LOOP_DELAY_MAX 30 // 24*60*60 sec

unsigned long t_sent, t_get = 0;
unsigned emon_upload_period = 120; //Upload period sec
unsigned emon_power_check_period = 30;
unsigned emon_power_check_period_max = 10;

int loop_delay = 2;

unsigned long time_last_data_sent, time_last_data_get, time_last_emon_data = 0;
unsigned emon_data_check_period = 10;

bool wifiFirstConnected = false;
bool FirstStart = true;
String ip;

WiFiClient Client;

void onSTAConnected(WiFiEventStationModeConnected ipInfo)
{
  Serial.printf("Connected to %s\r\n", ipInfo.ssid.c_str());
}

// Start NTP only after IP network is connected
void onSTAGotIP(WiFiEventStationModeGotIP ipInfo)
{
  Serial.printf("Got IP: %s\r\n", ipInfo.ip.toString().c_str());
  Serial.printf("Connected: %s\r\n", WiFi.status() == WL_CONNECTED ? "yes" : "no");
  digitalWrite(ONBOARDLED, LOW); // Turn on LED
  wifiFirstConnected = true;
}

// Manage network disconnection
void onSTADisconnected(WiFiEventStationModeDisconnected event_info)
{
  Serial.printf("Disconnected from SSID: %s\n", event_info.ssid.c_str());
  Serial.printf("Reason: %d\n", event_info.reason);
  digitalWrite(ONBOARDLED, HIGH); // Turn off LED
  //NTP.stop(); // NTP sync can be disabled to avoid sync errors
  WiFi.reconnect();
}

// **** GET EMON *********

//служебная функция для получения данных из emoncms в формате json
//используем для получения мощности
String get_emon_data(const char *node_id_to_get)
{

  String json;
  Serial.print("connect to Server ");
  Serial.println(EMON_DOMAIN);
  Serial.print("GET /emoncms/feed/timevalue.json?id=");
  Serial.println(node_id_to_get);

  if (Client.connect(EMON_DOMAIN, 80))
  {
    Serial.println("connected");
    Client.print("GET /emoncms/feed/timevalue.json?id="); //http://udom.ua/emoncms/feed/feed/timevalue.json?id=18
    Client.print(node_id_to_get);
    Client.println();

    unsigned long tstart = millis();
    while (Client.available() == 0)
    {
      if (millis() - tstart > EMON_GET_DATA_TIMEOUT)
      {
        Serial.println(" --- Client Timeout !");
        Client.stop();
        return "0";
      }
    }

    // Read all the lines of the reply from server and print them to Serial
    while (Client.available())
    {
      json = Client.readStringUntil('\r');
      Serial.print("json = ");
      Serial.println(json);
    }

    Serial.println();
    Serial.println("closing connection");
  }
  return json;
}

// **** !GET EMON *********

void WIFI_Connect(bool is_reconnect = false)
{
  if (is_reconnect)
  {
    WiFi.disconnect();
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(YOUR_WIFI_SSID, YOUR_WIFI_PASSWD);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    for (int i = 0; i < 10; i++)
    {
      digitalWrite(ONBOARDLED, LOW); // Turn on LED
      delay(50);
      digitalWrite(ONBOARDLED, HIGH); // Turn on LED
      delay(50);
    }

    Serial.print(".");
  }
  Serial.println();

  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
}

void setup()
{
  delay(1000);

  Serial.begin(115200);
  //Serial.setDebugOutput(true);
  Serial.flush();
  WiFi.mode(WIFI_STA);
  WiFi.begin(YOUR_WIFI_SSID, YOUR_WIFI_PASSWD);

  WIFI_Connect();

  pinMode(ONBOARDLED, OUTPUT);   // Onboard LED
  digitalWrite(ONBOARDLED, LOW); // Switch on LED

  ArduinoOTA.setHostname(OTA_HOSNAME); // Задаем имя сетевого порта
  //     ArduinoOTA.setPassword((const char *)"0000"); // Задаем пароль доступа для удаленной прошивки

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  /* Set the brightness min:1, max:15 */
  ld.setBright(10);

  /* Set the digit count */
  ld.setDigitLimit(8);

  ld.clear();
}

void loop()
{

  if (FirstStart)
  {
    Serial.println();
    Serial.println("*** FirstStart ***");
    Serial.println();
    Serial.println(" *** demo ***");
    // delay(1000);
    // демонстрируем, что работает
    // digitalWrite(FIRST_RELAY, HIGH); delay(1000); digitalWrite(FIRST_RELAY, LOW);
    // digitalWrite(POWER_RELAY, HIGH); delay(1000); digitalWrite(POWER_RELAY, LOW);
  }

  digitalWrite(ONBOARDLED, LOW);

  ArduinoOTA.handle(); // Всегда готовы к прошивке

  // проверяем, что можно включать 6 кВт
  String json = get_emon_data(emon_get_power_node_id);
  deserializeJson(doc, json);

  float power = doc["value"];

  Serial.println();
  Serial.print("power = ");
  Serial.print(int(power));
  Serial.println(" W");

  Serial.print("OTA_HOSNAME = ");
  Serial.println(OTA_HOSNAME);

ld.clear();
ld.printDigit(int(power), 4);

  if (emon_power_check_period < emon_power_check_period_max)
  {
    emon_power_check_period++;
  }

  Serial.print("loop_delay = ");
  Serial.print(loop_delay);
  Serial.print("(");
  Serial.print(LOOP_DELAY_MAX);
  Serial.println(") sec");
  Serial.println();
  digitalWrite(ONBOARDLED, HIGH); // Turn off LED
  delay(loop_delay * 1000);       //задержка большого цикла
  FirstStart = false;

  if (loop_delay < LOOP_DELAY_MAX)
  {                  //постепенно увеличиваем период обновлений до суток
    loop_delay += 1; //sec
  }

}
