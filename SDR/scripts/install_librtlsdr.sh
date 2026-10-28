#!/usr/bin/env bash
# Install RTL-SDR Blog fork of librtlsdr (required for proper RTL-SDR V4 support) on Raspberry Pi OS / Debian
set -euo pipefail

sudo apt update
sudo apt install -y git cmake build-essential pkg-config libusb-1.0-0-dev

# Remove stock librtlsdr packages: they do not support V4 (R828D + HF upconverter) correctly
sudo apt purge -y '^librtlsdr*' 'rtl-sdr' || true
sudo rm -rvf /usr/lib/librtlsdr* /usr/include/rtl-sdr* /usr/local/lib/librtlsdr* \
             /usr/local/include/rtl-sdr* /usr/local/include/rtl_* /usr/local/bin/rtl_*

WORKDIR=$(mktemp -d)
git clone --depth 1 https://github.com/rtlsdrblog/rtl-sdr-blog "$WORKDIR/rtl-sdr-blog"
cmake -S "$WORKDIR/rtl-sdr-blog" -B "$WORKDIR/rtl-sdr-blog/build" \
      -DINSTALL_UDEV_RULES=ON -DDETACH_KERNEL_DRIVER=ON
cmake --build "$WORKDIR/rtl-sdr-blog/build" -j"$(nproc)"
sudo cmake --install "$WORKDIR/rtl-sdr-blog/build"
sudo cp "$WORKDIR/rtl-sdr-blog/rtl-sdr.rules" /etc/udev/rules.d/
sudo ldconfig
sudo usermod -aG plugdev "$USER"   # udev rules give access to group plugdev
rm -rf "$WORKDIR"

# Keep kernel DVB-T driver away from the dongle
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf
sudo rmmod dvb_usb_rtl28xxu 2>/dev/null || true
sudo udevadm control --reload-rules && sudo udevadm trigger

echo "Done. Re-plug the dongle and run: rtl_test -t"
