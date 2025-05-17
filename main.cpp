#include <Arduino.h>
#include <WiFi.h>
#include <AsyncUDP.h>  // ESP32 için AsyncUDP kullanıyoruz
#include <HTTPClient.h>
#include <Wire.h>
#include "driver/i2s.h"
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "AudioFileSourceHTTPStream.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "SPIFFS.h"

// Wi-Fi ve Sunucu Ayarları
const char* WIFI_SSID = "İrem";
const char* WIFI_PASS = "01234567";
const char* SERVER_HOST = "192.168.137.22";  // Ana sunucu IP'si
const int SERVER_PORT = 5000;
#define SERVER_URL   String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/upload"
#define SENSOR_URL   String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/sensor"
#define SENSOR_LOG   "/sensor_log.txt"

// UDP Ayarları
AsyncUDP udp;  // AsyncUDP kullanıyoruz
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
#define CHUNK_TIME_SEC  2
#define TOTAL_TIME_SEC  10
#define CHUNK_SIZE      (SAMPLE_RATE * CHUNK_TIME_SEC)
#define BUFFER_SIZE     (CHUNK_SIZE * 2)  // 16-bit samples

uint8_t pcm_data[BUFFER_SIZE];
String session_id;
AudioFileSourceHTTPStream* file;
AudioOutputI2S* out;
AudioGeneratorWAV* wav;
bool i2s_initialized = false;

// Yanıt bekleme durumu
bool waiting_for_response = false;

// LED pin tanımı
#define LED_PIN 2  // ESP32'nin dahili LED'i

// WAV header oluşturur
void create_wav_header(uint8_t* h, size_t pcm_size, int sr) {
  // RIFF header
  memcpy(h, "RIFF", 4);
  // Chunk size
  uint32_t chunk_size = pcm_size + 36;
  h[4] = chunk_size & 0xFF;
  h[5] = (chunk_size >> 8) & 0xFF;
  h[6] = (chunk_size >> 16) & 0xFF;
  h[7] = (chunk_size >> 24) & 0xFF;
  // Format
  memcpy(h + 8, "WAVE", 4);
  // Subchunk1 ID
  memcpy(h + 12, "fmt ", 4);
  // Subchunk1 size
  h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;
  // Audio format (PCM)
  h[20] = 1; h[21] = 0;
  // Channels (Mono)
  h[22] = 1; h[23] = 0;
  // Sample rate
  h[24] = sr & 0xFF;
  h[25] = (sr >> 8) & 0xFF;
  h[26] = (sr >> 16) & 0xFF;
  h[27] = (sr >> 24) & 0xFF;
  // Byte rate
  uint32_t byte_rate = sr * 2;
  h[28] = byte_rate & 0xFF;
  h[29] = (byte_rate >> 8) & 0xFF;
  h[30] = (byte_rate >> 16) & 0xFF;
  h[31] = (byte_rate >> 24) & 0xFF;
  // Block align
  h[32] = 2; h[33] = 0;
  // Bits per sample
  h[34] = 16; h[35] = 0;
  // Subchunk2 ID
  memcpy(h + 36, "data", 4);
  // Subchunk2 size
  h[40] = pcm_size & 0xFF;
  h[41] = (pcm_size >> 8) & 0xFF;
  h[42] = (pcm_size >> 16) & 0xFF;
  h[43] = (pcm_size >> 24) & 0xFF;
}

// Sunucu bağlantısını kontrol et
bool check_server_connection() {
  WiFiClient client;
  if (client.connect(SERVER_HOST, SERVER_PORT)) {
    client.stop();
    return true;
  }
  Serial.println("❌ Sunucuya bağlanılamadı. Yeniden deneniyor...");
  return false;
}

// Wi-Fi bağlantısı
void wifi_connect() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi bağlandı: " + WiFi.localIP().toString());
  
  // Sunucu bağlantısını kontrol et
  while (!check_server_connection()) {
    delay(5000);
  }
  Serial.println("✅ Sunucu bağlantısı hazır");
}

// I2S0 - Mikrofon başlat
void i2s_record_init() {
  if (i2s_initialized) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_initialized = false;
  }

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
  
  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S sürücü kurulumu hatası: %d\n", err);
    return;
  }
  
  err = i2s_set_pin(I2S_NUM_0, &pins);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S pin ayarı hatası: %d\n", err);
    return;
  }
  
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_initialized = true;
  Serial.println("✅ I2S başlatıldı");
}

// I2S1 - Hoparlör başlat
void i2s_play_init() {
  i2s_driver_uninstall(I2S_NUM_1);
  
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));  // Tüm alanları sıfırla
  
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = SAMPLE_BITS;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 1024;
  
  i2s_pin_config_t pins = {
    .bck_io_num = DAC_BCK,
    .ws_io_num = DAC_WS,
    .data_out_num = DAC_DIN,
    .data_in_num = -1
  };
  
  esp_err_t err = i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S sürücü kurulumu hatası: %d\n", err);
    return;
  }
  
  err = i2s_set_pin(I2S_NUM_1, &pins);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S pin ayarı hatası: %d\n", err);
    return;
  }
  
  i2s_zero_dma_buffer(I2S_NUM_1);
  Serial.println("✅ I2S1 hoparlör başlatıldı");
}

// Sensör verilerini HTTP ile gönder ve dosyaya kaydet
void send_sensor_data(const String& url, const String& data) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi bağlantısı koptu. Yeniden bağlanılıyor...");
    wifi_connect();
  }

  // Dosyaya kaydet
  File dataFile = SPIFFS.open(SENSOR_LOG, FILE_APPEND);
  if (dataFile) {
    dataFile.println(data);
    dataFile.close();
    Serial.println("✅ Veri dosyaya kaydedildi: " + String(SENSOR_LOG));
  } else {
    Serial.println("❌ Dosya açılamadı: " + String(SENSOR_LOG));
  }

  // Sunucuya gönder
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(data);
  if (code == HTTP_CODE_OK) {
    Serial.println("✅ Sensör verisi gönderildi: " + url);
  } else {
    Serial.printf("🚫 HTTP Hatası: %d %s\n", code, http.errorToString(code).c_str());
    if (code == -1) {
      Serial.println("Sunucu bağlantısı kontrol ediliyor...");
      check_server_connection();
    }
  }
  http.end();
}

// Ses chunk'ını sunucuya gönder
String send_audio_chunk(uint8_t* data, size_t len, bool is_first, bool is_last) {
  WiFiClient client;
  client.setTimeout(60000);
  HTTPClient http;
  http.begin(client, SERVER_URL);
  http.setTimeout(60000);
  
  // WAV header ekle (sadece ilk chunk için)
  size_t total_len = len;
  uint8_t* send_data = data;
  uint8_t* temp_buffer = NULL;
  
  if (is_first) {
    // Toplam ses verisi boyutunu hesapla
    size_t total_audio_size = BUFFER_SIZE * (TOTAL_TIME_SEC / CHUNK_TIME_SEC);
    
    // Header için bellek ayır
    temp_buffer = (uint8_t*)malloc(len + 44);
    if (temp_buffer == NULL) {
      Serial.println("❌ Bellek ayırma hatası!");
      return "";
    }
    
    // Header oluştur ve veriyi kopyala
    create_wav_header(temp_buffer, total_audio_size, SAMPLE_RATE);
    memcpy(temp_buffer + 44, data, len);
    
    send_data = temp_buffer;
    total_len = len + 44;
    
    Serial.println("✅ WAV header eklendi");
    Serial.printf("Header boyutu: 44 bytes\n");
    Serial.printf("Veri boyutu: %d bytes\n", len);
    Serial.printf("Toplam boyut: %d bytes\n", total_len);
  }

  // Session ID kontrolü
  if (session_id == "0" || session_id.length() < 4) {
    session_id = String(random(0xFFFFFFFF), HEX);
  }

  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Session-ID", session_id);
  http.addHeader("X-First-Chunk", is_first ? "true" : "false");
  http.addHeader("X-Last-Chunk", is_last ? "true" : "false");

  int code = http.POST(send_data, total_len);
  String resp = "";
  
  if (code == HTTP_CODE_OK) {
    resp = http.getString();
    if (is_last) {
      Serial.println("📨 Sunucudan gelen yanıt URL:");
      Serial.println(resp);
    } else {
      Serial.println("✅ Chunk başarıyla gönderildi");
    }
  } else {
    Serial.printf("🚫 HTTP Hatası: %d %s\n", code, http.errorToString(code).c_str());
    Serial.println("Sunucu yanıtı: " + http.getString());
    
    // Hata durumunda yeni session ID oluştur
    if (code == 400) {
      session_id = String(random(0xFFFFFFFF), HEX);
      Serial.println("🔄 Yeni session ID oluşturuldu: " + session_id);
    }
  }

  // Belleği temizle
  if (temp_buffer != NULL) {
    free(temp_buffer);
  }

  http.end();
  return resp;
}

// Ses çalma durumunu kontrol et
bool is_audio_playing() {
  return (wav && wav->isRunning());
}

// Ses çalma işlemini bekle
void wait_for_audio() {
  Serial.println("\n🔊 Lütfen yanıtı dinleyin...");
  
  while (is_audio_playing()) {
    if (!wav->loop()) {
      wav->stop();
      delete wav;
      delete out;
      delete file;
      wav = nullptr;
      out = nullptr;
      file = nullptr;
      Serial.println("✅ Ses çalma tamamlandı");
      break;
    }
    delay(1);
  }
  
  Serial.println("\n🎙 Yeni komut için hazır...");
  Serial.println("(Lütfen konuşmaya başlayın)");
  delay(2000);  // Kullanıcıya hazırlanması için süre ver
}

// WAV sesini internetten çal
void play_wav_from_url(const String& url) {
  Serial.println("▶ Ses çalınıyor...");
  
  // Önceki I2S yapılandırmasını temizle
  if (i2s_initialized) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_initialized = false;
  }
  
  // I2S1'i temizle
  i2s_driver_uninstall(I2S_NUM_1);
  
  // Ses çıkışını yapılandır
  file = new AudioFileSourceHTTPStream(url.c_str());
  out = new AudioOutputI2S();
  out->SetPinout(DAC_BCK, DAC_WS, DAC_DIN);
  out->SetBitsPerSample(16);
  out->SetRate(16000);
  out->SetChannels(1);
  out->SetGain(1.0);
  
  wav = new AudioGeneratorWAV();
  if (!wav->begin(file, out)) {
    Serial.println("❌ WAV başlatılamadı.");
    return;
  }
  
  Serial.println("✅ Ses çalma başlatıldı");
}

// Kurulum
void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  Wire.begin(8, 9);
  
  // SPIFFS başlat
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS başlatılamadı!");
  } else {
    Serial.println("✅ SPIFFS başlatıldı");
  }
  
  if (!bme.begin(0x76)) {
    Serial.println("❌ BME280 bulunamadı!");
    while (true);
  }
  wifi_connect();
}

// Döngü
void loop() {
  // WiFi bağlantısını kontrol et
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi bağlantısı koptu. Yeniden bağlanılıyor...");
    wifi_connect();
    return;
  }

  // Eğer ses çalıyorsa bekle
  if (is_audio_playing()) {
    wav->loop();
    return;
  }

  // Sensör verisi oku ve HTTP ile gönder
  int hareket = digitalRead(PIR_PIN);
  String hareketDurumu = (hareket == HIGH) ? "Hareket var" : "Hareket yok";
  float sicaklik = bme.readTemperature();
  float nem = bme.readHumidity();
  float basinc = bme.readPressure() / 100.0F;

  // Sensör verilerini terminale yazdır
  Serial.println("\n📊 GÜNCEL SENSÖR VERİLERİ:");
  Serial.println("------------------------");
  Serial.printf("🚶 Hareket: %s\n", hareketDurumu.c_str());
  Serial.printf("🌡️ Sıcaklık: %.2f°C\n", sicaklik);
  Serial.printf("💧 Nem: %%%.2f\n", nem);
  Serial.printf("⭕ Basınç: %.2fhPa\n", basinc);
  Serial.println("------------------------\n");

  // JSON formatında sensör verisi oluştur
  String sensorData = "{";
  sensorData += "\"hareket\":\"" + hareketDurumu + "\",";
  sensorData += "\"sicaklik\":" + String(sicaklik, 2) + ",";
  sensorData += "\"nem\":" + String(nem, 2) + ",";
  sensorData += "\"basinc\":" + String(basinc, 2);
  sensorData += "}";

  // Sensör verilerini gönder ve dosyaya kaydet
  send_sensor_data(SENSOR_URL, sensorData);

  // Yeni kayıt başlamadan önce hazır olduğunu bildir
  if (!waiting_for_response) {
    Serial.println("\n🎙 Yeni komut için hazır...");
    Serial.println("(Lütfen konuşmaya başlayın)");
    delay(1000);  // Kullanıcıya hazırlanması için kısa süre ver
  }

  // Yeni kayıt oturumu başlat
  session_id = String(random(0xFFFFFFFF), HEX);
  if (session_id == "0") session_id = "1";
  i2s_record_init();
  Serial.println("\n🎙 Komut kaydediliyor...");

  String url = "";
  for (int chunk = 0; chunk < TOTAL_TIME_SEC / CHUNK_TIME_SEC; chunk++) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("❌ WiFi bağlantısı koptu. Kayıt iptal ediliyor...");
      break;
    }

    size_t bytes_read = 0;
    i2s_read(I2S_NUM_0, pcm_data, BUFFER_SIZE, &bytes_read, portMAX_DELAY);
    Serial.printf("✅ Chunk %d: %d byte okundu.\n", chunk + 1, bytes_read);

    bool is_first = (chunk == 0);
    bool is_last = (chunk == (TOTAL_TIME_SEC / CHUNK_TIME_SEC - 1));

    url = send_audio_chunk(pcm_data, bytes_read, is_first, is_last);
    if (url.length() && is_last) {
      Serial.println("\n⏳ LLM yanıtı bekleniyor...");
      waiting_for_response = true;
      play_wav_from_url(url);
      // Ses çalma işleminin bitmesini bekle
      wait_for_audio();
      waiting_for_response = false;
      break;
    }
    
    // Hata durumunda kısa bir bekleme
    if (url.length() == 0) {
      delay(1000);
    }
  }

  // I2S'i durdur
  if (i2s_initialized) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_initialized = false;
  }

  delay(1000);  // kısa bekleme
}