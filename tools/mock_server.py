#!/usr/bin/env python3
"""Mock of the ESP32 web API, to work on main/web/index.html in a normal browser.

    python3 tools/mock_server.py            # http://127.0.0.1:8765
    python3 tools/mock_server.py --ap       # setup-mode page (no home WiFi yet)
    python3 tools/mock_server.py --port 9000

Demo URL options (mock only, used for the README screenshots):
    /?theme=dark|light   /?lang=en|tr   /?open=settings   /?scroll=<element id>   /?pick=<room id>
"""
import argparse, json, pathlib, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument("--html", default=str(ROOT / "main/web/index.html"))
ap.add_argument("--ap", action="store_true", help="pretend to be in setup mode")
ap.add_argument("--port", type=int, default=8765)
args = ap.parse_args()
MODE = "ap" if args.ap else "sta"
T0 = time.time()

st = {
    "rooms": {1: "Living room", 2: "Kitchen", 3: "Bedroom"},
    "icons": {},
    "view": {1: {"on": True, "level": 62, "kelvin": 2700},
             2: {"on": False, "level": 80, "kelvin": None},
             3: {"on": True, "level": 18, "kelvin": 2200}},
    "night": {},
    "devices": [
        {"i": 0, "type": "lamp", "room": 1, "addr": "0x1a2b", "ieee": "000d6ffffe123456"},
        {"i": 1, "type": "lamp", "room": 1, "addr": "0x3c4d", "ieee": "000d6ffffe654321"},
        {"i": 2, "type": "remote", "room": 1, "addr": "0x5e6f", "ieee": "000d6ffffeabcdef"},
        {"i": 3, "type": "lamp", "room": 2, "addr": "0x7a8b", "ieee": "000d6ffffe111111"},
        {"i": 4, "type": "lamp", "room": 2, "addr": "0x7a8c", "ieee": "000d6ffffe111112"},
        {"i": 5, "type": "remote", "room": 2, "addr": "0x7a8d", "ieee": "000d6ffffe111113"},
        {"i": 6, "type": "lamp", "room": 3, "addr": "0x9a01", "ieee": "000d6ffffe222222"}],
    "pair": None, "events": [],
}


def ev(msg):
    st["events"].append({"id": len(st["events"]) + 1, "t": int(time.time() - T0), "msg": msg})


ev("Zigbee agi hazir (kanal 25)")
ev("WiFi bagli: http://192.168.1.57  ya da  http://ikea.local")
ev("Saat internetten alindi: 09.11.2026 21:40")
ev("Lamba 0x1a2b odanin grubunda (grup 1)")
ev("Kumanda odanin grubuna baglandi")
ev("Kumandanin ok tuslari ESP'ye baglandi")


def demo_script(q):
    """Small script injected for README screenshots."""
    pre = ""
    if q.get("theme"):
        pre += f"localStorage.setItem('theme',{json.dumps(q['theme'])});"
    if q.get("lang"):
        pre += f"localStorage.setItem('lang',{json.dumps(q['lang'])});"
    post = ""
    if q.get("open") == "settings":
        post += "document.querySelector('#settings').open=true;"
    if q.get("pick"):
        post += f"document.querySelector('[data-pick=\"{int(q['pick'])}\"]').click();"
    if q.get("scroll"):
        post += f"document.getElementById({json.dumps(q['scroll'])}).scrollIntoView({{block:'start'}});"
    head = f"<script>try{{{pre}}}catch(e){{}}</script>" if pre else ""
    tail = f"<script>setTimeout(()=>{{{post}}},700)</script>" if post else ""
    return head, tail


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        if u.path == "/":
            html = open(args.html, encoding="utf-8").read()
            head, tail = demo_script(q)
            html = html.replace("<head>", "<head>" + head, 1).replace("</body>", tail + "</body>", 1)
            b = html.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(b)
        elif u.path == "/api/state":
            p = st["pair"]
            left = max(0, int(p[1] - time.time())) if p else 0
            if p and left == 0:
                st["pair"] = None
                ev("Eslesme kapandi")
            rooms = []
            for rid in sorted(set(st["rooms"]) | {d["room"] for d in st["devices"] if d["room"]}):
                ds = [d for d in st["devices"] if d["room"] == rid]
                v = st["view"].get(rid, {"on": None, "level": None, "kelvin": None})
                n = st["night"].get(rid, {"on": True, "from": 0, "to": 4, "pct": 1, "delay": 5})
                rooms.append({"id": rid, "name": st["rooms"].get(rid, ""), "icon": st["icons"].get(rid, 0),
                              "lamps": sum(d["type"] == "lamp" for d in ds), "remotes": sum(d["type"] == "remote" for d in ds),
                              "on": v["on"], "level": v["level"], "kelvin": v["kelvin"], "night": n})
            self.send({"time": time.strftime("%H:%M"), "tz": st.get("tz", "GMT0BST,M3.5.0/1,M10.5.0"),
                       "ready": MODE == "sta", "channel": 25, "pan": 0xFAE9, "ev": len(st["events"]),
                       "wifi": {"mode": MODE, "connected": MODE == "sta", "ip": "192.168.1.57", "rssi": -52},
                       "pairing": {"open": bool(st["pair"]), "room": p[0] if p else 0, "left": left},
                       "rooms": rooms, "devices": st["devices"]})
        elif u.path == "/api/events":
            a = int(q.get("after", 0))
            self.send([e for e in st["events"] if e["id"] > a])
        elif u.path == "/api/scan":
            time.sleep(1)
            self.send([{"ssid": "HomeWiFi", "rssi": -50, "open": False}, {"ssid": "Neighbour", "rssi": -80, "open": False}])
        else:
            self.send({"ok": False}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        q = dict(urllib.parse.parse_qsl(self.rfile.read(n).decode()))
        p = urllib.parse.urlparse(self.path).path
        if p == "/api/cmd":
            room, a, val = int(q["room"]), q["a"], int(q.get("v", 0))
            for rid in ([room] if room else list(st["view"])):
                v = st["view"].setdefault(rid, {"on": None, "level": 80, "kelvin": None})
                if a in ("on", "off"):
                    v["on"] = a == "on"
                elif a == "toggle":
                    v["on"] = not v["on"]
                elif a == "dim":
                    v["on"], v["level"] = val > 0, val or v["level"]
                elif a == "temp":
                    v["kelvin"] = val
        elif p == "/api/room":
            if "icon" in q:
                st["icons"][int(q["id"])] = int(q["icon"])
            if "name" in q or "icon" not in q:
                st["rooms"][int(q["id"])] = q.get("name", "")
        elif p == "/api/pair":
            if q.get("stop"):
                st["pair"] = None
                ev("Eslesme kapandi")
            else:
                st["pair"] = (int(q["room"]), time.time() + int(q.get("s", 180)))
                ev("Eslesme acik: %s, 180 sn. Once lambalari, sonra kumandayi sifirla." % st["rooms"].get(int(q["room"]), q["room"]))
        elif p == "/api/device":
            i = int(q["i"])
            if q.get("forget"):
                st["devices"].pop(i)
                for k, d in enumerate(st["devices"]):
                    d["i"] = k
            elif "room" in q:
                st["devices"][i]["room"] = int(q["room"])
        elif p == "/api/night":
            st["night"][int(q["id"])] = {"on": q["on"] == "1", "from": int(q["from"]), "to": int(q["to"]),
                                         "pct": int(q.get("pct", 1)), "delay": int(q.get("delay", 5))}
        elif p == "/api/tz":
            st["tz"] = q["tz"]
        self.send({"ok": True})


print(f"Mock ESP on http://127.0.0.1:{args.port}  ({MODE} mode)")
ThreadingHTTPServer(("127.0.0.1", args.port), H).serve_forever()
