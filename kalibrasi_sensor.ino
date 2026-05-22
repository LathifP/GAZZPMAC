#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <SensirionI2cScd4x.h>
#include <DHT.h>
#include <SD.h>
#include <SPI.h>
#include "RTClib.h"

/* ================= OBJECT ================= */
Adafruit_ADS1115 ads;
SensirionI2cScd4x scd4x;
RTC_DS3231 rtc;

/* ================= DHT22 ================= */
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

/* ================= PIN & SD ================= */
#define SD_CS_PIN 5

/* ================= MQ-3 ================= */
#define MQ3_CH 1 // ADC pin 1 untuk MQ3

/* ================= TIMER ================= */
unsigned long lastLog = 0;
// Log setiap 5 detik (menyesuaikan kecepatan pembacaan aktual SCD40)
const unsigned long logInterval = 5000; 

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(100000);

  delay(2000); // Waktu jeda startup
  Serial.println("=== PROGRAM PENGAMBILAN DATA KALIBRASI ===");

  // Setup RTC
  if (!rtc.begin()) {
    Serial.println("Peringatan: RTC tidak merespons! Cek wiring.");
  } else if (rtc.lostPower()) {
    Serial.println("RTC kehilangan daya! Setel ulang waktu ke waktu kompilasi.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Setup ADS1115
  if (!ads.begin()) {
    Serial.println("Gagal menemukan ADS1115! Cek wiring I2C.");
    while (1);
  }
  Serial.println("ADS1115 OK");

  // Setup SCD40
  scd4x.begin(Wire, 0x62);
  scd4x.stopPeriodicMeasurement();
  delay(500);
  scd4x.startPeriodicMeasurement();
  Serial.println("SCD40 OK");

  // Setup DHT22
  dht.begin();
  Serial.println("DHT22 OK");

  // Setup SD Card
  Serial.print("Inisialisasi SD Card... ");
  if (!SD.begin(SD_CS_PIN, SPI, 4000000)) {
    Serial.println("Gagal! Cek koneksi SD Card.");
    while (1);
  }
  Serial.println("Berhasil!");

  // Buat File dan Header
  File dataFile = SD.open("/kalibrasi_raw.csv", FILE_APPEND);
  if (dataFile) {
    if (dataFile.size() == 0) {
      dataFile.println("sep=;"); // Pemisah untuk Excel
      dataFile.println("Timestamp;Suhu_DHT_C;RH_DHT_%;Suhu_SCD_C;RH_SCD_%;CO2_ppm;MQ3_ADC;MQ3_V;O2_ADC_Diff;O2_V;O2_mV");
    }
    dataFile.close();
    Serial.println("File /kalibrasi_raw.csv siap.");
  } else {
    Serial.println("Gagal membuat/membuka file kalibrasi_raw.csv");
  }

  Serial.println("Pengambilan data dimulai... (Mencatat setiap 5 detik)");
}

void loop() {
  if (millis() - lastLog >= logInterval) {
    lastLog = millis();

    // Dapatkan Waktu
    DateTime now = rtc.now();
    String strHari = (now.day() < 10 ? "0" : "") + String(now.day());
    String strBulan = (now.month() < 10 ? "0" : "") + String(now.month());
    String strJam = (now.hour() < 10 ? "0" : "") + String(now.hour());
    String strMenit = (now.minute() < 10 ? "0" : "") + String(now.minute());
    String strDetik = (now.second() < 10 ? "0" : "") + String(now.second());
    String strTimestamp = strHari + "/" + strBulan + "/" + String(now.year()) + " " + strJam + ":" + strMenit + ":" + strDetik;

    // 1. Baca DHT22
    float dht_t = dht.readTemperature();
    float dht_h = dht.readHumidity();

    // 2. Baca SCD40
    uint16_t co2_ppm = 0;
    float scd_t = 0.0f;
    float scd_h = 0.0f;
    bool isDataReady = false;
    uint16_t error = scd4x.getDataReadyStatus(isDataReady);
    if (error == 0 && isDataReady) {
      scd4x.readMeasurement(co2_ppm, scd_t, scd_h);
    }

    // 3. Baca MQ3 (Single Ended Channel 1)
    ads.setGain(GAIN_ONE);
    int16_t mq3_adc = ads.readADC_SingleEnded(MQ3_CH);
    float mq3_v = ads.computeVolts(mq3_adc);

    // 4. Baca OOM202 O2 Sensor (Differential A0-A3)
    ads.setGain(GAIN_SIXTEEN); // Gain 16x sangat penting untuk resolusi OOM202
    int16_t o2_adc = ads.readADC_Differential_0_3();
    float o2_v = ads.computeVolts(o2_adc);
    float o2_mv = o2_v * 1000.0; // Konversi Voltage ke miliVolt (mV)

    // Format Data (menggunakan ; sebagai separator yang cocok untuk Excel Indonesia)
    String dataLog = strTimestamp + ";" +
                     String(dht_t, 2) + ";" +
                     String(dht_h, 2) + ";" +
                     String(scd_t, 2) + ";" +
                     String(scd_h, 2) + ";" +
                     String(co2_ppm) + ";" +
                     String(mq3_adc) + ";" +
                     String(mq3_v, 4) + ";" +
                     String(o2_adc) + ";" +
                     String(o2_v, 6) + ";" +
                     String(o2_mv, 3);

    // Print ke Serial Monitor untuk pantauan langsung
    Serial.println(dataLog);

    // Tulis ke SD Card
    File dataFile = SD.open("/kalibrasi_raw.csv", FILE_APPEND);
    if (dataFile) {
      dataFile.println(dataLog);
      dataFile.close();
    } else {
      Serial.println("ERROR: Gagal menulis ke SD Card! Mencoba remount...");
      SD.end();
      delay(100);
      if (SD.begin(SD_CS_PIN, SPI, 4000000)) {
        File retryFile = SD.open("/kalibrasi_raw.csv", FILE_APPEND);
        if (retryFile) {
          retryFile.println(dataLog);
          retryFile.close();
          Serial.println("Remount BERHASIL, data tersimpan.");
        } else {
          Serial.println("Remount berhasil tapi tetap gagal membuka file.");
        }
      } else {
        Serial.println("Remount GAGAL! Cek fisik SD Card.");
      }
    }
  }
}
