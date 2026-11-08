-- ============================================================
-- OTA untuk CITE DB IOT, menempel pada skema yang sudah ada
-- (devices, device_tokens, submit_reading). Aman dijalankan ulang.
-- ============================================================

-- 1. Versi firmware & hasil OTA terakhir disimpan di devices
alter table public.devices add column if not exists fw_version text;
alter table public.devices add column if not exists ota_status text;
alter table public.devices add column if not exists ota_checked_at timestamptz;

-- 2. Katalog rilis firmware. ESP32 membaca rilis aktif terbaru untuk project-nya
--    (sama dengan devices.project), lalu memasang bila versinya berbeda.
create table if not exists public.firmware_releases (
  id          bigint generated always as identity primary key,
  created_at  timestamptz not null default now(),
  project     text not null,            -- mis. 'GroundTank' (= devices.project)
  version     text not null,            -- mis. '1.0.1'
  url         text not null,            -- alamat publik file .bin (https)
  sha256      text not null check (length(sha256) = 64),
  size_bytes  integer check (size_bytes > 0),
  is_active   boolean not null default true,
  notes       text,
  unique (project, version)
);

create index if not exists firmware_releases_lookup_idx
  on public.firmware_releases (project, is_active, created_at desc);

-- Hanya baca rilis aktif. Menambah rilis lewat SQL Editor / dashboard.
alter table public.firmware_releases enable row level security;
drop policy if exists firmware_releases_read on public.firmware_releases;
create policy firmware_releases_read on public.firmware_releases
  for select to anon, authenticated using (is_active);

-- 3. Laporan OTA dari perangkat, dengan token yang sama seperti submit_reading.
--    Tidak menyentuh devices.status / updated_at (itu urusan submit_reading).
create or replace function public.report_firmware(
  p_device_id  uuid,
  p_token      text,
  p_fw_version text,
  p_ota_status text default null
)
returns void
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hash text;
begin
  select token_hash into v_hash
  from public.device_tokens
  where device_id = p_device_id;

  if v_hash is null or p_token is null
     or extensions.crypt(p_token, v_hash) <> v_hash then
    raise exception 'invalid device token' using errcode = '28000';
  end if;

  update public.devices
     set fw_version     = left(btrim(p_fw_version), 32),
         ota_status     = left(p_ota_status, 200),
         ota_checked_at = now()
   where id = p_device_id;
end;
$$;

revoke all on function public.report_firmware(uuid, text, text, text) from public;
grant execute on function public.report_firmware(uuid, text, text, text)
  to anon, authenticated;

-- 4. Bucket file .bin: publik untuk diunduh, unggah hanya lewat dashboard
insert into storage.buckets (id, name, public)
values ('firmware', 'firmware', true)
on conflict (id) do nothing;

notify pgrst, 'reload schema';

-- ============================================================
-- CARA MERILIS FIRMWARE BARU
-- 1. Naikkan FW_VERSION di include/config.h, build (pio run)
-- 2. Unggah .pio/build/esp32-s3-devkitc-1/firmware.bin ke Storage > firmware,
--    mis. groundtank/1.0.1.bin
-- 3. Hitung sha256 di PC:  certutil -hashfile firmware.bin SHA256
-- 4. Daftarkan:
-- insert into public.firmware_releases (project, version, url, sha256, size_bytes, notes)
-- values ('GroundTank', '1.0.1',
--   'https://vupbropaxsqireeullbm.supabase.co/storage/v1/object/public/firmware/groundtank/1.0.1.bin',
--   '<64 karakter hex>', <ukuran byte>, 'catatan rilis');
-- Rollback: nonaktifkan rilis terbaru (is_active = false); ESP32 memasang
-- rilis aktif sebelumnya karena versinya berbeda.
-- ============================================================
