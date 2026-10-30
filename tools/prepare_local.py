"""Generate missing local pairing configuration.

Uses existing Python + Pillow; creates no environment and never replaces tokens.
"""
from pathlib import Path
import json
import secrets

root = Path(__file__).resolve().parents[1]
host = Path(r"C:\Users\10145294\Documents\MySoftware\Moonlight_S3_Host")
secret = root / "include/config_secrets.h"
if not secret.exists():
    token = secrets.token_hex(16)
    secret.write_text('#pragma once\n#define MOONLIGHT_PAIRING_TOKEN "' + token + '"\n')
else:
    token = secret.read_text().split('"')[1]
settings = host / "settings.json"
if not settings.exists():
    settings.write_text(json.dumps(dict(host="", port=47989, token=token,
        monitor=1, quality=60, fps=25, resolution="400x240", control=True, toolbar=True), indent=2))
print("Local pairing config ready; existing credentials preserved.")
