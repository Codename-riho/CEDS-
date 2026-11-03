# 💤 CEDS — Cryo Enhance Deep Sleep

Tuning VM hemat daya untuk kernel Linux/Android yang **benar-benar menempel**: dipasang langsung di sumber kernel (atau sebagai LKM), dengan penegas runtime yang mencatat di `dmesg` apakah ada pihak lain (init.rc, PowerHAL, modul) yang menimpa nilainya.

CEDS hanya menyentuh dua sysctl VM. Jalur suspend/resume **tidak** disentuh, dan tidak ada daemon atau loop tak berujung.

| sysctl | Bawaan upstream Linux | Bawaan CEDS | Satuan |
|---|---|---|---|
| `vm.stat_interval` | 1 | **5** | detik |
| `vm.dirty_writeback_centisecs` | 500 (5 dtk) | **1500** (15 dtk) | centisecond |

> Bawaan ROM/vendor bisa berbeda dari upstream. Cek nilai aslinya di perangkatmu dulu:
> `cat /proc/sys/vm/stat_interval /proc/sys/vm/dirty_writeback_centisecs`

---

## Pilih varian

| | **Antar kernel** (built-in) | **Topaz** (built-in) | **LKM** |
|---|---|---|---|
| Dokumen | [docs/GENERIC.md](docs/GENERIC.md) | [docs/TOPAZ.md](docs/TOPAZ.md) | [docs/LKM.md](docs/LKM.md) |
| Target | kernel Linux/Android mana pun | Redmi Note 12 4G (`topaz`) | kernel mana pun yang mendukung modul |
| Rebuild kernel | ya | ya | tidak, tapi modul harus dibangun terhadap kernel yang **sama** |
| Default diubah di sumber | ya | ya | tidak |
| Dimuat oleh | kernel (otomatis saat boot) | kernel (otomatis saat boot) | userspace (`insmod`) |
| Nilai asli bisa dikembalikan | tidak (rebuild) | tidak (rebuild) | ya (`rmmod`) |
| Butuh `CONFIG_MODULES` | tidak | tidak | ya |

Varian **Topaz** = varian antar kernel + profil nilai, prosedur ukur waktu boot, cara melacak penimpa di vendor, dan protokol uji baterai yang disesuaikan untuk perangkat itu.

---

## Cara kerja

CEDS memakai dua lapis supaya tidak bergantung pada satu kali tulis yang mudah kalah balapan:

1. **Lapis 1 — default di sumber.** Inisialisasi `sysctl_stat_interval` (`mm/vmstat.c`) dan `dirty_writeback_interval` (`mm/page-writeback.c`) diubah langsung. Kernel sudah lahir dengan nilai ini, tanpa kode tambahan.
2. **Lapis 2 — penegas runtime.** File `mm/cryo_deepsleep.c` menegaskan nilai beberapa kali dengan jeda berlipat ganda (bukan loop), dan mencatat setiap penimpaan.

```
boot ──────── ~90 s ─────────── ~270 s ──────────── ~630 s ───▶
 │ default CEDS  │ pass #1        │ pass #2           │ pass #3 (terakhir)
 │ (lapis 1)     │ cek + tegaskan │ cek + tegaskan    │ cek + tegaskan
```

Jika sebuah pass menemukan nilai yang berbeda dari target, ia menulis ulang dan mencatat `A -> B`. Baris seperti itu pada pass #1 atau lebih berarti **ada pihak lain yang menimpa**, dan itulah petunjuk pertama untuk melacak penyebab "tidak berefek".

---

## Mulai cepat (varian built-in)

1. Salin [`ci/ceds-inject-step.yml`](ci/ceds-inject-step.yml) ke workflow GitHub Actions-mu, **setelah** kernel di-checkout dan **sebelum** step build.
2. Jalankan workflow dengan input `deepsleep_tuning = Enabled`.
3. Flash hasil build, lalu di perangkat:

```sh
dmesg | grep cryo_deepsleep          # mungkin butuh root
cat /proc/sys/vm/stat_interval       # harus 5
cat /proc/sys/vm/dirty_writeback_centisecs   # harus 1500
```

Detail lengkap ada di dokumen varian masing-masing.

---

## Hasil yang sehat di dmesg

```
cryo_deepsleep: #1: vm.stat_interval ok (5s)
cryo_deepsleep: #1: dirty_writeback_centisecs ok (1500)
cryo_deepsleep: #2: vm.stat_interval ok (5s)
cryo_deepsleep: #2: dirty_writeback_centisecs ok (1500)
cryo_deepsleep: #3: ...
```

Kalau ada baris `A -> B` (misalnya `vm.stat_interval 1s -> 5s`) padahal Lapis 1 terpasang, nilainya **ditimpa userspace**. Lihat bagian pemecahan masalah di [docs/GENERIC.md](docs/GENERIC.md).

---

## Trade-off (baca sebelum dipakai harian)

| Perubahan | Dampak yang perlu kamu tahu |
|---|---|
| `stat_interval` 1 → 5 dtk | Statistik VM per-CPU (`/proc/vmstat`, `/proc/zoneinfo`) diperbarui lebih jarang. Keputusan di userspace yang membacanya, misalnya `lmkd`, dapat memakai angka yang sedikit basi. Pantau perilaku penutupan aplikasi. |
| `dirty_writeback` 5 → 15 dtk | Data kotor menunggu lebih lama sebelum ditulis. Jendela kehilangan data saat mati mendadak atau crash naik dari sekitar 35 dtk menjadi sekitar 45 dtk (dengan `dirty_expire_centisecs` 30 dtk yang tidak diubah). `fsync`/`sync` tidak terpengaruh, jadi aplikasi yang memintanya tetap aman. |

CEDS **tidak** menjanjikan angka penghematan baterai tertentu. Ukur sendiri dengan protokol di [docs/TOPAZ.md](docs/TOPAZ.md#6-protokol-uji-baterai-ab).

---

## Status pengujian

| Komponen | Status |
|---|---|
| Logika step workflow (validasi, patch default, Makefile, idempoten, jalur gagal) | ✅ diuji di pohon kernel tiruan |
| `cryo_deepsleep.c` (semua kombinasi simbol ada/tidak) | ✅ lolos kompilasi `-Wall -Wextra` dengan header stub |
| Logika LKM (penegasan, penimpaan, manual, restore, rentang parameter) | ✅ diuji fungsional di userspace dengan stub |
| Build terhadap sumber kernel sungguhan | ⬜ belum |
| Berjalan di perangkat (`dmesg`, nilai sysctl, baterai) | ⬜ belum |

Jika kamu menguji di perangkat, mohon catat hasilnya (lihat tabel di [docs/TOPAZ.md](docs/TOPAZ.md#8-data-perangkat-dan-hasil-uji)).

---

## Struktur repo

```
.
├── README.md
├── ci/
│   └── ceds-inject-step.yml     # step workflow varian built-in
├── docs/
│   ├── GENERIC.md               # varian antar kernel
│   ├── TOPAZ.md                 # varian khusus topaz
│   └── LKM.md                   # varian LKM
└── lkm/
    ├── ceds_lkm.c
    └── Makefile
```

## Lisensi

Kode yang disuntikkan ke kernel (`mm/cryo_deepsleep.c`) dan LKM berlisensi **GPL-2.0** (`SPDX-License-Identifier: GPL-2.0`). Tambahkan berkas `LICENSE` di root repo sesuai pilihanmu.
