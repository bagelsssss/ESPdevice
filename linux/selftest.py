#!/usr/bin/env python3
"""Hardware-free self-test for the Linux build (run by CI, also handy locally):
   ./selftest.py ./espdevice
Starts ESPdevice in --dry-run mode against a simulated ESP32 (a pseudo-terminal that answers the 0xFE identity query),
presses keys 1-3, and checks the web page, the state API and the key events that would have been typed."""
import json, os, pty, select, shutil, subprocess, sys, tempfile, threading, time, tty, urllib.request

binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./espdevice")
tmp = tempfile.mkdtemp(prefix="espdevice-selftest-")
cfg = os.path.join(tmp, "cfg"); link = os.path.join(tmp, "ttyFAKE"); outp = os.path.join(tmp, "out.txt")
fails = 0
def check(ok, msg):
    global fails
    print(("PASS " if ok else "FAIL ") + msg)
    if not ok: fails += 1

master, slave = pty.openpty(); tty.setraw(slave)
os.symlink(os.ttyname(slave), link)
stop = False
def board():
    while not stop:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            try: d = os.read(master, 64)
            except OSError: return
            if b"\xfe" in d: os.write(master, b"8KEY\n")
threading.Thread(target=board, daemon=True).start()
def press(n, ms):
    end = time.time() + ms / 1000
    while time.time() < end: os.write(master, bytes([n * 2 - 1])); time.sleep(0.02)
    os.write(master, bytes([n * 2]))
def get(path, **kw):
    req = urllib.request.Request("http://127.0.0.1:9191" + path, **kw)
    with urllib.request.urlopen(req, timeout=5) as r: return r.status, r.read().decode()

env = dict(os.environ, ESPDEVICE_CONFIG=cfg)
proc = subprocess.Popen([binary, "--background", "--dry-run", "--device", link], stdout=open(outp, "w"), stderr=subprocess.STDOUT, env=env)
try:
    st = None
    for _ in range(60):
        time.sleep(0.25)
        try:
            st = json.loads(get("/api/state")[1])
            if st["connected"]: break
        except Exception: pass
    check(st is not None and st["connected"], "app started and found the simulated keypad")
    check("linux" in (st or {}).get("version", ""), "version says linux: %s" % (st or {}).get("version"))
    check("<html" in get("/")[1].lower(), "settings page is served")
    press(1, 80); time.sleep(0.2); press(2, 80); time.sleep(0.2); press(3, 300); time.sleep(0.4)
    out = open(outp).read().split("\n")
    keys = [l for l in out if l.startswith("KEY ")]
    check(keys[:2] == ["KEY 30 down", "KEY 30 up"], "key 1 types A (30)")
    check(keys[2:4] == ["KEY 31 down", "KEY 31 up"], "key 2 types S (31)")
    check("KEY 32 down" in keys and "KEY 32 repeat" in keys and keys[-1] == "KEY 32 up", "key 3 holds and repeats D (32)")
    # rebind to a combo, see it typed in the right order
    req = urllib.request.Request("http://127.0.0.1:9191/api/rebind", data=json.dumps({"index": 0, "key": 0, "value": "CTRL+SHIFT+A"}).encode(),
                                 headers={"X-ESPdevice": "1", "Content-Type": "application/json"}, method="POST")
    urllib.request.urlopen(req, timeout=5); n0 = len([l for l in open(outp).read().split("\n") if l.startswith("KEY ")])
    press(1, 60); time.sleep(0.4)
    keys = [l for l in open(outp).read().split("\n") if l.startswith("KEY ")][n0:]
    check(keys == ["KEY 29 down", "KEY 42 down", "KEY 30 down", "KEY 30 up", "KEY 42 up", "KEY 29 up"], "CTRL+SHIFT+A goes down in order and up in reverse")
    check(json.loads(get("/api/state")[1])["clicksTotal"] >= 4, "clicks are counted")
    try: urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:9191/api/state", headers={"Host": "evil.example"}), timeout=5); bad = False
    except urllib.error.HTTPError as e: bad = e.code == 403
    check(bad, "a foreign Host header is refused")
    check(os.path.exists(os.path.join(cfg, "profiles.json")) or os.path.exists(os.path.join(cfg, "espdevice.log")), "config folder is written")
    proc.terminate()
    try: proc.wait(timeout=5); check(True, "quits on SIGTERM")
    except subprocess.TimeoutExpired: proc.kill(); check(False, "quits on SIGTERM")
finally:
    stop = True
    if proc.poll() is None: proc.kill()
    shutil.rmtree(tmp, ignore_errors=True)
print("ALL PASSED" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)
