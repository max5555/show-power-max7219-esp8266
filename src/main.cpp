#include <Arduino.h>
#include <TimeLib.h>
#include "WifiConfig.h"
#include <ESP8266WiFi.h>
#include "EmonConfig.h"
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <NtpClientLib.h>

#include "LedControl.h"

/*
 Now we need a LedControl to work with.
 ***** These pin numbers will probably not work with your hardware *****
 pin 12 is connected to the DataIn 
 pin 11 is connected to the CLK 
 pin 10 is connected to LOAD 
 We have only a single MAX72XX.
 */

#define DIN D7
#define CS D6 //LOAD/CS   Rclk
#define CLK D5

LedControl lc = LedControl(D7, D5, D6, 1);

DynamicJsonDocument doc(1024);
String json;

#ifndef YOUR_WIFI_SSID
#define YOUR_WIFI_SSID "YOUR_WIFI_SSID"
#define YOUR_WIFI_PASSWD "YOUR_WIFI_PASSWD"
#endif // !YOUR_WIFI_SSID

// #define OTA_HOSNAME "osmos_pump_timer"
#define OTA_HOSNAME "show-power"

#ifndef EMON_APIKEY
#define EMON_APIKEY "XXXXXXXXXXXXX"
#endif // !EMON_APIKEY

const char *emon_get_power_node_id = "84,97";
char node_id_to_get[6]; //2+1 просто переменная которая умеет хранить 2 знака

#define EMON_DOMAIN "udom.ua"
#define EMON_PATH "emoncms"
#define EMON_GET_DATA_TIMEOUT 10000 //ms

#define ONBOARDLED D4    // Built in LED on ESP-12/ESP-07
#define LOOP_DELAY_MAX 1 // 24*60*60 sec

unsigned long t_sent, t_get = 0;
unsigned emon_upload_period = 120; //Upload period sec
unsigned emon_power_check_period = 1;
unsigned emon_power_check_period_max = 5;

int hour_cur, hour_prev;
int energy_total;
int energy_day_delta, energy_night_delta, energy_night_beg, energy_day_beg;
float power;

int loop_delay = 1;

unsigned long time_last_data_sent, time_last_data_get, time_last_emon_data = 0;
unsigned emon_data_check_period = 10;

bool wifiFirstConnected = false;
bool FirstStart = true;
String ip;

WiFiClient Client;

// **** GET EMON *********

//служебная функция для получения данных из emoncms в формате json
//используем для получения мощности
String get_emon_dataS(const char *node_id_to_get)
{

  String json;
  Serial.print("connect to Server ");
  Serial.println(EMON_DOMAIN);
  Serial.print("GET /emoncms/feed/fetch.json?ids=");
  Serial.println(node_id_to_get);

  if (Client.connect(EMON_DOMAIN, 80))
  {
    Serial.println("connected");
    Client.print("GET /emoncms/feed/fetch.json?ids="); //http://udom.ua/emoncms/feed/feed/fetch.json?ids=84,97
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

// **** NTP ****

#define SHOW_TIME_PERIOD 10       //sec
#define NTP_TIMEOUT 2000          // ms Response timeout for NTP requests //1500 говорят минимальное 2000
#define NTP_SYNC_PERIOD_MAX 86400 // 24*60*60  sec
int ntp_sync_period = 63;
int8_t timeZone = 2;
int8_t minutesTimeZone = 0;
const PROGMEM char *ntpServer = "ua.pool.ntp.org"; //"europe.pool.ntp.org"; //"ua.pool.ntp.org"; //"time.google.com"; //"ua.pool.ntp.org";//"pool.ntp.org";
//pool1.ntp.od.ua

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

void processSyncEvent(NTPSyncEvent_t ntpEvent)
{
  if (ntpEvent < 0)
  {
    Serial.printf("Time Sync error: %d\n", ntpEvent);
    if (ntpEvent == noResponse)
      Serial.println("NTP server not reachable");
    else if (ntpEvent == invalidAddress)
      Serial.println("Invalid NTP server address");
    else if (ntpEvent == errorSending)
      Serial.println("Error sending request");
    else if (ntpEvent == responseError)
      Serial.println("NTP response error");
  }
  else
  {
    if (ntpEvent == timeSyncd)
    {
      Serial.print("Got NTP time: ");
      Serial.println(NTP.getTimeDateString(NTP.getLastNTPSync()));
    }
  }
}

boolean syncEventTriggered = false; // True if a time even has been triggered
NTPSyncEvent_t ntpEvent;            // Last triggered event

bool startNTP()
{
  Serial.println();
  Serial.println("*** startNTP ***");
  NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
  //NTP.begin("pool.ntp.org", 2, true);
  delay(3000); // there seems to be a 1 second delay before first sync will be attempted, delay 2 seconds allows request to be made and received
  int counter = 1;
  Serial.print("NTP.getLastNTPSync() = ");
  Serial.println(NTP.getLastNTPSync());
  while (!NTP.getLastNTPSync() && counter <= 3)
  {
    NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
    Serial.print("NTP CHECK: #");
    Serial.println(counter);
    counter += 1;
    delay(counter * 2000);
  };
  NTP.setInterval(ntp_sync_period); // in seconds
  if (now() < 100000)
  {
    return false;
  }
  else
  {
    return true;
  }
}

void TimeValidator()
{ //проверяем время, если неправильное - перезагружаемся

  Serial.println("TimeValidator");
  for (int ectr = 1; ectr < 4; ectr++)
  {
    ip = WiFi.localIP().toString();
    if (now() < 100000 and (ip != "0.0.0.0"))
    {
      bool isntpok = startNTP();
      if (isntpok)
      {
        return;
      }
      Serial.print("Wrong UNIX time: now() = ");
      //Serial.println(NTP.getTimeStr());
      Serial.println(now());
      Serial.print("ip = ");
      Serial.println(ip);
      Serial.print("ectr = ");
      Serial.println(ectr);
      Serial.print("delay ");
      Serial.print(10000 * ectr);
      Serial.println(" sec");
      delay(30000 * ectr);
    }
    else
    {
      return;
    }
  }
  Serial.println("**** restart **** "); //перезагружаемся только при 3-х ошибках подряд
  delay(2000);

  //            WiFi.forceSleepBegin(); wdt_reset(); ESP.restart(); while(1)wdt_reset();
  //            ESP.reset();
  ESP.restart();
}

//! **** NTP ****

//служебная функция вывода многозначных чисел на экран
void lc_print(int v, int poz, bool dp)
{
  do
  {
    lc.setDigit(0, poz, v % 10, dp);
    v = v / 10;
    dp = false;
    poz++;
  } while (v >= 1);
}

// String  var = getValue( StringVar, ',', 2); // if  a,4,D,r  would return D
String getValue(String data, char separator, int index)
{
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length();

  for (int i = 0; i <= maxIndex && found <= index; i++)
  {
    if (data.charAt(i) == separator || i == maxIndex)
    {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
} // END

void setup()
{
  delay(1000);
  static WiFiEventHandler e1, e2, e3;
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

  // **** NTP *******
  NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {
    ntpEvent = event;
    syncEventTriggered = true;
  });

  e1 = WiFi.onStationModeGotIP(onSTAGotIP); // As soon WiFi is connected, start NTP Client
  e2 = WiFi.onStationModeDisconnected(onSTADisconnected);
  e3 = WiFi.onStationModeConnected(onSTAConnected);

  //! **** NTP *******

  /*
   The MAX72XX is in power-saving mode on startup,
   we have to do a wakeup call
   */
  lc.shutdown(0, false);
  /* Set the brightness to a medium values */
  lc.setIntensity(0, 8);
  /* and clear the display */
  lc.clearDisplay(0);
}

void loop()
{

  if (FirstStart)
  {
    Serial.println();
    Serial.println("*** FirstStart ***");
    Serial.println();
    Serial.println(" *** demo ***");
    hour_prev = hour_cur;
    // delay(1000);
    // демонстрируем, что работает
    // digitalWrite(FIRST_RELAY, HIGH); delay(1000); digitalWrite(FIRST_RELAY, LOW);
    // digitalWrite(POWER_RELAY, HIGH); delay(1000); digitalWrite(POWER_RELAY, LOW);
  }

  digitalWrite(ONBOARDLED, LOW);

  ArduinoOTA.handle(); // Всегда готовы к прошивке

  // **** NTP *******
  static int i = 0;
  static unsigned long last_show_time = 0;

  if (wifiFirstConnected)
  {
    Serial.println("*** wifiFirstConnected ***");
    wifiFirstConnected = false;
    NTP.setInterval(63);            //60 * 5 + 3    //63 Changes sync period. New interval in seconds.
    NTP.setNTPTimeout(NTP_TIMEOUT); //Configure response timeout for NTP requests milliseconds
    startNTP();
    //NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
    NTP.getTimeDateString(); //dummy
  }

  if (syncEventTriggered)
  {
    processSyncEvent(ntpEvent);
    syncEventTriggered = false;
  }

  if ((millis() - last_show_time) > SHOW_TIME_PERIOD or FirstStart)
  {
    //Serial.println(millis() - last_show_time);
    last_show_time = millis();
    Serial.println();
    Serial.print("i = ");
    Serial.print(i);
    Serial.print(" ");
    Serial.print(NTP.getTimeDateString());
    Serial.print(" ");
    Serial.print(NTP.isSummerTime() ? "Summer Time. " : "Winter Time. ");
    Serial.print("WiFi is ");
    Serial.print(WiFi.isConnected() ? "connected" : "not connected");
    Serial.print(". ");
    Serial.print("Uptime: ");
    Serial.print(NTP.getUptimeString());
    Serial.print(" since ");
    Serial.println(NTP.getTimeDateString(NTP.getFirstSync()).c_str());

    Serial.printf("ESP8266 Chip id = %06X\n", ESP.getChipId());
    Serial.printf("WiFi.status () = %d", WiFi.status());
    Serial.println(", WiFi.localIP() = " + WiFi.localIP().toString());
    Serial.print("OTA_HOSNAME = ");
    Serial.println(OTA_HOSNAME);
    //        Serial.printf ("Free heap: %u\n", ESP.getFreeHeap ());
    i++;
  }

  if (now() > 100000 and ip != "0.0.0.0" and ntp_sync_period < NTP_SYNC_PERIOD_MAX)
  { //постепенно увеличиваем период обновлений до суток
    ntp_sync_period += 63;
    Serial.print("ntp_sync_period = ");
    Serial.println(ntp_sync_period);
    NTP.setInterval(ntp_sync_period); // in seconds
    if (loop_delay < LOOP_DELAY_MAX)
    {
      loop_delay += 1; //sec //постепенно увеличиваем период цикла
    }
  }
  else if (now() < 100000 and ip != "0.0.0.0")
  {
    TimeValidator();
  }
  //! **** NTP *******

  //   json = get_emon_dataS(emon_get_power_node_id);

  //   power = getValue(json, ',', 0);
  // energy_total = getValue(json, ',', 1);

  json = get_emon_dataS(emon_get_power_node_id);

  deserializeJson(doc, json);

  // dat = doc["value"];
  power = doc[0];
  energy_total = doc[1];

  Serial.println();
  Serial.print("power = ");
  Serial.print(int(power));
  Serial.println(" W");

  hour_cur = hour();
  bool is_day = hour_cur >= 7 and hour_cur < 23;

  if (FirstStart)
  {
    if (is_day)
    {
      energy_day_beg = energy_total;
      Serial.println("is_day");
      Serial.print("energy_day_beg = ");
      Serial.println(energy_day_beg);
    }
    else
    {
      energy_night_beg = energy_total;
      Serial.println("is_night");
      Serial.println("energy_night_beg = ");
      Serial.println(energy_night_beg);
    }
  }

  if (is_day)
  {
    energy_day_delta = energy_total - energy_day_beg;
    Serial.print("energy_day_delta = ");
    Serial.print(energy_day_delta);
    Serial.print(" kWh = ");
    Serial.print(energy_total - energy_day_beg);
    Serial.print(" Wh");
  }
  else
  {
    energy_night_delta = energy_total - energy_night_beg;
    Serial.print("energy_night_delta = ");
    Serial.println(energy_night_delta);
  }

  if (hour_prev == 22 and hour_cur == 23)
  {
    energy_night_beg = energy_total;
    Serial.println("energy_night_beg = ");
    Serial.println(energy_night_beg);
  }
  if (hour_prev == 6 and hour_cur == 7)
  {
    energy_day_beg = energy_total;
    Serial.println("energy_day_beg = ");
    Serial.println(energy_day_beg);
  }

  hour_prev = hour_cur;

  Serial.println();
  Serial.print("energy_total = ");
  Serial.print(energy_total);
  Serial.print("kWh = ");
  Serial.print("energy_night_delta = ");
  Serial.print(energy_night_delta);
  Serial.print(" kWh, ");
  Serial.print("energy_day_delta = ");
  Serial.print(energy_day_delta);
  Serial.println(" kWh");

  lc.clearDisplay(0);

  // setDigit(int addr, int digit, byte value, boolean dp);

  // lc_print(int v, int poz, bool dp)
  lc_print(int(power), 4, true);
  delay(500);
  lc_print(energy_night_delta, 2, true);
  delay(500);
  lc_print(energy_day_delta, 0, true);
  delay(500);
  // lc.setDigit(1, 0, int(power), true);
  // lc.setDigit(0, 0, int(round(energy_night_delta / 1000)), true);
  // lc.setDigit(0, 2, int(round(energy_day_delta / 1000)), true);

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

  // Serial.println(B10000000 & B00000001);

  digitalWrite(ONBOARDLED, HIGH); // Turn off LED
  delay(loop_delay * 1000);       //задержка большого цикла
  FirstStart = false;
}
