"""Build the Turn In Place (DWTurnInPlace) archive for Nexus.

Output: release/dist/TurnInPlace-<version>.zip. The archive carries the ue4ss/Mods/DWTurnInPlace/
path, so extracting it into Dawnwalker\Binaries\Win64 installs the mod:

    ue4ss/Mods/DWTurnInPlace/dlls/main.dll
    ue4ss/Mods/DWTurnInPlace/enabled.txt
    ue4ss/Mods/DWTurnInPlace/LICENSE
    ue4ss/Mods/DWTurnInPlace/mod_settings.ini
    ue4ss/Mods/DWTurnInPlace/config/turninplace.ini

No PDB: it carries build paths.

Source: mod/ in this repo; mod/dlls/main.dll is the git-ignored build output of src/.
Version: ModVersion in src/dllmain.cpp. The build fails if:
  - ModVersion, mod_settings.ini [Mod] Version and the newest entry in the changelog block of
    nexus-page-metadata.md disagree;
  - a ConfigKey in mod_settings.ini is missing from its ConfigSection in turninplace.ini or present
    more than once (either stops the Mod Menu page opening);
  - a shipped turninplace.ini value is outside its setting's Minimum/Maximum, off its Step, or not in
    its PresetValues;
  - main.dll is older than the newest file in src/ (rebuild first);
  - mod/ has a scripts/ subfolder (UE4SS would log a red main.lua error on every start).

Usage: python release/Build-Package.py (from any directory)
"""
from __future__ import annotations

import re
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
MOD = REPO / "mod"
SRC = REPO / "src"
SHEET = HERE / "nexus-page-metadata.md"
DIST = HERE / "dist"
MOD_NAME = "DWTurnInPlace"
INI_REL = "config/turninplace.ini"

BUILD_HINT = ("cmake --build <build dir> "
              "--config Game__Shipping__Win64 --target DWTurnInPlace --parallel")


def fail(msg: str) -> None:
    sys.exit(f"FAIL: {msg}")


def mod_version() -> str:
    m = re.search(r'ModVersion\s*=\s*STR\("([^"]+)"\)', (SRC / "dllmain.cpp").read_text(encoding="utf-8"))
    if not m:
        fail("ModVersion not found in dllmain.cpp")
    return m.group(1)


def ini_sections(path: Path) -> dict[str, dict[str, list[str]]]:
    """Every key = value line per section, comments stripped. A list per key, so duplicates show."""
    sections: dict[str, dict[str, list[str]]] = {}
    current = sections.setdefault("", {})
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.split(";", 1)[0].strip()
        m = re.match(r"^\[(.+)\]$", s)
        if m:
            current = sections.setdefault(m.group(1), {})
            continue
        m = re.match(r"^([\w.]+)\s*=\s*(.*)$", s)
        if m:
            current.setdefault(m.group(1), []).append(m.group(2).strip())
    return sections


def manifest_sections(manifest: Path) -> dict[str, dict[str, str]]:
    return {name: {k: v[-1] for k, v in keys.items()} for name, keys in ini_sections(manifest).items() if name}


def newest_changelog_version() -> str:
    text = SHEET.read_text(encoding="utf-8")
    head = text.find("## Changelog tab")
    if head < 0:
        fail(f"no '## Changelog tab' section in {SHEET.name}")
    m = re.search(r"^```\s*\n\s*(\d+(?:\.\d+)+)\s+-", text[head:], re.M)
    if not m:
        fail(f"no changelog entry found in {SHEET.name}")
    return m.group(1)


def check_versions(ver: str, sections: dict[str, dict[str, str]]) -> None:
    manifest_ver = sections.get("Mod", {}).get("Version")
    log_ver = newest_changelog_version()
    if not (ver == manifest_ver == log_ver):
        fail(f"versions disagree: dllmain.cpp ModVersion {ver!r}, mod_settings.ini [Mod] Version "
             f"{manifest_ver!r}, newest changelog entry {log_ver!r}")


def check_settings(sections: dict[str, dict[str, str]], ini: dict[str, dict[str, list[str]]]) -> int:
    errors = []
    count = 0
    for name, sec in sections.items():
        if not name.startswith("Setting."):
            continue
        key = sec.get("ConfigKey")
        if not key:
            errors.append(f"[{name}] has no ConfigKey")
            continue
        if sec.get("ConfigFile") != INI_REL:
            errors.append(f"{key}: ConfigFile {sec.get('ConfigFile')!r}, expected {INI_REL!r}")
            continue
        count += 1
        section = sec.get("ConfigSection", "")
        got = ini.get(section, {}).get(key, [])
        if len(got) != 1:
            errors.append(f"{key}: {len(got)} lines in [{section}] of turninplace.ini, needs exactly 1")
            continue
        try:
            v = float(got[0])
        except ValueError:
            errors.append(f"{key} = {got[0]!r} is not a number")
            continue
        if "PresetValues" in sec:
            if v not in [float(p) for p in sec["PresetValues"].split("|")]:
                errors.append(f"{key} = {got[0]} not in PresetValues {sec['PresetValues']}")
            continue
        lo, hi = float(sec["Minimum"]), float(sec["Maximum"])
        if not lo <= v <= hi:
            errors.append(f"{key} = {got[0]} outside {sec['Minimum']}..{sec['Maximum']}")
        step = float(sec.get("Step", "0") or 0)
        if step > 0:
            steps = (v - lo) / step
            if abs(steps - round(steps)) > 1e-6:
                errors.append(f"{key} = {got[0]} off Step {sec['Step']} from Minimum {sec['Minimum']}")
    if errors:
        fail("mod_settings.ini vs turninplace.ini:\n  " + "\n  ".join(errors))
    return count


def check_dll_fresh(dll: Path) -> None:
    if not dll.is_file():
        fail(f"{dll} missing; build it:\n  {BUILD_HINT}")
    newest = max((p for p in SRC.rglob("*") if p.is_file()), key=lambda p: p.stat().st_mtime)
    if dll.stat().st_mtime < newest.stat().st_mtime:
        fail(f"main.dll is older than src/{newest.name}; rebuild:\n  {BUILD_HINT}")


def main() -> int:
    manifest = MOD / "mod_settings.ini"
    ini = MOD / INI_REL
    dll = MOD / "dlls" / "main.dll"
    for p in (manifest, ini, MOD / "LICENSE", MOD / "enabled.txt"):
        if not p.is_file():
            fail(f"missing {p}")

    ver = mod_version()
    sections = manifest_sections(manifest)
    check_versions(ver, sections)
    settings = check_settings(sections, ini_sections(ini))
    check_dll_fresh(dll)
    if (MOD / "scripts").exists():
        fail("mod/scripts exists: UE4SS would start a Lua mod and log a red main.lua error")

    DIST.mkdir(exist_ok=True)
    out = DIST / f"TurnInPlace-{ver}.zip"
    prefix = f"ue4ss/Mods/{MOD_NAME}/"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dll, prefix + "dlls/main.dll")
        z.writestr(prefix + "enabled.txt", "")
        z.write(MOD / "LICENSE", prefix + "LICENSE")
        z.write(manifest, prefix + "mod_settings.ini")
        z.write(ini, prefix + INI_REL)
    print(f"version {ver}")
    print(f"{settings} Mod Menu settings checked against turninplace.ini")
    print(out, f"{out.stat().st_size:,} bytes")
    for i in zipfile.ZipFile(out).infolist():
        print(f"  {i.file_size:>9,} {i.filename}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
