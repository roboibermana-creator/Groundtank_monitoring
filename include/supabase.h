#ifndef SUPABASE_H
#define SUPABASE_H

#include <stddef.h>
#include <stdint.h>

// Sinkron waktu via NTP (dibutuhkan untuk validasi sertifikat TLS). Panggil setelah LAN dapat IP.
bool supabase_sync_time();

// Kirim satu pembacaan (nilai string) untuk sensorId lewat RPC submit_reading.
// Return kode HTTP (204 = tersimpan), atau 0 bila gagal koneksi/timeout.
int supabase_send(const char* sensorId, const char* value);

// Jarak mentah ultrasonik (cm), satu desimal.
int supabase_send(float distanceCm);

// Float switch: "1" = penuh, "0" = normal. Perlu FLOAT_SENSOR_UUID di secrets.h.
int supabase_send_float(bool full);

// REST generik ke Supabase (GT-05). method "GET"/"POST"/"PATCH"; path mis.
// "/rest/v1/device_commands?select=id". body boleh nullptr. Badan respons disalin ke resp
// (dipotong ke respLen-1). Return kode HTTP, atau 0 bila gagal koneksi/timeout.
// HANYA boleh dipanggil dari Task_Net (aturan GT-04: satu-satunya pemakai TLS).
int supabase_rest(const char* method, const char* path, const char* body,
                  char* resp = nullptr, size_t respLen = 0);

// Buka koneksi TLS (terverifikasi) ke host lewat jalur yang aktif (LAN/WiFi).
// Return nullptr bila gagal. Panggil stop() pada klien setelah selesai.
class Client;
Client* supabase_tls_connect(const char* host, uint16_t port = 443);

#endif // SUPABASE_H
