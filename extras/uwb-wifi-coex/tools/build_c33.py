#!/usr/bin/env python3
"""Compile and link an Arduino sketch for the Portenta C33 without arduino-cli.

It follows the recipes of ArduinoCore-renesas/platform.txt and boards.txt
(portenta_c33) and resolves libraries the way arduino-cli does: run the
preprocessor, read the first missing header, add the library that provides it,
repeat.

  python3 build_c33.py --core ../ArduinoCore-renesas \
      --libs ../PortentaUWBShield ../PortentaUWBWiFiCoex \
      --sketch ../PortentaUWBWiFiCoex/examples/CoexAnchorUdp --out ../build/coex

Default toolchain: arm-none-eabi-* on PATH. The official core pins
arm-none-eabi-gcc 7-2017q4; this script works with any recent GCC, but the
resulting binary is a compile/link check, not the binary the IDE produces.
"""
import argparse
import concurrent.futures as cf
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SRC_EXT = (".c", ".cpp", ".S")
MISSING_RE = re.compile(r"fatal error: ([^:]+): No such file or directory")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--core", required=True, help="ArduinoCore-renesas checkout")
    p.add_argument("--libs", nargs="*", default=[],
                   help="extra library folders (searched before the core libraries)")
    p.add_argument("--sketch", required=True, help="sketch folder")
    p.add_argument("--out", required=True, help="build folder")
    p.add_argument("--prefix", default="arm-none-eabi-")
    p.add_argument("--warnings", choices=["none", "default", "all"], default="none")
    p.add_argument("--warn-paths", nargs="*", default=[],
                   help="compile files under these paths with -Wall -Wextra")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    return p.parse_args()


FUNC_RE = re.compile(
    r"^(?!\s)(?!(?:if|else|for|while|switch|return|do|case|typedef|struct|class|enum|"
    r"namespace|using|template|static_assert)\b)"
    r"([A-Za-z_][\w\s\*&:<>,]*?[\s\*&])([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?\{?\s*$")


def sketch_prototypes(text):
    """Very small stand-in for the ctags based prototype generator of
    arduino-cli: top-level 'type name(args) {' definitions only."""
    lines = text.split("\n")
    protos, first, depth, in_comment = [], None, 0, False
    for i, line in enumerate(lines):
        stripped = line.strip()
        if in_comment:
            if "*/" in stripped:
                in_comment = False
            continue
        if stripped.startswith("/*") and "*/" not in stripped:
            in_comment = True
            continue
        if depth == 0 and not stripped.startswith(("#", "//")):
            m = FUNC_RE.match(line)
            nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
            if m and (line.rstrip().endswith("{") or nxt.startswith("{")):
                ret, name, args = m.group(1).strip(), m.group(2), m.group(3).strip()
                if name not in ("setup", "loop") and "=" not in args:
                    protos.append(f"{ret} {name}({args});")
                if first is None:
                    first = i
        code = re.sub(r'"(?:\\.|[^"\\])*"', "", line.split("//")[0])
        depth += code.count("{") - code.count("}")
    return protos, (first or 0)


class Library:
    def __init__(self, root: Path):
        self.root = root
        self.name = root.name
        props = {}
        lp = root / "library.properties"
        if lp.exists():
            for line in lp.read_text(encoding="utf-8", errors="replace").splitlines():
                if "=" in line and not line.lstrip().startswith("#"):
                    k, v = line.split("=", 1)
                    props[k.strip()] = v.strip()
        self.props = props
        self.src = root / "src" if (root / "src").is_dir() else root
        self.recursive = (root / "src").is_dir()
        self.precompiled = props.get("precompiled", "false") in ("true", "full")

    def headers(self):
        try:
            return {f.name for f in self.src.iterdir() if f.is_file()}
        except FileNotFoundError:
            return set()

    def sources(self):
        if self.props.get("precompiled") == "full":
            return []
        if self.recursive:
            files = walk_sources(self.src)
        else:
            files = [f for f in self.src.iterdir() if f.suffix in SRC_EXT]
            util = self.root / "utility"
            if util.is_dir():
                files += [f for f in util.iterdir() if f.suffix in SRC_EXT]
        # skip examples/extras that may sit under src in some libraries
        def skipped(f):
            rel = f.relative_to(self.root).parts
            return "examples" in rel or "extras" in rel
        return sorted(f for f in files if not skipped(f))

    def include_dirs(self):
        dirs = [self.src]
        if not self.recursive and (self.root / "utility").is_dir():
            dirs.append(self.root / "utility")
        return dirs

    def archives(self, mcu, fpu_dir):
        if not self.precompiled:
            return []
        cands = [self.src / mcu / fpu_dir, self.src / mcu]
        for c in cands:
            if c.is_dir():
                a = sorted(c.glob("*.a"))
                if a:
                    return a
        return []


class Builder:
    def __init__(self, a):
        self.a = a
        self.core = Path(a.core).resolve()
        self.out = Path(a.out).resolve()
        self.sketch = Path(a.sketch).resolve()
        self.variant = self.core / "variants" / "PORTENTA_C33"
        self.core_src = self.core / "cores" / "arduino"
        self.mcu = "cortex-m33"
        self.fpu_dir = "fpv5-sp-d16-hard"
        self.gcc = a.prefix + "gcc"
        self.gxx = a.prefix + "g++"
        self.warn_paths = [Path(p).resolve() for p in a.warn_paths]
        self.libs = [Library(Path(p).resolve()) for p in a.libs]
        core_libs = sorted((self.core / "libraries").iterdir())
        self.libs += [Library(p) for p in core_libs if p.is_dir()]
        self.used = []  # resolved Library objects, in discovery order
        self.out.mkdir(parents=True, exist_ok=True)

    # ---- flags -------------------------------------------------------------
    def common_defs(self):
        return ["-DARDUINO=10607", "-DARDUINO_PORTENTA_C33", "-DARDUINO_ARCH_RENESAS_PORTENTA",
                "-DARDUINO_ARCH_RENESAS", "-DARDUINO_FSP", "-D_XOPEN_SOURCE=700"]

    def fsp_flags(self):
        return ["-mthumb", f"@{self.variant/'defines.txt'}", "-DLWIP_DNS=1",
                "-DCFG_TUSB_MCU=OPT_MCU_RAXXX"]

    def includes(self):
        inc = [f"-I{self.core_src/'tinyusb'}", f"-I{self.core_src/'api/deprecated'}",
               f"-I{self.core_src/'api/deprecated-avr-comp'}",
               f"-I{self.core_src}", f"-I{self.variant}"]
        for lib in self.used:
            inc += [f"-I{d}" for d in lib.include_dirs()]
        inc += [f"-I{self.sketch}"]
        inc += [f"-iprefix{self.core}", f"@{self.variant/'includes.txt'}"]
        return inc

    def warn(self, src: Path):
        if any(str(src).startswith(str(w)) for w in self.warn_paths):
            return ["-Wall", "-Wextra"]
        return {"none": ["-w"], "default": [], "all": ["-Wall", "-Wextra"]}[self.a.warnings]

    def cmd_for(self, src: Path, obj: Path, preprocess_only=False):
        arch = [f"-mcpu={self.mcu}", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16"]
        defs = ["-DF_CPU=200000000"] + self.common_defs()
        if src.suffix == ".c":
            base = [self.gcc, "-c", *self.warn(src), "-Os", "-g3", "-nostdlib", *defs, "-MMD",
                    "-std=gnu11", *arch, "-fmessage-length=0", "-fsigned-char",
                    "-ffunction-sections", "-fdata-sections", "-fno-builtin", *self.fsp_flags()]
        elif src.suffix == ".S":
            base = [self.gxx, "-c", "-g", "-x", "assembler-with-cpp", "-Os", *arch,
                    "-fsigned-char", "-ffunction-sections", "-fdata-sections", *defs]
        else:
            base = [self.gxx, "-c", *self.warn(src), "-Os", "-g3", "-fno-use-cxa-atexit",
                    "-fno-rtti", "-fno-exceptions", "-MMD", "-nostdlib", *defs,
                    f'-DPROJECT_NAME="{self.out/self.sketch.name}"', "-std=gnu++17", *arch,
                    "-fsigned-char", "-ffunction-sections", "-fdata-sections",
                    "-fmessage-length=0", "-fno-builtin", *self.fsp_flags()]
        cmd = base + self.includes() + [str(src)]
        if preprocess_only:
            cmd = [c for c in cmd if c not in ("-c", "-MMD")] + ["-E", "-o", os.devnull]
        else:
            cmd += ["-o", str(obj)]
        return cmd

    # ---- sketch ------------------------------------------------------------
    def prepare_sketch(self):
        sk_out = self.out / "sketch"
        if sk_out.exists():
            shutil.rmtree(sk_out)
        sk_out.mkdir(parents=True)
        inos = sorted(self.sketch.glob("*.ino"))
        main = self.sketch / f"{self.sketch.name}.ino"
        if main in inos:
            inos.remove(main)
            inos.insert(0, main)
        body = ["#include <Arduino.h>"]
        for ino in inos:
            text = ino.read_text(encoding="utf-8")
            protos, first = sketch_prototypes(text)
            if protos:
                lines = text.split("\n")
                head, tail = lines[:first], lines[first:]
                body.append(f'#line 1 "{ino}"')
                body.append("\n".join(head))
                body.extend(protos)
                body.append(f'#line {first + 1} "{ino}"')
                body.append("\n".join(tail))
            else:
                body.append(f'#line 1 "{ino}"')
                body.append(text)
        cpp = sk_out / f"{self.sketch.name}.ino.cpp"
        cpp.write_text("\n".join(body) + "\n", encoding="utf-8")
        srcs = [cpp] + [f for f in self.sketch.iterdir() if f.suffix in SRC_EXT]
        return srcs

    # ---- library discovery -------------------------------------------------
    def find_lib(self, header):
        name = Path(header).name
        first = header.split("/")[0]
        for lib in self.libs:
            if lib in self.used:
                continue
            hs = lib.headers()
            if name in hs or (lib.src / header).exists() or first == lib.src.name:
                return lib
        return None

    def discover(self, seeds):
        queue = list(seeds)
        seen = set()
        while queue:
            src = queue.pop(0)
            if src in seen:
                continue
            seen.add(src)
            while True:
                r = subprocess.run(self.cmd_for(src, None, preprocess_only=True),
                                   capture_output=True, text=True)
                if r.returncode == 0:
                    break
                m = MISSING_RE.search(r.stderr)
                if not m:
                    sys.stderr.write(r.stderr)
                    raise SystemExit(f"preprocessing failed: {src}")
                lib = self.find_lib(m.group(1))
                if lib is None:
                    sys.stderr.write(r.stderr)
                    raise SystemExit(f"no library provides {m.group(1)} (needed by {src})")
                print(f"  lib  {lib.name:24s} <- {m.group(1)}")
                self.used.append(lib)
                queue.extend(lib.sources())

    # ---- compile -----------------------------------------------------------
    def obj_path(self, src: Path, group: str):
        h = hashlib.sha1(str(src).encode()).hexdigest()[:8]
        return self.out / "obj" / group / f"{src.stem}.{h}{src.suffix}.o"

    def compile_all(self, jobs):
        errors = []
        objs = []

        def one(item):
            src, obj = item
            obj.parent.mkdir(parents=True, exist_ok=True)
            r = subprocess.run(self.cmd_for(src, obj), capture_output=True, text=True)
            return src, obj, r

        with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
            for src, obj, r in ex.map(one, jobs_list(self)):
                if r.stderr.strip():
                    sys.stdout.write(r.stderr if r.returncode else _warnings_only(r.stderr))
                if r.returncode != 0:
                    errors.append(src)
                objs.append(obj)
        return objs, errors


def _warnings_only(text):
    return "".join(l + "\n" for l in text.splitlines() if "warning" in l or "note" in l
                   or l.startswith(" ") or "In function" in l or "In member" in l)


def walk_sources(root: Path):
    """Recursive source list that follows directory symlinks (cores/arduino/api
    and cores/arduino/tinyusb are symlinks into ArduinoCore-API / extras)."""
    out = []
    for d, _, files in os.walk(root, followlinks=True):
        for name in files:
            f = Path(d) / name
            if f.suffix in SRC_EXT and f.exists():
                out.append(f)
    return sorted(out)


def jobs_list(b: Builder):
    items = []
    for f in walk_sources(b.core_src):
        items.append((f, b.obj_path(f, "core")))
    # the variant is compiled recursively: tmp_gen_c_files/main.c holds main()
    for f in walk_sources(b.variant):
        items.append((f, b.obj_path(f, "core")))
    for lib in b.used:
        for f in lib.sources():
            items.append((f, b.obj_path(f, "lib_" + lib.name)))
    for f in b.sketch_srcs:
        items.append((f, b.obj_path(f, "sketch")))
    return items


def main():
    a = parse_args()
    b = Builder(a)
    print(f"sketch {b.sketch}")
    b.sketch_srcs = b.prepare_sketch()
    print("discovering libraries ...")
    b.discover(b.sketch_srcs)
    print("compiling ...")
    objs, errors = b.compile_all(a.jobs)
    if errors:
        print(f"\nBUILD FAILED: {len(errors)} file(s)")
        for e in errors:
            print("  ", e)
        return 1

    core_objs = [o for o in objs if "/obj/core/" in str(o)]
    other_objs = [o for o in objs if o not in core_objs]
    core_a = b.out / "core.a"
    if core_a.exists():
        core_a.unlink()
    subprocess.run([a.prefix + "ar", "rcs", str(core_a)] + [str(o) for o in core_objs], check=True)

    archives = []
    for lib in b.used:
        archives += [str(x) for x in lib.archives(b.mcu, b.fpu_dir)]
    elf = b.out / f"{b.sketch.name}.elf"
    link = [b.gxx, "-Wl,--gc-sections", "--specs=nosys.specs", "-w", f"-mcpu={b.mcu}",
            "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16", "-o", str(elf), f"-L{b.out}",
            f"-L{b.variant}", f"-T{b.variant/'fsp.ld'}", *[str(o) for o in other_objs],
            "-Wl,--whole-archive", "-Wl,--start-group", str(b.variant / "libs" / "libfsp.a"),
            str(core_a), "-Wl,--no-whole-archive", "-lstdc++", "-lsupc++", "-lm", "-lc",
            "-lgcc", "-lnosys", *archives, "-Wl,--end-group",
            f"-Wl,-Map,{b.out / (b.sketch.name + '.map')}"]
    print("linking ...")
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stderr)
        print("\nLINK FAILED")
        return 1
    if r.stderr.strip():
        sys.stdout.write(r.stderr)
    subprocess.run([a.prefix + "objcopy", "-O", "binary", "-j", ".text", "-j", ".data",
                    str(elf), str(elf.with_suffix(".bin"))], check=True)
    size = subprocess.run([a.prefix + "size", str(elf)], capture_output=True, text=True).stdout
    print(size)
    print("libraries:", ", ".join(l.name for l in b.used))
    print(f"OK  {elf}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
