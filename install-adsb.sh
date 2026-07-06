#!/usr/bin/env bash
# ===========================================================================
#  Cyberdeck Radar - Raspberry Pi ADS-B stack installer
#
#  Sets up an RTL-SDR Blog V4 + readsb decoder on a Raspberry Pi 4, then
#  installs radar_feed.py as a service the ESP32 scope polls.
#
#  Run on the Pi:   chmod +x install-adsb.sh && sudo ./install-adsb.sh
#  Re-run safe (idempotent-ish). Review before running; edit HOME_LAT/LON below.
# ===========================================================================
set -e

HOME_LAT="42.9017"      # <-- your antenna latitude
HOME_LON="-78.4919"     # <-- your antenna longitude
FEED_DIR="/opt/cyberdeck-radar"

if [[ $EUID -ne 0 ]]; then echo "Run with sudo."; exit 1; fi

echo "== 1/5  Base packages =="
apt-get update
apt-get install -y \
  git \
  build-essential \
  cmake \
  pkg-config \
  python3 \
  rtl-sdr \
  librtlsdr-dev \
  libusb-1.0-0-dev

# Some sources include <libusb.h> while Debian/Ubuntu ship it as
# /usr/include/libusb-1.0/libusb.h. Provide a compatibility symlink when needed.
if [[ -f /usr/include/libusb-1.0/libusb.h && ! -e /usr/include/libusb.h ]]; then
  ln -s /usr/include/libusb-1.0/libusb.h /usr/include/libusb.h
fi

echo "== 2/5  RTL-SDR Blog V4 drivers =="
# The V4 uses the R828D tuner and needs the RTL-SDR Blog driver fork.
# Remove the distro rtl-sdr first to avoid a clash, then build the fork.
apt-get purge -y ^librtlsdr || true
rm -rf /usr/lib/librtlsdr* /usr/include/rtl-sdr* /usr/local/lib/librtlsdr* || true
if [[ ! -d /tmp/rtl-sdr-blog ]]; then
  git clone https://github.com/rtlsdrblog/rtl-sdr-blog /tmp/rtl-sdr-blog
fi
cd /tmp/rtl-sdr-blog
mkdir -p build && cd build
cmake ../ -DINSTALL_UDEV_RULES=ON
make -j"$(nproc)"
make install
cp ../rtl-sdr.rules /etc/udev/rules.d/ || true
ldconfig
# stop the DVB-T TV driver from grabbing the dongle
echo -e "blacklist dvb_usb_rtl28xxu\nblacklist rtl2832\nblacklist rtl2830" > /etc/modprobe.d/blacklist-rtl.conf

echo "== 3/5  readsb decoder (wiedehopf automated install) =="
# Installs readsb, sets it to decode 1090 MHz and write aircraft.json.
bash -c "$(wget -nv -O - https://github.com/wiedehopf/adsb-scripts/raw/master/readsb-install.sh)"
# point readsb at your location and enable the JSON output the feed reads
readsb-set-location "$HOME_LAT" "$HOME_LON" || true

echo "== 4/5  (optional) tar1090 web map at http://<pi>/tar1090 =="
bash -c "$(wget -nv -O - https://github.com/wiedehopf/tar1090/raw/master/install.sh)" || true

echo "== 5/5  radar_feed service =="
mkdir -p "$FEED_DIR"
cp "$(dirname "$0")/radar_feed.py" "$FEED_DIR/"

# Create systemd service with proper environment variables
# SOURCE_URL uses tar1090's data endpoint; SOURCE_JSON falls back to readsb's JSON file
cat > /etc/systemd/system/cyberdeck-radar.service <<EOF
[Unit]
Description=Cyberdeck radar feed
After=network.target readsb.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 $FEED_DIR/radar_feed.py
Restart=always
RestartSec=10
User=root
Environment="HOME_LAT=$HOME_LAT"
Environment="HOME_LON=$HOME_LON"
Environment="SOURCE_URL=http://localhost:8081/data/aircraft.json"
Environment="SOURCE_JSON=/run/readsb/aircraft.json"
Environment="PORT=8090"

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now cyberdeck-radar

echo
echo "Done. Check readsb:   sudo systemctl status readsb"
echo "Check the feed:       curl localhost:8090/radar"
echo "Set the ESP32 PI_HOST to this Pi's IP."
