#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h> // Yeni eklenen kütüphane

// OLED Ayarları
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_MOSI   23
#define OLED_CLK    18
#define OLED_DC     17
#define OLED_CS     5
#define OLED_RESET  16
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RESET, OLED_CS);

// Wi-Fi Ayarları
const char* ssid = "İrem";
const char* password = "01234567";

// UDP Ayarları
WiFiUDP udp;
const int localUdpPort = 5000;
char incomingPacket[512];

// Sensör Veri Yapısı
struct SensorData {
  String hareket = "Yok";
  float sicaklik = 0.0;
  float nem = 0.0;
  float basinc = 0.0;
} sensorData;

void setup() {
  Serial.begin(115200);
  
  // OLED Başlatma
  display.begin(0, true);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  displayHeader("Baglanti Kuruluyor...");

  // Wi-Fi Bağlantısı
  connectToWiFi();

  // UDP Başlatma
  udp.begin(localUdpPort);
}

void loop() {
  handleUDP();
  updateDisplay();
}

void connectToWiFi() {
  WiFi.begin(ssid, password);
  displayHeader("WiFi Baglaniyor...");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    displayHeader("WiFi Baglandi!");
    display.println("IP: " + WiFi.localIP().toString());
    Serial.println("IP: " + WiFi.localIP().toString());
    display.display();
    delay(2000);
  } else {
    displayHeader("Baglanti Hatasi!");
    while(1) delay(1000); // Sonsuz döngü
  }
}

void handleUDP() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    int len = udp.read(incomingPacket, 512);
    if (len > 0) {
      incomingPacket[len] = 0;
      parseSensorData(incomingPacket);
    }
  }
}

void parseSensorData(const char* json) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial.print("JSON Parse Hatasi: ");
    Serial.println(error.c_str());
    return;
  }

  sensorData.hareket = doc["hareket"].as<String>();
  sensorData.sicaklik = doc["sicaklik"].as<float>();
  sensorData.nem = doc["nem"].as<float>();
  sensorData.basinc = doc["basinc"].as<float>();

  Serial.println("Yeni Veri Alindi:");
  Serial.println(sensorData.hareket);
  Serial.println(sensorData.sicaklik);
  Serial.println(sensorData.nem);
  Serial.println(sensorData.basinc);
}

void updateDisplay() {
  display.clearDisplay();
  displayHeader("Son Olcumler");
  
  // Veri Gösterimi
  display.setCursor(0, 15);
  display.printf("Hareket : %s\n", sensorData.hareket.c_str());
  display.printf("Sicaklik: %.2f C\n", sensorData.sicaklik);
  display.printf("Nem     : %.2f %%\n", sensorData.nem);
  display.printf("Basinc  : %.2f hPa", sensorData.basinc);
  
  display.display();
}

void displayHeader(const char* title) {
  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextColor(SH110X_WHITE);
  display.println(title);
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SH110X_WHITE);
}