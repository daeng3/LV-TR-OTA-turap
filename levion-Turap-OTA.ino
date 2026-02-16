#include <HardwareSerial.h>
#include <TM1637Display.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <Update.h>
// #include <ArduinoOTA.h> // Dihapus karena tidak digunakan, menghemat ukuran binary

// ==================== KONFIGURASI PROYEK ====================
// ===== GANTI ANGKA INI PER DEVICE (1, 2, 3, ... 10, dst.) =====
const int TANK_NUMBER = 3;

// Versi Firmware Saat Ini (Ubah ini setiap kali compile baru)
const int currentVersion = 1;

// URL GitHub untuk Auto-Update
// Ganti USERNAME dan REPO dengan milik Anda!
const String repoRaw = "https://raw.githubusercontent.com/daeng3/LevionProject/main/";
const String firmwareURL = repoRaw + "tank3_firmware.bin"; 
const String versionURL  = repoRaw + "version.txt";        

// ==================== HARDWARE CONFIG ====================
#define CLK 22
#define DIO 23
TM1637Display display(CLK, DIO);

HardwareSerial A01Serial(2);
#define RXD2 16
#define TXD2 17

#define BATTERY_PIN 35       
#define VOLTAGE_CALIBRATION 25.91 
#define LED_WIFI  2

// ==================== NETWORK CONFIG ====================
const char* ssid = "levion-turap";
const char* password = "1234567890";

const char* mqtt_server = "103.197.190.235";
const int mqtt_port = 1883;

// Topik MQTT (Otomatis berdasarkan TANK_NUMBER)
String mqtt_topic_data    = "sensor/tank-" + String(TANK_NUMBER); 
String mqtt_topic_command = "levion/cmd/tank-" + String(TANK_NUMBER);

// Web Server Login
const char* webUser = "admin";
const char* webPass = "levion123";

// ==================== VARIABLES ====================
const int tinggi_tangki_cm = 287; 
const int titik_buta_cm = 33;

WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);

unsigned long lastMQTTSend = 0;
const unsigned long mqttInterval = 2000;

// Timer Auto-Check GitHub (Backup jika lupa trigger manual)
unsigned long lastOTACheck = 0;
const unsigned long otaInterval = 3600000; // Cek setiap 1 Jam (3.600.000 ms)

// ==================== [FIX #2] Timer Non-Blocking untuk MQTT Reconnect ====================
unsigned long lastMQTTRetry = 0;
const unsigned long mqttRetryInterval = 5000; // Retry setiap 5 detik tanpa blocking

// Flag Eksekusi di Loop Utama
bool flagRestart = false;
bool flagUpdate  = false;

// HTML page untuk upload manual
const char* uploadPage = R"rawliteral(
<!doctype html>
<html>
  <head>
    <meta charset="utf-8">
    <title>Levion Turap (Tank %TANK%)</title>
    <style>body{font-family:sans-serif;padding:20px;text-align:center;} 
           input,button{padding:10px;margin:10px;}</style>
  </head>
  <body>
    <h3>Levion Turap (Tank %TANK%) - Control Panel</h3>
    <p>Status: <strong>System Ready</strong> | Version: <strong>%VER%</strong></p>
    <hr>
    <h4>Manual OTA Upload</h4>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="update"><br>
      <input type="submit" value="Upload Firmware .bin">
    </form>
  </body>
</html>
)rawliteral";

// ==================== FUNGSI BACA SENSOR ====================
float readBatteryVoltage() {
  float total = 0;
  for (int i = 0; i < 10; i++) { total += analogRead(BATTERY_PIN); delay(2); }
  return (total / 10.0 / 4095.0) * 3.3 * VOLTAGE_CALIBRATION;
}

// ==================== FUNGSI WIFI & OTA GITHUB ====================
void setup_wifi() {
  if(WiFi.status() == WL_CONNECTED) return;
  
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    delay(500); Serial.print("."); 
    digitalWrite(LED_WIFI, !digitalRead(LED_WIFI));
    attempt++;
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    digitalWrite(LED_WIFI, HIGH);
  } else {
    Serial.println("\nWiFi Failed.");
  }
}

// Fungsi Download Update dari GitHub
void perform_github_update() {
  Serial.println(">>> STARTING GITHUB OTA UPDATE... <<<");
  
  // Beri tanda visual di 7-segment
  display.showNumberHex(0xAAAA); // Tampil "AAAA"
  digitalWrite(LED_WIFI, LOW);

  WiFiClientSecure clientSecure;
  clientSecure.setInsecure(); // Bypass SSL
  clientSecure.setTimeout(12000);
  
  // ==================== [FIX #1] Progress Callback Aman dari Division by Zero ====================
  httpUpdate.onProgress([](int cur, int total) {
      // Guard: jika total terlalu kecil atau nol, hindari modulo by zero
      if (total <= 0) return;
      int step = total / 10;
      if (step == 0) step = 1; // Pastikan step minimal 1
      
      if (cur % step == 0) {
        Serial.printf("Downloading: %d%%\n", (cur * 100) / total);
      }
      digitalWrite(LED_WIFI, !digitalRead(LED_WIFI)); // Kedip cepat
  });
  
  httpUpdate.rebootOnUpdate(true); 

  t_httpUpdate_return ret = httpUpdate.update(clientSecure, firmwareURL);

  if (ret == HTTP_UPDATE_FAILED) {
    Serial.printf("OTA Failed (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    display.clear(); // Reset layar jika gagal
    flagUpdate = false;
  }
}

// Cek Versi di Server
void check_and_update() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.print("[Auto-Check] Checking version.txt... ");
  WiFiClientSecure clientSecure;
  clientSecure.setInsecure();
  HTTPClient http;
  
  http.begin(clientSecure, versionURL);
  http.addHeader("Cache-Control", "no-cache");
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    int serverVer = payload.toInt();
    Serial.printf("Server: %d | Device: %d\n", serverVer, currentVersion);
    
    if (serverVer > currentVersion) {
      Serial.println("New version found! Triggering update...");
      flagUpdate = true; // Set flag agar update dijalankan di loop utama
    }
  } else {
    Serial.printf("Error HTTP: %d\n", httpCode);
  }
  http.end();
}

// ==================== MQTT CALLBACK & HANDLING ====================
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) message += (char)payload[i];
  
  Serial.printf("MQTT [%s]: %s\n", topic, message.c_str());
  message.trim();
  message.toLowerCase();

  // 1. Perintah Restart
  if (message == "restart" || message == "reboot") {
    Serial.println("Command: Restart diterima.");
    flagRestart = true;
  }
  // 2. Perintah Force Update (Text)
  else if (message == "update") {
    Serial.println("Command: Force Update diterima.");
    flagUpdate = true;
  }
  // 3. Perintah Update via Angka Versi
  else if (message.length() > 0 && isDigit(message[0])) {
    int cmdVer = message.toInt();
    if (cmdVer > currentVersion) {
      Serial.printf("Command: Update ke versi %d diterima.\n", cmdVer);
      flagUpdate = true;
    }
  }
}

// ==================== [FIX #2] MQTT Reconnect Non-Blocking ====================
void reconnect_mqtt() {
  // Hanya coba reconnect jika belum connected DAN sudah lewat interval retry
  if (!client.connected() && (millis() - lastMQTTRetry >= mqttRetryInterval)) {
    lastMQTTRetry = millis(); // Catat waktu percobaan terakhir
    
    Serial.print("Connecting MQTT...");
    String clientId = "ESP32_Tank" + String(TANK_NUMBER) + "_Turap";
    
    if (client.connect(clientId.c_str())) {
      Serial.println(" Connected!");
      client.subscribe(mqtt_topic_command.c_str()); // Subscribe topik perintah
      Serial.printf("Subscribed to: %s\n", mqtt_topic_command.c_str());
    } else {
      // Hanya log error, TIDAK ADA delay() — langsung lanjut loop
      Serial.printf(" Failed rc=%d, retry in %ds\n", client.state(), (int)(mqttRetryInterval / 1000));
    }
  }
}

// ==================== WEB SERVER HANDLERS ====================
void handleRoot() {
  if (!server.authenticate(webUser, webPass)) return server.requestAuthentication();
  
  // Inject nomor versi ke HTML
  String page = uploadPage;
  page.replace("%VER%", String(currentVersion));
  page.replace("%TANK%", String(TANK_NUMBER));
  server.send(200, "text/html", page);
}

void handleUpdate() {
  if (!server.authenticate(webUser, webPass)) return server.requestAuthentication();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
  delay(100);
  ESP.restart();
}

// ==================== MAIN SETUP ====================
void setup() {
  Serial.begin(115200);
  
  // Init Hardware
  A01Serial.begin(9600, SERIAL_8N1, RXD2, TXD2);
  display.setBrightness(0x0f);
  display.showNumberDec(8888); delay(1000); display.clear();
  
  pinMode(LED_WIFI, OUTPUT);
  pinMode(BATTERY_PIN, INPUT); 

  // Init Network
  setup_wifi();
  
  // Init MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqtt_callback);

  // Init Web Server Manual
  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST, [](){ handleUpdate(); }, [](){
      if (!server.authenticate(webUser, webPass)) return;
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("Success: %u bytes\n", upload.totalSize);
        else Update.printError(Serial);
      }
  });
  server.begin();

  Serial.printf("System Ready v%d | Tank %d | IP: %s\n", currentVersion, TANK_NUMBER, WiFi.localIP().toString().c_str());
}

// ==================== MAIN LOOP ====================
void loop() {
  // 1. Network Maintenance
  if (WiFi.status() != WL_CONNECTED) setup_wifi();
  reconnect_mqtt();      // [FIX #2] Sekarang non-blocking, aman dipanggil setiap loop
  client.loop();         // Penting untuk terima pesan MQTT
  server.handleClient(); // Penting untuk Web Upload

  // 2. Eksekusi Perintah Flag (Prioritas Utama)
  if (flagRestart) {
    Serial.println("Rebooting system...");
    display.showNumberHex(0xDEAD); // Tampil "DEAD" (Restarting)
    delay(1000);
    ESP.restart();
  }

  if (flagUpdate) {
    perform_github_update();
    flagUpdate = false; // Jika gagal, reset flag agar sistem lanjut jalan
  }

  // 3. Auto-Check Update Berkala (Timer)
  if (millis() - lastOTACheck >= otaInterval) {
    lastOTACheck = millis();
    check_and_update();
  }

  // 4. Baca Sensor & Kirim Data
  while (A01Serial.available() >= 4) {
    if (A01Serial.read() == 0xFF) {
      byte h = A01Serial.read(); byte l = A01Serial.read(); byte sum = A01Serial.read();
      if (((0xFF + h + l) & 0xFF) == sum) {
        int dist = ((h << 8) | l) / 10;
        
        // ==================== [FIX #3] Conditional Block Menggantikan return ====================
        // Sebelumnya: if (dist < titik_buta_cm || dist > tinggi_tangki_cm) return;
        // Masalah: return keluar dari SELURUH loop(), bukan hanya skip pembacaan ini
        // Fix: Gunakan conditional block agar hanya skip data invalid, loop tetap jalan normal
        if (dist >= titik_buta_cm && dist <= tinggi_tangki_cm) {
          int level = tinggi_tangki_cm - dist;
          display.showNumberDec(level, false);

          if (millis() - lastMQTTSend >= mqttInterval && client.connected()) {
              char payload[200];
              snprintf(payload, sizeof(payload), 
                "{\"level_air_cm\": %d, \"tangki\": %d, \"battery_v\": %.2f, \"ver\": %d, \"ip\": \"%s\"}", 
                level, TANK_NUMBER, readBatteryVoltage(), currentVersion, WiFi.localIP().toString().c_str());
              
              if (client.publish(mqtt_topic_data.c_str(), payload)) {
                digitalWrite(LED_WIFI, LOW); delay(50); digitalWrite(LED_WIFI, HIGH);
                lastMQTTSend = millis();
                Serial.printf("Data Sent: %s\n", payload);
              }
          }
        }
        // Data di luar range diabaikan, tapi loop() tetap berjalan normal
      }
    }
  }
}
