#!/usr/bin/env python3
"""Turn a clean uefi-vermeer checkout into a vermeerProbePkg build tree.

The upstream vermeer port is a UniXNU device package: its DSC/FDF reference
UniXNUPkg and DarwinBootPkg, which live in other repositories. This script
creates a separate package that contains only the hardware probe:

  * copies Platforms/Xiaomi/vermeerPkg -> Platforms/Xiaomi/vermeerProbePkg
  * renames the package references (dsc/fdf/dec/inf/inc/py)
  * drops UniXNUPkg, UniXNULib and DarwinBootPkg from the DSC/FDF
  * disables the Chinese UI and adds VermeerProbe + VermeerProbeDxe
  * raises the DEBUG font scale to 6 and shortens the per-line delay
  * installs the extracted vermeer.dtb as Resources/DTBs/vermeerProbe.dtb
  * empties Resources/ramdisk (the probe image must have ramdisk_size = 0)

It fails loudly if the pinned upstream layout no longer matches, so a silent
fallback to the stock vermeer firmware is impossible.
"""

import argparse
import re
import shutil
import struct
import sys
import tomllib
from pathlib import Path

TEXT_SUFFIXES = {".dsc", ".fdf", ".inf", ".dec", ".py", ".inc"}


def fail(message: str) -> None:
    raise SystemExit(f"prepare_probe: {message}")


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        fail(f"{path} is not UTF-8: {error}")


def write_text(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def replace_once(text: str, old: str, new: str, what: str) -> str:
    count = text.count(old)
    if count != 1:
        fail(f"expected exactly one {what}, found {count}")
    return text.replace(old, new, 1)


def sub_once(text: str, pattern: str, replacement: str, what: str) -> str:
    new_text, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
    if count != 1:
        fail(f"expected exactly one {what}, found {count}")
    return new_text


def remove_lines(text: str, pattern: str, what: str, expected: int) -> str:
    new_text, count = re.subn(pattern, "", text, flags=re.MULTILINE)
    if count != expected:
        fail(f"expected {expected} {what} line(s), found {count}")
    return new_text


def copy_package(root: Path, destination: Path) -> tuple[Path, Path]:
    source = root / "Platforms/Xiaomi/vermeerPkg"
    if not source.is_dir():
        fail(f"missing upstream package: {source}")

    if destination.exists():
        print(f"removing existing generated package: {destination}")
        shutil.rmtree(destination)

    shutil.copytree(source, destination, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    for path in destination.rglob("*"):
        if not path.is_file() or path.suffix not in TEXT_SUFFIXES:
            continue
        text = read_text(path)
        if "vermeerPkg" not in text:
            continue
        write_text(path, text.replace("vermeerPkg", "vermeerProbePkg"))

    old_dsc = destination / "vermeer.dsc"
    old_fdf = destination / "vermeer.fdf"
    new_dsc = destination / "vermeerProbe.dsc"
    new_fdf = destination / "vermeerProbe.fdf"
    if not old_dsc.is_file() or not old_fdf.is_file():
        fail("upstream vermeer.dsc/vermeer.fdf not found after copy")
    old_dsc.rename(new_dsc)
    old_fdf.rename(new_fdf)
    return new_dsc, new_fdf


def patch_dsc(path: Path) -> None:
    text = read_text(path)

    text = sub_once(
        text,
        r"^(\s*PLATFORM_NAME\s*=\s*)Vermeer\s*$",
        r"\1VermeerProbe",
        "PLATFORM_NAME",
    )
    text = sub_once(
        text,
        r"^(\s*VERMEER_CHINESE_UI\s*=\s*)1\s*$",
        r"\g<1>0",
        "VERMEER_CHINESE_UI = 1",
    )
    text = replace_once(
        text,
        "vermeerProbePkg/vermeer.fdf",
        "vermeerProbePkg/vermeerProbe.fdf",
        "FLASH_DEFINITION",
    )
    text = sub_once(
        text,
        r"^(\s*gSiliciumPkgTokenSpaceGuid\.PcdFrameBufferFontScale\|)3\b",
        r"\g<1>6",
        "PcdFrameBufferFontScale|3",
    )
    text = sub_once(
        text,
        r"^(\s*gSiliciumPkgTokenSpaceGuid\.PcdFrameBufferDelay\|)30000\b",
        r"\g<1>2000",
        "PcdFrameBufferDelay|30000",
    )
    text = sub_once(
        text,
        r"^(\s*gSiliciumPkgTokenSpaceGuid\.PcdFrameBufferDelay\|2000[^\r\n]*\r?\n)",
        r"\1  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferFreezeWhenFull|FALSE\n",
        "PcdFrameBufferDelay insertion point",
    )

    text = remove_lines(text, r"^\s*UniXNUPkg/[^\r\n]*\r?\n", "UniXNUPkg component", 2)
    text = remove_lines(text, r"^\s*UniXNULib\|[^\r\n]*\r?\n", "UniXNULib library", 1)
    text = remove_lines(text, r"^\s*DarwinBootPkg/[^\r\n]*\r?\n", "DarwinBootPkg component", 1)

    for token in ("UniXNUPkg", "UniXNULib", "DarwinBootPkg"):
        if token in text:
            fail(f"{token} still referenced by {path}")

    components = "[Components]\n"
    if text.count(components) != 1:
        fail(f"expected one [Components] section in {path}")

    additions = (
        "  #\n"
        "  # Hardware probe: the DXE registrar creates the Boot#### entry, the\n"
        "  # application prints CPU/GOP/memory facts and waits for a key.\n"
        "  #\n"
        "  vermeerProbePkg/Drivers/VermeerProbe/VermeerProbeDxe.inf\n"
        "  vermeerProbePkg/Drivers/VermeerProbe/VermeerProbe.inf\n"
    )
    text = text.replace(components, components + additions, 1)
    write_text(path, text)


def patch_fdf(path: Path) -> None:
    text = read_text(path)

    unixnu_block = (
        "  # UniXNU\n"
        "  INF UniXNUPkg/Application/UniXNU/UniXNU.inf\n"
        "  INF UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf\n"
    )
    text = replace_once(text, unixnu_block, "", "UniXNU FDF block")
    if "UniXNUPkg" in text:
        fail(f"UniXNUPkg still referenced by {path}")

    text = replace_once(
        text,
        "  !include Include/RAW.inc\n",
        "  !include Include/RAW.inc\n\n"
        "  # Probe application. The registrar in Include/DXE.inc points a\n"
        "  # Boot#### option at this FV file.\n"
        "  INF vermeerProbePkg/Drivers/VermeerProbe/VermeerProbe.inf\n",
        "RAW.inc include",
    )
    write_text(path, text)


def patch_dxe_inc(path: Path) -> None:
    text = read_text(path)

    # The probe build does not contain DarwinBoot; leaving this INF in the FDF
    # makes GenFv fail with a missing-package error.
    xnu_block = (
        "  # The XNU loader is part of the firmware: a driver loaded from a volume is\n"
        "  # refused by the image verification of this build.\n"
        "  INF DarwinBootPkg/Driver/DarwinBoot.inf\n"
    )
    probe_block = (
        "  # Probe boot-option registrar. Same variable-services DEPEX as the\n"
        "  # Chinese UI registrar, so DXE dispatches it before BDS starts.\n"
        "  INF vermeerProbePkg/Drivers/VermeerProbe/VermeerProbeDxe.inf\n"
    )
    text = replace_once(text, xnu_block, probe_block, "DarwinBoot DXE entry")
    if "DarwinBootPkg" in text:
        fail(f"DarwinBootPkg still referenced by {path}")

    marker = "!if $(VERMEER_CHINESE_UI) == 1\n"
    if text.count(marker) != 1:
        fail(f"expected one Chinese UI guard in {path}")

    write_text(path, text)


def patch_device_build(path: Path) -> None:
    text = read_text(path)

    unixnu_line = (
        '        os.path.join(os.environ.get("UNIXNU_ROOT", '
        'os.path.join(WorkspaceRoot, "..", "UniXNU")), "package"),\n'
    )
    if text.count(unixnu_line) != 1:
        fail(f"expected one UNIXNU_ROOT package path in {path}")
    text = text.replace(unixnu_line, "", 1)

    old_dsc = "vermeerProbePkg/vermeer.dsc"
    if text.count(old_dsc) != 2:
        fail(f"expected two {old_dsc} references in {path}, found {text.count(old_dsc)}")
    text = text.replace(old_dsc, "vermeerProbePkg/vermeerProbe.dsc")

    if "UNIXNU_ROOT" in text:
        fail(f"UNIXNU_ROOT still referenced by {path}")

    write_text(path, text)


def install_probe(scaffold: Path, destination: Path) -> None:
    source = scaffold / "probe"
    target = destination / "Drivers/VermeerProbe"
    files = (
        "VermeerProbe.c",
        "VermeerProbe.inf",
        "VermeerProbeDxe.c",
        "VermeerProbeDxe.inf",
    )

    for name in files:
        if not (source / name).is_file():
            fail(f"missing scaffold file: {source / name}")

    target.mkdir(parents=True, exist_ok=True)
    for name in files:
        shutil.copy2(source / name, target / name)


def install_dtb(scaffold: Path, root: Path) -> None:
    source = scaffold / "dtb/vermeer.dtb"
    if not source.is_file():
        fail(f"missing DTB: {source}")

    data = source.read_bytes()
    if len(data) < 0x1000 or data[:4] != b"\xD0\x0D\xFE\xED":
        fail(f"{source} is not a flattened device tree")

    totalsize = struct.unpack_from(">I", data, 4)[0]
    if totalsize < 0x1000 or totalsize > len(data):
        fail(f"{source}: invalid FDT totalsize {totalsize} (file {len(data)})")

    target = root / "Resources/DTBs/vermeerProbe.dtb"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data[:totalsize])


def install_config(root: Path) -> None:
    source = root / "Resources/Configs/vermeer.toml"
    target = root / "Resources/Configs/vermeerProbe.toml"
    if not source.is_file():
        fail(f"missing upstream config: {source}")
    shutil.copy2(source, target)

    with target.open("rb") as handle:
        config = tomllib.load(handle)

    expected = {
        "uefi_fd": {"base": 0xA7100000, "size": 0x300000},
        "boot_image": {
            "header_version": 0,
            "base": 0,
            "kernel_offset": 0x8000,
            "tags_offset": 0x1E00000,
            "pagesize": 4096,
        },
    }
    for section, values in expected.items():
        if section not in config:
            fail(f"{target}: missing [{section}]")
        for key, value in values.items():
            if config[section].get(key) != value:
                fail(
                    f"{target}: [{section}] {key} = {config[section].get(key)!r}, "
                    f"expected {value!r}"
                )


def empty_ramdisk(root: Path) -> None:
    ramdisk = root / "Resources/ramdisk"
    if not ramdisk.is_file():
        fail(f"missing upstream ramdisk: {ramdisk}")
    ramdisk.write_bytes(b"")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uefi", type=Path, default=Path("uefi"), help="uefi-vermeer checkout")
    parser.add_argument(
        "--scaffold",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="directory that contains probe/ and dtb/",
    )
    args = parser.parse_args()

    root = args.uefi.resolve()
    scaffold = args.scaffold.resolve()
    if not (root / "build_uefi.py").is_file():
        fail(f"{root} does not look like a uefi-vermeer checkout")

    destination = root / "Platforms/Xiaomi/vermeerProbePkg"
    dsc, fdf = copy_package(root, destination)
    patch_dsc(dsc)
    patch_fdf(fdf)
    patch_dxe_inc(destination / "Include/DXE.inc")
    patch_device_build(destination / "DeviceBuild.py")
    install_probe(scaffold, destination)
    install_dtb(scaffold, root)
    install_config(root)
    empty_ramdisk(root)

    print(f"prepared probe package: {destination}")
    print(f"  DSC:      {dsc}")
    print(f"  FDF:      {fdf}")
    print(f"  DTB:      {root / 'Resources/DTBs/vermeerProbe.dtb'}")
    print(f"  ramdisk:  {root / 'Resources/ramdisk'} (0 bytes)")
    print("next:")
    print(f"  cd {root}")
    print("  CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu- python3 build_uefi.py -d vermeerProbe -r DEBUG -c -m 0")


if __name__ == "__main__":
    main()
