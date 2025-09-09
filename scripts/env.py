from SCons.Script import Import
Import("env")
from pathlib import Path
import json

def find_up(start, name):
    p = Path(start).resolve()
    for d in (p, *p.parents):
        f = d / name
        if f.is_file():
            return f

cfg_path = find_up(env["PROJECT_DIR"], "env.json")
if cfg_path:
    try:
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        ssid, pwd = cfg.get("wifiName"), cfg.get("wifiPwd")
        if ssid is not None and pwd is not None:
            ssid = str(ssid).replace('"', r'\"')
            pwd  = str(pwd).replace('"', r'\"')
            env.Append(CPPDEFINES=[("WIFI_SSID", f'\\"{ssid}\\"'), ("WIFI_PWD", f'\\"{pwd}\\"')])
            print(f"[env script] WIFI_SSID/WIFI_PWD loaded from {cfg_path}")
        else:
            print(f"[env script] wifiName/wifiPwd missing in {cfg_path}; skipping")
    except Exception as e:
        print(f"[env script] failed to read {cfg_path}: {e}")
else:
    print("[env script] env.json not found; skipping")
