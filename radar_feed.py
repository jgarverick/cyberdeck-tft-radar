#!/usr/bin/env python3
"""
radar_feed.py  -  compact ADS-B feed for the Cyberdeck radar scope.

readsb/dump1090 produce a large aircraft.json (every field for every aircraft).
This service reads it, keeps aircraft with a recent position, computes range +
bearing from your deck's location, sorts by range, and serves a small JSON blob
the ESP32 can parse without choking.

    python3 radar_feed.py            # serves 0.0.0.0:8090/radar

Point HOME_LAT/HOME_LON at your antenna. SOURCE_JSON default matches readsb;
for dump1090-fa use /run/dump1090-fa/aircraft.json.
"""

import json
import math
import os
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --------------------------- CONFIG ----------------------------------------
HOME_LAT = float(os.getenv("HOME_LAT", "42.9017"))
HOME_LON = float(os.getenv("HOME_LON", "-78.4919"))
MAX_NM = float(os.getenv("MAX_NM", "40"))
MAX_AC = int(os.getenv("MAX_AC", "30"))
MAX_AGE = int(os.getenv("MAX_AGE", "60"))
SOURCE_JSON = os.getenv("SOURCE_JSON", "/run/dump1090-mutability/aircraft.json")
SOURCE_URL = os.getenv("SOURCE_URL", "").strip()
HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", "8090"))
# ---------------------------------------------------------------------------

EARTH_NM = 3440.065     # earth radius in nautical miles


def range_bearing(lat1, lon1, lat2, lon2):
    """Great-circle distance (nm) and initial bearing (deg, 0=N clockwise)."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)
    a = math.sin(dlat / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlon / 2) ** 2
    rng = EARTH_NM * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    y = math.sin(dlon) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dlon)
    brg = (math.degrees(math.atan2(y, x)) + 360) % 360
    return rng, brg


def process(data, home_lat, home_lon, max_nm, max_ac, max_age):
    out = []
    for a in data.get("aircraft", []):
        lat, lon = a.get("lat"), a.get("lon")
        if lat is None or lon is None:
            continue
        if a.get("seen_pos", 999) > max_age:
            continue
        rng, brg = range_bearing(home_lat, home_lon, lat, lon)
        if rng > max_nm:
            continue
        flight = (a.get("flight") or "").strip()
        alt = a.get("alt_baro")
        if alt == "ground":
            alt = 0
        out.append({
            "fl":  flight or a.get("hex", "").upper(),
            "rng": round(rng, 1),
            "brg": round(brg),
            "alt": int(alt) if isinstance(alt, (int, float)) else 0,
            "trk": round(a.get("track", 0)),
            "gs":  round(a.get("gs", 0)),
        })
    out.sort(key=lambda x: x["rng"])
    out = out[:max_ac]
    return {"home": {"lat": home_lat, "lon": home_lon},
            "max_nm": max_nm, "n": len(out), "ac": out}


def read_source():
    errors = []

    if SOURCE_URL:
        try:
            with urllib.request.urlopen(SOURCE_URL, timeout=3) as r:
                payload = r.read()
            return json.loads(payload.decode("utf-8")), None
        except (urllib.error.URLError, TimeoutError, ValueError, OSError) as ex:
            errors.append(f"url:{ex}")

    if SOURCE_JSON:
        try:
            with open(SOURCE_JSON) as f:
                return json.load(f), None
        except (OSError, ValueError) as ex:
            errors.append(f"file:{ex}")

    return None, "; ".join(errors) if errors else "no_source_configured"


def collect():
    data, err = read_source()
    if not data:
        return {"home": {"lat": HOME_LAT, "lon": HOME_LON},
                "max_nm": MAX_NM, "n": 0, "ac": [], "err": "no_feed", "detail": err}
    return process(data, HOME_LAT, HOME_LON, MAX_NM, MAX_AC, MAX_AGE)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path not in ("/radar", "/"):
            self.send_error(404); return
        body = json.dumps(collect()).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    print(f"radar feed on http://{HOST}:{PORT}/radar  (home {HOME_LAT},{HOME_LON}, {MAX_NM}nm)")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
