// =====================================================================
//  config.h  –  Konfiguration SDM120-Zähler-Gateway (D1 Mini ESP32)
//  Alle anpassbaren Werte stehen hier. Die .ino muss nicht geändert werden.
// =====================================================================
#pragma once

#define FW_VERSION       "2.1"

// ---------------- Bluetooth (Classic / SPP) ----------------
#define BT_ENABLED       1                 // 0 = Bluetooth aus
#define BT_NAME          "SDM120-Zaehler"  // Name beim Koppeln am Handy
#define BT_INTERVAL_MS   1000              // Sendeintervall der Datenzeile ans Handy

// ---------------- WLAN ----------------
#define WIFI_ENABLED     1                 // 0 = reiner Bluetooth-Betrieb (kein MQTT/Web)
#define WIFI_SSID        "DeinWLAN"
#define WIFI_PASS        "DeinPasswort"
#define HOSTNAME         "sdm120"          // -> http://sdm120.local

// ---------------- MQTT ----------------
#define MQTT_ENABLED     1                 // 0 = MQTT aus (benötigt WIFI_ENABLED 1)
#define MQTT_HOST        "192.168.1.10"    // IP oder Hostname des Brokers
#define MQTT_PORT        1883
#define MQTT_USER        ""                // leer = ohne Anmeldung
#define MQTT_PASS        ""
#define MQTT_BASE        "strom/sdm120"    // Basis-Topic, ohne abschließendes '/'

// ---------------- Modbus / SDM120 ----------------
// Werkseinstellung SDM120: 2400 Baud, 8N1, Adresse 1
#define MODBUS_BAUD      2400              // 1200 / 2400 / 4800 / 9600 (wie am Zähler)
#define MODBUS_SLAVE     1                 // Modbus-Adresse des Zählers (1..247)

// ---------------- Pins (D1 Mini ESP32) ----------------
// RS485 läuft über den Hardware-UART2 (Serial2).
// Referenz-Hardware: Waveshare "TTL TO RS485 (B)", Hutschiene, galvanisch getrennt.
// Dessen TTL-Seite führt nur VCC/GND/TXD/RXD und schaltet die Richtung selbst um.
// Wichtig: VCC an 3V3, NICHT an 5 V - der TTL-Pegel folgt der Versorgungsspannung.
#define PIN_RS485_RX     16                // IO16 <- TXD des Moduls (bei DE/RE-Modul: RO)
#define PIN_RS485_TX     17                // IO17 -> RXD des Moduls (bei DE/RE-Modul: DI)
#define PIN_RS485_DE     -1                // -1 = automatische Richtungsumschaltung, IO4 bleibt frei
                                           // Modul mit DE+RE (gebrückt): hier den GPIO eintragen, z. B. 4

// ---------------- Zeiten ----------------
#define POLL_INTERVAL_MS   2000            // Abstand zwischen zwei kompletten Messzyklen
#define MODBUS_PAUSE_MS    30              // Pause zwischen zwei Telegrammen innerhalb eines Zyklus
#define MQTT_RETRY_MS      5000            // Abstand zwischen MQTT-Verbindungsversuchen
#define MAX_FEHLER_IN_FOLGE 3              // danach gelten die Messwerte als ungültig
