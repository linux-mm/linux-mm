#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
trigger-view.py - Load a test module, trigger it with kcov_dataflow
recording active, then pretty-print the captured records.

Usage:
    python3 trigger-view.py eight_struct_args_c
    python3 trigger-view.py rust_ffi_contract --raw -C 8
    python3 trigger-view.py rust_kworker_remote --remote
    python3 trigger-view.py <module> --vmlinux vmlinux --kaslr-offset 0x...

run_capture() does the work and is also what test_modules.py drives:
  1. Opens /sys/kernel/debug/kcov_dataflow, inits and mmaps the buffer
  2. Loads the module via finit_module() (its init noise is not recorded)
  3. Enables recording: KCOV_DF_ENABLE for this task, or with --remote
     KCOV_DF_REMOTE_ENABLE with handle REMOTE_HANDLE, which the module's
     kworker opens with kcov_df_remote_start(REMOTE_HANDLE)
  4. Writes the trigger file(s) the module created under TRIGGER_DIR
  5. Disables recording and unloads the module
  6. Parses the records (layout: include/uapi/linux/kcov_dataflow.h)

The CLI then prints them as a call tree, or flat with --raw, with kallsyms
symbol resolution and addr2line source lines (vmlinux / module .ko).

Recorded PCs have the KASLR offset removed (same as mainline kcov), so
the runtime offset is derived from /proc/kallsyms and System.map / vmlinux
(or a per-architecture default) and added back for symbolization; use
--kaslr-offset to override. Records must contain at least one value word
and one of the three record types, otherwise the parser resyncs word by
word (e.g. after a userspace reset of area[0] mid-run).
"""
import os
import sys
import struct
import ctypes
import ctypes.util
import argparse
import fcntl
import platform
import subprocess
import shutil

# Constants -- must match include/uapi/linux/kcov_dataflow.h
DF_TYPE_CMP = 0xC
DF_TYPE_ENTRY = 0xE
DF_TYPE_RET = 0xF
MAGIC_BAD = 0xBADADD85
BUF_SIZE = 1048576  # 1M words = 8MB

# Record header word: bits 0-23 seq | 28-31 type | 32-47 nvals |
# 48-55 arg/ret size | 56-63 arg index. Word 1 is the pc (KASLR offset
# removed, like mainline kcov), word 2 the traced pointer (ENTRY/RET) or the
# comparison type (CMP), then nvals value words.
def hdr_seq(h):
    return h & 0x00FFFFFF

def hdr_type(h):
    return (h >> 28) & 0xF

def hdr_nvals(h):
    return (h >> 32) & 0xFFFF

def hdr_size(h):
    return (h >> 48) & 0xFF

def hdr_arg_idx(h):
    return (h >> 56) & 0xFF

RECORD_HDR_WORDS = 3

# Runtime KASLR offset (see kaslr_offset()); added back to every recorded pc
# so /proc/kallsyms lookups work, subtracted again for addr2line on vmlinux.
KASLR_OFFSET = 0

# Ioctl numbers
def _IOR(t, nr, size):
    return (2 << 30) | (ord(t) << 8) | nr | (size << 16)

def _IOW(t, nr, size):
    return (1 << 30) | (ord(t) << 8) | nr | (size << 16)

def _IO(t, nr):
    return (ord(t) << 8) | nr

KCOV_DF_INIT_TRACK = _IOR('d', 1, 8)
KCOV_DF_ENABLE = _IO('d', 100)
KCOV_DF_DISABLE = _IO('d', 101)
KCOV_DF_REMOTE_ENABLE = _IOW('d', 102, 8)  # arg: pointer to a __u64 handle
KCOV_DF_REMOTE_DISABLE = _IO('d', 103)

KCOV_DF_PATH = "/sys/kernel/debug/kcov_dataflow"

# Every test module creates its trigger file(s) in this debugfs directory;
# writing to them runs the instrumented test functions.
TRIGGER_DIR = "/sys/kernel/debug/kcov_dataflow_test"

# Remote handle registered with KCOV_DF_REMOTE_ENABLE; must match the
# kcov_df_remote_start(1) call in the rust_kworker_remote test module
# (KCOV_SUBSYSTEM_COMMON, instance 1).
REMOTE_HANDLE = 1

# syscall numbers
_machine = platform.machine()
if _machine == "aarch64":
    SYS_FINIT_MODULE = 273
    SYS_DELETE_MODULE = 106
else:  # x86_64
    SYS_FINIT_MODULE = 313
    SYS_DELETE_MODULE = 176

SELFTEST_DIR = os.path.dirname(os.path.abspath(__file__))


def load_kallsyms():
    """Load kernel symbols for PC resolution."""
    syms = []
    try:
        with open("/proc/kallsyms") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 3:
                    addr = int(parts[0], 16)
                    name = parts[2]
                    mod = parts[3].strip("[]") if len(parts) > 3 else ""
                    syms.append((addr, name, mod))
    except (PermissionError, FileNotFoundError):
        pass
    syms.sort()
    return syms


def runtime_text(syms):
    """Runtime address of _text from kallsyms, 0 if hidden."""
    return next((a for a, n, m in syms if n == "_text" and not m), 0)


# Link-time address of _text per architecture, used only when neither
# System.map nor vmlinux is available: x86_64 __START_KERNEL
# (__START_KERNEL_map + CONFIG_PHYSICAL_START), arm64 KIMAGE_VADDR.
LINKTIME_TEXT_DEFAULT = {
    "x86_64": 0xffffffff81000000,
    "aarch64": 0xffff800080000000,
}


def linktime_text(vmlinux=None):
    """Return (link-time address of _text, source description) or (0, "")."""
    rel = os.uname().release
    candidates = []
    if vmlinux:
        candidates.append(os.path.join(os.path.dirname(vmlinux) or ".", "System.map"))
    candidates += ["System.map", f"/boot/System.map-{rel}",
                   f"/usr/lib/debug/boot/System.map-{rel}"]
    for sm in candidates:
        try:
            with open(sm) as f:
                for line in f:
                    parts = line.split()
                    if len(parts) == 3 and parts[2] == "_text":
                        return int(parts[0], 16), sm
        except (OSError, ValueError):
            continue
    if vmlinux and shutil.which("nm"):
        try:
            r = subprocess.run(["nm", "--defined-only", vmlinux],
                               capture_output=True, text=True, timeout=300)
            for line in r.stdout.splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[2] == "_text":
                    return int(parts[0], 16), f"nm {vmlinux}"
        except (OSError, subprocess.TimeoutExpired):
            pass
    link = LINKTIME_TEXT_DEFAULT.get(platform.machine(), 0)
    return link, f"{platform.machine()} default" if link else ""


def kaslr_offset(syms, vmlinux=None):
    """
    Runtime KASLR offset: recorded PCs have it removed (kcov's
    canonicalize_ip()), /proc/kallsyms has it applied. Computed as the
    runtime _text (kallsyms) minus the link-time _text (System.map, nm
    vmlinux, or the architecture default). KASLR offsets are 2 MiB aligned
    on x86_64 and arm64, which is used as a sanity check on the result.
    """
    runtime = runtime_text(syms)
    if not runtime:
        print("# warning: _text not in /proc/kallsyms (kptr_restrict?); "
              "PCs will not symbolize", file=sys.stderr)
        return 0
    link, source = linktime_text(vmlinux)
    if not link:
        print(f"# warning: no System.map/vmlinux and no default _text for "
              f"{platform.machine()}; pass --kaslr-offset", file=sys.stderr)
        return 0
    off = runtime - link
    if off % (2 << 20):
        print(f"# warning: kaslr offset 0x{off:x} from {source} is not 2 MiB "
              f"aligned; check CONFIG_PHYSICAL_START/KIMAGE_VADDR or pass "
              f"--kaslr-offset", file=sys.stderr)
    return off


# Rust symbol demangling via llvm-cxxfilt or rustfilt
_demangler = None

def _init_demangler():
    global _demangler
    for tool in ["llvm-cxxfilt", "rustfilt", "c++filt"]:
        path = shutil.which(tool)
        if path:
            _demangler = path
            return
    _demangler = ""

_demangled = {}

def demangle(name):
    """Demangle a Rust/C++ symbol name (memoized: one process per name)."""
    global _demangler
    if _demangler is None:
        _init_demangler()
    if not _demangler or not name.startswith("_R"):
        return name
    if name not in _demangled:
        try:
            r = subprocess.run([_demangler, name], capture_output=True,
                               text=True, timeout=2)
            _demangled[name] = r.stdout.strip() if r.returncode == 0 else name
        except (OSError, subprocess.TimeoutExpired):
            _demangled[name] = name
    return _demangled[name]


def find_vmlinux(vmlinux=None):
    """Locate vmlinux for addr2line: explicit path, else the usual places."""
    if vmlinux:
        return vmlinux
    for p in ["vmlinux", "/boot/vmlinux", "/usr/lib/debug/boot/vmlinux"]:
        if os.path.exists(p):
            return p
    return None


def _a2l_target(pc, vmlinux, ko_path, mod_text_base):
    """(binary, address in it) to symbolize pc with, or None."""
    if ko_path and mod_text_base and pc >= mod_text_base:
        return ko_path, pc - mod_text_base
    if vmlinux:
        return vmlinux, pc - KASLR_OFFSET  # vmlinux holds link-time addresses
    return None


def resolve_lines(pcs, vmlinux, cache, ko_path=None, mod_text_base=0):
    """
    Resolve every pc in @pcs to file:line into @cache, one addr2line run
    per binary: a DWARF5 vmlinux takes hundreds of ms to open, so one
    process per record does not scale to thousands of records.
    """
    todo = {}
    for pc in pcs:
        if pc in cache:
            continue
        cache[pc] = ""
        tgt = _a2l_target(pc, vmlinux, ko_path, mod_text_base)
        if tgt:
            todo.setdefault(tgt[0], []).append((pc, tgt[1]))
    for binary, pairs in todo.items():
        try:
            r = subprocess.run(
                ["addr2line", "-e", binary] + [f"0x{a:x}" for _, a in pairs],
                capture_output=True, text=True, timeout=300)
        except (subprocess.TimeoutExpired, FileNotFoundError):
            continue
        for (pc, _), loc in zip(pairs, r.stdout.splitlines()):
            loc = loc.strip()
            if loc and loc != "??:0" and loc != "??:?":
                # Shorten path: keep only filename:line
                cache[pc] = loc.rsplit("/", 1)[-1]


def resolve_line(pc, vmlinux, cache, ko_path=None, mod_text_base=0):
    """Resolve one PC to source file:line using addr2line (cached)."""
    if pc not in cache:
        resolve_lines([pc], vmlinux, cache, ko_path, mod_text_base)
    return cache[pc]


def get_kernel_meta():
    """Collect kernel build metadata."""
    meta = {"release": os.uname().release}
    try:
        with open("/proc/version") as f:
            v = f.read().strip()
        meta["version"] = v
        # Extract compiler version
        if "gcc" in v.lower():
            meta["compiler"] = v.split("(")[1].split(")")[0] if "(" in v else ""
        elif "clang" in v.lower():
            idx = v.lower().find("clang")
            meta["compiler"] = v[idx:idx+30].split(")")[0]
    except OSError:
        pass
    return meta


def print_kernel_meta(meta, ko_path=None):
    """Print kernel metadata header/footer."""
    print(f"# {'=' * 60}")
    print(f"# Kernel: {meta.get('release', 'unknown')}")
    print(f"# Build:  {meta.get('version', 'unknown')[:80]}")
    if meta.get('compiler'):
        print(f"# Compiler: {meta['compiler']}")
    # Read rustc version from .ko .comment section
    if ko_path:
        try:
            r = subprocess.run(
                ["readelf", "-p", ".comment", ko_path],
                capture_output=True, text=True, timeout=5)
            for line in r.stdout.splitlines():
                if "rustc" in line:
                    ver = line.split("]", 1)[-1].strip()
                    print(f"# Rustc: {ver}")
                    break
        except (OSError, subprocess.TimeoutExpired):
            pass
    print(f"# {'=' * 60}")


def lookup(pc, syms):
    """Nearest kallsyms entry <= pc as (name, offset, module) or None."""
    if not syms:
        return None
    lo, hi = 0, len(syms) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if syms[mid][0] <= pc:
            lo = mid
        else:
            hi = mid - 1
    addr, name, mod = syms[lo]
    if addr > pc:
        return None
    return name, pc - addr, mod


def symbolize(pc, syms):
    """Find nearest symbol <= pc. Returns (display_name, module_tag)."""
    hit = lookup(pc, syms)
    if not hit:
        return f"0x{pc:x}", ""
    name, offset, mod = hit
    dname = demangle(name)
    display = f"{dname}+0x{offset:x}" if offset else dname
    return display, f" [{mod}]" if mod else ""


def format_val(v):
    """Format a captured value."""
    if v == MAGIC_BAD:
        return "FAULT"
    if v == 0:
        return "0x0"
    return f"0x{v:x}"


def find_module(name):
    """
    Find the .ko for test @name: <name>/<name>.ko in the source tree, or
    <name>.ko next to this script in an installed (make install) tree.
    """
    for ko_path in (os.path.join(SELFTEST_DIR, name, f"{name}.ko"),
                    os.path.join(SELFTEST_DIR, f"{name}.ko")):
        if os.path.exists(ko_path):
            return ko_path
    return None


def finit_module(ko_path):
    """Load a kernel module via finit_module syscall."""
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    fd = os.open(ko_path, os.O_RDONLY)
    ret = libc.syscall(SYS_FINIT_MODULE, fd, b"", 0)
    os.close(fd)
    if ret != 0:
        errno = ctypes.get_errno()
        raise OSError(errno, f"finit_module({ko_path}): {os.strerror(errno)}")


def delete_module(name):
    """Unload a kernel module."""
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    ret = libc.syscall(SYS_DELETE_MODULE, name.encode(), 0)
    if ret != 0:
        errno = ctypes.get_errno()
        raise OSError(errno, f"delete_module({name}): {os.strerror(errno)}")


def trigger_module():
    """
    Write to every trigger file the loaded module created under TRIGGER_DIR.
    Opened without O_CREAT: debugfs directories have no ->create, so a
    "w"-mode open of a missing name fails with EOPNOTSUPP, not ENOENT.
    """
    try:
        names = sorted(os.listdir(TRIGGER_DIR))
    except OSError:
        names = []
    hits = []
    for n in names:
        path = os.path.join(TRIGGER_DIR, n)
        try:
            fd = os.open(path, os.O_WRONLY)
        except OSError:
            continue
        try:
            os.write(fd, b"1")
        finally:
            os.close(fd)
        hits.append(path)
    if not hits:
        raise FileNotFoundError(f"no trigger file under {TRIGGER_DIR}")
    return hits


def parse_records(buf, total_words):
    """Parse the ring buffer into a list of records."""
    records = []
    pos = 1
    end = min(1 + total_words, BUF_SIZE)
    while pos + RECORD_HDR_WORDS <= end:
        hdr = buf[pos]
        rtype = hdr_type(hdr)
        num_vals = hdr_nvals(hdr)

        # Every record the kernel writes has nvals >= 1 and a known type;
        # anything else is garbage (e.g. a userspace reset mid-run): resync.
        if rtype not in (DF_TYPE_ENTRY, DF_TYPE_RET, DF_TYPE_CMP) \
                or num_vals == 0 or pos + RECORD_HDR_WORDS + num_vals > end:
            pos += 1
            continue

        pc = int(buf[pos + 1]) + KASLR_OFFSET
        ptr = int(buf[pos + 2])  # ENTRY/RET: traced pointer; CMP: cmp type
        if rtype == DF_TYPE_CMP:
            pos += RECORD_HDR_WORDS + num_vals
            continue

        # Valid records always have a non-zero PC (kernel text address)
        if pc == 0:
            pos += 1
            continue

        vals = [int(buf[pos + RECORD_HDR_WORDS + vi]) for vi in range(num_vals)]
        records.append({
            "type": rtype,
            "seq": hdr_seq(hdr),
            "pc": pc,
            "ptr": ptr,
            "arg_idx": hdr_arg_idx(hdr),
            "size": hdr_size(hdr),
            "val": vals[0],
            "vals": vals,
        })
        pos += RECORD_HDR_WORDS + num_vals
    return records


class Capture:
    """Everything run_capture() collected for one module run."""

    def __init__(self, ko_path, mod_name, records, syms, total_words,
                 mod_text_start, kaslr_off):
        self.ko_path = ko_path
        self.mod_name = mod_name
        self.records = records
        self.syms = syms
        self.total_words = total_words
        self.mod_text_start = mod_text_start
        self.kaslr_offset = kaslr_off
        self.runtime_text = runtime_text(syms)
        self._mod_syms = any(m == mod_name for _, _, m in syms)
        # Aliases: rustc's merge-functions makes identical bodies (e.g. the
        # one-field rsf_1 and rstf_1) share one address, so a PC can carry
        # several names.
        self._names = {}
        for addr, name, mod in syms:
            self._names.setdefault((addr, mod), set()).add(name)

    def is_module_pc(self, pc):
        """True if pc lies in the test module (kallsyms, else .text start)."""
        if self._mod_syms:
            hit = lookup(pc, self.syms)
            return bool(hit) and hit[2] == self.mod_name
        # Fallback: if no module symbols (kptr_restrict), use .text start
        return bool(self.mod_text_start) and pc >= self.mod_text_start

    def funcs(self, rec):
        """All raw kallsyms names of the function a record belongs to."""
        hit = lookup(rec["pc"], self.syms)
        if not hit:
            return set()
        name, offset, mod = hit
        return self._names.get((rec["pc"] - offset, mod), {name})

    def module_records(self):
        return [r for r in self.records if self.is_module_pc(r["pc"])]

    def context_records(self, n):
        """Module records plus n records before/after each of them."""
        keep = set()
        for i, r in enumerate(self.records):
            if self.is_module_pc(r["pc"]):
                keep.update(range(max(0, i - n),
                                  min(len(self.records), i + n + 1)))
        return [self.records[i] for i in sorted(keep)]


def run_capture(ko_path, remote=False, vmlinux=None, kaslr_override=None,
                log=None):
    """
    Load @ko_path, record while its trigger file(s) are written, unload it
    and return a Capture. @remote publishes the buffer for REMOTE_HANDLE
    instead of enabling recording for this task. Raises OSError.
    """
    global KASLR_OFFSET
    log = log or (lambda msg: print(f"# {msg}"))

    # Ensure kallsyms shows real addresses
    try:
        with open("/proc/sys/kernel/kptr_restrict", "w") as f:
            f.write("0")
    except OSError:
        pass

    df_fd = os.open(KCOV_DF_PATH, os.O_RDWR)
    try:
        # Init + mmap
        fcntl.ioctl(df_fd, KCOV_DF_INIT_TRACK, BUF_SIZE)
        libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
        libc.mmap.restype = ctypes.c_void_p
        libc.mmap.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
            ctypes.c_int, ctypes.c_int, ctypes.c_long
        ]
        buf_ptr = libc.mmap(None, BUF_SIZE * 8, 0x3, 0x01, df_fd, 0)
        if buf_ptr == ctypes.c_void_p(-1).value:
            errno = ctypes.get_errno()
            raise OSError(errno, f"mmap: {os.strerror(errno)}")
        buf = (ctypes.c_uint64 * BUF_SIZE).from_address(buf_ptr)

        # Load module first (its init generates noise with INSTRUMENT_ALL)
        mod_name = os.path.basename(ko_path).replace(".ko", "")
        finit_module(ko_path)
        log(f"Loaded {mod_name}")
        try:
            # Module .text address, the PC filter fallback without kallsyms
            mod_text_start = 0
            try:
                with open(f"/sys/module/{mod_name}/sections/.text") as f:
                    mod_text_start = int(f.read().strip(), 16)
            except (OSError, ValueError):
                pass

            # Enable recording AFTER load, BEFORE trigger (no loader noise).
            # Remote: the handle is passed by pointer (a __u64 in a buffer),
            # so the full 64-bit value survives 32-bit/compat callers.
            if remote:
                fcntl.ioctl(df_fd, KCOV_DF_REMOTE_ENABLE,
                            struct.pack("Q", REMOTE_HANDLE))
            else:
                fcntl.ioctl(df_fd, KCOV_DF_ENABLE, 0)
            buf[0] = 0
            try:
                for path in trigger_module():
                    log(f"Triggered {path}")
            finally:
                fcntl.ioctl(df_fd, KCOV_DF_REMOTE_DISABLE if remote
                            else KCOV_DF_DISABLE, 0)

            # Read kallsyms while the module is still loaded
            syms = load_kallsyms()
        finally:
            try:
                delete_module(mod_name)
            except OSError as e:
                log(f"warning: {e}")

        if kaslr_override is not None:
            KASLR_OFFSET = kaslr_override
        else:
            KASLR_OFFSET = kaslr_offset(syms, find_vmlinux(vmlinux))

        total = int(buf[0])
        records = parse_records(buf, total)
        return Capture(ko_path, mod_name, records, syms, total,
                       mod_text_start, KASLR_OFFSET)
    finally:
        os.close(df_fd)


def print_raw(records, syms, vmlinux=None, cache=None, ko_path=None, mod_text_base=0):
    """Print records in raw format with source line on left."""
    if cache is None:
        cache = {}
    # Pre-resolve all locations (one addr2line run) to find max width
    resolve_lines([r["pc"] for r in records], vmlinux, cache, ko_path,
                  mod_text_base)
    locs = [cache[r["pc"]] for r in records]
    max_w = max((len(l) for l in locs if l), default=0)
    max_w = max(max_w, 10)  # minimum width

    for i, r in enumerate(records):
        name, mod = symbolize(r["pc"], syms)
        sym = f"{name}{mod}"
        t = "ENTRY" if r["type"] == DF_TYPE_ENTRY else "RET  "
        arg_idx = r["arg_idx"]
        size = r["size"]
        left = f"{locs[i]:>{max_w}s}" if locs[i] else f"{'':>{max_w}s}"
        vals = format_val(r["val"]) if len(r["vals"]) == 1 else \
            "{" + ", ".join(format_val(v) for v in r["vals"]) + "}"
        print(f"{left}   [{t}] seq={r['seq']:3d} {sym} "
              f"arg[{arg_idx}]({size}) @0x{r['ptr']:x} = {vals}")


def print_tree(records, syms, vmlinux=None, cache=None, ko_path=None, mod_text_base=0):
    """Print records as indented call tree with source line on left."""
    if cache is None:
        cache = {}
    # Pre-resolve all PCs (one addr2line run) for alignment
    resolve_lines([r["pc"] for r in records], vmlinux, cache, ko_path,
                  mod_text_base)
    max_w = max((len(v) for v in cache.values() if v), default=10)
    max_w = max(max_w, 10)

    depth = 0
    call_stack = []  # Stack of (name, mod, args_str, pc) for matching returns
    i = 0
    while i < len(records):
        r = records[i]
        name, mod = symbolize(r["pc"], syms)

        if r["type"] == DF_TYPE_ENTRY:
            # Collect all args for this call (same PC, consecutive entries);
            # order by index, as the pass emits dead-arg traces last.
            args = []
            pc = r["pc"]
            while i < len(records) and records[i]["type"] == DF_TYPE_ENTRY \
                    and records[i]["pc"] == pc:
                vals = records[i]["vals"]
                if len(vals) > 1:
                    fields = ", ".join(format_val(v) for v in vals)
                    args.append((records[i]["arg_idx"], "{" + fields + "}"))
                else:
                    args.append((records[i]["arg_idx"],
                                 format_val(records[i]["val"])))
                i += 1
            args_str = ", ".join(a for _, a in sorted(args, key=lambda x: x[0]))
            call_stack.append((name, mod, args_str, pc))
            depth += 1
        else:
            # Pop void calls (no return record) until we find matching PC
            while call_stack and call_stack[-1][3] != r["pc"]:
                depth = max(0, depth - 1)
                indent = "  " * depth
                vname, vmod, vargs, vpc = call_stack.pop()
                loc = resolve_line(vpc, vmlinux, cache, ko_path, mod_text_base)
                left = f"{loc:>{max_w}s}" if loc else f"{'':>{max_w}s}"
                print(f"{left}   {indent}{vname}({vargs}){vmod}")
            depth = max(0, depth - 1)
            indent = "  " * depth
            ret_size = r["size"]
            loc = resolve_line(r["pc"], vmlinux, cache, ko_path, mod_text_base)
            left = f"{loc:>{max_w}s}" if loc else f"{'':>{max_w}s}"
            if call_stack:
                cname, cmod, cargs, _ = call_stack.pop()
                if ret_size == 0:
                    print(f"{left}   {indent}{cname}({cargs}){cmod}")
                else:
                    print(f"{left}   {indent}{format_val(r['val'])} = {cname}({cargs}){cmod}")
            else:
                if ret_size == 0:
                    print(f"{left}   {indent}{name}(){mod}")
                else:
                    print(f"{left}   {indent}{format_val(r['val'])} = {name}(){mod}")
            i += 1

    # Flush remaining void calls on the stack
    while call_stack:
        depth = max(0, depth - 1)
        indent = "  " * depth
        vname, vmod, vargs, vpc = call_stack.pop()
        loc = resolve_line(vpc, vmlinux, cache, ko_path, mod_text_base)
        left = f"{loc:>{max_w}s}" if loc else f"{'':>{max_w}s}"
        print(f"{left}   {indent}{vname}({vargs}){vmod}")


def main():
    parser = argparse.ArgumentParser(
        description="Load a test module with kcov_dataflow and view records")
    parser.add_argument("module", help="Test module name (e.g. eight_struct_args_c)")
    parser.add_argument("--raw", action="store_true",
                        help="Print raw records instead of tree")
    parser.add_argument("--ko", help="Explicit path to .ko file")
    parser.add_argument("--context", "-C", type=int, default=0,
                        help="Show N records before/after each module record")
    parser.add_argument("--vmlinux", help="Path to vmlinux for addr2line")
    parser.add_argument("--remote", action="store_true",
                        help="Use KCOV_DF_REMOTE_ENABLE for kworker capture")
    parser.add_argument("--kaslr-offset", type=lambda x: int(x, 0),
                        help="Override the runtime KASLR offset added to PCs")
    args = parser.parse_args()

    ko_path = args.ko or find_module(args.module)
    if not ko_path or not os.path.exists(ko_path):
        print(f"Cannot find module for '{args.module}'", file=sys.stderr)
        print("Build it first: make -C tools/testing/selftests "
              "TARGETS=kcov_dataflow LLVM=1 CC=clang", file=sys.stderr)
        sys.exit(1)

    try:
        cap = run_capture(ko_path, remote=args.remote, vmlinux=args.vmlinux,
                          kaslr_override=args.kaslr_offset)
    except OSError as e:
        print(f"{args.module}: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"# Captured {cap.total_words} words (kaslr_offset=0x{cap.kaslr_offset:x}, "
          f"_text=0x{cap.runtime_text:x})")
    print(f"# {len(cap.records)} records")

    if cap.syms or cap.mod_text_start:
        if args.context > 0:
            records = cap.context_records(args.context)
            print(f"# showing {len(records)} records with context={args.context} "
                  f"around {cap.mod_name}\n")
        else:
            records = cap.module_records()
            print(f"# {len(records)} from {cap.mod_name}\n")
    else:
        records = cap.records
        print("")

    meta = get_kernel_meta()
    print_kernel_meta(meta, ko_path=ko_path)

    vmlinux = find_vmlinux(args.vmlinux)
    show = print_raw if args.raw else print_tree
    show(records, cap.syms, vmlinux, {}, ko_path, cap.mod_text_start)

    print_kernel_meta(meta, ko_path=ko_path)


if __name__ == "__main__":
    main()
