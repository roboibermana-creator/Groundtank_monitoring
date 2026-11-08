// Task_Net (GT-04 + GT-05): satu-satunya task yang memakai jaringan.
//   - network_loop / lan_update_loop / OTA
//   - uplink Queue_Net -> Supabase (POST tabel Hydrosense; level via RPC submit_reading)
//   - polling perintah dashboard (device_commands, type=control) -> Queue_Cmd
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "gw_lora.h"
#include "gw_net.h"
#include "lan_update.h"
#include "network.h"
#include "supabase.h"

QueueHandle_t queueNet = nullptr;
QueueHandle_t queueCmd = nullptr;
volatile bool gwOtaBerjalan = false;

void gwQueueBuat() {
    queueNet = xQueueCreate(24, sizeof(NetItem));
    queueCmd = xQueueCreate(8, sizeof(CmdItem));
}

void gwNetKirim(const NetItem& item) {
    if (queueNet) xQueueSend(queueNet, &item, 0);
}

// ---------------- GT-05: uplink ----------------
static const int MAKS_PENDING = 16;
static NetItem pending[MAKS_PENDING];
static int pendingJumlah = 0;
static unsigned long uplinkBackoffMs = 0, uplinkBerikutnya = 0;

static void tambahPending(const NetItem& it) {
    if (pendingJumlah == MAKS_PENDING) { // penuh: buang yang paling lama
        memmove(&pending[0], &pending[1], sizeof(NetItem) * (MAKS_PENDING - 1));
        pendingJumlah--;
    }
    pending[pendingJumlah++] = it;
}

// Status alat terakhir dari node (lewat ping LoRa); dilaporkan ke Supabase oleh laporkanStatusLora().
static bool statusAlatAda = false;
static uint8_t statusAlat = 0;
static bool paksaLapor = false; // status berubah -> lapor sekarang, jangan tunggu interval

// Jadwal siklus otomatis yang sedang dikirim ke node / yang terakhir di-ACK node.
static uint16_t cfgIdMenunggu = 0; // 0 = tidak ada yang menunggu
static unsigned long cfgMenungguMs = 0;
static LoraJadwalPayload cfgMenunggu = {};
static LoraJadwalPayload cfgTerkirim = {};
static bool cfgTerkirimAda = false;

// Status penuh terakhir dari node (ping LoRa): dilaporkan ke devices dan dipakai untuk baris level/hujan.
static LoraStatusPayload stTerakhir = {};
static bool stPenuhAda = false;

// Notifikasi perintah baru ke node, jenis 1..4 (LoraNotifJenis).
static uint32_t notifLast[5] = {0, 0, 0, 0, 0};     // id terakhir yang di-ACK node
static uint32_t notifKirimId[5] = {0, 0, 0, 0, 0};  // id yang sedang dikirim
static uint16_t notifCmdId[5] = {0, 0, 0, 0, 0};    // cmdId antrean (0 = tidak menunggu hasil)
static bool notifBaselineAda[5] = {false, false, false, false, false};

static String angkaAtauNull(bool ok, float v, int desimal) {
    return ok ? String(v, desimal) : String("null");
}

// true = item selesai (sukses atau sengaja dibuang); false = coba lagi nanti
static bool kirimItem(const NetItem& it) {
    switch (it.tipe) {
    case NI_NODE_DATA: {
        const LoraDataPayload& d = it.data;
        bool phOk = d.ph_x100 != INT16_MIN;
        bool suhuOk = d.suhu_x10 != INT16_MIN;
        bool tssOk = d.tss_x10 != 0xFFFF;
        if (!phOk && !suhuOk && !tssOk) { // paket uji ('J' di node): jangan buat baris kosong
            Serial.printf("[NET] paket uji LoRa dari node %u diterima (RSSI %d), tidak dikirim ke Supabase\n",
                          it.nodeId, it.rssi);
            return true;
        }
        String body = "{\"ph\":" + angkaAtauNull(phOk, d.ph_x100 / 100.0f, 2) +
                      ",\"tss\":" + angkaAtauNull(tssOk, d.tss_x10 / 10.0f, 2) +
                      ",\"water_temperature\":" + angkaAtauNull(suhuOk, d.suhu_x10 / 10.0f, 1) + "}";
        int code = supabase_rest("POST", "/rest/v1/Hydrosense%20table", body.c_str());
        Serial.printf("[NET] DATA node %u (RSSI %d) -> HTTP %d\n", it.nodeId, it.rssi, code);
        // 4xx = payload/konfigurasi salah: jangan diulang selamanya
        return (code >= 200 && code < 300) || (code >= 400 && code < 500);
    }
    case NI_NODE_EVENT:
        if (it.kode == LORA_EVENT && it.evKode == 3) { // ping tautan (+ status alat): tanpa log
            if (it.adaStatus) {
                if (!statusAlatAda || statusAlat != it.statusAlat) paksaLapor = true;
                statusAlatAda = true;
                statusAlat = it.statusAlat;
            }
            if (it.statusPenuh) {
                if (!stPenuhAda || stTerakhir.mode != it.st.mode || stTerakhir.fase != it.st.fase) paksaLapor = true;
                stTerakhir = it.st;
                stPenuhAda = true;
            }
            return true;
        }
        Serial.printf("[NET] node %u kirim %s (kode %u): dicatat di Serial, belum ada tabel tujuan\n",
                      it.nodeId, it.kode == LORA_ALARM ? "ALARM" : "EVENT", it.evKode);
        return true;
    case NI_CMD_STATUS: {
        for (uint8_t j = 1; j <= 4; j++) { // hasil pemberitahuan perintah baru ke node
            if (notifCmdId[j] != 0 && it.cmdId == notifCmdId[j]) {
                if (it.kode == CS_SELESAI) notifLast[j] = notifKirimId[j]; // node sudah diberi tahu
                else Serial.printf("[NET] pemberitahuan ke node (jenis %u) gagal; diulang\n", j);
                notifCmdId[j] = 0;
                return true;
            }
        }
        if (cfgIdMenunggu != 0 && it.cmdId == cfgIdMenunggu) { // hasil kirim jadwal
            if (it.kode == CS_SELESAI) {
                cfgTerkirim = cfgMenunggu;
                cfgTerkirimAda = true;
                Serial.println("[NET] jadwal diterima node (ACK)");
            } else {
                Serial.println("[NET] jadwal GAGAL sampai ke node; diulang pada pembacaan berikutnya");
            }
            cfgIdMenunggu = 0;
            return true;
        }
        static const char* nama[] = {"SELESAI", "GAGAL", "EXPIRED", "DITOLAK"};
        Serial.printf("[NET] perintah %u -> %s\n", it.cmdId, it.kode < 4 ? nama[it.kode] : "?");
        return true; // device_commands belum punya kolom status; hanya dicatat
    }
    default:
        return true;
    }
}

static void kirimUplink() {
    if (pendingJumlah == 0 || millis() < uplinkBerikutnya) return;
    if (kirimItem(pending[0])) {
        memmove(&pending[0], &pending[1], sizeof(NetItem) * (pendingJumlah - 1));
        pendingJumlah--;
        uplinkBackoffMs = 0;
    } else {
        uplinkBackoffMs = uplinkBackoffMs ? min(uplinkBackoffMs * 2, SUPABASE_BACKOFF_MAX_MS)
                                          : SUPABASE_BACKOFF_MIN_MS;
        uplinkBerikutnya = millis() + uplinkBackoffMs;
    }
}

// Level (jarak mentah + float): perilaku lama loop() dipertahankan, termasuk backoff.
static float levelTerakhir = 0;
static bool floatTerakhir = false;
static unsigned long levelSegarSampai = 0;

static void kirimLevelDanFloat() {
    static unsigned long lastSend = 0, waitMs = 0, backoff = 0;
    if (levelTerakhir > 0 && millis() < levelSegarSampai && millis() - lastSend >= waitMs) {
        lastSend = millis();
        int code = supabase_send(levelTerakhir);
        if (code == 0 || code >= 500) {
            backoff = backoff ? min(backoff * 2, SUPABASE_BACKOFF_MAX_MS) : SUPABASE_BACKOFF_MIN_MS;
            waitMs = backoff;
        } else {
            backoff = 0;
            waitMs = SUPABASE_SEND_INTERVAL_MS;
        }
    }
    static int floatSent = -1;
    static unsigned long floatTry = 0;
    if (floatSent != (int)floatTerakhir &&
        (floatTry == 0 || millis() - floatTry >= SUPABASE_SEND_INTERVAL_MS)) {
        floatTry = millis();
        if (supabase_send_float(floatTerakhir) == 204) {
            floatSent = floatTerakhir;
            floatTry = 0;
        }
    }
}

// ---------------- GT-05: polling perintah ----------------
static uint16_t cmdCounter = 0;

static void enqueueCmd(uint8_t aksi, uint8_t nilai) {
    if (++cmdCounter == 0) cmdCounter = 1;
    CmdItem c = {};
    c.cmdId = cmdCounter;
    c.aksi = aksi;
    c.nilai = nilai;
    c.ttlS = GW_CMD_TTL_S;
    c.kedaluwarsaMs = millis() + (uint32_t)GW_CMD_TTL_S * 1000UL;
    if (xQueueSend(queueCmd, &c, 0) != pdTRUE) Serial.println("[NET] Queue_Cmd penuh, perintah dibuang");
}

static void ambilPerintah() {
    static unsigned long berikutnya = 0;
    if (millis() < berikutnya) return;
    berikutnya = millis() + GW_CMD_POLL_MS;

    char resp[512];
    int code = supabase_rest("GET",
        "/rest/v1/device_commands?select=id,params&type=eq.control&order=id.desc&limit=1",
        nullptr, resp, sizeof(resp));
    if (code != 200) return;
    char* awal = strchr(resp, '[');
    char* akhir = strrchr(resp, ']');
    if (!awal || !akhir || akhir < awal) return;
    akhir[1] = 0;
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, awal) || !doc.is<JsonArray>() || doc.size() == 0) return;
    JsonObject baris = doc[0];
    long id = baris["id"] | -1L;
    JsonObject params = baris["params"];
    String mode = (const char*)(params["mode"] | "otomatis");
    String pump = (const char*)(params["pump_cmd"] | "berhenti");
    String valve = (const char*)(params["valve_cmd"] | "tutup");

    static long idTerakhir = -2; // -2 = belum pernah baca
    static bool manualAktif = false;
    static unsigned long kirimTerakhir = 0;
    if (idTerakhir == -2) { // SAFETY BOOT: perintah yang sudah ada saat gateway menyala tidak dijalankan
        idTerakhir = id;
        return;
    }
    if (id != idTerakhir) {
        idTerakhir = id;
        if (mode == "otomatis") {
            manualAktif = false;
            enqueueCmd(4, 0);
        } else {
            manualAktif = true;
            enqueueCmd(1, pump == "jalan");
            enqueueCmd(2, valve == "buka");
        }
        kirimTerakhir = millis();
    } else if (manualAktif && mode != "otomatis" && millis() - kirimTerakhir >= GW_CMD_REFRESH_MS) {
        // Perpanjang TTL di node (watchdog fail-safe) selama dashboard masih manual
        enqueueCmd(1, pump == "jalan");
        kirimTerakhir = millis();
    }
}

// ---------------- Laporan status tautan LoRa + kondisi alat -> devices ----------------
// Satu PATCH ke baris node di devices berisi: lora_* (indikator LoRa) dan, bila node pernah
// mengirim status, pompa_nyala/valve_buka/float_high + heartbeat_at (dashboard menganggap alat
// "hidup" bila heartbeat_at < 20 dtk, jadi dilaporkan ulang tiap LORA_REPORT_MS selama tautan
// hidup, dan segera saat status berubah).
static void laporkanStatusLora() {
    static unsigned long berikutnya = 0;
    static bool gagalDicatat = false;
    if (millis() < berikutnya && !paksaLapor) return;
    paksaLapor = false;
    berikutnya = millis() + LORA_REPORT_MS;

    const char* status;
    int rssi;
    long umur;
    gwLoraLaporan(&status, &rssi, &umur);

    char body[480];
    int n = snprintf(body, sizeof(body), "{\"lora_status\":\"%s\",", status);
    if (umur >= 0) {
        n += snprintf(body + n, sizeof(body) - n, "\"lora_rssi\":%d,\"lora_ack_age_s\":%ld,", rssi, umur);
    } else {
        n += snprintf(body + n, sizeof(body) - n, "\"lora_rssi\":null,\"lora_ack_age_s\":null,");
    }
    // Status alat hanya bila tautan hidup (paket terakhir < 180 dtk) -- jangan melaporkan
    // status basi sebagai kenyataan.
    if (statusAlatAda && umur >= 0 && umur < 180) {
        n += snprintf(body + n, sizeof(body) - n,
                      "\"pompa_nyala\":%s,\"valve_buka\":%s,\"float_high\":%s,\"heartbeat_at\":\"now\",",
                      (statusAlat & 1) ? "true" : "false", (statusAlat & 2) ? "true" : "false",
                      (statusAlat & 4) ? "true" : "false");
    }
    // Mode, fase siklus, hitung mundur, TSS mentah dari ping status penuh (pengganti heartbeat WiFi).
    if (stPenuhAda && umur >= 0 && umur < 180) {
        static const char* const faseTeks[] = {"manual", "menunggu", "siklus: pompa mengisi",
                                               "siklus: menunggu stabil", "siklus: membaca sensor",
                                               "siklus: valve membuang"};
        n += snprintf(body + n, sizeof(body) - n, "\"mode\":\"%s\",\"fase\":\"%s\",",
                      stTerakhir.mode ? "otomatis" : "manual",
                      stTerakhir.fase <= 5 ? faseTeks[stTerakhir.fase] : "menunggu");
        if (stTerakhir.nextCycleS >= 0)
            n += snprintf(body + n, sizeof(body) - n, "\"next_cycle_s\":%ld,", (long)stTerakhir.nextCycleS);
        else
            n += snprintf(body + n, sizeof(body) - n, "\"next_cycle_s\":null,");
        if (stTerakhir.tssRawX100 != 0xFFFF)
            n += snprintf(body + n, sizeof(body) - n, "\"tss_raw\":%.2f,", stTerakhir.tssRawX100 / 100.0f);
    }
    snprintf(body + n, sizeof(body) - n, "\"lora_status_at\":\"now\"}");

    int code = supabase_rest("PATCH",
        "/rest/v1/devices?project=eq.hydrosense&name=eq." HS_DEVICE_NAME, body);
    bool ok = code >= 200 && code < 300;
    if (!ok && !gagalDicatat) {
        gagalDicatat = true;
        Serial.printf("[NET] Gagal melaporkan status LoRa ke Supabase (HTTP %d). Kolom lora_* belum ada? "
                      "Jalankan migrasi 0012_lora_status.sql di SQL Editor.\n", code);
    }
    if (ok) gagalDicatat = false;
}

// ---------------- Jadwal siklus otomatis dari dashboard -> node (paket CFG) ----------------
// Dashboard menyimpan jadwal di baris node pada tabel devices. Gateway membacanya tiap 30 dtk
// dan mengirim ke node lewat LoRa bila berbeda dari yang terakhir diterima node (ACK). Gagal
// terkirim (node mati/jauh) -> diulang pada pembacaan berikutnya sampai ACK.
static void ambilJadwal() {
    static unsigned long berikutnya = 0;
    if (millis() < berikutnya) return;
    berikutnya = millis() + 30000UL;

    if (cfgIdMenunggu != 0) { // masih menunggu hasil kirim sebelumnya
        if (millis() - cfgMenungguMs < 120000UL) return;
        cfgIdMenunggu = 0; // hasilnya tidak pernah datang; coba lagi
    }

    char resp[512];
    int code = supabase_rest("GET",
        "/rest/v1/devices?project=eq.hydrosense&name=eq." HS_DEVICE_NAME
        "&select=interval_detik,pompa_detik,valve_detik,baca_sensor_detik,stabil_detik,sampel_ms,trim_persen",
        nullptr, resp, sizeof(resp));
    if (code != 200) return;
    char* awal = strchr(resp, '[');
    char* akhir = strrchr(resp, ']');
    if (!awal || !akhir || akhir < awal) return;
    akhir[1] = 0;
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, awal) || !doc.is<JsonArray>() || doc.size() == 0) return;
    JsonObject b = doc[0];
    long interval = b["interval_detik"] | -1L, pompa = b["pompa_detik"] | -1L,
         valve = b["valve_detik"] | -1L, baca = b["baca_sensor_detik"] | -1L,
         stabil = b["stabil_detik"] | -1L, sampel = b["sampel_ms"] | -1L,
         trim = b["trim_persen"] | -1L;
    if (interval < 0 || pompa < 0 || valve < 0 || baca < 0 || stabil < 0 || sampel < 0 || trim < 0)
        return; // jadwal belum lengkap di dashboard: jangan kirim nilai kosong

    LoraJadwalPayload j = {};
    j.intervalDetik = (uint32_t)constrain(interval, 10L, 86400L);
    j.pompaDetik = (uint16_t)constrain(pompa, 1L, 7200L);
    j.valveDetik = (uint16_t)constrain(valve, 1L, 7200L);
    j.bacaSensorDetik = (uint16_t)constrain(baca, 5L, 3600L);
    j.stabilDetik = (uint16_t)constrain(stabil, 1L, 600L);
    j.sampelMs = (uint16_t)constrain(sampel, 200L, 10000L);
    j.trimPersen = (uint8_t)constrain(trim, 0L, 40L);
    if (cfgTerkirimAda && memcmp(&j, &cfgTerkirim, sizeof(j)) == 0) return; // node sudah punya

    if (++cmdCounter == 0) cmdCounter = 1;
    CmdItem c = {};
    c.cmdId = cmdCounter;
    c.jenis = 1; // CFG
    c.jadwal = j;
    c.kedaluwarsaMs = millis() + 120000UL;
    if (xQueueSend(queueCmd, &c, 0) != pdTRUE) {
        Serial.println("[NET] Queue_Cmd penuh, jadwal ditunda");
        return;
    }
    cfgIdMenunggu = c.cmdId;
    cfgMenunggu = j;
    cfgMenungguMs = millis();
    Serial.printf("[NET] Jadwal baru dari dashboard -> dikirim ke node (id %u): siklus %lu dtk, pompa %u, valve %u\n",
                  c.cmdId, (unsigned long)j.intervalDetik, j.pompaDetik, j.valveDetik);
}

// ---------------- Jembatan HTTP untuk node tanpa WiFi ----------------
// Task_LoRa menyusun fragmen permintaan dari node lalu menyerahkannya ke sini; Task_Net
// menjalankannya ke Supabase (satu-satunya pemakai TLS) dan menyimpan respons; Task_LoRa
// mengirim respons balik ke node. Satu permintaan sekali waktu.
static uint8_t proxyReqBuf[LORA_PROXY_MAKS_REQ];
static volatile size_t proxyReqLen = 0;
static volatile bool proxyReqAda = false;
static uint8_t proxyResBuf[LORA_PROXY_MAKS_RES + 2];
static volatile size_t proxyResLen = 0;
static volatile bool proxyResAda = false;

bool gwProxyOcupado() { return proxyReqAda || proxyResAda; }

bool gwProxyTerimaPermintaan(const uint8_t* m, size_t n) {
    if (gwProxyOcupado() || n > sizeof(proxyReqBuf)) return false;
    memcpy(proxyReqBuf, m, n);
    proxyReqLen = n;
    proxyReqAda = true;
    return true;
}

bool gwProxyAdaRespons(uint8_t* out, size_t* n) {
    if (!proxyResAda) return false;
    memcpy(out, proxyResBuf, proxyResLen);
    *n = proxyResLen;
    return true;
}

void gwProxyResponsTerkirim() { proxyResAda = false; }

// Daftar putih tabel yang boleh disentuh node (tanpa autentikasi paket, jangan buka lebih luas).
static bool pathDiizinkan(const char* path) {
    static const char* const ok[] = {"/rest/v1/device_commands", "/rest/v1/calibration",
                                     "/rest/v1/Hydrosense%20table", "/rest/v1/devices"};
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (strncmp(path, ok[i], strlen(ok[i])) == 0) return true;
    return false;
}

static void proxyJalankan() {
    if (!proxyReqAda) return;
    static char path[340];
    static char body[460];
    static char resp[LORA_PROXY_MAKS_RES + 1];
    size_t n = proxyReqLen;
    uint8_t metode = proxyReqBuf[0];
    size_t pl = proxyReqBuf[1] | (proxyReqBuf[2] << 8);
    int kode = 400;
    size_t rl = 0;
    resp[0] = 0;
    if (n >= 3 && 3 + pl <= n && pl < sizeof(path) && n - 3 - pl < sizeof(body)) {
        memcpy(path, proxyReqBuf + 3, pl);
        path[pl] = 0;
        size_t bl = n - 3 - pl;
        memcpy(body, proxyReqBuf + 3 + pl, bl);
        body[bl] = 0;
        bool bersih = true;
        for (size_t i = 0; i < pl; i++)
            if ((uint8_t)path[i] <= ' ' || (uint8_t)path[i] > 126) bersih = false;
        const char* m = metode == PROXY_GET ? "GET" : metode == PROXY_POST ? "POST" : metode == PROXY_PATCH ? "PATCH" : nullptr;
        if (bersih && m && pathDiizinkan(path)) {
            kode = supabase_rest(m, path, bl ? body : nullptr, resp, sizeof(resp));
            rl = strlen(resp);
            Serial.printf("[PROXY] %s %.70s -> HTTP %d (%u byte)\n", m, path, kode, (unsigned)rl);
        } else {
            Serial.println("[PROXY] permintaan ditolak (metode atau path tidak diizinkan)");
        }
    }
    proxyResBuf[0] = (uint8_t)(kode & 0xFF);
    proxyResBuf[1] = (uint8_t)(kode >> 8);
    memcpy(proxyResBuf + 2, resp, rl);
    proxyResLen = 2 + rl;
    proxyResAda = true;
    proxyReqAda = false;
}

// ---------------- Notifikasi perintah baru dashboard -> node ----------------
// Node tanpa WiFi tidak mem-poll Supabase. Gateway yang memantau perintah baru (modbus,
// kalibrasi pH, wizard TSS, faktor TSS) lalu memberi tahu node lewat LoRa; node mengambil
// barisnya lewat jembatan HTTP dan menjalankan penangan yang sama seperti memakai WiFi.
static bool ambilArray(char* resp, JsonDocument& doc) {
    char* awal = strchr(resp, '[');
    char* akhir = strrchr(resp, ']');
    if (!awal || !akhir || akhir < awal) return false;
    akhir[1] = 0;
    return !deserializeJson(doc, awal) && doc.is<JsonArray>();
}

static void kirimNotif(uint8_t jenis, uint32_t id) {
    if (notifCmdId[jenis] != 0) return; // masih menunggu hasil pemberitahuan sebelumnya
    if (++cmdCounter == 0) cmdCounter = 1;
    CmdItem c = {};
    c.cmdId = cmdCounter;
    c.jenis = 2;
    c.notifJenis = jenis;
    c.notifId = id;
    c.kedaluwarsaMs = millis() + 60000UL;
    if (xQueueSend(queueCmd, &c, 0) != pdTRUE) return;
    notifCmdId[jenis] = c.cmdId;
    notifKirimId[jenis] = id;
    Serial.printf("[NET] Perintah baru dari dashboard (jenis %u, id %lu) -> memberi tahu node\n", jenis,
                  (unsigned long)id);
}

static void ambilNotifikasi() {
    static unsigned long berikutnya = 0, faktorBerikut = 0;
    char resp[640];
    if (millis() >= berikutnya) {
        berikutnya = millis() + 4000UL;
        int code = supabase_rest("GET",
            "/rest/v1/device_commands?select=id,type&type=in.(modbus,ph_cal,tss_hw)&status=eq.pending&order=id.asc&limit=20",
            nullptr, resp, sizeof(resp));
        StaticJsonDocument<1024> doc;
        if (code == 200 && ambilArray(resp, doc)) {
            uint32_t maks[4] = {0, 0, 0, 0}, terkecil[4] = {0, 0, 0, 0};
            for (JsonObject r : doc.as<JsonArray>()) {
                const char* tipe = r["type"] | "";
                uint8_t j = !strcmp(tipe, "modbus") ? 1 : !strcmp(tipe, "ph_cal") ? 2 : !strcmp(tipe, "tss_hw") ? 3 : 0;
                uint32_t id = r["id"] | 0UL;
                if (!j || !id) continue;
                if (id > maks[j]) maks[j] = id;
                if (id > notifLast[j] && (terkecil[j] == 0 || id < terkecil[j])) terkecil[j] = id;
            }
            for (uint8_t j = 1; j <= 3; j++) {
                if (!notifBaselineAda[j]) { // yang sudah menunggu saat gateway menyala tidak diberitahukan
                    notifLast[j] = maks[j];
                    notifBaselineAda[j] = true;
                } else if (terkecil[j]) {
                    kirimNotif(j, terkecil[j]);
                }
            }
        }
    }
    if (millis() >= faktorBerikut) { // faktor kalibrasi TSS (tabel calibration) berubah
        faktorBerikut = millis() + 15000UL;
        int code = supabase_rest("GET",
            "/rest/v1/calibration?select=id&sensor=eq.tss&aksi=eq.faktor&order=id.desc&limit=1",
            nullptr, resp, sizeof(resp));
        StaticJsonDocument<256> doc;
        if (code == 200 && ambilArray(resp, doc) && doc.size() > 0) {
            uint32_t id = doc[0]["id"] | 0UL;
            if (!notifBaselineAda[4]) {
                notifLast[4] = id;
                notifBaselineAda[4] = true;
            } else if (id > notifLast[4]) {
                kirimNotif(4, id);
            }
        }
    }
}

// ---------------- Baris level air & hujan dari ping status node ----------------
// Pengganti kirimLevelKeAPI()/task hujan di node tanpa WiFi: gateway menulis baris ke
// "Hydrosense table" dengan kolom yang sama (water_level+distance_cm, rain_mm_*).
static void kirimBarisLevelHujan() {
    static unsigned long levelBerikut = 0, hujanBerikut = 0;
    static uint16_t jamTerakhir = 0xFFFE, hariTerakhir = 0xFFFE;
    const char* st;
    int rssi;
    long umur;
    gwLoraLaporan(&st, &rssi, &umur);
    if (!stPenuhAda || umur < 0 || umur > 90) return; // hanya selagi ping node masih segar

    if (stTerakhir.levelPctX10 != 0xFFFF && stTerakhir.distanceX10 != 0xFFFF && millis() >= levelBerikut) {
        levelBerikut = millis() + 60000UL;
        char body[120];
        snprintf(body, sizeof(body), "{\"water_level\":%.1f,\"distance_cm\":%.1f}",
                 stTerakhir.levelPctX10 / 10.0f, stTerakhir.distanceX10 / 10.0f);
        int code = supabase_rest("POST", "/rest/v1/Hydrosense%20table", body);
        Serial.printf("[NET] level air dari node -> HTTP %d\n", code);
    }
    if (stTerakhir.rainJamX10 != 0xFFFF && stTerakhir.rainHariX10 != 0xFFFF) {
        bool berubah = stTerakhir.rainJamX10 != jamTerakhir || stTerakhir.rainHariX10 != hariTerakhir;
        // Sama seperti node: tiap menit selagi hujan, tiap 5 menit selagi kering / tidak berubah.
        if ((berubah && stTerakhir.rainJamX10 > 0 && millis() >= hujanBerikut - 240000UL) || millis() >= hujanBerikut) {
            hujanBerikut = millis() + 300000UL;
            jamTerakhir = stTerakhir.rainJamX10;
            hariTerakhir = stTerakhir.rainHariX10;
            char body[120];
            snprintf(body, sizeof(body), "{\"rain_mm_per_hour\":%.1f,\"rain_mm_today\":%.1f}",
                     stTerakhir.rainJamX10 / 10.0f, stTerakhir.rainHariX10 / 10.0f);
            int code = supabase_rest("POST", "/rest/v1/Hydrosense%20table", body);
            Serial.printf("[NET] hujan dari node -> HTTP %d\n", code);
        }
    }
}

// ---------------- Task_Net ----------------
static OtaStartCb cbStart = nullptr;
static OtaProgressCb cbProgress = nullptr;

static void drainQueueNet() {
    NetItem it;
    while (xQueueReceive(queueNet, &it, 0) == pdTRUE) {
        if (it.tipe == NI_LEVEL) {
            levelTerakhir = it.level;
            floatTerakhir = it.full;
            levelSegarSampai = millis() + RAW_MAX_AGE_MS;
        } else {
            tambahPending(it);
        }
    }
}

static void taskNet(void*) {
    for (;;) {
        network_loop();
        proxyJalankan(); // jembatan HTTP untuk node: layani dulu supaya respons cepat
        lan_update_loop(cbStart, cbProgress);
        gwOtaBerjalan = false; // kembali dari lan_update berarti tidak ada pemasangan yang berjalan
        drainQueueNet();
        if (network_link() != LINK_NONE) {
            kirimUplink();
            ambilPerintah();
            ambilJadwal();
            ambilNotifikasi();
            kirimBarisLevelHujan();
            laporkanStatusLora();
            kirimLevelDanFloat();
            if (ota_due()) {
                gwOtaBerjalan = true;
                ota_run(cbStart, cbProgress);
                gwOtaBerjalan = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void taskNetMulai(OtaStartCb onStart, OtaProgressCb onProgress) {
    cbStart = onStart;
    cbProgress = onProgress;
    xTaskCreatePinnedToCore(taskNet, "Task_Net", 16384, nullptr, 2, nullptr, 0);
}
