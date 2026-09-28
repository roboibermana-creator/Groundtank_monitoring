-- Ground Tank SCADA -- skema tabel Supabase untuk data dari ESP32.
-- Jalankan sekali di Supabase SQL Editor (project groundtank_monitoring).

create table if not exists public.tank_readings (
  id bigint generated always as identity primary key,
  created_at timestamptz not null default now(),
  device_id text not null default 'esp32-01',

  -- data mentah dari sensor
  distance_cm numeric,           -- jarak sensor ultrasonik ke muka air (cm)
  float_switch boolean,          -- true = float switch LSH-01 tersentuh (penuh)

  -- hasil olahan (boleh dihitung di ESP32 sebelum dikirim, atau dikosongkan
  -- dan biarkan dashboard yang menghitung dari distance_cm)
  level_percent numeric,         -- 0-100
  volume_l numeric,
  status text,                   -- NORMAL / HIGH / HIGH-HIGH / LOW / LOW-LOW / PENUH
  pump_filling boolean           -- true = mode pengisian aktif
);

create index if not exists tank_readings_created_at_idx
  on public.tank_readings (created_at desc);

alter table public.tank_readings enable row level security;

-- Dashboard boleh membaca semua baris (dipakai kunci anon/publishable di web).
drop policy if exists "tank_readings_public_read" on public.tank_readings;
create policy "tank_readings_public_read"
  on public.tank_readings for select
  using (true);

-- ESP32 boleh menambah baris baru. Untuk purwarupa ini memakai kunci
-- anon/publishable yang sama seperti dashboard. Kalau nanti mau lebih aman,
-- ganti jadi kunci service_role khusus di firmware (jangan dipakai di web)
-- dan hapus kebijakan insert publik ini.
drop policy if exists "tank_readings_public_insert" on public.tank_readings;
create policy "tank_readings_public_insert"
  on public.tank_readings for insert
  with check (true);

-- Contoh baris untuk uji coba dashboard sebelum ESP32 benar-benar mengirim data:
-- insert into public.tank_readings (distance_cm, level_percent, volume_l, status, pump_filling, float_switch)
-- values (250, 42.5, 8500, 'NORMAL', false, false);
