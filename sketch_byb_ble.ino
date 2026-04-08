// ============================================================
//  BYB-NORTE — Firmware ESP32 (4 CANALES)
//  CONEXIÓN: BLE (Nordic UART) + USB Serial simultáneos
//  PINES: S1->D25, S2->D13, S3->D26, S4->D14
// ============================================================

#include <Arduino.h>
#include <Adafruit_MAX31865.h>
#include <ArduinoJson.h>

// ── BLE ─────────────────────────────────────────────────────
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Nordic UART Service (NUS) — máxima compatibilidad con Web Bluetooth
#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer*         pServer        = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
bool bleConnected  = false;
bool bleAdvertising = false;

// ── PINES SPI COMPARTIDOS ───────────────────────────────────
#define MAX31865_MOSI 23
#define MAX31865_MISO 19
#define MAX31865_CLK  18

// ── CHIP SELECT ─────────────────────────────────────────────
#define CS_S1 25
#define CS_S2 13
#define CS_S3 26
#define CS_S4 14

// ── PT100 CONFIG ─────────────────────────────────────────────
#define PT100_WIRES MAX31865_3WIRE
#define RREF        430.0
#define RNOMINAL    100.0

// ── SENSORES ────────────────────────────────────────────────
Adafruit_MAX31865 sensor1 = Adafruit_MAX31865(CS_S1, MAX31865_MOSI, MAX31865_MISO, MAX31865_CLK);
Adafruit_MAX31865 sensor2 = Adafruit_MAX31865(CS_S2, MAX31865_MOSI, MAX31865_MISO, MAX31865_CLK);
Adafruit_MAX31865 sensor3 = Adafruit_MAX31865(CS_S3, MAX31865_MOSI, MAX31865_MISO, MAX31865_CLK);
Adafruit_MAX31865 sensor4 = Adafruit_MAX31865(CS_S4, MAX31865_MOSI, MAX31865_MISO, MAX31865_CLK);

// ── VARIABLES DE PROMEDIO ────────────────────────────────────
const unsigned long SEND_INTERVAL = 1000;
unsigned long lastSend = 0;
float sum1=0; int cnt1=0;
float sum2=0; int cnt2=0;
float sum3=0; int cnt3=0;
float sum4=0; int cnt4=0;

// ── BLE CALLBACKS ────────────────────────────────────────────
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pSvr) override {
    bleConnected = true;
    bleAdvertising = false;
    Serial.println("{\"ble\":\"cliente conectado\"}");
  }
  void onDisconnect(BLEServer* pSvr) override {
    bleConnected = false;
    // Reiniciar publicidad para permitir reconexión
    pSvr->getAdvertising()->start();
    bleAdvertising = true;
    Serial.println("{\"ble\":\"cliente desconectado, buscando...\"}");
  }
};

// ── ENVIAR POR BLE (chunks de 20 bytes) ─────────────────────
void bleSend(const String& msg) {
  if (!bleConnected) return;
  // El MTU BLE estándar es 20 bytes por notificación
  int len = msg.length();
  int offset = 0;
  while (offset < len) {
    int chunk = min(20, len - offset);
    pTxCharacteristic->setValue((uint8_t*)(msg.c_str() + offset), chunk);
    pTxCharacteristic->notify();
    offset += chunk;
    delay(10); // pequeño delay entre chunks
  }
  // Terminador de mensaje
  pTxCharacteristic->setValue((uint8_t*)"\n", 1);
  pTxCharacteristic->notify();
}

// ── LECTURA PT100 ────────────────────────────────────────────
float leerPT100(Adafruit_MAX31865 &s) {
  uint8_t fault = s.readFault();
  if (fault) { s.clearFault(); return -999.0; }
  return s.temperature(RNOMINAL, RREF);
}

// ── TIMESTAMP ────────────────────────────────────────────────
String obtenerReloj() {
  unsigned long s = millis() / 1000;
  char buf[10];
  sprintf(buf, "%02lu:%02lu:%02lu", (s/3600)%24, (s%3600)/60, s%60);
  return String(buf);
}

// ── SETUP ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  delay(300);

  // Iniciar sensores
  sensor1.begin(PT100_WIRES);
  sensor2.begin(PT100_WIRES);
  sensor3.begin(PT100_WIRES);
  sensor4.begin(PT100_WIRES);

  // ── Iniciar BLE ──────────────────────────────────────────
  BLEDevice::init("BYB-NORTE-ESP32");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Característica TX (ESP32 → PC): notificación
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  // Publicitar BLE
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  bleAdvertising = true;

  Serial.println("{\"status\":\"BYB-NORTE 4-CH READY\",\"ble\":\"publicando como BYB-NORTE-ESP32\"}");
}

// ── LOOP ─────────────────────────────────────────────────────
void loop() {
  // Acumular lecturas para promediar
  float t1 = leerPT100(sensor1); if(t1 > -900.0){ sum1 += t1; cnt1++; }
  float t2 = leerPT100(sensor2); if(t2 > -900.0){ sum2 += t2; cnt2++; }
  float t3 = leerPT100(sensor3); if(t3 > -900.0){ sum3 += t3; cnt3++; }
  float t4 = leerPT100(sensor4); if(t4 > -900.0){ sum4 += t4; cnt4++; }

  unsigned long ahora = millis();
  if (ahora - lastSend >= SEND_INTERVAL) {
    lastSend = ahora;

    StaticJsonDocument<256> doc;
    doc["t"] = obtenerReloj();
    if (cnt1 > 0) doc["s1"] = (float)(round((sum1/cnt1)*10.0)/10.0); else doc["s1"] = nullptr;
    if (cnt2 > 0) doc["s2"] = (float)(round((sum2/cnt2)*10.0)/10.0); else doc["s2"] = nullptr;
    if (cnt3 > 0) doc["s3"] = (float)(round((sum3/cnt3)*10.0)/10.0); else doc["s3"] = nullptr;
    if (cnt4 > 0) doc["s4"] = (float)(round((sum4/cnt4)*10.0)/10.0); else doc["s4"] = nullptr;

    // Serializar
    String json;
    serializeJson(doc, json);

    // Enviar por Serial USB (siempre)
    Serial.println(json);

    // Enviar por BLE (si hay cliente conectado)
    bleSend(json);

    // Reset acumuladores
    sum1=0; cnt1=0; sum2=0; cnt2=0;
    sum3=0; cnt3=0; sum4=0; cnt4=0;
  }

  delay(50);
}
