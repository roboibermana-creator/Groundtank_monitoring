-- Project baru "CITE DB IOT" -- bagian BERSAMA + GROUNDTANK.
-- Tempel seluruh file ini di Supabase SQL Editor, lalu Run. Aman diulang.
-- Tabel Hydrosense dibuat terpisah (file lain), proyek berikutnya menyusul.
-- Konvensi nama (semua di schema public): iot_ = bersama, gt_ = Groundtank,
-- hs_ = Hydrosense. Proyek berikutnya: beri awalan sendiri (mis. ag_).

-- ============================================================
-- 1. DATA PEMBACAAN TANGKI (ditulis ESP32, dibaca APK + web)
-- ============================================================
create table if not exists public.gt_tank_readings (
  id            bigint generated always as identity primary key,
  created_at    timestamptz not null default now(),
  device_id     text not null default 'esp32-01',
  distance_cm   numeric check (distance_cm >= 0),
  level_percent numeric check (level_percent between 0 and 100),
  float_switch  boolean,                -- true = float switch LSH-01 tersentuh (penuh)
  status        text                    -- NORMAL / HIGH / HIGH-HIGH / LOW / LOW-LOW / PENUH
);

create index if not exists gt_tank_readings_device_time_idx
  on public.gt_tank_readings (device_id, created_at desc);

-- Baris terbaru tiap perangkat (untuk kartu "level sekarang")
create or replace view public.gt_tank_latest
  with (security_invoker = true) as
  select distinct on (device_id) *
  from public.gt_tank_readings
  order by device_id, created_at desc;

-- ============================================================
-- 2. STATUS PERANGKAT (versi firmware, terakhir online)
--    ESP32 mengisi ini tiap boot / berkala.
-- ============================================================
create table if not exists public.iot_devices (
  device_id    text primary key,
  project      text not null,           -- 'groundtank', 'hydrosense', dst.
  fw_version   text,
  link         text check (link in ('LAN', 'WIFI')),
  ip           text,
  last_seen    timestamptz not null default now(),
  ota_status   text,                    -- mis. 'ok', 'mengunduh', 'gagal: sha256'
  created_at   timestamptz not null default now()
);

-- last_seen diisi server tiap ESP32 melapor (insert maupun update)
create or replace function public.iot_touch_last_seen()
returns trigger language plpgsql as $$
begin
  new.last_seen = now();
  return new;
end;
$$;

drop trigger if exists trg_iot_devices_last_seen on public.iot_devices;
create trigger trg_iot_devices_last_seen
  before insert or update on public.iot_devices
  for each row execute function public.iot_touch_last_seen();

-- ============================================================
-- 3. KATALOG FIRMWARE (untuk OTA)
--    ESP32 membaca rilis aktif terbaru, membandingkan versi,
--    lalu mengunduh file .bin dari url dan memeriksa sha256.
-- ============================================================
create table if not exists public.iot_firmware_releases (
  id          bigint generated always as identity primary key,
  created_at  timestamptz not null default now(),
  device_type text not null default 'groundtank',
  version     text not null,            -- mis. '1.0.3'
  url         text not null,            -- alamat publik file .bin
  sha256      text not null check (length(sha256) = 64),
  size_bytes  integer check (size_bytes > 0),
  is_active   boolean not null default true,
  notes       text,
  unique (device_type, version)
);

create index if not exists iot_firmware_releases_lookup_idx
  on public.iot_firmware_releases (device_type, is_active, created_at desc);

-- Bucket penyimpanan file .bin (publik untuk diunduh; unggah hanya dari dashboard Anda)
insert into storage.buckets (id, name, public)
values ('firmware', 'firmware', true)
on conflict (id) do nothing;

-- ============================================================
-- 4. KEAMANAN (RLS)
-- ============================================================
alter table public.gt_tank_readings     enable row level security;
alter table public.iot_devices       enable row level security;
alter table public.iot_firmware_releases enable row level security;

-- gt_tank_readings: siapa pun dengan kunci publishable boleh baca & tambah (sama seperti sekarang)
drop policy if exists gt_tank_readings_read   on public.gt_tank_readings;
drop policy if exists gt_tank_readings_insert on public.gt_tank_readings;
create policy gt_tank_readings_read   on public.gt_tank_readings for select using (true);
create policy gt_tank_readings_insert on public.gt_tank_readings for insert
  with check (device_id is not null);

-- iot_devices: baca, tambah, dan perbarui status sendiri
drop policy if exists iot_devices_read   on public.iot_devices;
drop policy if exists iot_devices_insert on public.iot_devices;
drop policy if exists iot_devices_update on public.iot_devices;
create policy iot_devices_read   on public.iot_devices for select using (true);
create policy iot_devices_insert on public.iot_devices for insert with check (true);
create policy iot_devices_update on public.iot_devices for update using (true) with check (true);

-- iot_firmware_releases: HANYA baca. Menambah rilis lewat SQL Editor / dashboard (role Anda).
drop policy if exists iot_firmware_releases_read on public.iot_firmware_releases;
create policy iot_firmware_releases_read on public.iot_firmware_releases
  for select using (is_active);

-- storage: siapa pun boleh mengunduh dari bucket firmware; tidak ada policy tulis
-- untuk anon, jadi hanya Anda yang bisa mengunggah lewat dashboard.
drop policy if exists firmware_public_read on storage.objects;
create policy firmware_public_read on storage.objects
  for select using (bucket_id = 'firmware');

-- ============================================================
-- CONTOH: cara mendaftarkan rilis OTA setelah mengunggah .bin
-- (hitung sha256 di PC:  certutil -hashfile firmware.bin SHA256)
-- ============================================================
-- insert into public.iot_firmware_releases (version, url, sha256, size_bytes, notes)
-- values ('1.0.1',
--   'https://<ref-project-baru>.supabase.co/storage/v1/object/public/firmware/groundtank/1.0.1.bin',
--   '<64 karakter hex>', 1048576, 'ganti database ke Hydrosense');
