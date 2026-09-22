#!/usr/bin/env python3
"""Build a full SH68F90 flash image from an SMK firmware hex and a stock dump.

The SMK build only emits the firmware region. The ISP bootloader lives at the
top of the flash, so a full image is: firmware bytes, an erased gap for the
settings/marker sectors, then the bootloader. Flashing this over ICP restores
the USB ISP path that `sinowisp` needs.

    utils/make_full_image.py --firmware build/royalkludge-rk61plus_default_smk.hex \
        --bootloader assets/keyboards/royalkludge-rk61plus/reference/rk61plus-stock-full.hex \
        --output build/royalkludge-rk61plus_full.hex
"""
import argparse


def parse_ihex(path):
    mem = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            n = int(line[1:3], 16)
            addr = int(line[3:7], 16)
            rec = int(line[7:9], 16)
            if rec != 0:
                continue
            for i in range(n):
                mem[addr + i] = int(line[9 + 2 * i : 11 + 2 * i], 16)
    return mem


def write_ihex(path, img):
    lines = []
    for base in range(0, len(img), 16):
        chunk = img[base : base + 16]
        rec = [len(chunk), (base >> 8) & 0xFF, base & 0xFF, 0] + list(chunk)
        rec.append((-sum(rec)) & 0xFF)
        lines.append(":" + "".join(f"{b:02X}" for b in rec))
    lines.append(":00000001FF")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware", required=True, help="SMK firmware .hex")
    ap.add_argument("--bootloader", required=True, help="stock dump .hex (firmware + bootloader)")
    ap.add_argument("--output", required=True, help="full image .hex to write")
    ap.add_argument("--flash-size", type=lambda s: int(s, 0), default=65536)
    ap.add_argument("--bootloader-start", type=lambda s: int(s, 0), default=0xF000)
    args = ap.parse_args()

    image = bytearray(b"\xff" * args.flash_size)
    fw = parse_ihex(args.firmware)
    for addr, byte in fw.items():
        if 0 <= addr < args.bootloader_start:
            image[addr] = byte
    bl = parse_ihex(args.bootloader)
    for addr, byte in bl.items():
        if args.bootloader_start <= addr < args.flash_size:
            image[addr] = byte

    fw_bytes = sum(1 for a in fw if a < args.bootloader_start)
    bl_bytes = sum(1 for a in bl if a >= args.bootloader_start)
    write_ihex(args.output, image)
    print(f"wrote {args.output}: {fw_bytes} firmware bytes, {bl_bytes} bootloader bytes")


if __name__ == "__main__":
    main()
