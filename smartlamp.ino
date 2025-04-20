#define BLYNK_TEMPLATE_ID "TMPL6oc6e_vIb"
#define BLYNK_TEMPLATE_NAME "Smart Lamp Blynk"

#define BLYNK_PRINT Serial
#include <Wire.h>
#include <BH1750.h>
#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <RBDdimmer.h>
#include <FirebaseESP8266.h>

// Konfigurasi Firebase
#define FIREBASE_HOST "https://sistemotomatisasilampu-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH "7gdCUCdhTDeKkbVEsebBkfdMS0TQdtev3gTttcjL"

FirebaseData firebaseData;
FirebaseConfig config;
FirebaseAuth auth;

// Konfigurasi WiFi dan Blynk
char blynkauth[] = "qetcSMP8W1VnJXqA2es8P8JRDQHi4dmU";
char ssid[] = "Antika";
char pass[] = "12345678";

// Definisi pin sensor PIR
#define PIR_PIN D7   // Pin input sensor gerak PIR

// Definisi pin untuk modul AC light dimmer
#define ZC_PIN  D5   // Zero Crossing input pada pin D5 (GPIO14)
#define DIMMER_PIN D6 // Output dimmer ke pin D6 (GPIO12)

dimmerLamp dimmer(DIMMER_PIN, ZC_PIN); // Inisialisasi objek dimmer
BlynkTimer timer;
BH1750 lightMeter; // Inisialisasi objek sensor BH1750

// Variabel kontrol sistem
int mode = 0;  // 0 = Manual, 1 = Otomatis
int brightness = 100;  // Nilai default kecerahan lampu (0-100%)
int roomType = 0;  // Jenis ruangan, default: Ruang Tidur
int lastMotionState = LOW;
unsigned long lastMotionTime = 0;
const unsigned long motionDelay = 60000; // Penundaan reset gerakan
unsigned long manualStartTime = 0;
bool manualWarningSent = false;

// Struktur parameter pencahayaan per ruangan
struct LightingParams {
  int luxThreshold;    // Ambang batas lux cahaya alami
  int maxBrightness;   // Nilai maksimum kecerahan lampu
  int minBrightness;   // Nilai minimum kecerahan lampu
};

// Fungsi untuk memeriksa koneksi ke Blynk
void checkConnection() {
  if (!Blynk.connected()) {
    Serial.println("Koneksi terputus! Mencoba menghubungkan kembali...");
    Blynk.connect();
  }
}

// Fungsi untuk memilih mode kontrol dari aplikasi Blynk
BLYNK_WRITE(V2) {
  mode = param.asInt();

  if (mode == 1) {
      Blynk.setProperty(V5, "enabled", false);
      Blynk.logEvent("mode_otomatis", "Sistem dalam mode otomatis.");
      Serial.println("Mode Otomatis Aktif - Slider V5 Dinonaktifkan");
  } else {
      Blynk.setProperty(V5, "enabled", true);
      Blynk.logEvent("mode_manual", "Sistem dalam mode manual.");
      Serial.println("Mode Manual Aktif - Slider V5 Diaktifkan");
  }
}

// Fungsi untuk kontrol lampu ON/OFF manual dari Blynk (V0)
BLYNK_WRITE(V0) {
  if (mode == 0) {  
    if (param.asInt() == 1) {
      if (brightness == 0) brightness = 50; 
    } else {
      brightness = 0;
    }
    dimmer.setPower(brightness);
    
    Blynk.setProperty(V5, "enabled", brightness > 0);
    Serial.print("Mode Manual - Lampu: ");
    Serial.println(brightness > 0 ? "Menyala" : "Mati");
  }
}

// Fungsi pengaturan kecerahan manual melalui slider Blynk (V5)
BLYNK_WRITE(V5) {
  if (mode == 0 && brightness > 0) { 
    brightness = param.asInt();  
    dimmer.setPower(brightness);
    Blynk.virtualWrite(V5, brightness);
    Serial.print("Kecerahan diatur ke: ");
    Serial.println(brightness);
  }
}

// Fungsi untuk memilih jenis ruangan dari aplikasi Blynk (V4)
BLYNK_WRITE(V4) {
  roomType = param.asInt();
  sendLightIntensity();
}

// Fungsi pengaturan parameter pencahayaan berdasarkan jenis ruangan
LightingParams getLightingParams() {
  LightingParams params;
  switch (roomType) {
    case 1: params = {50, 30, 0}; break;          // Ruang Tidur
    case 2: params = {350, 100, 90}; break;       // Ruang Belajar/Kerja
    case 3: params = {100, 60, 20}; break;        // Ruang Makan
    case 4: params = {100, 80, 50}; break;        // Ruang Keluarga
    case 5: params = {150, 90, 70}; break;        // Ruang Tamu
    case 6: params = {250, 100, 90}; break;       // Dapur
    default: params = {50, 30, 0};                // Default: Ruang Tidur
  }
  return params;
}

// Fungsi untuk kirim notifikasi jika lampu mati dan cahaya alami cukup
void checkLightingStatus(int brightness, float lux, int luxThreshold) {
  if (brightness == 0 && lux >= luxThreshold) {
    Blynk.logEvent("lampu_mati", "Lampu mati, cahaya alami cukup");
    Serial.println("Notifikasi: Lampu mati, cahaya alami cukup");
  }
}

// Fungsi untuk mengirim data ke Firebase Realtime Database
void sendToFirebase(float lux, int motion, int brightness, int mode, String roomName) {
  FirebaseJson json;
  unsigned long timestamp = millis();
  String path = "/history_logs/" + String(timestamp);

  json.set("timestamp", timestamp);
  json.set("mode", mode == 0 ? "manual" : "auto");
  json.set("lamp/brightness", brightness);
  json.set("lamp/status", brightness > 0 ? "ON" : "OFF");
  json.set("sensor/lux", lux);
  json.set("sensor/pir", motion);
  json.set("room/name", roomName);
  
  if (Firebase.updateNode(firebaseData, path, json)) {
    Serial.println("Data berhasil dikirim ke: " + path);
  } else {
    Serial.println("Gagal mengirim data: " + firebaseData.errorReason());
  }

  FirebaseJson statusJson;
  statusJson.set("last_update", timestamp);
  statusJson.set("current_mode", mode == 0 ? "manual" : "auto");
  statusJson.set("current_brightness", brightness);
  statusJson.set("current_room", roomName);
  Firebase.updateNode(firebaseData, "/system_status", statusJson);
}

// Fungsi utama untuk membaca sensor cahaya dan gerakan
void sendLightIntensity() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate < 3000) return;
  lastUpdate = millis();

  float lux = lightMeter.readLightLevel();
  if (lux < 0) {
    Serial.println("Gagal membaca sensor BH1750!");
    return;
  }
  Serial.print("Nilai Lux: ");
  Serial.println(lux);
  Blynk.virtualWrite(V1, lux);

  int motion = digitalRead(PIR_PIN);
  if (motion == HIGH) {
    lastMotionTime = millis();
    lastMotionState = HIGH;
  }
  if (motion == HIGH && millis() - lastMotionTime > 5000) { 
    lastMotionTime = millis();
    lastMotionState = HIGH;
  } else if (millis() - lastMotionTime > 1000) {
    lastMotionState = LOW;
  }
  motion = lastMotionState;

  Serial.print("Status Gerakan: ");
  Serial.println(motion ? "Terdeteksi!" : "Tidak Ada");
  Blynk.virtualWrite(V3, motion);

  LightingParams params = getLightingParams();
  int newBrightness = brightness;

  if (mode == 1) {
    if (lux >= params.luxThreshold) {
      newBrightness = 0;
    } else if (lux < params.luxThreshold && motion == HIGH) {
      newBrightness = map(lux, 0, params.luxThreshold, params.maxBrightness, params.minBrightness);
      newBrightness = constrain(newBrightness, params.minBrightness, params.maxBrightness);
    } else if (motion == LOW) {
      newBrightness = 0;
    }

    if (newBrightness != brightness) {
      brightness = newBrightness;
      dimmer.setPower(brightness);
      Serial.print("Mode Otomatis - Kecerahan diatur ke: ");
      Serial.println(brightness);
    }
    Blynk.virtualWrite(V5, brightness);
  }

  Blynk.virtualWrite(V0, brightness > 0 ? 1 : 0);
  checkLightingStatus(brightness, lux, params.luxThreshold);

  String roomName;
  switch (roomType) {
    case 1: roomName = "Ruang Tidur"; break;
    case 2: roomName = "Ruang Belajar/Kerja"; break;
    case 3: roomName = "Ruang Makan"; break;
    case 4: roomName = "Ruang Keluarga"; break;
    case 5: roomName = "Ruang Tamu"; break;
    case 6: roomName = "Dapur"; break;
    default: roomName = "Ruang Tidur";
  }
  sendToFirebase(lux, motion, brightness, mode, roomName);
}

// Setup awal program
void setup() {
  Serial.begin(115200);
  Serial.println("Menghubungkan ke Blynk...");
  timer.setInterval(2000L, sendLightIntensity);

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Wire.begin(D2, D1);
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("Sensor BH1750 berhasil diinisialisasi!");
  } else {
    Serial.println("Gagal menginisialisasi BH1750! Periksa koneksi I2C.");
  }

  dimmer.begin(NORMAL_MODE, ON);
  pinMode(PIR_PIN, INPUT);

  Blynk.begin(blynkauth, ssid, pass, "blynk.cloud", 80);
  Blynk.syncVirtual(V0);
}

// Loop utama program
void loop() {
  if (Blynk.connected()) {
    Blynk.run();
  }
  timer.run();  // Jalankan fungsi berkala dengan BlynkTimer
}
