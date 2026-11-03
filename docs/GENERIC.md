# CEDS — Varian Antar Kernel (built-in)

Varian ini bisa dipasang ke kernel Linux/Android mana pun yang memiliki simbol yang dicari. Step workflow mendeteksi sendiri simbol yang ada di tree dan **hanya memasang bagian yang cocok**.

## 1. Prasyarat

| Kebutuhan | Keterangan |
|---|---|
| Sumber kernel | berisi `mm/vmstat.c`, `mm/page-writeback.c`, `mm/Makefile` |
| Kernel | ≥ 3.11 (memakai `system_power_efficient_wq`) |
| Shell di runner | `bash`, GNU `sed`, `grep`, `head` (tersedia di `ubuntu-latest`) |
| Simbol (salah satu cukup) | `sysctl_stat_interval` di `mm/vmstat.c`, `dirty_writeback_interval` di `mm/page-writeback.c` |

Kalau tidak ada satu pun simbol yang cocok, step berakhir dengan peringatan `::warning::` dan **tidak mengubah apa pun** (exit 0).

## 2. Memasang lewat GitHub Actions

### 2.1 Input workflow

Contoh deklarasi input (sesuaikan dengan workflow-mu):

```yaml
on:
  workflow_dispatch:
    inputs:
      deepsleep_tuning:
        description: "CEDS Deep Sleep tuning"
        type: choice
        options: [Disabled, Enabled]
        default: Disabled
```

### 2.2 Tempel step

Salin isi [`ci/ceds-inject-step.yml`](../ci/ceds-inject-step.yml) ke daftar `steps:`, **setelah** kernel di-checkout (dan setelah patch lain yang mungkin menyentuh `mm/`), **sebelum** step build.

Step memakai dua variabel env dari workflow-mu:

| Variabel | Fungsi |
|---|---|
| `KERNEL_LOC_DIR` | folder induk sumber kernel |
| `KERNEL_NAME` | nama folder sumber kernel |

`working-directory` harus menunjuk ke **root** kernel. Kalau tidak, step berhenti dengan error `mm/Makefile tidak ada`.

### 2.3 Parameter

Diatur lewat `env:` di step. Semua divalidasi dan dinormalisasi (`090` menjadi `90`). Nilai di luar rentang menghentikan build dengan `::error::`.

| Env | Bawaan | Rentang | Arti |
|---|---|---|---|
| `DS_DELAY_SEC` | 90 | 1–3600 | jeda sebelum pass pertama (detik, dihitung dari late_initcall) |
| `DS_PASSES` | 3 | 1–8 | jumlah pass penegasan; jeda tiap pass digandakan |
| `DS_STAT_SEC` | 5 | 1–600 | target `vm.stat_interval` (detik) |
| `DS_DIRTY_WB_CS` | 1500 | 1–60000 | target `vm.dirty_writeback_centisecs`. Nilai 0 sengaja ditolak karena mematikan writeback periodik |

Jadwal pass dengan bawaan: sekitar 90 dtk, 270 dtk, 630 dtk sejak boot.

## 3. Memasang tanpa CI (manual)

Ekstrak blok `run:` dari step menjadi skrip, lalu jalankan dari **root kernel**:

```sh
python3 -c "import yaml; print(yaml.safe_load(open('ci/ceds-inject-step.yml'))[0]['run'])" > ceds-inject.sh

cd /path/ke/kernel
DS_DELAY_SEC=90 DS_PASSES=3 DS_STAT_SEC=5 DS_DIRTY_WB_CS=1500 bash /path/ke/ceds-inject.sh
```

`GITHUB_STEP_SUMMARY` bersifat opsional; di luar CI ringkasan dibuang ke `/dev/null`. Skrip aman dijalankan berulang (idempoten).

## 4. Apa saja yang berubah di tree

| Berkas | Perubahan |
|---|---|
| `mm/vmstat.c` | 1 baris: inisialisasi `sysctl_stat_interval` |
| `mm/page-writeback.c` | 1 baris: inisialisasi `dirty_writeback_interval` |
| `mm/cryo_deepsleep.c` | **baru**: penegas runtime |
| `mm/Makefile` | +1 baris: `obj-y += cryo_deepsleep.o` |

Contoh diff Lapis 1:

```diff
-int sysctl_stat_interval __read_mostly = HZ;
+int sysctl_stat_interval __read_mostly = 5 * HZ;

-unsigned int dirty_writeback_interval = 5 * 100; /* centiseconds */
+unsigned int dirty_writeback_interval = 1500; /* centiseconds */
```

Berkas dipasang di `mm/` (bukan `kernel/power/`) karena `mm/` selalu dibangun, sedangkan `kernel/power/` bergantung pada `CONFIG_PM`.

Jika pola inisialisasi di tree-mu berbeda dan `sed` tidak mengenalinya, step memberi `::warning::` dan hanya penegas runtime (Lapis 2) yang aktif. Build tetap lanjut.

## 5. Kontrol runtime

Setelah boot, tanpa rebuild:

```sh
# tegaskan nilai sekarang juga (log dengan nomor #0)
echo 1 > /sys/module/cryo_deepsleep/parameters/apply

# matikan / hidupkan penegasan
echo 0 > /sys/module/cryo_deepsleep/parameters/enable
echo 1 > /sys/module/cryo_deepsleep/parameters/enable
```

Lewat kernel cmdline: `cryo_deepsleep.enable=0`.

`apply` adalah alat diagnosis yang paling berguna. Jika setelah `apply` nilainya benar tetapi kembali berubah beberapa saat kemudian, penimpanya hampir pasti skrip atau layanan userspace.

## 6. Verifikasi di perangkat

```sh
dmesg | grep cryo_deepsleep               # mungkin butuh root
cat /proc/sys/vm/stat_interval            # detik, harus = DS_STAT_SEC
cat /proc/sys/vm/dirty_writeback_centisecs  # harus = DS_DIRTY_WB_CS
ls /sys/module/cryo_deepsleep/parameters/   # enable, apply
```

Arti log:

| Log | Arti |
|---|---|
| `#N: ... ok (...)` | nilai sudah sesuai target |
| `#N: ... A -> B` | nilai berbeda dan ditulis ulang. Jika Lapis 1 terpasang, ada yang menimpa |
| `#N: dinonaktifkan, dilewati` | `enable=0` |
| `#0` | pemicu manual lewat `apply` |

## 7. Pemecahan masalah

| Gejala | Kemungkinan penyebab | Tindakan |
|---|---|---|
| Tidak ada log `cryo_deepsleep` sama sekali | Step di-skip, kernel lama belum diganti, atau `dmesg` tidak terbaca | Cek log CI untuk `::warning::`/ringkasan; pastikan boot image baru yang aktif; coba `dmesg` sebagai root |
| `ls /sys/module/cryo_deepsleep` tidak ada | `cryo_deepsleep.o` tidak masuk build | Cek `grep cryo_deepsleep mm/Makefile` dan `System.map` |
| Log `A -> B` di pass #1 padahal Lapis 1 terpasang | Userspace menimpa saat boot | Cari penulisnya (lihat di bawah); naikkan `DS_DELAY_SEC` melewati `boot_completed` |
| Nilai benar di dmesg, salah saat dicek belakangan | Layanan userspace menulis ulang setelah pass terakhir | Naikkan `DS_PASSES`, atau perbaiki sumber penimpa |
| Ringkasan CI: `default di sumber: gagal` | Pola inisialisasi tidak dikenali | Cek baris deklarasi di `mm/vmstat.c`/`mm/page-writeback.c`; Lapis 2 tetap bekerja |
| Build error pada `cryo_deepsleep.c` | Deklarasi `sysctl_stat_interval` berbeda tipe | Buka issue dengan potongan deklarasinya dari tree-mu |

Mencari penulis sysctl di userspace (butuh root, lokasi bisa berbeda tiap ROM):

```sh
grep -rn "stat_interval\|dirty_writeback" \
  /vendor/etc/init /system/etc/init /vendor/bin /system/bin 2>/dev/null
ls /data/adb/modules 2>/dev/null     # modul Magisk/KernelSU yang aktif
```

## 8. Menonaktifkan atau mencopot

- **Di CI**: set input `deepsleep_tuning` ke `Disabled` (step tidak berjalan). Karena sumber di-checkout baru tiap run, tidak ada yang perlu dibersihkan.
- **Di tree lokal**: `git checkout -- mm/vmstat.c mm/page-writeback.c mm/Makefile && rm mm/cryo_deepsleep.c`.
- **Di perangkat tanpa rebuild**: `enable=0` (lihat bagian 5). Nilai sysctl yang sudah tertulis tidak dikembalikan otomatis; tulis manual atau reboot dengan `cryo_deepsleep.enable=0`. Jika Lapis 1 terpasang, default kernel tetap nilai CEDS sampai kernel diganti.

## 9. Batasan

- Hanya dua sysctl: `vm.stat_interval` dan `vm.dirty_writeback_centisecs`.
- Penegasan terbatas pada `DS_PASSES` kali. Penimpa yang aktif lebih lama dari jadwal tidak akan dilawan.
- Perubahan `dirty_writeback_interval` berlaku penuh pada siklus writeback berikutnya, bukan seketika.
- Lihat tabel trade-off di [README](../README.md#trade-off-baca-sebelum-dipakai-harian).
