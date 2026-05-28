/**
 * ============================================================
 *  ESP32 + SX1276 -> TTN OTAA -> TagoIO  (RadioLib)
 *  Sensor  : MQ135 (GPIO 34)
 *  Display : OLED SSD1306 128x64 I2C (SDA=21, SCL=22)
 *  LED     : Green=2 | Yellow=15 | Red=14
 *  Region  : AS923 (Indonesia)
 *
 *  Fix DevNonce: menggunakan getBufferNonces/setBufferNonces
 *  dan getBufferSession/setBufferSession — API resmi RadioLib
 *  versi ini. Nonces + Session disimpan ke NVS (Preferences).
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <RadioLib.h>
#include <SPI.h>
#include <Preferences.h>

// ============================================================
// Pin Definition
// ============================================================
#define PIN_MQ135       34
#define PIN_OLED_SDA    21
#define PIN_OLED_SCL    22
#define PIN_LED_GREEN   2
#define PIN_LED_YELLOW  15
#define PIN_LED_RED     14

#define LORA_SCK        5
#define LORA_MISO       19
#define LORA_MOSI       27
#define LORA_CS         18
#define LORA_DIO0       26
#define LORA_DIO1       33
#define LORA_RST        23

// ============================================================
// TTN OTAA Credentials
// ============================================================
uint64_t joinEUI = 0x0000000000000000;
uint64_t devEUI  = 0x70B3D57ED0077762;
uint8_t  appKey[] = {
    0x57, 0x1F, 0x86, 0x72,
    0x45, 0x7B, 0xA0, 0xD4,
    0x09, 0x2B, 0x19, 0xB5,
    0xEC, 0xCA, 0x0C, 0x90
};

// ============================================================
// Object Declarations
// ============================================================
SX1276      radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
LoRaWANNode node(&radio, &AS923, 2);
Preferences prefs;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE,
                                           PIN_OLED_SCL, PIN_OLED_SDA);

// ============================================================
// Global State
// ============================================================
bool oledOk     = false;
bool loraJoined = false;

unsigned long lastTx  = 0;
unsigned long txCount = 0;
const unsigned long TX_INTERVAL = 60000; // 60 detik

// ============================================================
// NVS — simpan/muat Nonces (DevNonce) dan Session
// Nonces WAJIB disimpan agar DevNonce tidak duplikat.
// Session disimpan agar tidak perlu join ulang saat reboot.
// ============================================================
void saveNonces() {
    uint8_t* buf = node.getBufferNonces();
    prefs.begin("lorawan", false);
    prefs.putBytes("nonces", buf, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    prefs.end();
    Serial.println("[NVS] Nonces disimpan.");
}

bool loadNonces() {
    prefs.begin("lorawan", true);
    size_t len = prefs.getBytesLength("nonces");
    prefs.end();
    if (len != RADIOLIB_LORAWAN_NONCES_BUF_SIZE) {
        Serial.println("[NVS] Nonces belum ada.");
        return false;
    }
    uint8_t buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    prefs.begin("lorawan", true);
    prefs.getBytes("nonces", buf, len);
    prefs.end();
    int16_t state = node.setBufferNonces(buf);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[NVS] setBufferNonces gagal: %d\n", state);
        return false;
    }
    Serial.println("[NVS] Nonces dimuat.");
    return true;
}

void saveSession() {
    uint8_t* buf = node.getBufferSession();
    prefs.begin("lorawan", false);
    prefs.putBytes("session", buf, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
    prefs.end();
    Serial.println("[NVS] Session disimpan.");
}

bool loadSession() {
    prefs.begin("lorawan", true);
    size_t len = prefs.getBytesLength("session");
    prefs.end();
    if (len != RADIOLIB_LORAWAN_SESSION_BUF_SIZE) {
        Serial.println("[NVS] Session belum ada.");
        return false;
    }
    uint8_t buf[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
    prefs.begin("lorawan", true);
    prefs.getBytes("session", buf, len);
    prefs.end();
    int16_t state = node.setBufferSession(buf);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[NVS] setBufferSession gagal: %d\n", state);
        return false;
    }
    Serial.println("[NVS] Session dimuat.");
    return true;
}

void clearAllNVS() {
    prefs.begin("lorawan", false);
    prefs.clear();
    prefs.end();
    Serial.println("[NVS] Semua data dihapus.");
}

// ============================================================
// MQ135 — Baca NH3 (ppm)
// ============================================================
float readMQ135_ppm() {
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += analogRead(PIN_MQ135);
        delay(5);
    }
    float adcVal = (float)(sum / 10);
    if (adcVal < 10) return 0;
    float sensorVolt = (adcVal / 4095.0f) * 3.3f;
    if (sensorVolt < 0.1f) return 0;
    float Rs = ((3.3f - sensorVolt) / sensorVolt) * 10.0f;
    if (Rs <= 0) return 0;
    float ratio = Rs / 3.65f;
    float ppm   = 102.2f * pow(ratio, -2.473f);
    if (ppm < 0 || isnan(ppm) || isinf(ppm)) return 0;
    return ppm;
}

const char* nh3Category(float ppm) {
    if (ppm < 5.0f)   return "Aman";
    if (ppm <= 20.0f) return "Ganggu";
    if (ppm <= 50.0f) return "Risiko";
    return "BAHAYA!";
}

// ============================================================
// LED Status
// ============================================================
void setLED(uint8_t status) {
    digitalWrite(PIN_LED_GREEN,  LOW);
    digitalWrite(PIN_LED_YELLOW, LOW);
    digitalWrite(PIN_LED_RED,    LOW);
    if      (status == 0) digitalWrite(PIN_LED_GREEN,  HIGH);
    else if (status == 1) digitalWrite(PIN_LED_YELLOW, HIGH);
    else                  digitalWrite(PIN_LED_RED,    HIGH);
}

uint8_t evalNH3(float p) { return (p > 25.0f) ? 2 : (p >= 5.0f) ? 1 : 0; }

// ============================================================
// Build Payload 2 bytes — NH3 x10
// ============================================================
uint8_t buildPayload(uint8_t* buf, float nh3) {
    uint16_t iNH3 = (uint16_t)(nh3 * 10);
    buf[0] = (iNH3 >> 8) & 0xFF;
    buf[1] =  iNH3       & 0xFF;
    return 2;
}

// ============================================================
// OLED Helper
// ============================================================
void oledMsg(const char* l1, const char* l2 = "", const char* l3 = "") {
    if (!oledOk) return;
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    if (strlen(l1)) u8g2.drawStr(10, 22, l1);
    if (strlen(l2)) u8g2.drawStr(10, 38, l2);
    if (strlen(l3)) u8g2.drawStr(10, 54, l3);
    u8g2.sendBuffer();
}

// ============================================================
// LoRaWAN Init + OTAA Join
// ============================================================
bool initLoRaWAN() {
    // Hard reset SX1276
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);  delay(50);
    digitalWrite(LORA_RST, HIGH); delay(200);

    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    delay(200);

    Serial.println("[LoRa] Inisialisasi LoRaWAN...");
    Serial.print("[LoRa] Init SX1276... ");
    int state = radio.begin();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("GAGAL! Error: %d\n", state);
        return false;
    }
    Serial.println("OK!");

    // beginOTAA: nwkKey = appKey untuk TTN v3 LoRaWAN 1.0.x
    state = node.beginOTAA(joinEUI, devEUI, appKey, appKey);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[TTN] beginOTAA GAGAL! Error: %d\n", state);
        return false;
    }

    // Muat Nonces dari NVS (berisi DevNonce terakhir)
    // INI WAJIB agar TTN tidak tolak dengan "DevNonce already used"
    loadNonces();

    // Coba muat Session — jika berhasil, skip join
    if (loadSession()) {
        state = node.activateOTAA();
        if (state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
            Serial.println("[TTN] Session dipulihkan dari NVS. Skip join!");
            return true;
        }
        Serial.printf("[TTN] Restore session gagal: %d. Akan join ulang.\n", state);
    }

    // Join baru
    for (int attempt = 1; attempt <= 5; attempt++) {
        Serial.printf("[TTN] Join attempt %d/5...\n", attempt);
        char buf[28];
        snprintf(buf, sizeof(buf), "Join %d/5...", attempt);
        oledMsg("Joining TTN...", buf);

        LoRaWANJoinEvent_t ev;
        state = node.activateOTAA(&ev);
        Serial.printf("[TTN] state: %d | DevNonce: %u\n", state, ev.devNonce);

        if (state == RADIOLIB_LORAWAN_NEW_SESSION) {
            // Simpan Nonces (mengandung DevNonce baru) dan Session
            saveNonces();
            saveSession();
            Serial.println("[TTN] Join BERHASIL! Session aktif.");
            return true;
        }

        Serial.printf("[TTN] Gagal Error: %d. Retry 15 detik...\n", state);
        snprintf(buf, sizeof(buf), "Err: %d", state);
        oledMsg("TTN Gagal", buf, "Retry 15s...");
        delay(15000);
    }

    Serial.println("[TTN] Join GAGAL setelah 5x percobaan!");
    return false;
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println(  "║   ESP32 + MQ135 -> TTN -> TagoIO     ║");
    Serial.println(  "╚══════════════════════════════════════╝");

    // Init LED
    pinMode(PIN_LED_GREEN,  OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_RED,    OUTPUT);
    for (int i = 0; i < 2; i++) {
        digitalWrite(PIN_LED_GREEN,  HIGH); delay(150); digitalWrite(PIN_LED_GREEN,  LOW);
        digitalWrite(PIN_LED_YELLOW, HIGH); delay(150); digitalWrite(PIN_LED_YELLOW, LOW);
        digitalWrite(PIN_LED_RED,    HIGH); delay(150); digitalWrite(PIN_LED_RED,    LOW);
    }

    // ADC
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // Init OLED
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    delay(300);
    oledOk = u8g2.begin();
    if (oledOk) {
        Serial.println("[OLED] Init OK");
        oledMsg("ESP32 + MQ135", "TTN -> TagoIO", "Init...");
        delay(1500);
    } else {
        Serial.println("[OLED] FAIL - cek wiring SDA/SCL");
    }

    // MQ135 Warm-up 60 detik
    Serial.println("[MQ135] Warm-up 60 detik...");
    for (int i = 60; i > 0; i--) {
        char buf[28];
        snprintf(buf, sizeof(buf), "MQ135 Warmup: %2ds", i);
        Serial.println(buf);
        if (oledOk) {
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_6x10_tf);
            u8g2.drawStr(10, 20, "Harap Tunggu...");
            u8g2.drawStr(10, 36, buf);
            u8g2.drawFrame(0, 48, 128, 10);
            int bw = (int)((60 - i) / 60.0f * 126.0f);
            if (bw > 0) u8g2.drawBox(1, 49, bw, 8);
            u8g2.sendBuffer();
        }
        delay(1000);
    }

    // Init LoRaWAN
    oledMsg("Joining TTN...");
    loraJoined = initLoRaWAN();

    if (oledOk) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_7x13_tf);
        u8g2.drawStr(10, 32, loraJoined ? "TTN: JOINED!" : "TTN: GAGAL!");
        u8g2.sendBuffer();
        delay(2000);
    }

    Serial.println("[SYS] Setup selesai!");
    Serial.println("════════════════════════════════════════");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    float nh3 = readMQ135_ppm();

    Serial.printf("[MQ135] NH3 : %.1f ppm  |  %s\n", nh3, nh3Category(nh3));

    uint8_t stGlob = evalNH3(nh3);
    setLED(stGlob);
    Serial.printf("[LED] Status: %s\n",
        stGlob == 0 ? "AMAN" : stGlob == 1 ? "WASPADA" : "BAHAYA");

    // OLED
    if (oledOk) {
        char buf[32];
        u8g2.clearBuffer();

        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(0, 10, "-- MQ135 SENSOR --");

        u8g2.drawStr(0, 26, "NH3 :");
        snprintf(buf, sizeof(buf), "%.1f ppm", nh3);
        u8g2.setFont(u8g2_font_7x13_tf);
        u8g2.drawStr(45, 27, buf);

        u8g2.drawHLine(0, 34, 128);

        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(0, 46, "Status:");
        snprintf(buf, sizeof(buf), "%s", nh3Category(nh3));
        u8g2.setFont(u8g2_font_7x13_tf);
        u8g2.drawStr(55, 47, buf);

        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(0, 63, loraJoined ? "TTN:OK" : "TTN:NO");
        snprintf(buf, sizeof(buf), "Tx#%lu", txCount);
        u8g2.drawStr(90, 63, buf);

        u8g2.sendBuffer();
    }

    // Kirim ke TTN setiap TX_INTERVAL
    if (loraJoined && (millis() - lastTx >= TX_INTERVAL)) {
        lastTx = millis();

        uint8_t payload[2];
        uint8_t len = buildPayload(payload, nh3);

        Serial.println("--------------------------------------");
        Serial.printf("[Tx] #%lu | NH3: %.1f ppm\n", ++txCount, nh3);
        Serial.printf("[Tx] Payload: %02X %02X\n", payload[0], payload[1]);
        Serial.print("[Tx] Kirim ke TTN... ");

        int state = node.sendReceive(payload, len, 1);

        if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_LORAWAN_DOWNLINK) {
            Serial.println("OK! -> TTN -> TagoIO");
            // Update session setelah Tx sukses (frame counter berubah)
            saveNonces();
            saveSession();
        } else {
            Serial.printf("GAGAL! Error: %d\n", state);

            if (state == RADIOLIB_ERR_NETWORK_NOT_JOINED) {
                Serial.println("[TTN] Session expired, hapus NVS & rejoin...");
                oledMsg("TTN Rejoin...");
                clearAllNVS();
                node.clearSession();
                loraJoined = initLoRaWAN();
                if (loraJoined) Serial.println("[TTN] Rejoin OK!");
                else            Serial.println("[TTN] Rejoin GAGAL!");
            }
        }
        Serial.println("--------------------------------------");
    }

    Serial.println("----------------------------------------");
    delay(3000);
}