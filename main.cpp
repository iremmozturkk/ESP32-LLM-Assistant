#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <WiFiUdp.h>
#include "driver/i2s.h"
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "AudioFileSourceHTTPStream.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

// Wi-Fi Ayarları
#define WIFI_SSID    "İrem"
#define WIFI_PASS    "01234567"
#define SERVER_URL   "http://192.168.137.22:5000/upload"

// UDP Ayarları
WiFiUDP udp;
const char* udpAddress = "192.168.137.22";
const int udpPort = 4210;

const char* udpAddress1 = "192.168.137.80";  // İkinci ESP32'nin IP'si
const int udpPort1 = 5000;

// INMP441 Mikrofon - I2S0
#define I2S0_BCK 21
#define I2S0_WS  20
#define I2S0_SD  19

// PCM5102A Hoparlör - I2S1
#define DAC_BCK 15
#define DAC_WS  14
#define DAC_DIN 13

// Sensör
#define PIR_PIN 33
Adafruit_BME280 bme;

// Ses parametreleri
#define SAMPLE_RATE     16000
#define SAMPLE_BITS     I2S_BITS_PER_SAMPLE_16BIT
#define CHANNEL_FORMAT  I2S_CHANNEL_FMT_ONLY_LEFT
#define RECORD_TIME_SEC 3
#define BUFFER_SIZE     (SAMPLE_RATE * RECORD_TIME_SEC)
#define MAX_WAV_SIZE    (BUFFER_SIZE * 2 + 44)

uint8_t pcm_data[BUFFER_SIZE * 2];
uint8_t wav_data[MAX_WAV_SIZE];
AudioFileSourceHTTPStream* file;
AudioOutputI2S* out;
AudioGeneratorWAV* wav;

// WAV header oluşturur
void create_wav_header(uint8_t* h, size_t pcm_size, int sr) {
  int byte_rate   = sr * 2;
  int block_align = 2;
  memcpy(h, "RIFF", 4);
  uint32_t cs = pcm_size + 36;
  memcpy(h + 4, &cs, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t sub1 = 16;
  memcpy(h + 16, &sub1, 4);
  h[20] = 1; h[21] = 0;
  h[22] = 1; h[23] = 0;
  memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byte_rate, 4);
  h[32] = block_align; h[33] = 0;
  h[34] = 16; h[35] = 0;
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &pcm_size, 4);
}

// Wi-Fi bağlantısı
void wifi_connect() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi bağlandı: " + WiFi.localIP().toString());
}

// I2S0 - Mikrofon başlat
void i2s_record_init() {
  i2s_driver_uninstall(I2S_NUM_0);
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = SAMPLE_BITS,
    .channel_format = CHANNEL_FORMAT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count = 4,
    .dma_buf_len = 1024
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S0_BCK,
    .ws_io_num = I2S0_WS,
    .data_out_num = -1,
    .data_in_num = I2S0_SD
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// I2S1 - Hoparlör başlat
void i2s_play_init() {
  i2s_driver_uninstall(I2S_NUM_1);
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = SAMPLE_BITS,
    .channel_format = CHANNEL_FORMAT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = true
  };
  i2s_pin_config_t pins = {
    .bck_io_num = DAC_BCK,
    .ws_io_num = DAC_WS,
    .data_out_num = DAC_DIN,
    .data_in_num = -1
  };
  i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_zero_dma_buffer(I2S_NUM_1);
  i2s_set_clk(I2S_NUM_1, SAMPLE_RATE, SAMPLE_BITS, I2S_CHANNEL_MONO);
}

// WAV dosyasını HTTP ile gönder
String send_audio_to_server(uint8_t* data, size_t len) {
  WiFiClient client;
  client.setTimeout(60000);
  HTTPClient http;
  http.begin(client, SERVER_URL);
  http.setTimeout(60000);
  http.addHeader("Content-Type", "audio/wav");

  int code = http.sendRequest("POST", data, len);
  String resp = "";
  if (code == HTTP_CODE_OK) {
    resp = http.getString();
    Serial.println("📨 Sunucudan gelen yanıt URL:");
    Serial.println(resp);
  } else {
    Serial.printf("🚫 HTTP Hatası: %d %s\n", code, http.errorToString(code).c_str());
  }
  http.end();
  return resp;
}

// WAV sesini internetten çal
void play_wav_from_url(const String& url) {
  Serial.println("▶ Ses çalınıyor...");
  i2s_play_init();
  file = new AudioFileSourceHTTPStream(url.c_str());
  out = new AudioOutputI2S();
  out->SetPinout(DAC_BCK, DAC_WS, DAC_DIN);
  out->SetGain(1.0);
  wav = new AudioGeneratorWAV();
  if (!wav->begin(file, out)) {
    Serial.println("❌ WAV başlatılamadı.");
  }
}

// Kurulum
void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  Wire.begin(8, 9);
  if (!bme.begin(0x76)) {
    Serial.println("❌ BME280 bulunamadı!");
    while (true);
  }
  wifi_connect();
}

// Döngü
void loop() {
  // Eğer ses çalıyorsa sadece devam et
  if (wav && wav->isRunning()) {
    wav->loop();
    return;
  }

  // Sensör verisi oku ve UDP ile gönder
  int hareket = digitalRead(PIR_PIN);
  String hareketDurumu = (hareket == HIGH) ? "Hareket var" : "Hareket yok";
  float sicaklik = bme.readTemperature();
  float nem = bme.readHumidity();
  float basinc = bme.readPressure() / 100.0F;

  String udpMesaj = "{";
  udpMesaj += "\"hareket\":\"" + hareketDurumu + "\",";
  udpMesaj += "\"sicaklik\":" + String(sicaklik, 2) + ",";
  udpMesaj += "\"nem\":" + String(nem, 2) + ",";
  udpMesaj += "\"basinc\":" + String(basinc, 2);
  udpMesaj += "}";

  udp.beginPacket(udpAddress, udpPort);
  udp.print(udpMesaj);
  udp.endPacket();

  udp.beginPacket(udpAddress1, udpPort1);
  udp.print(udpMesaj);
  udp.endPacket();

Serial.println("📤 UDP veri 2 cihaza gönderildi:");
Serial.println(udpMesaj);

 

  // Sesli komut kaydı başlat
  i2s_record_init();
  Serial.println("🎙 Komut kaydediliyor...");
  size_t bytes_read = 0;
  i2s_read(I2S_NUM_0, pcm_data, BUFFER_SIZE * 2, &bytes_read, portMAX_DELAY);
  Serial.printf("✅ %d byte okundu.\n", bytes_read);
  i2s_stop(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);

  // Sunucuya gönder
  create_wav_header(wav_data, bytes_read, SAMPLE_RATE);
  memcpy(wav_data + 44, pcm_data, bytes_read);
  String url = send_audio_to_server(wav_data, bytes_read + 44);
  if (url.length()) {
    play_wav_from_url(url);
  }

  delay(5000);  // kısa bekleme
}