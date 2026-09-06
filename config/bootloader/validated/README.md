# Orange Pi 5B validated bootloader payloads

Baseline này dùng trực tiếp hai payload đã được kiểm chứng trên Orange Pi 5B:

- `idbloader.img`
- `u-boot.itb`

Provenance ghi nhận từ dự án validated cũ:

- U-Boot source: `https://github.com/radxa/u-boot.git`
- Git revision: `39cd993e5d6296635438e84f4576b3a9bf76f86e`
- Armbian artifact: `2017.09-S39cd-P2e20-Hbe55-Vd696-B5da4-R448a`
- DDR training binary: RK3588 v1.20
- BL31: RK3588 v1.45
- Board: Orange Pi 5B, RK3588S

Checksums:

```text
ed05b59ceb3613892d340e55b5ce0e9f816148d5528424fc251e507bd4396b5b  idbloader.img
f45e84c47454d72d00c705ae54a08a14116b1b899b0defe6810a32fccb7af4e0  u-boot.itb
```

Image writer ghi `idbloader.img` tại LBA 64 và `u-boot.itb` tại LBA 16384.
