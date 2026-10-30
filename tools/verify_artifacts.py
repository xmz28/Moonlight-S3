"""Offline artifact checks. Never opens a serial port or flashes a device."""
import hashlib
import argparse
import json
from pathlib import Path
import re
import struct

root=Path(__file__).resolve().parents[1]
build=root/".pio/build/moonlight_s3"
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--host-dir", type=Path, help="Optional host checkout for local credential checks")
args=parser.parse_args()
app=(build/"firmware.bin").read_bytes()
factory=(build/"firmware.factory.bin").read_bytes()
boot=(build/"bootloader.bin").read_bytes()
parts=(build/"partitions.bin").read_bytes()
for name,image in (("app",app),("bootloader",boot)):
    assert image[0]==0xe9, (name,"bad image magic")
    assert image[2]==2, (name,"Flash mode must be DIO")
    assert image[3]>>4==4, (name,"Flash size must be 16 MB")
    assert struct.unpack_from("<H",image,12)[0]==9, (name,"image must target ESP32-S3")
partitions=[]
for offset in range(0,len(parts),32):
    entry=parts[offset:offset+32]
    if len(entry)<32 or entry[:2]!=b"\xaa\x50": continue
    magic,kind,subtype,start,size,label,flags=struct.unpack("<HBBII16sI",entry)
    label=label.split(b"\0")[0].decode()
    assert start+size<=16*1024*1024
    partitions.append(dict(name=label,type=kind,subtype=subtype,offset=start,size=size))
application=next(p for p in partitions if p["type"]==0)
assert application["offset"]==0x10000 and len(app)<=application["size"]
assert factory[:len(boot)]==boot
assert factory[0x8000:0x8000+len(parts)]==parts
assert factory[0x10000:0x10000+len(app)]==app
font=(root/"assets/ui_font.bin").read_bytes()
font_partition=next(p for p in partitions if p["name"]=="ui_font")
assert font_partition["offset"]==0x610000 and len(font)<=font_partition["size"]
assert factory[0x610000:0x610000+len(font)]==font
secret_path=root/"include/config_secrets.h"
legacy=re.search(r'MOONLIGHT_PAIRING_TOKEN\s+"([0-9a-f]{32})"',secret_path.read_text(encoding="utf-8") if secret_path.exists() else "")
if legacy:
    assert legacy.group(1).encode() not in app, "Old compiled pairing token must not occur in firmware"
configs=[]
if args.host_dir:
    if not args.host_dir.is_dir():
        parser.error("--host-dir must be an existing host checkout")
    configs=[args.host_dir/"settings.json",args.host_dir/"dist/MoonlightS3Host/settings.json"]
for config in configs:
    if not config.exists():
        continue
    settings=json.loads(config.read_text(encoding="utf-8"))
    assert "token" not in settings, "Top-level legacy token must be migrated out"
    for identity,record in settings.get("pairings",{}).items():
        assert re.fullmatch(r"[0-9a-f]{12}",identity)
        assert re.fullmatch(r"[0-9a-f]{32}",record["host_id"])
        assert re.fullmatch(r"[0-9a-f]{32}",record["token"])
manifest={"target":"ESP32-S3-N16R8","flash_mode":"DIO","flash_bytes":16*1024*1024,
          "application_partition_bytes":application["size"],"partitions":partitions,"artifacts":{}}
for name in ("firmware.bin","firmware.factory.bin","bootloader.bin","partitions.bin"):
    data=(build/name).read_bytes()
    manifest["artifacts"][name]={"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest()}
(build/"artifact-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
print("PASS: ESP32-S3 chip ID, 16 MB DIO headers, partition bounds, combined image contents.")
print("PASS: legacy token check complete (when a local legacy token is available).")
if args.host_dir:
    print("PASS: available local host settings use explicit device identities.")
print(f"App bytes: {len(app)} / {application['size']}; factory bytes: {len(factory)}")
print("Manifest:",build/"artifact-manifest.json")
