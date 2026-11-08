#pragma once
// Protokol paket LoRa P2P HydroSense <-> Gateway Groundtank (flowchart HS-04).
// File ini IDENTIK di kedua proyek (node & gateway) -- ubah di satu tempat,
// salin ke yang lain.
//
// Format byte (little-endian):
//   magic(1) | tipe(1) | node_id(1) | seq(2) | len(1) | payload(len) | crc16(2)
// CRC16-CCITT (poly 0x1021, init 0xFFFF) atas semua byte sebelum CRC.
// ACK/NAK: payload = seq paket yang dikonfirmasi (2 byte).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LORA_MAGIC 0xA5
#define LORA_HDR_LEN 6
#define LORA_MAX_PAYLOAD 48
#define LORA_MAX_PACKET (LORA_HDR_LEN + LORA_MAX_PAYLOAD + 2)

enum LoraTipe : uint8_t {
  LORA_DATA = 1,  // node -> gateway: pH, TSS, level, status
  LORA_ALARM = 2, // node -> gateway: proteksi pompa / selang
  LORA_CMD = 3,   // gateway -> node: perintah manual
  LORA_ACK = 4,
  LORA_NAK = 5,   // perintah ditolak interlock
  LORA_EVENT = 6, // node -> gateway: mis. watchdog TTL habis
  LORA_TEXT = 7,  // uji komunikasi: payload = teks ASCII (<= LORA_MAX_PAYLOAD byte)
  LORA_CFG = 8,   // gateway -> node: jadwal siklus otomatis (LoraJadwalPayload); dibalas ACK
  LORA_NOTIFY = 9,    // gateway -> node: ada perintah baru di dashboard (LoraNotifPayload)
  LORA_HTTP_REQ = 10, // node -> gateway: potongan permintaan HTTP ke Supabase (jembatan WiFi-less)
  LORA_HTTP_RES = 11, // gateway -> node: potongan respons HTTP
};

// Payload DATA/ALARM (packed, 17 byte). Nilai tak valid = INT16_MIN / 0xFFFF.
struct __attribute__((packed)) LoraDataPayload {
  int16_t ph_x100;     // pH x100
  uint16_t tss_x10;    // TSS mg/L x10
  int16_t suhu_x10;    // suhu C x10
  uint16_t level_x10;  // tinggi air cm x10
  uint8_t status;      // bit0 pompa, bit1 valve, bit2 float, bit3 error sensor
  uint8_t alarm;       // 0 = tidak ada, 1 = selang/pompa
};

// Payload CMD (packed, 8 byte).
struct __attribute__((packed)) LoraCmdPayload {
  uint16_t cmd_id;
  uint8_t aksi;       // 1 pompa, 2 valve, 3 wiper, 4 kembali otomatis
  uint8_t nilai;      // 0/1
  uint16_t ttl_s;     // masa berlaku sejak diterima node
  uint16_t reserved;
};

// Payload CFG: jadwal siklus otomatis dari dashboard (satuan sama dengan kolom devices).
// seq paket = id konfigurasi; ACK membawa id itu (sama seperti CMD).
struct __attribute__((packed)) LoraJadwalPayload {
  uint32_t intervalDetik;   // jeda antar awal siklus (10..86400)
  uint16_t pompaDetik;      // batas waktu pompa nyala (1..7200)
  uint16_t valveDetik;      // batas waktu valve buka (1..7200)
  uint16_t bacaSensorDetik; // interval baca+kirim pH/TSS (5..3600)
  uint16_t stabilDetik;     // tunggu stabil sebelum baca (1..600)
  uint16_t sampelMs;        // jeda antar sampel (200..10000)
  uint8_t trimPersen;       // % data dibuang tiap sisi (0..40)
  uint8_t reserved;
};
static_assert(sizeof(LoraJadwalPayload) == 16, "paket CFG harus 16 byte (node & gateway sama)");

// ---- Jembatan HTTP lewat LoRa ----
// Node tanpa WiFi mengirim permintaan HTTP ke Supabase lewat gateway. Pesan (permintaan atau
// respons) dipotong menjadi fragmen; tiap fragmen = satu paket ber-ACK. Payload fragmen:
//   [txId u8][idx u8][total u8][data ...]  (LORA_FRAG_DATA byte data per fragmen)
// Permintaan (gabungan semua data): [metode u8][panjangPath u16 LE][path][body]
// Respons: fragmen 0 diawali [kodeHttp u16 LE], lalu badan respons.
#define LORA_FRAG_HDR 3
#define LORA_FRAG_DATA (LORA_MAX_PAYLOAD - LORA_FRAG_HDR)
#define LORA_PROXY_MAKS_REQ 640
#define LORA_PROXY_MAKS_RES 520
enum LoraProxyMetode : uint8_t { PROXY_GET = 1, PROXY_POST = 2, PROXY_PATCH = 3 };

// Notifikasi perintah baru di dashboard (gateway -> node). Node lalu mengambil barisnya
// lewat jembatan HTTP dan menjalankan penangan yang sama seperti saat memakai WiFi.
enum LoraNotifJenis : uint8_t {
  NOTIF_MODBUS = 1,     // device_commands type=modbus
  NOTIF_PH_CAL = 2,     // device_commands type=ph_cal
  NOTIF_TSS_HW = 3,     // device_commands type=tss_hw
  NOTIF_TSS_FAKTOR = 4, // faktor kalibrasi TSS (tabel calibration) berubah
};
struct __attribute__((packed)) LoraNotifPayload {
  uint8_t jenis; // LoraNotifJenis
  uint32_t id;   // id baris perintah (atau id faktor terbaru)
};

// Status periodik node (EVENT kode 3): kondisi alat + data pengganti heartbeat/level/hujan,
// diteruskan gateway ke Supabase. Nilai tak ada = 0xFFFF (u16) / -1 (nextCycleS).
struct __attribute__((packed)) LoraStatusPayload {
  uint8_t kode;          // selalu 3
  uint8_t status;        // bit0 pompa nyala, bit1 valve buka, bit2 float HIGH
  uint8_t mode;          // 0 manual, 1 otomatis
  uint8_t fase;          // 0 manual, 1 menunggu, 2 pompa mengisi, 3 menunggu stabil, 4 membaca sensor, 5 valve membuang
  int32_t nextCycleS;    // detik ke siklus berikutnya, -1 = tidak ada
  uint16_t tssRawX100;   // TSS mentah x100
  uint16_t levelPctX10;  // level air % x10
  uint16_t distanceX10;  // jarak sensor cm x10
  uint16_t rainJamX10;   // hujan mm/jam x10
  uint16_t rainHariX10;  // hujan hari ini mm x10
};
static_assert(sizeof(LoraStatusPayload) == 18, "status LoRa harus 18 byte");

inline uint16_t loraCrc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

struct LoraPaket {
  uint8_t tipe;
  uint8_t nodeId;
  uint16_t seq;
  uint8_t len;
  uint8_t payload[LORA_MAX_PAYLOAD];
};

// Susun paket ke buf (>= LORA_MAX_PACKET). Return panjang, 0 jika payload kebesaran.
inline size_t loraEncode(const LoraPaket &p, uint8_t *buf) {
  if (p.len > LORA_MAX_PAYLOAD)
    return 0;
  buf[0] = LORA_MAGIC;
  buf[1] = p.tipe;
  buf[2] = p.nodeId;
  buf[3] = (uint8_t)(p.seq & 0xFF);
  buf[4] = (uint8_t)(p.seq >> 8);
  buf[5] = p.len;
  memcpy(buf + LORA_HDR_LEN, p.payload, p.len);
  uint16_t crc = loraCrc16(buf, LORA_HDR_LEN + p.len);
  buf[LORA_HDR_LEN + p.len] = (uint8_t)(crc & 0xFF);
  buf[LORA_HDR_LEN + p.len + 1] = (uint8_t)(crc >> 8);
  return LORA_HDR_LEN + p.len + 2;
}

// Urai paket; false bila magic/panjang/CRC salah.
inline bool loraDecode(const uint8_t *buf, size_t n, LoraPaket &p) {
  if (n < LORA_HDR_LEN + 2 || buf[0] != LORA_MAGIC)
    return false;
  uint8_t len = buf[5];
  if (len > LORA_MAX_PAYLOAD || n != (size_t)(LORA_HDR_LEN + len + 2))
    return false;
  uint16_t crc = (uint16_t)buf[LORA_HDR_LEN + len] |
                 ((uint16_t)buf[LORA_HDR_LEN + len + 1] << 8);
  if (crc != loraCrc16(buf, LORA_HDR_LEN + len))
    return false;
  p.tipe = buf[1];
  p.nodeId = buf[2];
  p.seq = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
  p.len = len;
  memcpy(p.payload, buf + LORA_HDR_LEN, len);
  return true;
}

// Paket ACK/NAK untuk seq tertentu.
inline LoraPaket loraBuatAck(uint8_t tipe, uint8_t nodeId, uint16_t seq) {
  LoraPaket p = {};
  p.tipe = tipe;
  p.nodeId = nodeId;
  p.seq = seq;
  p.len = 2;
  p.payload[0] = (uint8_t)(seq & 0xFF);
  p.payload[1] = (uint8_t)(seq >> 8);
  return p;
}
