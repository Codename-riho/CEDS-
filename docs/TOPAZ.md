# CEDS — Varian Khusus Topaz (Redmi Note 12 4G)

## 1. Ringkasan

Varian ini adalah [varian antar kernel](GENERIC.md) ditambah hal-hal yang hanya masuk akal untuk satu perangkat:

- profil nilai yang disarankan sebagai titik awal,
- cara **mengukur** waktu boot topaz untuk memilih `DS_DELAY_SEC`,
- cara melacak penimpa sysctl di sisi vendor,
- protokol uji baterai A/B, checklist rilis, dan prosedur rollback,
- tabel data perangkat yang diisi maintainer.

> Nilai pada dokumen ini adalah **titik awal yang belum diuji di topaz**. Jangan menganggapnya hasil pengukuran. Isi tabel di [bagian 8](#8-data-perangkat-dan-hasil-uji) dengan hasil ujimu sendiri.

## 2. Prasyarat dan keselamatan

- Bootloader sudah terbuka, `adb` dan `fastboot` berfungsi di komputermu.
- **Simpan boot image stock** sebelum memasang kernel apa pun. Tanpanya, rollback sulit.
- Akses root atau `adb root` memudahkan membaca `dmesg` dan `/proc/sys/vm/*`. Tanpa root, sebagian perintah di bawah akan ditolak.
- Kernel dibangun dengan sumber, defconfig, dan toolchain yang sama dengan yang kamu pakai sehari-hari. CEDS tidak mengubah konfigurasi kernel.

## 3. Profil nilai

| Profil | `DS_STAT_SEC` | `DS_DIRTY_WB_CS` | Jendela data kotor\* | Catatan |
|---|---|---|---|---|
| Hati-hati | 2 | 1000 | sekitar 40 dtk | perubahan kecil dari upstream |
| **Bawaan CEDS** | **5** | **1500** | **sekitar 45 dtk** | **mulai dari sini** |
| Agresif | 10 | 3000 | sekitar 60 dtk | statistik VM bisa tertinggal sampai 10 dtk. Uji perilaku `lmkd` dengan ketat |

\* Perkiraan waktu terlama data kotor menunggu sebelum mulai ditulis: `dirty_expire_centisecs` (30 dtk bila tidak diubah) + interval writeback. `fsync`/`sync` tidak terpengaruh.

Sebelum membandingkan, catat nilai bawaan ROM-mu:

```sh
adb shell cat /proc/sys/vm/stat_interval \
              /proc/sys/vm/dirty_writeback_centisecs \
              /proc/sys/vm/dirty_expire_centisecs
```

## 4. Menentukan `DS_DELAY_SEC` dan melacak penimpa

### 4.1 Ukur waktu boot_completed

Pass pertama sebaiknya jatuh **setelah** layanan boot selesai menulis sysctl.

```sh
adb wait-for-device
until [ "$(adb shell getprop sys.boot_completed | tr -d '\r')" = "1" ]; do sleep 1; done
adb shell cat /proc/uptime        # angka pertama = detik sejak boot
```

Alternatif: `adb logcat -b events -d | grep boot_progress_ams_ready`.

Aturan awal: `DS_DELAY_SEC` ≥ uptime saat `boot_completed` + 30 dtk. Ukur beberapa kali (boot dingin dan reboot) dan pakai yang terbesar. Bawaan 90 dtk hanya benar jika boot-mu selesai sebelum itu.

### 4.2 Cari siapa yang menulis sysctl

Di perangkat Qualcomm, penulis umumnya skrip/rc vendor atau PowerHAL. Nama berkas berbeda tiap ROM, jadi cari, jangan menebak:

```sh
adb shell su -c 'grep -rn "stat_interval\|dirty_writeback" \
  /vendor/etc/init /system/etc/init /vendor/bin /system/bin 2>/dev/null'
adb shell su -c 'ls /data/adb/modules' 2>/dev/null
```

Cara membaca hasil `dmesg | grep cryo_deepsleep`:

| Pola log | Kesimpulan |
|---|---|
| Semua pass `ok` | tidak ada penimpa dalam jadwal. Selesai |
| Pass #1 `A -> B`, pass berikutnya `ok` | penimpa selesai sebelum pass #1 lalu berhenti. Naikkan `DS_DELAY_SEC` supaya pass #1 sudah di belakangnya |
| `A -> B` di setiap pass | ada penulis berulang. Temukan lewat 4.2, nonaktifkan, atau ubah nilainya di sumbernya |
| Tidak ada log | kernel/CEDS tidak aktif. Lihat [pemecahan masalah](GENERIC.md#7-pemecahan-masalah) |

## 5. Memasang dan verifikasi

Pakai step yang sama: [`ci/ceds-inject-step.yml`](../ci/ceds-inject-step.yml). Isi `env` dengan hasil pengukuranmu:

```yaml
env:
  DS_DELAY_SEC: 90        # ganti: uptime boot_completed topaz + 30
  DS_PASSES: 3
  DS_STAT_SEC: 5
  DS_DIRTY_WB_CS: 1500
```

Agar step tidak tak sengaja berjalan untuk perangkat lain, tambahkan penjaga pada `if:` (contoh, dengan asumsi workflow-mu punya env `DEVICE_CODENAME`):

```yaml
if: github.event.inputs.deepsleep_tuning == 'Enabled' && env.DEVICE_CODENAME == 'topaz'
```

Verifikasi setelah flash:

```sh
adb shell getprop ro.product.device           # pastikan perangkat benar
adb shell su -c 'dmesg | grep cryo_deepsleep'
adb shell cat /proc/sys/vm/stat_interval      # = DS_STAT_SEC
adb shell cat /proc/sys/vm/dirty_writeback_centisecs   # = DS_DIRTY_WB_CS
```

Tunggu minimal 15 menit setelah `boot_completed`, lalu baca nilai sekali lagi untuk memastikan tidak ada penimpa yang telat.

## 6. Protokol uji baterai (A/B)

Tujuan: memastikan ada perbedaan nyata, bukan kebetulan satu malam.

1. **Dua build dari commit yang sama.** Satu dengan `deepsleep_tuning = Enabled` (B), satu `Disabled` (A).
2. **Kondisi sama tiap uji**: tidak diisi daya, layar mati, jaringan yang sama (misal Wi-Fi saja), aplikasi dan sinkronisasi yang sama, perangkat tidak disentuh, durasi minimal 6 jam.
3. **Catat** sebelum dan sesudah:
   ```sh
   adb shell dumpsys batterystats --reset
   adb shell dumpsys battery | grep level      # awal
   # ... biarkan semalaman ...
   adb shell dumpsys battery | grep level      # akhir
   ```
   Hitung persen turun per jam.
4. **Selang-seling** A, B, A, B, A, B (minimal 3 uji per build) untuk meredam variasi harian.
5. **Bandingkan rata-rata.** Jika selisihnya lebih kecil dari sebaran antar-malam pada build yang sama, **tidak ada bukti** perbedaan. Jangan menulis klaim hemat di rilis berdasarkan satu malam.

## 7. Checklist rilis dan rollback

- [ ] Boot normal, tidak bootloop
- [ ] `dmesg` memuat log `cryo_deepsleep` #1 sampai #N sesuai `DS_PASSES`
- [ ] Nilai sysctl benar saat `boot_completed` + 15 menit
- [ ] `echo 1 > /sys/module/cryo_deepsleep/parameters/apply` tidak memicu baris `A -> B` berulang
- [ ] Pemakaian harian 24 jam: kamera, video, game ringan, multitasking. Tidak ada penutupan aplikasi yang tidak biasa
- [ ] Mati paksa lalu hidup lagi: data aplikasi uji tidak rusak
- [ ] Rollback ke boot image stock berhasil

Rollback (nama partisi dan slot mengikuti cara kamu memasang kernel, jangan lanjut sebelum punya boot image stock cadangan):

```sh
adb reboot bootloader
fastboot flash boot stock_boot.img
fastboot reboot
```

## 8. Data perangkat dan hasil uji

Isi oleh maintainer. Perintah di kolom kanan.

| Item | Nilai | Cara mendapatkan |
|---|---|---|
| Kernel | _isi_ | `adb shell uname -r` |
| Sumber / branch kernel | _isi_ | repo yang kamu pakai |
| Defconfig | _isi_ | file di `arch/arm64/configs/` |
| Toolchain | _isi_ | `clang --version` di workflow |
| `CONFIG_HZ` | _isi_ | `adb shell su -c 'zcat /proc/config.gz \| grep CONFIG_HZ'` |
| Mode SELinux | _isi_ | `adb shell getenforce` |
| Nilai bawaan ROM (`stat_interval` / `dirty_writeback_centisecs` / `dirty_expire_centisecs`) | _isi_ | perintah di bagian 3 |
| Uptime saat `boot_completed` (dtk) | _isi_ | bagian 4.1 |
| Penimpa ditemukan | _isi_ | bagian 4.2 |
| `DS_DELAY_SEC` terpilih | _isi_ | bagian 4.1 |
| Profil dipakai | _isi_ | bagian 3 |
| Hasil baterai (A vs B, %/jam, jumlah uji) | _isi_ | bagian 6 |
| Tanggal dan build yang diuji | _isi_ | |

## 9. LKM di topaz

Kalau kamu tidak ingin membangun ulang kernel, atau ingin nilai asli bisa dikembalikan, pakai [LKM](LKM.md). Di topaz, periksa dulu apakah kernel-mu mendukung modul:

```sh
adb shell su -c 'zcat /proc/config.gz | grep -E "CONFIG_MODULES|CONFIG_MODULE_UNLOAD|CONFIG_MODVERSIONS|CONFIG_MODULE_SIG"'
```

Contoh pemuatan setelah `boot_completed` (jeda singkat sudah cukup karena modul dimuat setelah layanan boot selesai):

```sh
insmod ceds_lkm.ko delay_sec=10 passes=2 stat_sec=5 wb_cs=1500
```

Gunakan **salah satu** varian pada satu kernel. Keduanya aman berdampingan (nilai targetnya sama), tetapi berlebihan, dan `rmmod` hanya akan mengembalikan ke nilai yang ditulis varian built-in.
