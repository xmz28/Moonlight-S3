"""Create optional local Wi-Fi defaults; authorization happens on the screen."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
secret = root / "include/config_secrets.h"
if not secret.exists():
    secret.write_text('#pragma once\n#define MOONLIGHT_WIFI_SSID ""\n#define MOONLIGHT_WIFI_PASSWORD ""\n',encoding="utf-8")
print("Local defaults ready; pair with the four-digit code shown by S3. Existing files preserved.")
