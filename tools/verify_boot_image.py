#!/usr/bin/env python3
"""Verify the probe boot image and (optionally) the compiled probe modules.

Checks that matter for `fastboot boot`:

  * Android boot header v0 with the vermeer addresses
  * ramdisk_size = 0
  * gzip(BootShim + FD) followed by the appended vermeer DTB
  * FD size 0x300000 and a valid FV header
  * the build tree really contains VermeerProbe.efi / VermeerProbeDxe.efi
"""

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

PAGE_SIZE = 4096
FD_SIZE = 0x300000
DTB_MAGIC = b"\xD0\x0D\xFE\xED"
APP_GUID = bytes.fromhex("912a3c7f4d5b684e9a106c2d8e4f1b37")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")
    print(f"ok: {message}")


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def verify_image(path: Path) -> None:
    data = path.read_bytes()
    check(len(data) >= PAGE_SIZE, "boot image has a header page")
    check(data[:8] == b"ANDROID!", "Android boot magic")

    kernel_size = u32(data, 8)
    kernel_addr = u32(data, 12)
    ramdisk_size = u32(data, 16)
    ramdisk_addr = u32(data, 20)
    second_size = u32(data, 24)
    second_addr = u32(data, 28)
    tags_addr = u32(data, 32)
    page_size = u32(data, 36)
    header_version = u32(data, 40)

    check(page_size == PAGE_SIZE, f"page size is {PAGE_SIZE}")
    check(header_version == 0, "boot header is v0")
    check(kernel_addr == 0x8000, "kernel address is 0x00008000")
    check(tags_addr == 0x1E00000, "tags address is 0x01e00000")
    check(ramdisk_size == 0, "ramdisk_size is 0")
    check(second_size == 0, "second_size is 0")
    check(ramdisk_addr == 0, "ramdisk address is 0 (unused)")
    check(second_addr == 0, "second address is 0 (unused)")

    kernel = data[page_size:page_size + kernel_size]
    check(len(kernel) == kernel_size, f"kernel payload is {kernel_size} bytes")

    expected_size = page_size * (1 + (kernel_size + page_size - 1) // page_size)
    check(len(data) == expected_size, f"file size is {expected_size} bytes")

    decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
    payload = decompressor.decompress(kernel) + decompressor.flush()
    rest = decompressor.unused_data
    check(len(payload) > FD_SIZE, "gzip payload is larger than the FD")

    shim_size = len(payload) - FD_SIZE
    check(64 <= shim_size <= 4096, f"BootShim payload is {shim_size} bytes")
    fd = payload[-FD_SIZE:]
    check(fd[40:44] == b"_FVH", "FD has a firmware-volume header")

    check(len(rest) >= 8, "appended DTB is present")
    check(rest[:4] == DTB_MAGIC, "appended DTB magic")
    dtb_size = be32(rest, 4)
    check(dtb_size == len(rest), f"DTB totalsize {dtb_size} matches the appended bytes")
    check(dtb_size > 0x1000, "DTB is non-empty")

    print()
    print(f"image:        {path}")
    print(f"size:         {len(data)} bytes")
    print(f"kernel:       {kernel_size} bytes (gzip payload + DTB)")
    print(f"shim:         {shim_size} bytes")
    print(f"FD:           {FD_SIZE} bytes")
    print(f"DTB:          {dtb_size} bytes")
    print(f"ramdisk:      {ramdisk_size} bytes")
    print(f"kernel addr:  0x{kernel_addr:08x}")
    print(f"tags addr:    0x{tags_addr:08x}")
    print(f"page size:    {page_size}")
    print(f"header:       v{header_version}")
    print(f"SHA-256:      {hashlib.sha256(data).hexdigest()}")


def locate_one(root: Path, name: str) -> Path:
    matches = sorted(root.glob(f"Build/vermeerProbePkg/**/{name}"))
    check(len(matches) == 1, f"found exactly one {name} in the build tree")
    return matches[0]


def verify_build_tree(root: Path) -> None:
    app = locate_one(root, "VermeerProbe.efi")
    dxe = locate_one(root, "VermeerProbeDxe.efi")

    app_data = app.read_bytes()
    dxe_data = dxe.read_bytes()
    check(app_data[:2] == b"MZ", "VermeerProbe.efi is a PE image")
    check(dxe_data[:2] == b"MZ", "VermeerProbeDxe.efi is a PE image")
    check(b"VERMEER PROBE v1" in app_data, "probe banner is compiled in")
    check(b"CNTFRQ %lu Hz" in app_data, "CNTFRQ output is compiled in")
    check("vermeer:probe:v1".encode("utf-16le") in dxe_data, "DXE marker is compiled in")
    check(APP_GUID in dxe_data, "DXE references the probe application FILE_GUID")

    print()
    print(f"app:          {app}")
    print(f"registrar:    {dxe}")
    print(f"app SHA-256:  {hashlib.sha256(app_data).hexdigest()}")
    print(f"DXE SHA-256:  {hashlib.sha256(dxe_data).hexdigest()}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Mu-vermeerProbe.img")
    parser.add_argument("--uefi", type=Path, help="uefi-vermeer checkout for build-tree checks")
    args = parser.parse_args()

    if not args.image.is_file():
        raise SystemExit(f"FAIL: no such image: {args.image}")

    verify_image(args.image)
    if args.uefi is not None:
        verify_build_tree(args.uefi.resolve())


if __name__ == "__main__":
    main()
