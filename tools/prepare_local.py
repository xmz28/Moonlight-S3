"""Create optional local Wi-Fi defaults; authorization happens on the screen."""
from pathlib import Path
import json

root = Path(__file__).resolve().parents[1]
host = Path(r"C:\Users\10145294\Documents\MySoftware\Moonlight_S3_Host")
secret = root / "include/config_secrets.h"
if not secret.exists():
    secret.write_text('#pragma once\n#define MOONLIGHT_WIFI_SSID ""\n#define MOONLIGHT_WIFI_PASSWORD ""\n',encoding="utf-8")
settings = host / "settings.json"
if not settings.exists():
    settings.write_text(json.dumps(dict(host="", port=47989, pairings={},
        monitor=1, quality=60, fps=25, resolution="400x240", control=True, toolbar=True), indent=2),encoding="utf-8")
print("Local defaults ready; pair with the four-digit code shown by S3. Existing files preserved.")
