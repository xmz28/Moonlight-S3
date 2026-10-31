"""Package first-install and configuration-preserving firmware updates offline."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def checksums(directory):
    lines = []
    for path in sorted(directory.rglob("*")):
        if path.is_file() and path.name != "SHA256SUMS.txt":
            lines.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(directory).as_posix()}")
    (directory / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--host-commit", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"v\d+\.\d+\.\d+", args.version):
        parser.error("Use a version such as v0.1.0")
    if not re.fullmatch(r"[0-9a-f]{40}", args.host_commit):
        parser.error("--host-commit must be a full commit SHA")
    if (ROOT / "include/config_secrets.h").exists():
        raise RuntimeError("Use a clean checkout without config_secrets.h for public firmware")
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip():
        raise RuntimeError("Commit source changes before packaging")
    subprocess.run(["python", str(ROOT / "tools/verify_artifacts.py")], cwd=ROOT, check=True)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    output = ROOT / "release" / args.version
    output.mkdir(parents=True, exist_ok=True)
    build = ROOT / ".pio/build/moonlight_s3"
    source = (f"Version: {args.version}\nFirmware commit: {commit}\nHost commit: {args.host_commit}\n"
              "Working tree: clean; no config_secrets.h\n"
              "Target: ESP32-S3-N16R8, 800x480 RGB565, GT911, project pinout only\n"
              "Protocol: MS3 v3 session control / v2 PIN authorization / v1 JPEG framing\n"
              "Public build has empty compile-time Wi-Fi defaults. No NVS dump is distributed.\n"
              "This release binary was not reflashed for a new hardware acceptance run.\n")
    (output / "SOURCE.txt").write_text(source, encoding="utf-8")
    shutil.copyfile(ROOT / "docs/INSTALL.zh-CN.md", output / "INSTALL.zh-CN.md")
    archives = []
    for kind, files in (
        ("first-install", {"firmware.factory.bin": build / "firmware.factory.bin"}),
        ("update", {"firmware.bin": build / "firmware.bin", "partitions.bin": build / "partitions.bin",
                    "ui_font.bin": ROOT / "assets/ui_font.bin"}),
    ):
        stage = output / kind
        stage.mkdir(exist_ok=True)
        for name, path in files.items():
            shutil.copyfile(path, stage / name)
        for name in ("SOURCE.txt", "INSTALL.zh-CN.md"):
            shutil.copyfile(output / name, stage / name)
        shutil.copyfile(ROOT / "THIRD_PARTY_NOTICES.md", stage / "THIRD_PARTY_NOTICES.md")
        shutil.copytree(ROOT / "licenses", stage / "licenses", dirs_exist_ok=True)
        shutil.copyfile(ROOT / "lib/esp_new_jpeg/LICENSE", stage / "licenses/esp_new_jpeg-LICENSE.txt")
        shutil.copyfile(ROOT / "lib/esp_new_jpeg/PROVENANCE.md", stage / "licenses/esp_new_jpeg-PROVENANCE.md")
        checksums(stage)
        archive = output / f"Moonlight-S3-{args.version}-{kind}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    bundle.write(path, path.relative_to(stage))
        archives.append(archive)
    (output / "SHA256SUMS.txt").write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
        for path in [*archives, output / "INSTALL.zh-CN.md", output / "SOURCE.txt"]), encoding="utf-8")
    print(f"Public firmware packages: {output}")


if __name__ == "__main__":
    main()
