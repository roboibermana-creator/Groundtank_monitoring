#pragma once
// Driver ringan modul EBYTE E220-900T30D (UART, chip LLCC68) untuk paket LoraProto.h.
// File ini IDENTIK di node & gateway -- ubah di satu tempat, salin ke yang lain.
//
// Ringkasan modul (dari manual EBYTE; VERIFIKASI bit-bit register di manual sebelum
// dipercaya -- firmware mencetak konfigurasi hasil baca-ulang agar bisa dicocokkan):
//  - Mode (M1 M0): 00 = normal (kirim/terima), 11 = konfigurasi (UART 9600 8N1 tetap).
//  - Konfigurasi: tulis "C0 00 08" + 8 byte [ADDH ADDL REG0 REG1 REG2 REG3 CRYPTH CRYPTL]
//    (disimpan permanen); baca "C1 00 08". E220 TIDAK punya NETID (itu seri E22).
//    Bawaan pabrik terbaca 00 00 62 00 12 03 00 00 (kanal 0x12 = 868,125 MHz).
//  - REG0: bit7-5 baud (011 = 9600), bit4-3 paritas (00 = 8N1), bit2-0 air rate
//    (100 = 9,6 kbps). REG1: bit1-0 daya (T30D: 00=30, 01=27, 10=24, 11=21 dBm).
//    REG2: kanal, frekuensi = 850,125 + kanal MHz. REG3: bit7 = byte RSSI ditambahkan
//    di akhir tiap paket terima, bit6 = mode fixed-point (paket diawali ADDH ADDL KANAL).
//  - AUX: LOW = sibuk (kirim / inisialisasi), HIGH = siap.
//  - RSSI (dBm) = -(256 - byte).
//
// Mode transparan/fixed tidak memberi batas paket, jadi penerima menyusun ulang paket
// dari aliran byte memakai magic + len + CRC16 milik LoraProto.h.
#include <Arduino.h>
#include "LoraProto.h"

struct E220Config {
  uint16_t addr;      // alamat modul ini
  uint8_t channel;    // 0..80 -> 850,125 + channel MHz
  uint8_t airRateCode; // 0..7 (4 = 9,6 kbps)
  uint8_t powerCode;   // 0..3 (T30D: 0=30, 1=27, 2=24, 3=21 dBm)
  bool fixedPoint;
  bool rssiByte;
};

class LoraE220 {
public:
  // Konfigurasi modul (bila beda dari yang tersimpan) lalu masuk mode normal.
  bool begin(HardwareSerial &ser, int rx, int tx, int m0, int m1, int aux,
             const E220Config &cfg, uint32_t airRateBps) {
    ser_ = &ser;
    m0_ = m0;
    m1_ = m1;
    aux_ = aux;
    airBps_ = airRateBps;
    rssiByte_ = cfg.rssiByte;
    pinMode(m0_, OUTPUT);
    pinMode(m1_, OUTPUT);
    if (aux_ >= 0)
      pinMode(aux_, INPUT);
    ser_->setRxBufferSize(512); // harus SEBELUM begin()
    ser_->begin(9600, SERIAL_8N1, rx, tx);

    mode(true);
    uint8_t want[8], have[8];
    buatRegister(cfg, want);
    bool ok = bacaRegister(have);
    if (!ok) {
      Serial.println(F("[E220] Tidak ada jawaban saat baca konfigurasi (cek kabel TX/RX, M0/M1, catu 5 V)"));
    } else if (!cocok(want, have)) {
      cetakHex("[E220] Konfigurasi modul sekarang :", have);
      cetakHex("[E220] Konfigurasi yang diminta   :", want);
      if (!tulisRegister(want)) {
        // Modul menolak (FF FF FF = format/nilai ditolak). Cari register penyebabnya.
        Serial.println(F("[E220] Penulisan penuh ditolak; uji bertahap per register..."));
        uint8_t cur[8];
        memcpy(cur, have, 8);
        if (!tulisRegister(cur)) {
          Serial.println(F("[E220] Menulis konfigurasi yang SAMA pun ditolak: format perintah tulis tidak diterima modul"));
        } else {
          Serial.println(F("[E220] Menulis konfigurasi yang sama DITERIMA; menambah perubahan satu per satu:"));
          static const char *nama[8] = {"ADDH", "ADDL", "REG0", "REG1", "REG2(kanal)", "REG3", "CRYPT_H", "CRYPT_L"};
          for (int k = 0; k < 6; k++) {
            if (want[k] == cur[k])
              continue;
            uint8_t uji[8];
            memcpy(uji, cur, 8);
            uji[k] = want[k];
            if (tulisRegister(uji)) {
              cur[k] = want[k];
              Serial.printf("[E220]   %s = %02X diterima\n", nama[k], want[k]);
            } else {
              Serial.printf("[E220]   %s = %02X DITOLAK\n", nama[k], want[k]);
            }
          }
        }
      }
      // Keberhasilan ditentukan dari hasil baca ulang. Beri waktu modul menerapkan
      // konfigurasi, baca ulang sampai 3 kali.
      ok = false;
      for (int i = 0; i < 3 && !ok; i++) {
        delay(300);
        ok = bacaRegister(have) && cocok(want, have);
      }
      cetakHex("[E220] Hasil baca ulang           :", have);
      if (!ok)
        Serial.println(F("[E220] Konfigurasi belum berubah setelah ditulis; bandingkan tiga baris hex di atas dengan manual E220"));
    }
    if (ok) {
      Serial.printf("[E220] Konfigurasi: addr=%02X%02X REG0=%02X REG1=%02X kanal=%u (%.3f MHz) REG3=%02X\n",
                    have[0], have[1], have[2], have[3], have[4],
                    850.125 + have[4], have[5]);
    }
    mode(false);
    return ok;
  }

  // Kirim satu paket ke alamat/kanal tujuan (mode fixed-point). true = sudah diserahkan ke modul.
  bool kirim(uint16_t dest, uint8_t channel, const LoraPaket &p) {
    uint8_t buf[3 + LORA_MAX_PACKET];
    size_t n = loraEncode(p, buf + 3);
    if (n == 0)
      return false;
    buf[0] = dest >> 8;
    buf[1] = dest & 0xFF;
    buf[2] = channel;
    if (!tungguAux(2000))
      return false;
    ser_->write(buf, n + 3);
    ser_->flush();
    return true;
  }

  // Tidak memblok. true bila satu paket valid (CRC cocok) selesai disusun.
  bool baca(LoraPaket &p, int16_t *rssi) {
    while (ser_->available() && nBuf_ < (int)sizeof(buf_)) {
      buf_[nBuf_++] = ser_->read();
      rxByte_++;
      terakhirByteMs_ = millis();
    }
    const int ekstra = rssiByte_ ? 1 : 0;
    while (nBuf_ > 0) {
      int i = 0; // buang byte sebelum magic
      while (i < nBuf_ && buf_[i] != LORA_MAGIC)
        i++;
      if (i > 0)
        buang(i);
      if (nBuf_ < LORA_HDR_LEN)
        break;
      uint8_t len = buf_[5];
      if (len > LORA_MAX_PAYLOAD) {
        buang(1);
        continue;
      }
      int total = LORA_HDR_LEN + len + 2;
      if (nBuf_ < total + ekstra) {
        // paket belum lengkap; buang bila sudah lama menggantung
        if (millis() - terakhirByteMs_ > 1500)
          buang(1);
        else
          break;
        continue;
      }
      if (loraDecode(buf_, total, p)) {
        rxValid_++;
        if (rssi)
          *rssi = ekstra ? (int16_t)(-(256 - (int)buf_[total])) : 0;
        buang(total + ekstra);
        return true;
      }
      rxRusak_++;
      buang(1); // CRC salah: mungkin magic palsu, geser satu byte
    }
    return false;
  }

  // Baca konfigurasi modul saat ini (masuk mode konfigurasi sebentar, lalu kembali ke normal).
  // Pemanggil harus memegang mutex radio; selama ini paket yang masuk tidak terbaca.
  bool bacaKonfigurasi(uint8_t *out) {
    mode(true);
    bool ok = bacaRegister(out);
    mode(false);
    return ok;
  }

  // Penghitung diagnosa: byte mentah yang masuk dari modul, paket valid, paket rusak (CRC salah).
  uint32_t rxByte() const { return rxByte_; }
  uint32_t rxValid() const { return rxValid_; }
  uint32_t rxRusak() const { return rxRusak_; }

  // Level pin AUX: 1 = HIGH (siap), 0 = LOW (sibuk), -1 = pin tidak dipakai.
  int auxLevel() const { return aux_ >= 0 ? digitalRead(aux_) : -1; }

  // Perkiraan waktu di udara (ms): n byte data + header modul, pada air rate nominal.
  // Hanya untuk menghitung timeout; angka pastinya tergantung parameter LoRa internal modul.
  unsigned long perkiraanToaMs(size_t n) const {
    return (unsigned long)(((n + 3 + 8) * 8UL * 1000UL) / airBps_) + 60UL;
  }

private:
  HardwareSerial *ser_ = nullptr;
  int m0_ = -1, m1_ = -1, aux_ = -1;
  uint32_t airBps_ = 9600;
  bool rssiByte_ = false;
  uint8_t buf_[256];
  int nBuf_ = 0;
  unsigned long terakhirByteMs_ = 0;
  uint32_t rxByte_ = 0, rxValid_ = 0, rxRusak_ = 0;

  void buang(int n) {
    if (n >= nBuf_) {
      nBuf_ = 0;
      return;
    }
    memmove(buf_, buf_ + n, nBuf_ - n);
    nBuf_ -= n;
  }

  bool tungguAux(uint32_t ms) {
    if (aux_ < 0) {
      delay(30);
      return true;
    }
    unsigned long t0 = millis();
    while (digitalRead(aux_) == LOW) {
      if (millis() - t0 > ms)
        return false;
      delay(1);
    }
    return true;
  }

  void mode(bool konfigurasi) {
    digitalWrite(m0_, konfigurasi ? HIGH : LOW);
    digitalWrite(m1_, konfigurasi ? HIGH : LOW);
    delay(50);
    tungguAux(1500);
    delay(30);
    while (ser_->available())
      ser_->read(); // buang sisa
  }

  // Bandingkan hanya register yang penting (bit cadangan & kunci enkripsi diabaikan).
  static bool cocok(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] &&
           (a[3] & 0x03) == (b[3] & 0x03) && a[4] == b[4] &&
           (a[5] & 0xC0) == (b[5] & 0xC0);
  }

  static void cetakHex(const char *label, const uint8_t *r) {
    Serial.print(label);
    for (int i = 0; i < 8; i++)
      Serial.printf(" %02X", r[i]);
    Serial.println();
  }

  static void buatRegister(const E220Config &c, uint8_t *r) {
    r[0] = c.addr >> 8;
    r[1] = c.addr & 0xFF;
    r[2] = (0b011 << 5) | (0b00 << 3) | (c.airRateCode & 0x07); // REG0: 9600 8N1 + air rate
    r[3] = c.powerCode & 0x03;                                  // REG1: paket 200 byte, daya
    r[4] = c.channel;                                           // REG2: kanal
    r[5] = (c.rssiByte ? 0x80 : 0) | (c.fixedPoint ? 0x40 : 0) | 0x03; // REG3, WOR bawaan
    r[6] = 0;
    r[7] = 0;
  }

  // Baca "C1 00 08" -> jawaban "C1 00 08 <8 byte>"
  bool bacaRegister(uint8_t *out) {
    while (ser_->available())
      ser_->read(); // buang sisa byte sebelum perintah baru
    const uint8_t cmd[3] = {0xC1, 0x00, 0x08};
    ser_->write(cmd, 3);
    return terimaJawaban(out);
  }

  bool tulisRegister(const uint8_t *r) {
    while (ser_->available())
      ser_->read(); // buang sisa byte sebelum perintah baru
    tungguAux(1000);
    uint8_t cmd[11] = {0xC0, 0x00, 0x08};
    memcpy(cmd + 3, r, 8);
    ser_->write(cmd, 11);
    uint8_t eko[8];
    bool ok = terimaJawaban(eko);
    tungguAux(2000);
    delay(100);
    return ok;
  }

  bool terimaJawaban(uint8_t *out) {
    uint8_t resp[11];
    int n = 0;
    unsigned long t0 = millis();
    while (n < 11 && millis() - t0 < 1000) {
      if (ser_->available())
        resp[n++] = ser_->read();
      else
        delay(2);
    }
    if (n < 11 || resp[0] != 0xC1 || resp[1] != 0x00 || resp[2] != 0x08) {
      Serial.printf("[E220] Jawaban mentah (%d byte):", n);
      for (int i = 0; i < n; i++)
        Serial.printf(" %02X", resp[i]);
      Serial.println();
      return false;
    }
    memcpy(out, resp + 3, 8);
    return true;
  }
};
