// Isi dua nilai di bawah ini dengan data dari project Supabase kamu
// (buka project -> Project Settings -> API).
//
// - url     : "Project URL", contoh "https://abcdefgh.supabase.co"
// - anonKey : "anon" / "publishable" key (BUKAN service_role key -- itu rahasia,
//             jangan pernah ditaruh di file yang dibuka browser)
// - table   : nama tabel tempat ESP32 menulis data pembacaan.
//             Skema tabelnya ada di file supabase/schema.sql, tinggal dijalankan
//             sekali di Supabase SQL Editor.
//
// Selama url/anonKey masih kosong, dashboard otomatis berjalan dalam mode
// simulasi seperti sebelumnya -- tidak akan error.
window.SUPABASE_CONFIG = {
  url: "https://pyeydpwuqqchwamczyno.supabase.co",
  anonKey: "sb_publishable_MHNd3JDAF-OMnhgUv_anXw_XyZnSIMU",
  table: "tank_readings",
  // Data dianggap "hidup" (ESP32 terhubung) kalau baris terakhir lebih baru
  // dari sekian detik ini. Sesuaikan dengan interval kirim data firmware.
  staleAfterSec: 20
};
