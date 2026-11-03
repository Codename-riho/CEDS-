# CEDS — Varian LKM (`ceds_lkm`)

CEDS sebagai Loadable Kernel Module. Berguna ketika kamu tidak ingin menyentuh sumber kernel, ingin memuat setelah `boot_completed`, atau ingin nilai asli bisa dikembalikan dengan `rmmod`.

## 1. Built-in atau LKM?

| | Built-in ([GENERIC](GENERIC.md)) | LKM |
|---|---|---|
| Default diubah di sumber | ✅ | ❌ |
| Aktif sejak boot tanpa userspace | ✅ | ❌ (perlu `insmod`) |
| Mengalahkan penimpa saat boot | pass terjadwal | dimuat **setelah** `boot_completed`, jadi penimpa boot sudah lewat |
| Nilai asli bisa dikembalikan | ❌ | ✅ `rmmod` |
| Butuh `CONFIG_MODULES` | ❌ | ✅ |
| Harus cocok persis dengan kernel berjalan | otomatis | ✅ (vermagic / modversions) |

## 2. Cara kerja

`sysctl_stat_interval` dan `dirty_writeback_interval` **tidak di-export**, jadi modul tidak bisa me-link-nya. LKM menulis lewat procfs, jalur yang sama dengan `sysctl -w`:

```
/proc/sys/vm/stat_interval              (detik)
/proc/sys/vm/dirty_writeback_centisecs  (centisecond)
```

Saat dimuat, modul menjadwalkan `passes` kali penegasan dengan jeda `delay_sec` yang digandakan tiap pass. Tidak ada loop tak berujung. Sebelum menulis pertama kali, nilai asli dicatat dan dikembalikan saat `rmmod` (kecuali `restore=0`). `insmod` tidak menunggu: pekerjaan berjalan di workqueue.

Kebutuhan kernel: `kernel_read`/`kernel_write` dengan signature 4.14+ (`(file, buf, count, &pos)`).

## 3. Prasyarat konfigurasi kernel

```sh
adb shell su -c 'zcat /proc/config.gz | grep -E "CONFIG_MODULES|CONFIG_MODULE_UNLOAD|CONFIG_MODVERSIONS|CONFIG_MODULE_SIG|CONFIG_MODULE_FORCE_LOAD"'
```

| Config | Dibutuhkan | Catatan |
|---|---|---|
| `CONFIG_MODULES=y` | ✅ | tanpa ini LKM tidak bisa dimuat. Pakai varian built-in |
| `CONFIG_MODULE_UNLOAD=y` | untuk `rmmod`/restore | tanpa ini modul tak bisa dilepas |
| `CONFIG_MODVERSIONS` | jika aktif | modul harus dibangun dari tree + `Module.symvers` kernel yang sama |
| `CONFIG_MODULE_SIG_FORCE` | jika aktif | modul harus ditandatangani dengan kunci build kernel |

## 4. Build

Modul harus dibangun terhadap **sumber, defconfig, toolchain, dan commit yang sama** dengan kernel yang berjalan di perangkat.

```sh
# 1) siapkan tree (sekali)
make -C /path/kernel O=out ARCH=arm64 LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- <defconfig-mu>
make -C /path/kernel O=out ARCH=arm64 LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare

# 2) bangun modul
make -C lkm KDIR=/path/kernel KOUT=/path/kernel/out \
     KARGS="LLVM=1 CROSS_COMPILE=aarch64-linux-gnu-"
```

Hasil: `lkm/ceds_lkm.ko`. Variabel Makefile:

| Variabel | Bawaan | Arti |
|---|---|---|
| `KDIR` | (wajib) | sumber kernel |
| `KOUT` | `$(KDIR)/out` | direktori output build kernel |
| `ARCH` | `arm64` | arsitektur |
| `KARGS` | kosong | argumen tambahan (LLVM, CROSS_COMPILE, dst.) |

Contoh step CI (sesuaikan path, belum diuji pada workflow sungguhan):

```yaml
      - name: "💤 Build CEDS LKM"
        shell: bash
        working-directory: ${{ env.KERNEL_LOC_DIR }}/${{ env.KERNEL_NAME }}
        run: |
          set -euo pipefail
          make O=out ARCH=arm64 LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
          make -C "$GITHUB_WORKSPACE/CEDS/lkm" KDIR="$PWD" KOUT="$PWD/out" \
               KARGS="LLVM=1 CROSS_COMPILE=aarch64-linux-gnu-"
          ls -l "$GITHUB_WORKSPACE/CEDS/lkm/ceds_lkm.ko"
```

## 5. Parameter

Diisi saat `insmod` (rentang divalidasi; di luar rentang modul menolak dimuat dengan `-EINVAL`).

| Parameter | Bawaan | Rentang | Arti |
|---|---|---|---|
| `delay_sec` | 10 | 0–3600 | jeda pass pertama. 0 = segera |
| `passes` | 2 | 1–8 | jumlah pass; jeda tiap pass digandakan |
| `stat_sec` | 5 | 1–600 | target `vm.stat_interval` (detik) |
| `wb_cs` | 1500 | 1–60000 | target `vm.dirty_writeback_centisecs` |

Parameter runtime di `/sys/module/ceds_lkm/parameters/`:

| Parameter | Mode | Arti |
|---|---|---|
| `enable` | rw | 1/0 hidupkan atau matikan penegasan |
| `restore` | rw | 1/0 kembalikan nilai asli saat `rmmod` |
| `apply` | tulis saja | tulis `1` untuk menegaskan sekarang (log `#0`) |

## 6. Memuat

### 6.1 Manual

```sh
adb push lkm/ceds_lkm.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/ceds_lkm.ko delay_sec=10 passes=2 stat_sec=5 wb_cs=1500'
```

### 6.2 Otomatis saat boot (modul Magisk/KernelSU)

`service.sh` di dalam modul (struktur `module.prop` mengikuti ketentuan Magisk/KernelSU):

```sh
#!/system/bin/sh
MODDIR=${0%/*}
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5
insmod "$MODDIR/ceds_lkm.ko" delay_sec=10 passes=2 stat_sec=5 wb_cs=1500
```

Memuat setelah `boot_completed` adalah alasan utama memakai LKM: penimpa yang bekerja saat boot sudah selesai ketika modul menegaskan nilainya.

## 7. Verifikasi

```sh
dmesg | grep ceds_lkm
cat /proc/sys/vm/stat_interval
cat /proc/sys/vm/dirty_writeback_centisecs
ls /sys/module/ceds_lkm/parameters/
```

Log yang sehat:

```
ceds_lkm: dimuat: stat_interval=5s dirty_writeback=1500cs, 2 pass mulai +10s
ceds_lkm: #1: vm.stat_interval 1 -> 5
ceds_lkm: #1: vm.dirty_writeback_centisecs 500 -> 1500
ceds_lkm: #2: vm.stat_interval ok (5)
ceds_lkm: #2: vm.dirty_writeback_centisecs ok (1500)
```

Pada LKM, `A -> B` di pass #1 normal (nilai asli diubah pertama kali). `A -> B` di pass #2 atau pada `#0` berarti nilainya **ditimpa pihak lain** sejak penegasan sebelumnya.

## 8. Melepas

```sh
rmmod ceds_lkm
```

Jika `restore=1` (bawaan), nilai asli yang dicatat sebelum penulisan pertama dikembalikan, dan log mencatat `restore ... -> N`. Hanya nilai yang memang pernah diubah modul yang dikembalikan. Jika varian built-in sudah memasang nilai yang sama sebelum LKM dimuat, "nilai asli" yang dicatat adalah nilai built-in itu.

## 9. Pemecahan masalah

| Gejala | Penyebab umum | Tindakan |
|---|---|---|
| `insmod: Exec format error` | vermagic atau versi simbol tidak cocok | `dmesg \| tail` memuat alasan persisnya. Bangun ulang terhadap tree, defconfig, toolchain, dan commit yang sama |
| `insmod: Operation not permitted` | tanpa root, atau penandatanganan modul dipaksa | jalankan sebagai root; cek `CONFIG_MODULE_SIG_FORCE` |
| `insmod: Function not implemented` | `CONFIG_MODULES` mati | pakai varian built-in |
| `rmmod: ... not found / cannot unload` | `CONFIG_MODULE_UNLOAD` mati | tidak bisa dilepas tanpa reboot |
| Log `gagal tulis ... (-13)` | ditolak (`EACCES`), kemungkinan SELinux | `dmesg \| grep avc` untuk melihat penolakannya. Modul menulis dari kernel thread, bukan dari shell-mu |
| Log `gagal baca ... (-2)` | berkas procfs tidak ada di kernel ini | cek `ls /proc/sys/vm/` |
| `insmod` gagal dengan `Invalid argument` | parameter di luar rentang | lihat tabel bagian 5 dan `dmesg` |
| Nilai balik berubah setelah pass terakhir | penimpa aktif lebih lama dari jadwal | naikkan `passes`/`delay_sec`, atau cari penimpa (lihat [TOPAZ §4.2](TOPAZ.md#4-menentukan-ds_delay_sec-dan-melacak-penimpa)) |

## 10. Batasan

- Modul harus dibangun ulang tiap kali kernel berubah.
- Tidak ada penegasan setelah pass terakhir. Penimpa yang aktif lebih lama dari jadwal tidak dilawan.
- Penulisan lewat procfs dari kernel thread dapat dibatasi kebijakan keamanan (SELinux). Kalau itu terjadi, varian built-in menulis variabel kernel langsung dan tidak terkena batasan itu.
- Trade-off nilai sama dengan varian lain, lihat [README](../README.md#trade-off-baca-sebelum-dipakai-harian).
