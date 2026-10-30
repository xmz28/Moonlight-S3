"""Generate the Moonlight UI bitmap derivative of Noto Sans CJK (SIL OFL).

Uses the existing Python/Pillow installation. Source OTF stays in .pio/font-cache.
Two-bit coverage and zero-byte runs keep CJK glyphs in flash, not runtime RAM.
"""
from pathlib import Path
import struct
from PIL import Image,ImageDraw,ImageFont

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'.pio/font-cache/NotoSansCJKsc-Regular.otf'
RANGES=((32,126),(160,255),(0x3000,0x303f),(0x4e00,0x9fff),(0xff01,0xff60),(0x25a1,0x25a1))

def glyphs():
    title=ImageFont.truetype(str(SOURCE),34)
    normal=ImageFont.truetype(str(SOURCE),22)
    for code in range(32,127): yield code,title,40,44,36
    for start,end in RANGES:
        for code in range(start,end+1): yield code,normal,24,30,23

def main():
    offsets=[0];advances=[];data=bytearray()
    for code,font,w,h,baseline in glyphs():
        image=Image.new('L',(w,h));ImageDraw.Draw(image).text((0,baseline),chr(code),font=font,fill=255,anchor='ls')
        pixels=list(image.get_flattened_data());raw=bytearray()
        for i in range(0,len(pixels),4):
            raw.append(sum(min(3,(pixels[i+j]+42)//85) << (6-j*2) for j in range(4)))
        i=0
        while i<len(raw):
            if raw[i]:data.append(raw[i]);i+=1
            else:
                end=i+1
                while end<len(raw) and raw[end]==0 and end-i<255:end+=1
                data.extend((0,end-i));i=end
        offsets.append(len(data));advances.append(max(1,round(font.getlength(chr(code)))))
    count=len(advances)
    blob=struct.pack('<4sIII',b'MSF1',count,0,0)+struct.pack('<'+'I'*len(offsets),*offsets)+bytes(advances)+data
    assert count==21439 and len(blob)<=0x300000, 'Font layout or partition capacity changed'
    (ROOT/'assets').mkdir(exist_ok=True)
    (ROOT/'assets/ui_font.bin').write_bytes(blob)
    print(f'{count} glyphs; {len(blob):,} flash bytes; source: {SOURCE.name}')

if __name__=='__main__':main()
