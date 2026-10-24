// =====================================================================
//  SDM120_Zaehler.ino   (Version siehe FW_VERSION in config.h)
//  Eastron SDM120 (Modbus RTU / RS485)  ->  D1 Mini ESP32
//  -> Bluetooth Classic (Handy-App, MIT App Inventor)
//  -> MQTT, Weboberfläche, JSON-API (über WLAN)
//  Programmieren ausschließlich per USB (kein OTA)
//
//  Benötigte Bibliotheken (Arduino-Bibliotheksverwalter):
//    - ModbusMaster   (Doc Walker)
//    - PubSubClient   (Nick O'Leary)
//  Bereits im ESP32-Boardpaket enthalten:
//    - WiFi, WebServer, ESPmDNS, BluetoothSerial
//
//  Board:      "ESP32 Dev Module"  (D1 Mini ESP32 = ESP32-WROOM-32)
//  Partition:  "Huge APP (3MB No OTA/1MB SPIFFS)"
//              -> Bluetooth + WLAN passen NICHT in die Standard-Partition!
//  Autor: RwTec
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <BluetoothSerial.h>
#include <ModbusMaster.h>
#include <PubSubClient.h>
#include "config.h"

#if BT_ENABLED && (!defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED) || !defined(CONFIG_BT_SPP_ENABLED))
#error "Bluetooth Classic (SPP) nicht verfuegbar: klassischen ESP32 waehlen, NICHT ESP32-S3/C3!"
#endif

// ---------------------------------------------------------------------
//  Messwerte
// ---------------------------------------------------------------------
struct Messwerte {
  float spannung        = NAN;   // V
  float strom           = NAN;   // A
  float wirkleistung    = NAN;   // W   (negativ = Einspeisung)
  float scheinleistung  = NAN;   // VA
  float blindleistung   = NAN;   // var
  float leistungsfaktor = NAN;   // -
  float frequenz        = NAN;   // Hz
  float importKWh       = NAN;   // kWh Bezug
  float exportKWh       = NAN;   // kWh Einspeisung
  float gesamtKWh       = NAN;   // kWh gesamt (Import + Export)
};

// Registertabelle SDM120: Input-Register (Funktion 0x04), je 2 Register = Float32 (Big Endian)
// Die REIHENFOLGE bestimmt auch die Feldreihenfolge im Bluetooth-Protokoll!
struct Register {
  uint16_t adresse;
  float Messwerte::*feld;        // Zeiger auf das Feld in der Struktur
  const char *name;              // Name für MQTT/JSON
  const char *einheit;
  uint8_t nachkomma;
};

const Register REGISTER[] = {
  { 0x0000, &Messwerte::spannung,        "spannung",        "V",   1 },
  { 0x0006, &Messwerte::strom,           "strom",           "A",   2 },
  { 0x000C, &Messwerte::wirkleistung,    "wirkleistung",    "W",   1 },
  { 0x0012, &Messwerte::scheinleistung,  "scheinleistung",  "VA",  1 },
  { 0x0018, &Messwerte::blindleistung,   "blindleistung",   "var", 1 },
  { 0x001E, &Messwerte::leistungsfaktor, "leistungsfaktor", "",    3 },
  { 0x0046, &Messwerte::frequenz,        "frequenz",        "Hz",  2 },
  { 0x0048, &Messwerte::importKWh,       "import_kwh",      "kWh", 3 },
  { 0x004A, &Messwerte::exportKWh,       "export_kwh",      "kWh", 3 },
  { 0x0156, &Messwerte::gesamtKWh,       "gesamt_kwh",      "kWh", 3 },
};
const uint8_t ANZAHL_REG = sizeof(REGISTER) / sizeof(REGISTER[0]);

Messwerte mw;            // zuletzt gültige Werte
Messwerte puffer;        // Werte des laufenden Zyklus
bool      mwGueltig      = false;
uint32_t  mwZeitstempel  = 0;   // millis() der letzten gültigen Messung
uint32_t  okZaehler      = 0;   // erfolgreiche Zyklen
uint32_t  fehlerZaehler  = 0;   // fehlgeschlagene Zyklen
uint8_t   fehlerInFolge  = 0;
uint8_t   letzterFehler  = 0;   // ModbusMaster-Ergebniscode

// ---------------------------------------------------------------------
//  Objekte
// ---------------------------------------------------------------------
HardwareSerial  &rs485 = Serial2;
ModbusMaster     sdm;
#if BT_ENABLED
BluetoothSerial  SerialBT;
#endif
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);
WebServer        web(80);

// Sequenzer-Zustand
bool     zyklusAktiv       = false;
uint8_t  regIndex          = 0;
uint32_t tLetzterZyklus    = 0;
uint32_t tLetzteAnfrage    = 0;
uint32_t tLetzterMqttTry   = 0;
uint32_t tLetzteBtZeile    = 0;

// ---------------------------------------------------------------------
//  Hilfsfunktionen
// ---------------------------------------------------------------------
void preTx()  { if (PIN_RS485_DE >= 0) digitalWrite(PIN_RS485_DE, HIGH); }
void postTx() { if (PIN_RS485_DE >= 0) digitalWrite(PIN_RS485_DE, LOW);  }

// Zwei Register aus dem Antwortpuffer -> Float32 (Big Endian, High-Word zuerst)
float pufferFloat(uint8_t idx) {
  uint32_t raw = ((uint32_t)sdm.getResponseBuffer(idx) << 16) | sdm.getResponseBuffer(idx + 1);
  float f;
  memcpy(&f, &raw, sizeof(f));
  return f;
}

// Text sicher an einen Puffer anhängen (printf-Syntax)
size_t anhaengen(char *buf, size_t len, size_t n, const char *fmt, ...) {
  if (n >= len) return n;
  va_list args;
  va_start(args, fmt);
  int r = vsnprintf(buf + n, len - n, fmt, args);
  va_end(args);
  return (r > 0) ? n + (size_t)r : n;
}

bool wlanVerbunden() {
#if WIFI_ENABLED
  return WiFi.status() == WL_CONNECTED;
#else
  return false;
#endif
}

// ---------------------------------------------------------------------
//  JSON (Web-API und MQTT)
// ---------------------------------------------------------------------
size_t baueJson(char *buf, size_t len) {
  size_t n = 0;
  uint32_t alter = mwGueltig ? (millis() - mwZeitstempel) / 1000 : 0;
  n = anhaengen(buf, len, n, "{\"gueltig\":%s,\"alter_s\":%lu",
                mwGueltig ? "true" : "false", (unsigned long)alter);
  for (uint8_t i = 0; i < ANZAHL_REG; i++) {
    float v = mw.*(REGISTER[i].feld);
    if (isnan(v)) n = anhaengen(buf, len, n, ",\"%s\":null", REGISTER[i].name);
    else          n = anhaengen(buf, len, n, ",\"%s\":%.*f", REGISTER[i].name, REGISTER[i].nachkomma, v);
  }
  n = anhaengen(buf, len, n,
                ",\"ok\":%lu,\"fehler\":%lu,\"letzter_fehler\":%u,\"rssi\":%d,\"uptime_s\":%lu}",
                (unsigned long)okZaehler, (unsigned long)fehlerZaehler, letzterFehler,
                wlanVerbunden() ? (int)WiFi.RSSI() : 0, (unsigned long)(millis() / 1000));
  return n;
}

// ---------------------------------------------------------------------
//  Bluetooth-Protokoll (siehe README Kapitel 7.1)
//    Datenzeile: $SDM;<gueltig>;<U>;<I>;<P>;<S>;<Q>;<PF>;<f>;<Import>;<Export>;<Gesamt>\n
//    Infozeile : $INF;<fw>;<ip>;<rssi>;<ok>;<fehler>;<letzter_fehler>;<uptime_s>\n
//    Befehle vom Handy: "I" = Infozeile anfordern
// ---------------------------------------------------------------------
#if BT_ENABLED
char    btBefehl[16];
uint8_t btBefehlLen = 0;

size_t baueBtDatenzeile(char *buf, size_t len) {
  size_t n = anhaengen(buf, len, 0, "$SDM;%d", mwGueltig ? 1 : 0);
  for (uint8_t i = 0; i < ANZAHL_REG; i++) {
    float v = mw.*(REGISTER[i].feld);
    if (isnan(v)) n = anhaengen(buf, len, n, ";-");
    else          n = anhaengen(buf, len, n, ";%.*f", REGISTER[i].nachkomma, v);
  }
  return anhaengen(buf, len, n, "\n");
}

void sendeBtInfo() {
  char buf[128];
  String ip = wlanVerbunden() ? WiFi.localIP().toString() : String("-");
  snprintf(buf, sizeof(buf), "$INF;%s;%s;%d;%lu;%lu;%u;%lu\n",
           FW_VERSION, ip.c_str(), wlanVerbunden() ? (int)WiFi.RSSI() : 0,
           (unsigned long)okZaehler, (unsigned long)fehlerZaehler, letzterFehler,
           (unsigned long)(millis() / 1000));
  SerialBT.print(buf);
}

void btBefehlAusfuehren(const char *cmd) {
  if (strcmp(cmd, "I") == 0) sendeBtInfo();
  // Platz für weitere Befehle, z. B. Relais schalten
}

void btTask() {
  // eingehende Befehle zeilenweise sammeln
  while (SerialBT.available()) {
    char c = (char)SerialBT.read();
    if (c == '\n' || c == '\r') {
      if (btBefehlLen) { btBefehl[btBefehlLen] = 0; btBefehlAusfuehren(btBefehl); btBefehlLen = 0; }
    } else if (btBefehlLen < sizeof(btBefehl) - 1) {
      btBefehl[btBefehlLen++] = c;
    }
  }
  // zyklisch Datenzeile senden, solange ein Handy verbunden ist
  if (!SerialBT.hasClient()) return;
  if (millis() - tLetzteBtZeile < BT_INTERVAL_MS) return;
  tLetzteBtZeile = millis();
  char zeile[160];
  baueBtDatenzeile(zeile, sizeof(zeile));
  SerialBT.print(zeile);
}
#endif

// ---------------------------------------------------------------------
//  MQTT
// ---------------------------------------------------------------------
void mqttVeroeffentlichen() {
#if WIFI_ENABLED && MQTT_ENABLED
  if (!mqtt.connected()) return;
  char topic[96];
  char wert[24];
  for (uint8_t i = 0; i < ANZAHL_REG; i++) {
    float v = mw.*(REGISTER[i].feld);
    if (isnan(v)) continue;
    snprintf(topic, sizeof(topic), "%s/%s", MQTT_BASE, REGISTER[i].name);
    snprintf(wert, sizeof(wert), "%.*f", REGISTER[i].nachkomma, v);
    mqtt.publish(topic, wert, true);                 // retained
  }
  char json[512];
  baueJson(json, sizeof(json));
  mqtt.publish(MQTT_BASE "/json", json, false);
#endif
}

void mqttTask() {
#if WIFI_ENABLED && MQTT_ENABLED
  if (!wlanVerbunden()) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (millis() - tLetzterMqttTry < MQTT_RETRY_MS) return;
  tLetzterMqttTry = millis();

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "%s-%08lX", HOSTNAME, (unsigned long)(uint32_t)ESP.getEfuseMac());
  const char *user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char *pass = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;

  Serial.print(F("[MQTT] Verbinde ... "));
  if (mqtt.connect(clientId, user, pass, MQTT_BASE "/status", 0, true, "offline")) {
    Serial.println(F("ok"));
    mqtt.publish(MQTT_BASE "/status", "online", true);
  } else {
    Serial.printf("Fehler rc=%d\n", mqtt.state());
  }
#endif
}

// ---------------------------------------------------------------------
//  Modbus-Sequenzer: pro loop()-Durchlauf genau ein Telegramm
// ---------------------------------------------------------------------
bool plausibel(const Messwerte &m) {
  if (isnan(m.spannung) || m.spannung < 0 || m.spannung > 300) return false;
  if (isnan(m.frequenz) || m.frequenz < 45 || m.frequenz > 65)  return false;
  if (isnan(m.gesamtKWh) || m.gesamtKWh < 0)                    return false;
  // Zählerstand darf nicht kleiner werden (kleine Toleranz für Rundung)
  if (mwGueltig && !isnan(mw.gesamtKWh) && m.gesamtKWh < mw.gesamtKWh - 0.01f) return false;
  return true;
}

void zyklusFehlgeschlagen(const char *grund) {
  zyklusAktiv = false;
  fehlerZaehler++;
  if (++fehlerInFolge >= MAX_FEHLER_IN_FOLGE) mwGueltig = false;
  Serial.printf("[MODBUS] Zyklus fehlgeschlagen: %s (Code 0x%02X, %u in Folge)\n",
                grund, letzterFehler, fehlerInFolge);
}

void zyklusAbschliessen() {
  zyklusAktiv = false;
  if (!plausibel(puffer)) { zyklusFehlgeschlagen("unplausible Werte"); return; }
  mw            = puffer;
  mwGueltig     = true;
  mwZeitstempel = millis();
  fehlerInFolge = 0;
  okZaehler++;
  Serial.printf("[SDM120] U=%.1f V  I=%.2f A  P=%.1f W  f=%.2f Hz  E=%.3f kWh\n",
                mw.spannung, mw.strom, mw.wirkleistung, mw.frequenz, mw.gesamtKWh);
  mqttVeroeffentlichen();
}

void modbusTask() {
  uint32_t jetzt = millis();

  if (!zyklusAktiv) {
    if (jetzt - tLetzterZyklus < POLL_INTERVAL_MS) return;
    tLetzterZyklus = jetzt;
    zyklusAktiv    = true;
    regIndex       = 0;
    puffer         = Messwerte();
  }

  if (jetzt - tLetzteAnfrage < MODBUS_PAUSE_MS) return;

  const Register &r = REGISTER[regIndex];
  uint8_t res = sdm.readInputRegisters(r.adresse, 2);
  tLetzteAnfrage = millis();

  if (res != sdm.ku8MBSuccess) {
    letzterFehler = res;
    zyklusFehlgeschlagen(r.name);      // Abbruch: bei Störung nicht 10x auf Timeout warten
    return;
  }
  puffer.*(r.feld) = pufferFloat(0);

  if (++regIndex >= ANZAHL_REG) zyklusAbschliessen();
}

// ---------------------------------------------------------------------
//  Weboberfläche
// ---------------------------------------------------------------------
const char INDEX_HTML[] = R"rawliteral(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SDM120 Zähler</title>
<style>
 body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px}
 h1{font-size:1.3em;margin:0 0 12px}
 .gross{font-size:2.6em;font-weight:700;margin:4px 0}
 .karte{background:#1e1e1e;border-radius:10px;padding:14px;margin-bottom:12px}
 table{width:100%;border-collapse:collapse}
 td{padding:6px 4px;border-bottom:1px solid #333}
 td.w{text-align:right;font-variant-numeric:tabular-nums}
 .status{font-size:.85em;color:#999}
 .ok{color:#6c6}.err{color:#e55}
</style></head><body>
<h1>Stromzähler SDM120</h1>
<div class="karte"><div>Zählerstand</div><div class="gross" id="kwh">–</div>
<div>Leistung: <b id="p">–</b></div></div>
<div class="karte"><table id="tab"></table></div>
<div class="status" id="st">lade ...</div>
<script>
const F=[["spannung","Spannung","V"],["strom","Strom","A"],["wirkleistung","Wirkleistung","W"],
["scheinleistung","Scheinleistung","VA"],["blindleistung","Blindleistung","var"],
["leistungsfaktor","Leistungsfaktor",""],["frequenz","Frequenz","Hz"],
["import_kwh","Bezug","kWh"],["export_kwh","Einspeisung","kWh"],["gesamt_kwh","Gesamt","kWh"]];
async function upd(){
 try{
  const d=await (await fetch('/api')).json();
  const v=(k,e)=>d[k]===null?'–':d[k]+(e?' '+e:'');
  document.getElementById('kwh').textContent=v('gesamt_kwh','kWh');
  document.getElementById('p').textContent=v('wirkleistung','W');
  document.getElementById('tab').innerHTML=F.map(f=>`<tr><td>${f[1]}</td><td class="w">${v(f[0],f[2])}</td></tr>`).join('');
  document.getElementById('st').innerHTML=(d.gueltig?'<span class="ok">● Messung ok</span>':'<span class="err">● keine gültigen Daten</span>')+
   ` · Alter ${d.alter_s}s · OK ${d.ok} / Fehler ${d.fehler} · RSSI ${d.rssi} dBm · Laufzeit ${Math.floor(d.uptime_s/60)} min`;
 }catch(e){document.getElementById('st').innerHTML='<span class="err">● keine Verbindung zum ESP</span>';}
}
upd();setInterval(upd,2000);
</script></body></html>)rawliteral";

void webEinrichten() {
  web.on("/", []() { web.send(200, "text/html", INDEX_HTML); });
  web.on("/api", []() {
    char json[512];
    baueJson(json, sizeof(json));
    web.sendHeader("Access-Control-Allow-Origin", "*");
    web.sendHeader("Cache-Control", "no-cache");
    web.send(200, "application/json", json);
  });
  web.onNotFound([]() { web.send(404, "text/plain", "Nicht gefunden"); });
  web.begin();
}

// ---------------------------------------------------------------------
//  Setup / Loop
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.printf("\n\nSDM120-Gateway %s startet ...\n", FW_VERSION);

  // RS485 / Modbus über Hardware-UART2
  if (PIN_RS485_DE >= 0) { pinMode(PIN_RS485_DE, OUTPUT); digitalWrite(PIN_RS485_DE, LOW); }
  rs485.begin(MODBUS_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
  sdm.begin(MODBUS_SLAVE, rs485);
  sdm.preTransmission(preTx);
  sdm.postTransmission(postTx);
  sdm.idle([]() { yield(); });        // andere Tasks während der Wartezeit bedienen

  // Bluetooth Classic
#if BT_ENABLED
  if (SerialBT.begin(BT_NAME)) Serial.printf("[BT] bereit als \"%s\"\n", BT_NAME);
  else                         Serial.println(F("[BT] Start fehlgeschlagen"));
#endif

  // WLAN (läuft parallel zu Bluetooth; Modem-Sleep NICHT abschalten!)
#if WIFI_ENABLED
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("[WLAN] Verbinde "));
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(250); Serial.print('.'); }
  if (WiFi.status() == WL_CONNECTED) Serial.printf(" ok, IP %s\n", WiFi.localIP().toString().c_str());
  else Serial.println(F(" noch nicht verbunden, versuche im Hintergrund weiter"));

  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);

#if MQTT_ENABLED
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);            // JSON ist größer als die Standard-256 Byte
#endif

  webEinrichten();
#endif
}

void loop() {
#if WIFI_ENABLED
  web.handleClient();
  mqttTask();
#endif
#if BT_ENABLED
  btTask();
#endif
  modbusTask();
}
