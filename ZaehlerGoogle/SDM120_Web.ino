/*
  SDM120_Web.ino
  ---------------------------------------------------------------------------
  Eastron SDM120 (Modbus RTU / RS485)  ->  D1 Mini ESP32  ->  Browser

  Der ESP32 spannt einen eigenen WLAN-Access-Point auf und liefert eine
  Webseite mit allen Messwerten aus. Kein Internet und kein Router noetig.

    WLAN:      SDM120-Zaehler   (Passwort siehe AP_PASS)
    Adresse:   http://192.168.4.1     (dank DNS-Umleitung geht auch jede andere)
    JSON-API:  http://192.168.4.1/api/werte

  Benoetigt nur das ESP32-Board-Paket (Arduino IDE), keine Zusatz-Bibliothek.
  Getestet fuer Arduino-ESP32 Core 2.x und 3.x.

  Verdrahtung (RS485-TTL-Modul, z. B. MAX485 / MAX3485):
    Modul RO  -> GPIO16 (RX2)
    Modul DI  -> GPIO17 (TX2)
    Modul DE + RE (gebrueckt) -> GPIO4   (bei Modulen mit Auto-Richtung: PIN_DE = -1)
    Modul VCC -> 3V3 (MAX3485) bzw. 5V (MAX485, dann RO ueber Spannungsteiler!)
    Modul GND -> GND
    Modul A   -> SDM120 Klemme A
    Modul B   -> SDM120 Klemme B
  ---------------------------------------------------------------------------
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// ============================ Konfiguration ================================
const char*    AP_SSID      = "SDM120-Zaehler";
const char*    AP_PASS      = "zaehler123";   // mind. 8 Zeichen, "" = offenes WLAN
const bool     DNS_UMLEITEN = true;           // jede eingegebene Adresse -> diese Seite

const uint8_t  SDM_ID       = 1;              // Modbus-Adresse des Zaehlers (Werk: 1)
const uint32_t SDM_BAUD     = 2400;           // Werkseinstellung SDM120: 2400 Baud, 8N1
const int      PIN_RX       = 16;
const int      PIN_TX       = 17;
const int      PIN_DE       = 4;              // -1 bei Modul mit automatischer Richtung

const uint32_t TIMEOUT_MS   = 250;            // max. Wartezeit auf Antwort
const uint32_t PAUSE_MS     = 60;             // Pause zwischen zwei Anfragen
// ===========================================================================

// Register-Tabelle (Input Register, Funktion 0x04, je 2 Register = 1 Float)
struct Register {
  uint16_t    adresse;
  const char* schluessel;   // Name im JSON
};

const Register REG[] = {
  {0x0000, "u"},     // Spannung            V
  {0x0006, "i"},     // Strom               A
  {0x000C, "p"},     // Wirkleistung        W   (negativ = Einspeisung)
  {0x0012, "s"},     // Scheinleistung      VA
  {0x0018, "q"},     // Blindleistung       var
  {0x001E, "pf"},    // Leistungsfaktor     -
  {0x0024, "phi"},   // Phasenwinkel        Grad
  {0x0046, "f"},     // Frequenz            Hz
  {0x0048, "eImp"},  // Bezug   (Import)    kWh
  {0x004A, "eExp"},  // Einspeisung (Export) kWh
  {0x0156, "eGes"},  // Gesamt-Wirkenergie  kWh
};
const size_t ANZ_REG = sizeof(REG) / sizeof(REG[0]);

float    wert[ANZ_REG];
bool     wertGueltig[ANZ_REG];
uint32_t wertZeit[ANZ_REG];

uint32_t zaehlerOk     = 0;
uint32_t zaehlerFehler = 0;
uint32_t fehlerInFolge = 0;
size_t   naechstesReg  = 0;
uint32_t letzteAnfrage = 0;

WebServer server(80);
DNSServer dns;

// ---------------------------------------------------------------------------
// Webseite (identisch mit index.html, Demo-Modus schaltet sich auf dem ESP ab)
// ---------------------------------------------------------------------------
const char HTML_SEITE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SDM120 Zähler</title>
<style>
:root{--bg:#f3f4f6;--panel:#fff;--fg:#111827;--muted:#6b7280;--line:#e5e7eb;--acc:#2563eb;--ok:#16a34a;--warn:#d97706;--bad:#dc2626}
@media (prefers-color-scheme:dark){:root{--bg:#0f1115;--panel:#181b22;--fg:#e5e7eb;--muted:#9ca3af;--line:#2a2f3a;--acc:#60a5fa;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:960px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:16px}
h1{font-size:1.25rem;margin:0}
.status{display:flex;align-items:center;gap:8px;font-size:.85rem;color:var(--muted)}
.dot{width:10px;height:10px;border-radius:50%;background:var(--muted)}
.dot.ok{background:var(--ok)}.dot.warn{background:var(--warn)}.dot.bad{background:var(--bad)}
.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px 16px}
.hero{margin-bottom:12px}
.lbl{font-size:.8rem;color:var(--muted);text-transform:uppercase;letter-spacing:.04em}
.big{font-size:2.8rem;font-weight:700;font-variant-numeric:tabular-nums;line-height:1.1}
.unit{font-size:1rem;font-weight:500;color:var(--muted);margin-left:4px}
.richtung{font-size:.9rem;font-weight:600}
.bar{height:12px;background:var(--line);border-radius:6px;overflow:hidden;margin-top:10px}
.bar i{display:block;height:100%;width:0;background:var(--acc);transition:width .4s}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:12px;margin-bottom:12px}
.val{font-size:1.6rem;font-weight:650;font-variant-numeric:tabular-nums;margin-top:4px}
h2{font-size:.95rem;margin:20px 0 8px;color:var(--muted);font-weight:600}
canvas{width:100%;height:180px;display:block}
footer{margin-top:16px;font-size:.78rem;color:var(--muted);display:flex;flex-wrap:wrap;gap:6px 18px}
.demo{background:var(--warn);color:#000;padding:2px 8px;border-radius:6px;font-weight:600}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>⚡ SDM120 Stromzähler</h1>
    <div class="status"><span id="demo"></span><span class="dot" id="dot"></span><span id="stxt">verbinde …</span></div>
  </header>

  <div class="card hero">
    <div class="lbl">Wirkleistung</div>
    <div><span class="big" id="p">—</span><span class="unit">W</span> <span class="richtung" id="ri"></span></div>
    <div class="bar"><i id="pbar"></i></div>
  </div>

  <div class="grid">
    <div class="card"><div class="lbl">Spannung</div><div class="val"><span id="u">—</span><span class="unit">V</span></div></div>
    <div class="card"><div class="lbl">Strom</div><div class="val"><span id="i">—</span><span class="unit">A</span></div></div>
    <div class="card"><div class="lbl">Frequenz</div><div class="val"><span id="f">—</span><span class="unit">Hz</span></div></div>
    <div class="card"><div class="lbl">Leistungsfaktor</div><div class="val"><span id="pf">—</span></div></div>
    <div class="card"><div class="lbl">Scheinleistung</div><div class="val"><span id="s">—</span><span class="unit">VA</span></div></div>
    <div class="card"><div class="lbl">Blindleistung</div><div class="val"><span id="q">—</span><span class="unit">var</span></div></div>
    <div class="card"><div class="lbl">Phasenwinkel</div><div class="val"><span id="phi">—</span><span class="unit">°</span></div></div>
  </div>

  <h2>Energie</h2>
  <div class="grid">
    <div class="card"><div class="lbl">Bezug (Import)</div><div class="val"><span id="eImp">—</span><span class="unit">kWh</span></div></div>
    <div class="card"><div class="lbl">Einspeisung (Export)</div><div class="val"><span id="eExp">—</span><span class="unit">kWh</span></div></div>
    <div class="card"><div class="lbl">Gesamt</div><div class="val"><span id="eGes">—</span><span class="unit">kWh</span></div></div>
  </div>

  <h2>Leistungsverlauf (letzte 5 Minuten)</h2>
  <div class="card"><canvas id="chart"></canvas></div>

  <footer>
    <span>Lesungen ok: <b id="sok">—</b></span>
    <span>Fehler: <b id="sfe">—</b></span>
    <span>Laufzeit ESP32: <b id="slz">—</b></span>
    <span>Werte-Alter: <b id="salt">—</b></span>
  </footer>
</div>

<script>
// ---------- Einstellungen ----------
const INTERVALL_MS = 2000;   // Abfrage-Intervall der Seite
const P_MAX_W      = 3680;   // Vollausschlag Balken (16 A x 230 V)
const HIST_PUNKTE  = 150;    // 150 x 2 s = 5 Minuten Verlauf
// Demo-Modus: Datei lokal geöffnet oder ?demo in der Adresse
const DEMO = location.protocol === 'file:' || new URLSearchParams(location.search).has('demo');

const $ = id => document.getElementById(id);
let hist = [];
let fehlerInFolge = 0;

function fmt(v, d) {
  if (v === null || v === undefined || isNaN(v)) return '—';
  return v.toLocaleString('de-DE', {minimumFractionDigits: d, maximumFractionDigits: d});
}

// Simulierte Werte für die Vorschau ohne ESP32
let demoE = 1234.567, demoT0 = Date.now();
function demoWerte() {
  const t = (Date.now() - demoT0) / 1000;
  const p = 450 + 380 * Math.sin(t / 25) + 120 * Math.sin(t / 3.7) + (Math.random() - .5) * 40;
  const u = 230 + 2.5 * Math.sin(t / 40) + (Math.random() - .5);
  const pf = 0.93 + 0.04 * Math.sin(t / 17);
  const s = Math.abs(p) / pf;
  demoE += Math.max(p, 0) * INTERVALL_MS / 3.6e9;
  return {
    ok: true, id: 1, alter_ms: 300,
    werte: { u, i: s / u, p, s, q: Math.sqrt(s * s - p * p), pf, phi: Math.acos(pf) * 180 / Math.PI,
             f: 50 + (Math.random() - .5) * .04, eImp: demoE, eExp: 12.345, eGes: demoE + 12.345 },
    stat: { ok: Math.floor(t * 5.5), fehler: 0, laufzeit_s: Math.floor(t) }
  };
}

async function holen() {
  if (DEMO) return demoWerte();
  const ctl = new AbortController();
  const tmo = setTimeout(() => ctl.abort(), 3000);
  try {
    const r = await fetch('/api/werte', {signal: ctl.signal, cache: 'no-store'});
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return await r.json();
  } finally { clearTimeout(tmo); }
}

function laufzeitText(s) {
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return h > 0 ? h + ' h ' + m + ' min' : m + ' min ' + (s % 60) + ' s';
}

function status(klasse, text) { $('dot').className = 'dot ' + klasse; $('stxt').textContent = text; }

function anzeigen(d) {
  const w = d.werte || {};
  $('p').textContent = fmt(w.p, 0);
  $('u').textContent = fmt(w.u, 1);
  $('i').textContent = fmt(w.i, 2);
  $('f').textContent = fmt(w.f, 2);
  $('pf').textContent = fmt(w.pf, 3);
  $('s').textContent = fmt(w.s, 0);
  $('q').textContent = fmt(w.q, 0);
  $('phi').textContent = fmt(w.phi, 1);
  $('eImp').textContent = fmt(w.eImp, 3);
  $('eExp').textContent = fmt(w.eExp, 3);
  $('eGes').textContent = fmt(w.eGes, 3);

  // Balken + Richtung (SDM120: negative Wirkleistung = Einspeisung)
  const p = w.p;
  const bar = $('pbar');
  if (p === null || p === undefined) { bar.style.width = '0'; $('ri').textContent = ''; }
  else {
    bar.style.width = Math.min(100, Math.abs(p) / P_MAX_W * 100) + '%';
    bar.style.background = p < 0 ? 'var(--ok)' : 'var(--acc)';
    $('ri').textContent = p < 0 ? '↗ Einspeisung' : '↘ Bezug';
    $('ri').style.color = p < 0 ? 'var(--ok)' : 'var(--acc)';
  }

  const st = d.stat || {};
  $('sok').textContent = st.ok ?? '—';
  $('sfe').textContent = st.fehler ?? '—';
  $('slz').textContent = st.laufzeit_s !== undefined ? laufzeitText(st.laufzeit_s) : '—';
  $('salt').textContent = d.alter_ms !== undefined && d.alter_ms !== null ? fmt(d.alter_ms / 1000, 1) + ' s' : '—';

  if (d.ok) status('ok', DEMO ? 'Demo-Werte' : 'Zähler online');
  else status('warn', 'ESP32 ok, Zähler antwortet nicht');
}

function zeichneChart() {
  const cv = $('chart');
  const dpr = window.devicePixelRatio || 1;
  const W = cv.clientWidth, H = cv.clientHeight;
  cv.width = W * dpr; cv.height = H * dpr;
  const g = cv.getContext('2d');
  g.scale(dpr, dpr);
  g.clearRect(0, 0, W, H);
  const css = getComputedStyle(document.documentElement);
  const cLine = css.getPropertyValue('--line'), cMuted = css.getPropertyValue('--muted'), cAcc = css.getPropertyValue('--acc');
  const pts = hist.filter(v => v !== null);
  if (pts.length < 2) {
    g.fillStyle = cMuted; g.font = '13px system-ui'; g.fillText('Sammle Werte …', 10, 20); return;
  }
  let min = Math.min(0, ...pts), max = Math.max(...pts);
  if (max - min < 50) max = min + 50;
  const pad = (max - min) * 0.1; max += pad; if (min < 0) min -= pad;
  const L = 48, R = 8, T = 8, B = 18;
  const x = k => L + k / (HIST_PUNKTE - 1) * (W - L - R);
  const y = v => T + (max - v) / (max - min) * (H - T - B);
  // Gitter
  g.strokeStyle = cLine; g.fillStyle = cMuted; g.font = '11px system-ui'; g.lineWidth = 1;
  for (let k = 0; k <= 4; k++) {
    const v = min + (max - min) * k / 4, yy = y(v);
    g.beginPath(); g.moveTo(L, yy); g.lineTo(W - R, yy); g.stroke();
    g.fillText(Math.round(v) + ' W', 2, yy + 4);
  }
  g.fillText('−5 min', L, H - 4);
  g.fillText('jetzt', W - R - 26, H - 4);
  // Linie
  const off = HIST_PUNKTE - hist.length;
  g.strokeStyle = cAcc; g.lineWidth = 2; g.beginPath();
  let start = true;
  hist.forEach((v, k) => {
    if (v === null) { start = true; return; }
    if (start) { g.moveTo(x(k + off), y(v)); start = false; } else g.lineTo(x(k + off), y(v));
  });
  g.stroke();
}

async function zyklus() {
  try {
    const d = await holen();
    fehlerInFolge = 0;
    anzeigen(d);
    hist.push(d.werte && d.werte.p !== null && d.werte.p !== undefined ? d.werte.p : null);
  } catch (e) {
    fehlerInFolge++;
    if (fehlerInFolge >= 2) status('bad', 'ESP32 nicht erreichbar');
    hist.push(null);
  }
  if (hist.length > HIST_PUNKTE) hist.shift();
  zeichneChart();
  setTimeout(zyklus, INTERVALL_MS);
}

if (DEMO) $('demo').innerHTML = '<span class="demo">DEMO</span>';
window.addEventListener('resize', zeichneChart);
zyklus();
</script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------
// Modbus RTU
// ---------------------------------------------------------------------------
uint16_t crc16(const uint8_t* daten, size_t laenge) {
  uint16_t crc = 0xFFFF;
  for (size_t n = 0; n < laenge; n++) {
    crc ^= daten[n];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
      else              crc >>= 1;
    }
  }
  return crc;
}

// Liest einen Float (2 Register) per Funktion 0x04. true = Erfolg.
bool leseFloat(uint16_t adresse, float& ergebnis) {
  uint8_t anfrage[8] = {
    SDM_ID, 0x04,
    (uint8_t)(adresse >> 8), (uint8_t)(adresse & 0xFF),
    0x00, 0x02, 0, 0
  };
  uint16_t crc = crc16(anfrage, 6);
  anfrage[6] = crc & 0xFF;      // CRC: Low-Byte zuerst
  anfrage[7] = crc >> 8;

  while (Serial2.available()) Serial2.read();     // alten Muell verwerfen

  if (PIN_DE >= 0) digitalWrite(PIN_DE, HIGH);    // senden
  Serial2.write(anfrage, sizeof(anfrage));
  Serial2.flush();                                // warten bis alles raus ist
  if (PIN_DE >= 0) digitalWrite(PIN_DE, LOW);     // empfangen

  // Erwartet: ID, 0x04, 0x04, 4 Datenbytes, CRC lo, CRC hi = 9 Bytes
  uint8_t antwort[9];
  size_t  n  = 0;
  uint32_t t0 = millis();
  while (n < sizeof(antwort) && millis() - t0 < TIMEOUT_MS) {
    if (Serial2.available()) {
      antwort[n++] = Serial2.read();
      // Exception-Antwort (Funktion | 0x80) ist nur 5 Bytes lang
      if (n == 5 && (antwort[1] & 0x80)) return false;
    } else {
      delay(1);
    }
  }
  if (n != sizeof(antwort))                         return false;
  if (antwort[0] != SDM_ID || antwort[1] != 0x04)   return false;
  if (antwort[2] != 4)                              return false;
  uint16_t crcRx = antwort[7] | (antwort[8] << 8);
  if (crcRx != crc16(antwort, 7))                   return false;

  // IEEE-754 Float, Big Endian
  uint32_t roh = ((uint32_t)antwort[3] << 24) | ((uint32_t)antwort[4] << 16) |
                 ((uint32_t)antwort[5] << 8)  |  (uint32_t)antwort[6];
  memcpy(&ergebnis, &roh, sizeof(ergebnis));
  return !isnan(ergebnis);
}

// Fragt pro Aufruf genau ein Register ab -> Webserver bleibt reaktionsfaehig
void zaehlerAbfragen() {
  if (millis() - letzteAnfrage < PAUSE_MS) return;
  letzteAnfrage = millis();

  float v;
  if (leseFloat(REG[naechstesReg].adresse, v)) {
    wert[naechstesReg]        = v;
    wertGueltig[naechstesReg] = true;
    wertZeit[naechstesReg]    = millis();
    zaehlerOk++;
    fehlerInFolge = 0;
  } else {
    zaehlerFehler++;
    fehlerInFolge++;
  }
  naechstesReg = (naechstesReg + 1) % ANZ_REG;
}

// Ein Wert gilt als veraltet, wenn er laenger als 10 s nicht gelesen wurde
bool istAktuell(size_t k) {
  return wertGueltig[k] && (millis() - wertZeit[k] < 10000);
}

// ---------------------------------------------------------------------------
// HTTP-Handler
// ---------------------------------------------------------------------------
void handleSeite() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", HTML_SEITE);
}

void handleWerte() {
  String j;
  j.reserve(512);
  bool alleAktuell = true;
  uint32_t aeltester = 0;

  j += "{\"id\":";
  j += SDM_ID;
  j += ",\"werte\":{";
  for (size_t k = 0; k < ANZ_REG; k++) {
    if (k) j += ',';
    j += '"'; j += REG[k].schluessel; j += "\":";
    if (istAktuell(k)) {
      char puffer[24];
      snprintf(puffer, sizeof(puffer), "%.4f", wert[k]);
      j += puffer;
      uint32_t alter = millis() - wertZeit[k];
      if (alter > aeltester) aeltester = alter;
    } else {
      j += "null";
      alleAktuell = false;
    }
  }
  j += "},\"ok\":";
  j += (alleAktuell && fehlerInFolge < ANZ_REG) ? "true" : "false";
  j += ",\"alter_ms\":";
  j += aeltester;
  j += ",\"stat\":{\"ok\":";
  j += zaehlerOk;
  j += ",\"fehler\":";
  j += zaehlerFehler;
  j += ",\"laufzeit_s\":";
  j += millis() / 1000;
  j += "}}";

  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

void handleUnbekannt() {
  // Captive-Portal-Anfragen von Android/iOS/Windows auf die Startseite lenken
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("SDM120 Web-Anzeige startet ..."));

  if (PIN_DE >= 0) {
    pinMode(PIN_DE, OUTPUT);
    digitalWrite(PIN_DE, LOW);
  }
  Serial2.begin(SDM_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print(F("Access-Point: "));  Serial.println(AP_SSID);
  Serial.print(F("Adresse:      http://")); Serial.println(WiFi.softAPIP());

  if (DNS_UMLEITEN) dns.start(53, "*", WiFi.softAPIP());

  server.on("/",          handleSeite);
  server.on("/index.html", handleSeite);
  server.on("/api/werte", handleWerte);
  server.onNotFound(handleUnbekannt);
  server.begin();
}

void loop() {
  if (DNS_UMLEITEN) dns.processNextRequest();
  server.handleClient();
  zaehlerAbfragen();

  // Alle 10 s eine Statuszeile auf den seriellen Monitor
  static uint32_t letzterLog = 0;
  if (millis() - letzterLog > 10000) {
    letzterLog = millis();
    Serial.printf("U=%.1f V  I=%.2f A  P=%.0f W  E=%.3f kWh  ok=%lu  fehler=%lu  clients=%d\n",
                  wert[0], wert[1], wert[2], wert[10],
                  (unsigned long)zaehlerOk, (unsigned long)zaehlerFehler,
                  WiFi.softAPgetStationNum());
  }
}
