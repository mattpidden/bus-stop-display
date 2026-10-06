#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"
#include <set>
#include "secrets.h" // WIFI_SSID and WIFI_PASSWORD - copy secrets.example.h to secrets.h

// ---- Settings: change these for your own stop ----
const String stopId = "490003683N"; // TfL NaPTAN stop ID (this one is Battersea Bridge Northbound)
const char* timezone = "GMT0BST,M3.5.0/1,M10.5.0"; // POSIX TZ string (UK time)
const int sdaPin = 25;
const int sclPin = 26;
int buttonPin = 33;
// ---------------------------------------------------

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* ntpServer = "pool.ntp.org";
LiquidCrystal_I2C lcd(0x27, 20, 4);
int mode = 0; // 0 = all buses, 1+ = specific routes
int scroll = 0; // 0 for 1-4, 1 for 1,5-7, 2 for 1,8-10
JsonArray arr;
DynamicJsonDocument doc(8192);
unsigned long lastDataFetch = 0;
unsigned long lastButtonPress = 0;
unsigned long lastScroll = 0;
const unsigned long fetchInterval = 20000;
const unsigned long buttonInterval = 30000;
const unsigned long scrollInterval = 10000;
int lastState = HIGH;
static std::set<String> seenBuses;
static unsigned long busCounter = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin(sdaPin, sclPin);
  lcd.init();
  lcd.backlight();
  pinMode(buttonPin, INPUT_PULLUP);
  WiFi.begin(ssid, password);
  lcd.setCursor(0,0);
  lcd.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    lcd.print(".");
  }
  lcd.clear();
  configTzTime(timezone, ntpServer);
  fetchData();
  updateDisplay();
}

void reconnectWifi() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Reconnecting WiFi");
  Serial.println("Reconnecting WiFi...");

  WiFi.disconnect();
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) { // try for 10s
    lcd.print(".");
    delay(500);
  }
}

void fetchData() {
  Serial.print("Fetching bus arrivals... ");
  String url = "https://api.tfl.gov.uk/StopPoint/" + stopId + "/Arrivals";

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  Serial.print("HTTP code: ");
  Serial.println(httpCode);

  if(httpCode == 200){
    String payload = http.getString();
    DeserializationError error = deserializeJson(doc, payload);
    if(error){
      Serial.print("JSON parse failed: ");
      Serial.println(error.c_str());
    } else {
      arr = doc.as<JsonArray>();
      static int lastDay = -1;
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        if (timeinfo.tm_mday != lastDay) {
          lastDay = timeinfo.tm_mday;
          busCounter = 0;
          Serial.println("Midnight detected, counter reset to 0");
        }
      }
      std::set<String> newSeen;
      for (JsonObject obj : arr) {
        String id = obj["vehicleId"].as<String>();
        newSeen.insert(id);
      }
      // detect buses that disappeared (they've departed)
      for (String oldId : seenBuses) {
        if (newSeen.find(oldId) == newSeen.end()) {
          busCounter++;
          Serial.print("Bus departed! Total count: ");
          Serial.println(busCounter);
        }
      }
      seenBuses = newSeen;
    }
  } else {
    Serial.print("HTTP Error: ");
    Serial.println(httpCode);
    lcd.setCursor(0,0);
    lcd.print("HTTP Error: " + String(httpCode));
  }
  http.end();

  
}

void updateDisplay() {
  // DynamicJsonDocument temp(doc); 
  // JsonArray localArr = temp.as<JsonArray>();
  
  String routeFilter = "";
  if (mode > 0) {
    std::vector<String> routes;
    for (JsonObject obj : arr) {
      String line = obj["lineName"].as<String>();
      bool found = false;
      for (String r : routes) {
        if (r == line) { found = true; break; }
      }
      if (!found) routes.push_back(line);
    }
    int numRoutes = routes.size();
    if (mode > numRoutes + 1) {
      mode = 0; // wrap around to show all again
      lastScroll = millis();
      scroll = 0;
    }
    if (mode == numRoutes + 1) {
      // special stats mode
      lcd.clear();
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        char timeStr[6];
        strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
        lcd.setCursor(0,1);
        lcd.print("       ");
        lcd.print(timeStr);
      }
      lcd.setCursor(0,2);
      lcd.print("  Buses Today: ");
      lcd.print(busCounter);
      return;
    }
    if (mode > 0 && mode <= numRoutes) {
      routeFilter = routes[mode-1];
    }
    Serial.print("Displaying bus route: ");
    Serial.println(routeFilter);
  }

  // sort local array in time order
  std::vector<JsonObject> sorted;
  for (JsonObject obj : arr) {
    long t = obj["timeToStation"].as<long>();
    auto it = std::find_if(sorted.begin(), sorted.end(), [&](JsonObject other){
      return t < other["timeToStation"].as<long>();
    });
    sorted.insert(it, obj);
  }

  // filter local array to only show routeFilter
  std::vector<JsonObject> filtered;
  for (JsonObject obj : sorted) {
    if (routeFilter == "" || obj["lineName"].as<String>() == routeFilter) {
      filtered.push_back(obj);
    }
  }

  
  
  // find the 4 soonest buses
  for (int num = 0; num < 4; num++){
    int lineNum = num;
    if (mode == 0 && lineNum > 1) {
      lineNum += (scroll * 2);
    }
    
    if(lineNum >= filtered.size()) {
      lcd.setCursor(0,num);
      lcd.print("                    ");
    } else {
      // Build formatted string
      String line = "";
      // 1) line number (1 char) 
      line += String(lineNum + 1);
      // (1 space)
      line += " ";
      // 2) bus number (3 chars) - aligned right
      String busNum = filtered[lineNum]["lineId"].as<String>();
      while(busNum.length() < 3) busNum = " " + busNum;
      line += busNum;
      // (1 space)
      line += " ";
      // 3) destination name (8 chars) - aligned left
      String dest = filtered[lineNum]["destinationName"].as<String>();
      if(dest.length() > 8) dest = dest.substring(0,8);
      while(dest.length() < 8) dest += " ";  // pad right
      line += dest;
      // (1 space)
      line += " ";
      // 4) due time (5 chars) - aligned right
      long secs = filtered[lineNum]["timeToStation"].as<long>();
      String minStr = (secs < 60) ? "Due" : String(secs/60) + "min";
      // align minutes to 2 spaces (right aligned)
      while(minStr.length() < 5) minStr = " " + minStr;
      line += minStr;
      // print to LCD and Serial
      lcd.setCursor(0,num);
      lcd.print(line); // overwrite old chars
      delay(125);
    }
  }
}

void loop() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastDataFetch >= fetchInterval) {
    lastDataFetch = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      reconnectWifi();
    } else {
      fetchData();
      Serial.println("Updating display after new data fetch");
      updateDisplay();
    }
  }

  if (currentMillis - lastScroll >= scrollInterval) {
    lastScroll = millis();
    scroll = (scroll + 1) % 3;
    updateDisplay();
  }

  if (currentMillis - lastButtonPress >= buttonInterval) {
    mode = 0;
    lastButtonPress = millis();
    lastScroll = millis();
    scroll = 0;
    Serial.println("Updating display after resetting to mode 0");
    updateDisplay();
  }

  int currentState = digitalRead(buttonPin);
  if (currentState != lastState) {
    lastState = currentState;
    if (currentState == LOW) {
      lastButtonPress = millis();
      mode++;
      Serial.println("Updating display after button press to new mode");
      lcd.clear();
      updateDisplay();
    }
  }
}
