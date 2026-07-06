#!/usr/bin/env python3
"""
mock_adsb.py - tiny ADS-B aircraft.json emulator for local testing.

Serves a dump1090/readsb-like /aircraft.json payload with moving aircraft so
radar_feed.py can be exercised without an SDR.
"""

import argparse
import json
import math
import random
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EARTH_NM = 3440.065


def destination_point(lat_deg, lon_deg, bearing_deg, distance_nm):
    """Return lat/lon from start point, bearing (deg), and distance (nm)."""
    lat1 = math.radians(lat_deg)
    lon1 = math.radians(lon_deg)
    brng = math.radians(bearing_deg)
    ang_dist = distance_nm / EARTH_NM

    lat2 = math.asin(
        math.sin(lat1) * math.cos(ang_dist)
        + math.cos(lat1) * math.sin(ang_dist) * math.cos(brng)
    )
    lon2 = lon1 + math.atan2(
        math.sin(brng) * math.sin(ang_dist) * math.cos(lat1),
        math.cos(ang_dist) - math.sin(lat1) * math.sin(lat2),
    )

    lon2 = (lon2 + math.pi) % (2 * math.pi) - math.pi
    return math.degrees(lat2), math.degrees(lon2)


def make_flights(count):
    random.seed(42)
    flights = []
    for i in range(count):
        flights.append(
            {
                "hex": f"{0xA00000 + i:06X}",
                "flight": f"SIM{i + 1:03d}",
                "rng": random.uniform(4, 35),
                "brg": random.uniform(0, 360),
                "trk": random.uniform(0, 360),
                "gs": random.uniform(180, 460),
                "alt": random.randint(1200, 39000),
                "turn": random.uniform(-2.5, 2.5),
            }
        )
    return flights


class Handler(BaseHTTPRequestHandler):
    flights = []
    home_lat = 0.0
    home_lon = 0.0
    started = time.time()

    def do_GET(self):
        if self.path not in ("/aircraft.json", "/data/aircraft.json"):
            self.send_error(404)
            return

        t = time.time() - self.started
        aircraft = []
        for f in self.flights:
            # Slow heading drift + orbit-like movement around home location.
            track = (f["trk"] + f["turn"] * t) % 360
            bearing = (f["brg"] + (f["gs"] / 3600.0) * t * 0.2) % 360
            lat, lon = destination_point(self.home_lat, self.home_lon, bearing, f["rng"])
            aircraft.append(
                {
                    "hex": f["hex"],
                    "flight": f["flight"],
                    "lat": round(lat, 6),
                    "lon": round(lon, 6),
                    "seen_pos": round(random.uniform(0.1, 1.5), 1),
                    "alt_baro": int(f["alt"]),
                    "track": round(track, 1),
                    "gs": round(f["gs"], 1),
                }
            )

        body = json.dumps(
            {
                "now": int(time.time()),
                "messages": 1000 + int(t),
                "aircraft": aircraft,
            }
        ).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description="Run a local ADS-B aircraft.json emulator")
    parser.add_argument("--host", default="0.0.0.0", help="bind host")
    parser.add_argument("--port", type=int, default=8081, help="bind port")
    parser.add_argument("--lat", type=float, default=42.9017, help="home latitude")
    parser.add_argument("--lon", type=float, default=-78.4919, help="home longitude")
    parser.add_argument("--count", type=int, default=12, help="number of simulated aircraft")
    args = parser.parse_args()

    Handler.home_lat = args.lat
    Handler.home_lon = args.lon
    Handler.flights = make_flights(max(1, args.count))

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Mock ADS-B feed: http://{args.host}:{args.port}/aircraft.json")
    server.serve_forever()


if __name__ == "__main__":
    main()
