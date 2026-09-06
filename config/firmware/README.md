# Orange Pi 5B AP6275P firmware

The AP6275P module on Orange Pi 5B needs Broadcom firmware that is not
provided by Ubuntu 26.04's `linux-firmware-broadcom-wireless` package at the
time this image baseline was created.

Required files copied into the image:

- `brcm/BCM4362A2.hcd`
- `brcm/brcmfmac43752-pcie.bin`
- `brcm/brcmfmac43752-pcie.clm_blob`
- `brcm/brcmfmac43752-pcie.txt`

The build also creates board aliases used by brcmfmac on Orange Pi 5B:

- `brcm/brcmfmac43752-pcie.xunlong,orangepi-5b.bin`
- `brcm/brcmfmac43752-pcie.xunlong,orangepi-5b.txt`

These are proprietary Broadcom firmware files. Review redistribution terms
before publishing an image outside your own device/testing flow.
