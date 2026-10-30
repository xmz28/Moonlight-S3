"""Keep UI glyphs out of the SDK's PSRAM-XIP rodata copy."""
Import("env")
env.Append(FLASH_EXTRA_IMAGES=[("0x610000",env.subst("$PROJECT_DIR/assets/ui_font.bin"))])
env.Depends("$BUILD_DIR/${PROGNAME}.bin",env.subst("$PROJECT_DIR/assets/ui_font.bin"))
font_path=env.subst("$PROJECT_DIR/assets/ui_font.bin")
if env.get("UPLOADCMD") and font_path not in env.get("UPLOADERFLAGS",[]):
    env.Append(UPLOADERFLAGS=["0x610000",font_path])
