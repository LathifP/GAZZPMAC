#include "RTClib.h"
#include <Adafruit_ADS1X15.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <SD.h>
#include <SPI.h>
#include <SensirionI2cScd4x.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

/* ================= DEBUG ================= */
#define DEBUG 1
#if DEBUG
#define DBG(x) Serial.println(x)
#else
#define DBG(x)
#endif

/* ================= WIFI ================= */
const char *ssid = "CaPT";
const char *password = "spontanuhuy";
String serverName = "https://script.google.com/macros/s/"
                    "AKfycbx8Vs0O9Ln98WIJi035loRK4nEmQHNjq6jv8NXAdb1YF8gIwIZ9Wq"
                    "qK4RY4PBFCJEtB/exec";

/* ================= OBJECT ================= */
Adafruit_ADS1115 ads;
SensirionI2cScd4x scd4x;
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;

/* ================= DHT22 ================= */
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

/* ================= PIN & SD ================= */
#define SD_CS_PIN 5

/* ================= KONSTANTA ================= */
const float m = 5.0; // Massa sampel cabai dalam kg (5000 gram)

const float Pmac =
    31960.0; // cm^3 (Volume dalam ruangan chamber 470x340x200 mm)

const float V_sampel = 6715.83; // Volume sampel cabai dalam ml (cm^3)

const float Vf =
    Pmac - V_sampel; // Free volume (Vf) = Volume chamber - Volume sampel

/* ================= PIN ================= */
#define MIST_PIN 26
#define BUZZ_PIN 33

/* ================= HISTERESIS RH ================= */
#define RH_ON 78.0
#define RH_OFF 82.0
bool mistState = false;

/* ================= EARLY WARNING ================= */
uint8_t alertStatus = 0; // 0:Normal, 1:Warning, 2:Danger, 3:Critical
uint8_t prevAlertStatus = 0;
uint8_t beepsRemaining = 0;
unsigned long lastBuzz = 0;
bool buzzState = false;

/* ================= MQ-3 ================= */
#define MQ3_CH 1
#define RL_MQ3 10.0
float R0_MQ3 = 1.235; // Hasil kalibrasi dari Rs/60 (Rs udara = 74.12)
float ethanol_ppm = 0;
float mq3_rs = 0;

/* ================= OOM202 ================= */
const float target_Ref_O2 =
    20.40;                     // referensi kalibrasi di udara dari Dansensor
float o2_baseline_mV = 14.109; // Hasil kalibrasi rata-rata (160 data)

/* ================= VARIABLE ================= */
float O2_t1 = 0, O2_t2 = 0;
float CO2_t1 = 0, CO2_t2 = 0;
float RO2, RCO2, RQ;
float temperature = 0, humidity = 0;
uint16_t co2_ppm_val = 0;

/* ================= TIMER ================= */
unsigned long lastSensorRead = 0;
const unsigned long sensorInterval = 60000; // 1 menit

unsigned long lastDataLog = 0;
const unsigned long logInterval = 600000; // 10 menit

unsigned long lastLCD = 0;
const unsigned long lcdInterval = 3000;
uint8_t lcdPage = 0;

/* ================= SETUP ================= */
void setup() {

  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(100000);

  pinMode(MIST_PIN, OUTPUT);
  digitalWrite(MIST_PIN, LOW);

  pinMode(BUZZ_PIN, OUTPUT);
  digitalWrite(BUZZ_PIN, LOW);

  // Beri jeda 2 detik meredam lonjakan arus awal (inrush current) dari pemanas
  // sebelum WiFi aktif
  delay(2000);

  WiFi.begin(ssid, password);
  DBG("Connecting WiFi...");
  int wifi_attempts = 0;
  // Batas 10 detik agar sistem tidak freeze selamanya misal hotspot mati
  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 20) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    DBG("\nWiFi connected");
  } else {
    DBG("\nWiFi Offline - Lanjut Mode SD Card");
  }

  ads.begin();

  scd4x.begin(Wire, 0x62);
  scd4x.stopPeriodicMeasurement();
  delay(500);
  scd4x.startPeriodicMeasurement();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Respiration");
  lcd.setCursor(0, 1);
  lcd.print("System Ready");

  // Inisialisasi DHT22
  dht.begin();

  // Inisialisasi RTC
  if (!rtc.begin()) {
    Serial.println("Peringatan: RTC tidak merespons! Cek wiring.");
  } else if (rtc.lostPower()) {
    Serial.println("RTC kehilangan daya! Mengatur ulang waktu ke kompilasi...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Inisialisasi SD Card
  Serial.print("Inisialisasi SD Card... ");
  if (!SD.begin(SD_CS_PIN, SPI,
                4000000)) { // Menggunakan kecepatan SPI 4 MHz untuk stabilitas
    Serial.println("Gagal! Pastikan modul SD terpasang.");
  } else {
    Serial.println("Berhasil!");
    File dataFile = SD.open("/cabai_2.csv", FILE_APPEND);
    if (dataFile) {
      if (dataFile.size() == 0) {
        dataFile.println(
            "sep=;"); // Memaksa Microsoft Excel pisah kolom otomatis
        dataFile.println(
            "Timestamp;Suhu_C;Kelembaban_RH;Ethanol_ppm;CO2_ppm;O2_ppm;O2_"
            "percent;CO2_percent;RQ;Mist_Status;Alert_Status");
      }
      dataFile.close();
    }
  }

  Serial.println("Ketik 'cal' di Serial Monitor untuk kalibrasi sensor O2");

  DBG("Menunggu data pertama SCD40...");

  //  TUNGGU DATA PERTAMA SIAP (Timeout 15 detik)
  bool ready = false;
  unsigned long startScd = millis();
  while (!ready && millis() - startScd < 15000) {
    uint16_t err = scd4x.getDataReadyStatus(ready);
    if (err) {
      Serial.print("I2C Error SCD40! Kode: ");
      Serial.println(err);
    }
    delay(500);
  }

  if (ready) {
    DBG("Data pertama siap!");
  } else {
    DBG("SCD40 Gagal siap, lanjut paksa baca awal 0!");
  }

  // BACA PERTAMA
  bacaSemuaSensor();

  // Simpan nilai referensi O2 dan CO2 awal
  O2_t1 = O2_t2;
  CO2_t1 = CO2_t2;

  // SIMPAN DATA JAM KE-0 (Awal eksperimen)
  RQ = 0; // RQ awal selalu 0 karena belum ada selisih gas
  Serial.println("\n=== MENGIRIM DATA JAM KE-0 (T=0) ===");
  simpanKeSDCard();
  kirimKeSpreadsheet();

  //  RESET TIMER AGAR DIHITUNG DARI SINI
  lastSensorRead = millis();
  lastDataLog = millis();
}

/* ================= LOOP ================= */
void loop() {
  // Auto-Reconnect WiFi jika putus di tengah jalan agar logging tidak terhenti
  // selamanya
  if (WiFi.status() != WL_CONNECTED) {
    DBG("WiFi Putus! Mencoba reconnect...");
    WiFi.disconnect();
    WiFi.reconnect();
    unsigned long startM = millis();
    // Tunggu max 3 detik untuk percobaan reconnect agar loop sistem tidak beku
    while (WiFi.status() != WL_CONNECTED && millis() - startM < 3000) {
      delay(500);
    }
  }

  // Cek input serial untuk perintah kalibrasi
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim(); // Hilangkan spasi/enter

    if (command.equalsIgnoreCase("cal")) {
      calibrate_OOM202();
    }
  }

  updateLCD();
  updateBuzzer();

  if (millis() - lastSensorRead >= sensorInterval) {
    lastSensorRead = millis();
    bacaSemuaSensor();
  }

  if (millis() - lastDataLog >= logInterval) {
    hitungRQDanLog();
  }
}

/* ================= BACA SENSOR ================= */
void bacaSemuaSensor() {

  DBG("\n=== BACA SENSOR (1 MENIT) ===");

  // O2_t1 dan CO2_t1 HANYA diperbarui saat logging 10 menit.

  bool isDataReady = false;
  uint16_t error = scd4x.getDataReadyStatus(isDataReady);

  if (error == 0 && isDataReady) {
    error = scd4x.readMeasurement(co2_ppm_val, temperature, humidity);
    if (error) {
      Serial.print("ERROR Membaca SCD40! Kode: ");
      Serial.println(error);
    }
  } else {
    Serial.println(
        "WARNING: SCD40 Data belum siap atau komunikasi I2C terputus!");
  }

  /* ===== DHT22 (SUMBER UTAMA) ===== */
  float dht_h = dht.readHumidity();
  float dht_t = dht.readTemperature();

  if (isnan(dht_h) || isnan(dht_t)) {
    Serial.println("WARNING: Gagal membaca DHT22! Menggunakan fallback suhu & "
                   "kelembaban dari SCD40");
  } else {
    // Timpa hasil SCD40 dengan bacaan DHT22
    temperature = dht_t;
    humidity = dht_h;
#if DEBUG
    Serial.println("DHT22 OK -> Suhu: " + String(temperature, 1) +
                   " C | RH: " + String(humidity, 1) + " %");
#endif
  }

  CO2_t2 = co2_ppm_val / 10000.0;

  /* ===== OOM202 ===== */
  // Algoritma Noise Rejection: 100 sampel, jeda 20ms (Lebih halus)
  float avg_mV = getStableOOM202_mV(100, 20);
  O2_t2 = (avg_mV / o2_baseline_mV) * target_Ref_O2;

  /* ===== MQ-3 ===== */
  ads.setGain(GAIN_ONE);
  int16_t adc_mq3 = ads.readADC_SingleEnded(MQ3_CH);
  float vout = ads.computeVolts(adc_mq3);
  if (vout < 0.01)
    vout = 0.01;

  mq3_rs = (5.0 - vout) * RL_MQ3 / vout;
  float ratio = mq3_rs / R0_MQ3;
  // Menambahkan pengali 100 agar nilai ppm etanol terlihat lebih banyak
  ethanol_ppm = pow(10, (log10(ratio) - 0.3) / -0.77) * 100.0;

  // Jika udara bersih (kadar sangat kecil), langsung nol-kan saja
  if (ethanol_ppm < 1.0) {
    ethanol_ppm = 0.0;
  }

#if DEBUG
  Serial.println(
      "MQ-3 Debug -> ADC: " + String(adc_mq3) + " | Vout: " + String(vout, 3) +
      " V | Rs: " + String(mq3_rs, 2) + " | Ratio: " + String(ratio, 2) +
      " | PPM Asli: " + String(ethanol_ppm, 4));
#endif

  // HITUNG RQ dipindah ke fungsi hitungRQDanLog() (10 Menit sekali)

  /* ===== KONTROL MIST ===== */
  if (!mistState && humidity <= RH_ON) {
    mistState = true;
    digitalWrite(MIST_PIN, HIGH);
  }
  if (mistState && humidity >= RH_OFF) {
    mistState = false;
    digitalWrite(MIST_PIN, LOW);
  }

  /* ===== EARLY WARNING SYSTEM ===== */
  uint8_t sRQ = 0;
  if (RQ >= 2.0)
    sRQ = 3;
  else if (RQ > 1.8)
    sRQ = 2;
  else if (RQ > 1.5)
    sRQ = 1;

  uint8_t sEtOH = 0;
  if (ethanol_ppm > 1000)
    sEtOH = 3;
  else if (ethanol_ppm > 600)
    sEtOH = 2;
  else if (ethanol_ppm > 300)
    sEtOH = 1;

  prevAlertStatus = alertStatus;
  alertStatus = sRQ > sEtOH ? sRQ : sEtOH;

  if (alertStatus != prevAlertStatus) {
    if (alertStatus == 1)
      beepsRemaining = 1; // Warning
    else if (alertStatus == 2)
      beepsRemaining = 3; // Danger
    else if (alertStatus == 3)
      beepsRemaining = 5; // Critical
    else if (alertStatus == 0)
      beepsRemaining = 0; // Normal
  }

#if DEBUG
  /* ===== SERIAL ===== */
  Serial.println("----- DATA SENSOR (1 MENIT) -----");
  Serial.print("O2 (%)   : ");
  Serial.println(O2_t2, 2);
  Serial.print("CO2 (%)  : ");
  Serial.println(CO2_t2, 3);
  Serial.print("MQ3 ppm  : ");
  Serial.println(ethanol_ppm, 1);
  Serial.print("Suhu (C) : ");
  Serial.println(temperature, 1);
  Serial.print("RH (%)   : ");
  Serial.println(humidity, 1);
  Serial.print("MIST     : ");
  Serial.println(mistState ? "ON" : "OFF");
#endif
}

/* ================= HITUNG RQ & LOGGING ================= */
void hitungRQDanLog() {
  DBG("\n=== MENGHITUNG RQ & LOGGING (10 MENIT) ===");

  /* ===== HITUNG RQ ===== */
  // Hitung dt dinamis berdasarkan waktu aktual yang berlalu (termasuk delay)
  unsigned long currentMillis = millis();
  float dt = (currentMillis - lastDataLog) / 3600000.0;
  lastDataLog = currentMillis; // Reset timer untuk siklus berikutnya
  RO2 = ((O2_t1 - O2_t2) * Vf) / (100 * m * dt);
  RCO2 = ((CO2_t2 - CO2_t1) * Vf) / (100 * m * dt);
  RQ = (RO2 > 0) ? (RCO2 / RO2) : 0;

  // Sesudah dihitung, perbarui t1 menjadi t2 untuk referensi 10 menit ke depan
  O2_t1 = O2_t2;
  CO2_t1 = CO2_t2;

  // Karena ini adalah sistem yang tak boleh kehilangan data,
  // kita selalu log dahulu ke SD Card di setiap siklus 10 menit
  simpanKeSDCard();

  kirimKeSpreadsheet();
}

/* ================= UPDATE BUZZER ================= */
void updateBuzzer() {
  if (beepsRemaining == 0) {
    if (buzzState) {
      digitalWrite(BUZZ_PIN, LOW);
      buzzState = false;
    }
    return;
  }

  unsigned long currentMillis = millis();
  unsigned long onTime = 200;
  unsigned long offTime = 200;

  if (buzzState) {
    if (currentMillis - lastBuzz >= onTime) {
      buzzState = false;
      lastBuzz = currentMillis;
      digitalWrite(BUZZ_PIN, LOW);
      beepsRemaining--;
    }
  } else {
    if (currentMillis - lastBuzz >= offTime) {
      buzzState = true;
      lastBuzz = currentMillis;
      digitalWrite(BUZZ_PIN, HIGH);
    }
  }
}

/* ================= LCD ================= */
void updateLCD() {
  if (millis() - lastLCD < lcdInterval)
    return;
  lastLCD = millis();

  DateTime now = rtc.now();
  String strJam = (now.hour() < 10 ? "0" : "") + String(now.hour());
  String strMenit = (now.minute() < 10 ? "0" : "") + String(now.minute());
  String strDetik = (now.second() < 10 ? "0" : "") + String(now.second());
  String strWaktu = strJam + ":" + strMenit + ":" + strDetik;

  lcd.clear();
  switch (lcdPage) {
  case 0:
    lcd.setCursor(0, 0);
    lcd.print("RQ:");
    lcd.print(RQ, 2);
    lcd.setCursor(0, 1);
    lcd.print("Jam: ");
    lcd.print(strWaktu);
    break;
  case 1:
    lcd.setCursor(0, 0);
    lcd.print("O2:");
    lcd.print(O2_t2, 1);
    lcd.print("%");
    lcd.setCursor(0, 1);
    lcd.print("CO2:");
    lcd.print(CO2_t2, 2);
    lcd.print("%");
    break;
  case 2:
    lcd.setCursor(0, 0);
    lcd.print("EtOH:");
    lcd.print(ethanol_ppm, 0);
    lcd.print("ppm");
    lcd.setCursor(0, 1);
    lcd.print("RH:");
    lcd.print(humidity, 0);
    lcd.print("% ");
    lcd.print(mistState ? "ON" : "OFF");
    break;
  }
  lcdPage++;
  if (lcdPage > 2)
    lcdPage = 0;
}

/* ================= BACKUP SD CARD ================= */
void simpanKeSDCard() {
  DateTime now = rtc.now();

  // Format jam, menit, detik 2 digit
  String strJam = (now.hour() < 10 ? "0" : "") + String(now.hour());
  String strMenit = (now.minute() < 10 ? "0" : "") + String(now.minute());
  String strDetik = (now.second() < 10 ? "0" : "") + String(now.second());
  String strWaktu = strJam + ":" + strMenit + ":" + strDetik;

  // Format Timestamp: DD/MM/YYYY HH:MM:SS
  String strHari = (now.day() < 10 ? "0" : "") + String(now.day());
  String strBulan = (now.month() < 10 ? "0" : "") + String(now.month());
  String strTimestamp =
      strHari + "/" + strBulan + "/" + String(now.year()) + " " + strWaktu;

  float o2_ppm_calc = O2_t2 * 10000.0;
  float co2_percent = co2_ppm_val / 10000.0;

  String statusStr = "Normal";
  if (alertStatus == 1)
    statusStr = "Warning";
  else if (alertStatus == 2)
    statusStr = "Danger";
  else if (alertStatus == 3)
    statusStr = "Critical";

  // Sesuai urutan header dengan format Regional Indonesia (; sebagai pemisah
  // kolom, koma sebagai desimal)
  String dataLog = strTimestamp + ";" + String(temperature, 1) + ";" +
                   String(humidity, 0) + ";" + String(ethanol_ppm, 1) + ";" +
                   String(co2_ppm_val) + ";" + String(o2_ppm_calc, 0) + ";" +
                   String(O2_t2, 2) + ";" + String(co2_percent, 3) + ";" +
                   String(RQ, 2) + ";" + String(mistState ? "ON" : "OFF") +
                   ";" + statusStr;

  // dataLog.replace(".", ",");

  File dataFile = SD.open("/cabai_2.csv", FILE_APPEND);
  if (dataFile) {
    dataFile.println(dataLog);
    dataFile.close();
    Serial.println("SD Card Backup: OK -> " + dataLog);
  } else {
    Serial.println("SD Card Backup: ERROR -> Gagal menulis ke /cabai_2.csv. "
                   "Mencoba remount SD...");
    SD.end(); // Tutup koneksi SD Card yang mungkin hang
    delay(100);
    if (SD.begin(SD_CS_PIN, SPI, 4000000)) {
      Serial.println("SD Card berhasil di-remount! Mencoba menulis ulang...");
      File retryFile = SD.open("/cabai_2.csv", FILE_APPEND);
      if (retryFile) {
        retryFile.println(dataLog);
        retryFile.close();
        Serial.println("SD Card Backup: OK -> Berhasil setelah remount!");
      } else {
        Serial.println("SD Card Backup: TETAP GAGAL menulis file setelah "
                       "remount! Cek Format (wajib FAT32) atau file corrupt.");
      }
    } else {
      Serial.println("SD Card remount gagal! Cek apakah kartu memori tercabut "
                     "atau kendor.");
    }
  }
}

/* ================= SPREADSHEET ================= */
void kirimKeSpreadsheet() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Spreadsheet: Batal dikirim karena WiFi mati (Data sudah "
                   "aman di SD Card).");
    return;
  }

  float o2_ppm_calc = O2_t2 * 10000.0;
  float co2_percent = co2_ppm_val / 10000.0;

  String statusStr = "Normal";
  if (alertStatus == 1)
    statusStr = "Warning";
  else if (alertStatus == 2)
    statusStr = "Danger";
  else if (alertStatus == 3)
    statusStr = "Critical";

  String url;
  url.reserve(250); // Mencegah heap fragmentation di RAM (memory leak) dengan
                    // memesan blok 250 byte
  url = serverName + "?suhu=" + String(temperature, 1) +
        "&hum=" + String(humidity, 0) + "&ethanol=" + String(ethanol_ppm, 1) +
        "&co2=" + String(co2_ppm_val) + "&o2ppm=" + String(o2_ppm_calc, 0) +
        "&o2p=" + String(O2_t2, 2) + "&co2p=" + String(co2_percent, 3) +
        "&rq=" + String(RQ, 2) + "&mist=" + String(mistState ? 1 : 0) +
        "&status=" + statusStr + "&alert=" + String(alertStatus > 0 ? 1 : 0);

  // PENTING: Google URL tidak boleh ada spasi
  url.replace(" ", "%20");

  Serial.print("Spreadsheet: Menyiapkan koneksi... Free Heap: ");
  Serial.println(ESP.getFreeHeap());

  // PENTING: Karena pakai HTTPS (Google Script), harus pakai secure client
  WiFiClientSecure client;
  client.setInsecure(); // Abaikan verifikasi SSL (menghindari error Connection
                        // Refused / Handshake Failed)
  client.setTimeout(15000); // Set timeout untuk koneksi SSL

  delay(
      1000); // Beri jeda 1 detik agar koneksi stabil setelah menulis ke SD Card

  HTTPClient http;
  http.begin(client, url);

  // Set timeout ke 15000ms (15 detik) menghindari error 'read Timeout'
  // karena Google Script kadang memang lambat merespons
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = 0;
  int retry_count = 0;

  while (retry_count < 3) {
    if (retry_count > 0) {
      Serial.println("Spreadsheet: Mencoba ulang pengiriman... (" +
                     String(retry_count) + "/3)");
      delay(2000);
    }

    httpCode = http.GET();

    if (httpCode > 0) {
      String payload =
          http.getString(); // [Kritis] Membaca body response WAJIB dilakukan
                            // agar socket HTTP ditutup bersiah oleh core ESP32
      Serial.println("Spreadsheet: Data berhasil diunggah.");
      break; // Keluar dari loop jika sukses
    } else {
      Serial.print("Spreadsheet: Gagal mengirim HTTP GET. Error: ");
      Serial.println(http.errorToString(httpCode).c_str());
      retry_count++;
    }
  }
  http.end();
}

/* ================= FILTER OOM202 ================= */
void sortFloatArray(float *arr, int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - i - 1; j++) {
      if (arr[j] > arr[j + 1]) {
        float temp = arr[j];
        arr[j] = arr[j + 1];
        arr[j + 1] = temp;
      }
    }
  }
}

float getStableOOM202_mV(int numSamples, int delayMs) {
  if (numSamples <= 0)
    return 0;

  float *samples = new float[numSamples];
  ads.setGain(GAIN_SIXTEEN);

  for (int i = 0; i < numSamples; i++) {
    int16_t reading = ads.readADC_Differential_0_3();
    samples[i] = ads.computeVolts(reading) * 1000.0;
    delay(delayMs);
  }

  sortFloatArray(samples, numSamples);

  // Buang 20% nilai terendah dan 20% nilai tertinggi (Noise Rejection)
  int trim = numSamples * 0.2;
  int count = 0;
  float sum = 0;

  for (int i = trim; i < numSamples - trim; i++) {
    sum += samples[i];
    count++;
  }

  delete[] samples;

  return (count > 0) ? (sum / count) : 0;
}

/* ================= KALIBRASI OOM202 ================= */
void calibrate_OOM202() {
  Serial.println("\n=== MEMULAI KALIBRASI O2 ===");
  Serial.println("PASTIKAN: Mist OFF, system sudah warm-up min 3 menit.");
  Serial.print("Mengambil 200 sampel untuk referensi ");
  Serial.print(target_Ref_O2);
  Serial.println("% ...");

  // Ambil rata-rata data dengan filter median + noise rejection (200 sampel,
  // jeda 25ms = ~5 detik)
  float avg_mV = getStableOOM202_mV(200, 25);

  // SETTING KUNCI:
  o2_baseline_mV = avg_mV;

  Serial.print("Kalibrasi Selesai! Tegangan Baseline baru: ");
  Serial.print(o2_baseline_mV, 3);
  Serial.println(" mV");
  Serial.print("Sekarang sensor dianggap membaca: ");
  Serial.print(target_Ref_O2);
  Serial.println(" %\n");
}