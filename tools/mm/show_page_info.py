#!/usr/bin/env drgn
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2025 Ye Liu <liuye@kylinos.cn>

import argparse
import sys
from drgn import Object, FaultError, PlatformFlags, Architecture, cast
from drgn.helpers.linux import find_task, follow_page
from drgn.helpers.linux.mm import (
    decode_page_flags, page_to_pfn, page_to_phys, page_to_virt,
    pfn_to_page, phys_to_virt, virt_to_phys,
    vma_find, vma_name,
    PageSlab, PageCompound, compound_head, compound_order, compound_nr,
    PageLRU, PageActive, PageUnevictable, PageSwapBacked,
)
from drgn.helpers.linux.rbtree import rbtree_inorder_for_each_entry
from drgn.helpers.linux.cgroup import cgroup_name, cgroup_path
from drgn.helpers.linux.fs import inode_path
from drgn.helpers.common.format import decode_flags

DESC = """
This is a drgn script to show the page state.
For more info on drgn, visit https://github.com/osandov/drgn.
"""


# ---------------------------------------------------------------------------
# Utility functions
# ---------------------------------------------------------------------------

def format_page_data(page):
    """Format raw page struct data as hex dump with 'RAW:' prefix."""
    try:
        address = page.value_()
        word_size = 8 if prog.platform.flags & PlatformFlags.IS_64_BIT else 4
        num_words = prog.type("struct page").size // word_size
        words = [f"{prog.read_word(address + i * word_size):0{word_size * 2}x}"
                 for i in range(num_words)]
        return "\n".join(f"RAW: {' '.join(words[i:i+4])}"
                         for i in range(0, len(words), 4))
    except FaultError as e:
        return f"Error reading memory: {e}"


def _print_fields(fields):
    """Print key-value fields with aligned colons."""
    max_len = max(len(k) for k in fields)
    indent = ' ' * (max_len + 2)
    for key, value in fields.items():
        lines = str(value).split('\n')
        print(f"{key}:".ljust(max_len + 2) + lines[0])
        for line in lines[1:]:
            print(indent + line)


# ---------------------------------------------------------------------------
# Memory cgroup info
# ---------------------------------------------------------------------------

def _memcg_name_path(memcg):
    """Get (name, path) from a mem_cgroup object."""
    cgrp = memcg.css.cgroup
    return (cgroup_name(cgrp).decode(),
            f"/sys/fs/cgroup/memory{cgroup_path(cgrp).decode()}")


def _slab_obj_ext_stride(slab):
    """Determine stride between slabobj_ext entries for a slab."""
    base = prog.type("struct slabobj_ext").size
    try:
        if slab.obj_exts_in_object.value_():
            return slab.slab_cache.size.value_()
    except AttributeError:
        pass
    if _slab_obj_ext_has_codetag():
        base *= 2
    return base


_codetag_enabled = None

def _slab_obj_ext_has_codetag():
    """Check if slab obj_exts include codetag entries (doubles stride)."""
    global _codetag_enabled
    if _codetag_enabled is not None:
        return _codetag_enabled
    try:
        key = prog["slab_obj_ext_has_codetag_key"]
        try:
            enabled = key.key.enabled.counter.value_()
        except AttributeError:
            enabled = key.enabled.counter.value_()
        _codetag_enabled = enabled > 0
    except (KeyError, AttributeError):
        _codetag_enabled = False
    return _codetag_enabled


def _collect_memcg_ptrs(base_addr, num_objects, stride):
    """Collect distinct memcg pointers from a slabobj_ext array."""
    ptrs = set()
    for i in range(num_objects):
        entry = Object(prog, "struct slabobj_ext *", base_addr + i * stride)
        try:
            objcg = entry._objcg
        except AttributeError:
            objcg = entry.objcg
        try:
            val = objcg.memcg.value_()
            if val:
                ptrs.add(val)
        except FaultError:
            continue
    return ptrs


def _slab_memcg_info(page, mask):
    """Retrieve memcg info for a slab page by iterating all objects."""
    base_addr = page.memcg_data.read_() & ~mask
    if base_addr == 0:
        return "unknown", "obj_exts allocation failed"

    slab = cast("struct slab *", page)
    try:
        if not slab.obj_exts_needs_objcg.value_():
            return "none", "/sys/fs/cgroup/memory/"
    except AttributeError:
        pass
    try:
        num_objects = slab.objects.value_()
    except (AttributeError, FaultError):
        num_objects = 1

    stride = _slab_obj_ext_stride(slab)

    memcg_ptrs = _collect_memcg_ptrs(base_addr, num_objects, stride)

    if not memcg_ptrs:
        return "none", "/sys/fs/cgroup/memory/"

    # Resolve names
    resolved = {}
    for addr in memcg_ptrs:
        try:
            resolved[addr] = _memcg_name_path(
                Object(prog, "struct mem_cgroup *", addr))
        except FaultError:
            continue

    if not resolved:
        return "unknown", f"{len(memcg_ptrs)} memcg(s), cgroup unreadable"
    names = [v[0] for v in resolved.values()]
    paths = [v[1] for v in resolved.values()]
    if len(resolved) == 1:
        return names[0], paths[0]
    nl = '\n'
    return f"{nl.join(names)}\n({len(resolved)} total)", nl.join(paths)


def get_memcg_info(page):
    """Retrieve memory cgroup information for a page."""
    try:
        mask = prog.constant('__NR_MEMCG_DATA_FLAGS').value_() - 1
        memcg_data = page.memcg_data.read_()

        if memcg_data == 0:
            return "none", "/sys/fs/cgroup/memory/"

        if memcg_data & prog.constant("MEMCG_DATA_OBJEXTS").value_():
            return _slab_memcg_info(page, mask)

        objcg_addr = memcg_data & ~mask
        if objcg_addr == 0:
            return "none", "/sys/fs/cgroup/memory/"
        memcg = Object(prog, "struct obj_cgroup *", objcg_addr).memcg
        if memcg.value_() == 0:
            return "none", "/sys/fs/cgroup/memory/"
        return _memcg_name_path(memcg)
    except FaultError as e:
        return "unknown", f"Error retrieving memcg info: {e}"
    except Exception as e:
        return "unknown", f"Unexpected error: {e}"


# ---------------------------------------------------------------------------
# Reverse mapping (rmap)
# ---------------------------------------------------------------------------

def _vma_anon_pgoff(vma):
    """Get anon pgoff for a VMA (vma_start_anon_pgoff)."""
    try:
        hi = vma.__vm_anon_pgoff_hi.value_()
        lo = vma.__vm_anon_pgoff_lo.value_()
        return (hi << 32) | lo
    except AttributeError:
        return vma.vm_pgoff.value_()


def _vma_owner(vma, folio_index, page_size, is_anon):
    """Extract (pid, comm, vm_start, vm_end, mm_ptr, vaddr) from a VMA."""
    mm_ptr = vma.vm_mm
    owner = mm_ptr.read_().owner.read_()
    if is_anon:
        pgoff = _vma_anon_pgoff(vma)
    else:
        pgoff = vma.vm_pgoff.value_()
    vaddr = vma.vm_start.value_() + \
        (folio_index - pgoff) * page_size
    return (owner.pid.value_(), owner.comm.string_().decode(),
            vma.vm_start.value_(), vma.vm_end.value_(), mm_ptr, vaddr)


def get_rmap_info(page):
    """Find processes that map this page via reverse mapping."""
    results = []
    head = compound_head(page) if PageCompound(page) else page
    mapping = head.mapping.value_()
    if mapping == 0:
        return results

    page_size = prog["PAGE_SIZE"].value_()
    try:
        folio_index = head.__folio_index.value_()
    except (AttributeError, FaultError):
        folio_index = 0
    # Add tail-page offset for compound pages
    if head.value_() != page.value_():
        folio_index += page_to_pfn(page).value_() - page_to_pfn(head).value_()

    try:
        if mapping & 0x3 == 0x3:
            # KSM page: skip (mapping points to ksm_stable_node, not anon_vma)
            return results
        if mapping & 0x1:
            # Anonymous page: walk anon_vma interval tree.
            anon_vma = Object(prog, "struct anon_vma *", mapping & ~3)
            for avc in rbtree_inorder_for_each_entry(
                "struct anon_vma_chain", anon_vma.rb_root.rb_root, "rb"):
                try:
                    vma = avc.vma.read_()
                    anon_pgoff = _vma_anon_pgoff(vma)
                    vma_pages = (vma.vm_end.value_() -
                                 vma.vm_start.value_()) // page_size
                    # Check if folio_index falls within this VMA's anon range
                    if anon_pgoff <= folio_index < anon_pgoff + vma_pages:
                        results.append(
                            _vma_owner(vma, folio_index, page_size, True))
                except FaultError:
                    continue
        else:
            addr_space = Object(prog, "struct address_space *", mapping & ~3)
            for vma in rbtree_inorder_for_each_entry(
                "struct vm_area_struct", addr_space.i_mmap.rb_root,
                "shared.rb"):
                try:
                    vma_pgoff = vma.vm_pgoff.value_()
                    vma_pages = (vma.vm_end.value_() -
                                 vma.vm_start.value_()) // page_size
                    if vma_pgoff <= folio_index < vma_pgoff + vma_pages:
                        results.append(
                            _vma_owner(vma, folio_index, page_size, False))
                except FaultError:
                    continue
    except (FaultError, AttributeError):
        pass

    return results


# ---------------------------------------------------------------------------
# VMA flags
# ---------------------------------------------------------------------------

def format_vma_flags(flags):
    """Format VMA flags into a human-readable string."""
    try:
        enum_type = prog.constant("VMA_READ_BIT").type_
        vm_flags = [(name[4:-4], 1 << value)
                    for name, value in enum_type.enumerators
                    if name.startswith("VMA_") and name.endswith("_BIT")]
        if vm_flags:
            return decode_flags(flags, vm_flags, bit_numbers=False)
    except (LookupError, AttributeError):
        pass
    return hex(flags)


# ---------------------------------------------------------------------------
# Node / Zone / Page Type lookup
# ---------------------------------------------------------------------------

def _page_node_zone(page):
    """Find node ID and zone name via PFN range lookup.

    Uses prog["node_data"] which is x86-specific. Falls back to
    contig_page_data for UMA (single-node) systems. May return
    (-1, "Unknown") on architectures where neither symbol exists.
    """
    pfn = page_to_pfn(page).value_()
    max_zones = prog.constant("__MAX_NR_ZONES").value_()
    try:
        nr_nodes = prog["nr_node_ids"].value_()
        node_data = prog["node_data"]
        for nid in range(nr_nodes):
            try:
                pgdat = node_data[nid].read_()
            except (IndexError, FaultError):
                continue
            for zid in range(max_zones):
                zone = pgdat.node_zones[zid]
                start = zone.zone_start_pfn.value_()
                spanned = zone.spanned_pages.value_()
                if spanned and start <= pfn < start + spanned:
                    return nid, zone.name.string_().decode()
    except (KeyError, FaultError):
        pass
    # UMA fallback
    try:
        pgdat = prog["contig_page_data"].address_of_()
        for zid in range(max_zones):
            zone = pgdat.node_zones[zid]
            start = zone.zone_start_pfn.value_()
            spanned = zone.spanned_pages.value_()
            if spanned and start <= pfn < start + spanned:
                return 0, zone.name.string_().decode()
    except (KeyError, FaultError):
        pass
    return -1, "Unknown"


def _page_type(head):
    """Decode page->page_type field via enum pagetype discovery.

    page_type stores the type in bits 24-31 (e.g. 0xf5000000 for PGTY_slab).
    The raw 8-bit enum constants (0xf0-0xff) must be compared against the
    shifted value. PGTY_mapcount_underflow (0xff) matches normal unmapped
    pages (page_type == 0xffffffff) and is excluded.
    """
    try:
        pt = head.page_type.value_()
    except AttributeError:
        return None
    pt_shifted = (pt >> 24) & 0xFF
    if pt_shifted == 0 or pt_shifted == 0xff:
        return None
    try:
        enum_type = prog.constant("PGTY_buddy").type_
        for name, value in enum_type.enumerators:
            if value == pt_shifted:
                return name[5:]  # strip PGTY_ prefix
    except (LookupError, AttributeError):
        pass
    return None


# ---------------------------------------------------------------------------
# PTE walk (x86_64 4-level paging)
# ---------------------------------------------------------------------------

# x86_64 PTE flag bits — preprocessor macros, not in DWARF
_PTE_FLAGS = [
    ("PRESENT",  1 << 0),
    ("RW",       1 << 1),
    ("USER",     1 << 2),
    ("PWT",      1 << 3),
    ("PCD",      1 << 4),
    ("ACCESSED", 1 << 5),
    ("DIRTY",    1 << 6),
    ("PS",       1 << 7),
    ("GLOBAL",   1 << 8),
    ("NX",       1 << 63),
]


def _walk_page_table(mm, vaddr):
    """Walk 4-level page table. Returns list of (level, entry_pa, entry_val).

    Note: only supports x86_64 4-level paging (48-bit VA). Does not support
    5-level paging (LA57, 57-bit VA) where an additional P4D level exists.
    """
    # Mask to extract physical address bits (bits 12-51 on x86_64).
    # Python's ~(PAGE_SIZE-1) has arbitrary precision with all high bits set,
    # which would retain NX (bit 63) and software bits (52-62).
    page_mask = 0x000FFFFFFFFFF000
    entries = []
    try:
        def _read(table_va, idx):
            return prog.read_word(table_va + idx * 8)

        base_va = mm.pgd.value_()

        # PGD
        pgd_idx = (vaddr >> 39) & 0x1FF
        pgd_val = _read(base_va, pgd_idx)
        pgd_pa = virt_to_phys(prog, base_va).value_() + pgd_idx * 8
        entries.append(("PGD", pgd_pa, pgd_val))
        if not (pgd_val & 1):
            return entries

        # PUD
        pud_table_pa = pgd_val & page_mask
        pud_idx = (vaddr >> 30) & 0x1FF
        pud_val = _read(phys_to_virt(prog, pud_table_pa).value_(), pud_idx)
        entries.append(("PUD", pud_table_pa + pud_idx * 8, pud_val))
        if not (pud_val & 1):
            return entries
        if pud_val & (1 << 7):
            return entries  # 1GB huge page

        # PMD
        pmd_table_pa = pud_val & page_mask
        pmd_idx = (vaddr >> 21) & 0x1FF
        pmd_val = _read(phys_to_virt(prog, pmd_table_pa).value_(), pmd_idx)
        entries.append(("PMD", pmd_table_pa + pmd_idx * 8, pmd_val))
        if not (pmd_val & 1):
            return entries
        if pmd_val & (1 << 7):
            return entries  # 2MB huge page

        # PTE
        pte_table_pa = pmd_val & page_mask
        pte_idx = (vaddr >> 12) & 0x1FF
        pte_val = _read(phys_to_virt(prog, pte_table_pa).value_(), pte_idx)
        entries.append(("PTE", pte_table_pa + pte_idx * 8, pte_val))
    except FaultError:
        pass
    return entries


def _pte_fields(mm, addr, fields):
    """Collect page table entries via page table walk."""
    if prog.platform.arch != Architecture.X86_64:
        return
    entries = _walk_page_table(mm, addr)
    for i, (level, pa, val) in enumerate(entries):
        if not (val & 1):
            fields[level] = "not present"
            continue
        if i == len(entries) - 1:
            # Final entry (PTE or huge page): show decoded flags
            flags = '|'.join(n for n, m in _PTE_FLAGS if val & m)
            fields[level] = f"{hex(pa)} => {hex(val)} ({flags})"
        else:
            # Intermediate level: just address => value
            fields[level] = f"{hex(pa)} => {hex(val)}"


# ---------------------------------------------------------------------------
# Field collectors
# ---------------------------------------------------------------------------

def _base_fields(page, head):
    """Collect basic page fields."""
    fields = {
        "Page Address": hex(page.value_()),
        "Page Flags": decode_page_flags(page),
        "Page Size": prog["PAGE_SIZE"].value_(),
        "Page PFN": hex(page_to_pfn(page).value_()),
        "Page Physical": hex(page_to_phys(page).value_()),
        "Page Virtual": hex(page_to_virt(page).value_()),
    }
    nid, zone = _page_node_zone(page)
    if nid >= 0:
        fields["Page Node"] = nid
    fields["Page Zone"] = zone
    ptype = _page_type(head)
    if ptype is not None:
        fields["Page Type"] = ptype
    return fields


def _page_status(head):
    """Determine page status string."""
    if PageSlab(head):
        return "In use (slab)"
    if head.mapping.value_() != 0:
        return "In use"
    if head._refcount.counter.value_() > 0:
        return "In use (unmapped)"
    return "Free"


def _slab_fields(head, fields):
    """Collect slab-specific fields."""
    try:
        slab = cast("struct slab *", head)
        fields["Slab Cache"] = slab.slab_cache.name.string_().decode()
        fields["Slab Objects"] = slab.objects.value_()
        fields["Slab Inuse"] = slab.inuse.value_()
        fields["Slab Frozen"] = slab.frozen.value_()
    except (AttributeError, FaultError):
        fields["Slab Cache"] = "Unknown"


def _lru_type(head):
    """Determine LRU list type from page flags."""
    if PageUnevictable(head):
        return "Unevictable"
    if not PageLRU(head):
        return None
    active = PageActive(head)
    anon = PageSwapBacked(head)
    return f"{'Active' if active else 'Inactive'} {'Anon' if anon else 'File'}"


def _swapcache_info(head):
    """Get swap entry value if page is in swap cache."""
    # PG_swapcache is an alias for PG_owner_priv_1 (same bit as PG_checked).
    # Must check PageSwapBacked to avoid false positives on checked file pages.
    if not PageSwapBacked(head):
        return None
    # drgn does not provide a PageSwapCache() helper, so we check the
    # decoded page flags string for PG_swapcache.
    flags_str = str(decode_page_flags(head))
    if "PG_swapcache" not in flags_str:
        return None
    try:
        return hex(head.private.value_())
    except (AttributeError, FaultError):
        return "Unknown"


def _page_fields(head, fields):
    """Collect non-slab page fields (refcount, mapping, anon/file, etc.)."""
    mapping = head.mapping.value_()
    fields["Page Mapping"] = hex(mapping)

    if mapping == 0:
        fields["Page Anon/File"] = "Unmapped"
        refcount = head._refcount.counter.value_()
        if refcount > 0:
            fields["Page Refcount"] = refcount
            fields["Page Mapcount"] = head._mapcount.counter.value_()
        lru = _lru_type(head)
        if lru:
            fields["LRU"] = lru
        swap = _swapcache_info(head)
        if swap:
            fields["Swap Entry"] = swap
        return

    fields["Page Refcount"] = head._refcount.counter.value_()
    fields["Page Mapcount"] = head._mapcount.counter.value_()

    lru = _lru_type(head)
    if lru:
        fields["LRU"] = lru

    swap = _swapcache_info(head)
    if swap:
        fields["Swap Entry"] = swap

    if mapping & 0x1:
        fields["Page Anon/File"] = "Anon"
    else:
        fields["Page Anon/File"] = "File"
        try:
            inode = Object(prog, "struct address_space *",
                           mapping & ~3).host
            ino = inode.i_ino.value_()
            fields["Filesystem"] = inode.i_sb.s_id.string_().decode()
            path = inode_path(inode)
            if path:
                fields["File Path"] = f"{path.decode()} (inode {ino})"
            else:
                fields["File Path"] = f"Unknown (inode {ino})"
        except (FaultError, AttributeError):
            fields["File Path"] = "Unknown"

    try:
        fields["Folio Index"] = hex(head.__folio_index.value_())
    except (AttributeError, FaultError):
        fields["Folio Index"] = "Unavailable"


def _compound_fields(page, head, fields):
    """Collect compound page fields."""
    if not PageCompound(page):
        return
    fields["Compound Order"] = compound_order(page).value_()
    offset = page_to_pfn(page).value_() - page_to_pfn(head).value_()
    fields["Folio Page Index"] = f"{offset}/{compound_nr(page).value_()}"
    if head.value_() != page.value_():
        fields["Head Page"] = hex(head.value_())


def _memcg_fields(head, fields):
    """Collect memcg data and resolved name/path."""
    fields["Page Memcg Data"] = hex(head.memcg_data.value_())
    name, path = get_memcg_info(head)
    fields["Memcg Name"] = name
    fields["Memcg Path"] = path


def _rmap_fields(page, head, fields, show_detail=False):
    """Collect rmap (reverse mapping) info."""
    if PageSlab(head) or head.mapping.value_() == 0:
        return
    rmap = get_rmap_info(page)
    fields["Rmap Count"] = len(rmap)
    if not rmap:
        fields["Rmap"] = "No mappings found"
        return

    entries = [f"{pid}({comm}) {hex(s)}-{hex(e)}"
               for pid, comm, s, e, _, _ in rmap]
    fields["Rmap"] = '\n'.join(entries)

    # Show VMA and PTE details for first 2 mappings (--pfn/--page mode only)
    if not show_detail:
        return
    detail = min(2, len(rmap))
    if len(rmap) > detail:
        fields["Rmap Detail"] = f"Showing first {detail} of {len(rmap)} mappings"
    for i in range(detail):
        pid, comm, s, e, mm, vaddr = rmap[i]
        prefix = f"Rmap #{i+1} {pid} " if detail > 1 else ""
        try:
            vma = vma_find(mm, vaddr)
            fields[f"{prefix}VMA"] = hex(vma.value_())
            fields[f"{prefix}VMA Start"] = hex(vma.vm_start.value_())
            fields[f"{prefix}VMA End"] = hex(vma.vm_end.value_())
            flags_hex = hex(vma.vm_flags.value_())
            flags_str = format_vma_flags(vma.vm_flags.value_())
            fields[f"{prefix}VMA Flags"] = f"{flags_hex} ({flags_str})"
            name = vma_name(vma)
            fields[f"{prefix}VMA Name"] = name.decode() if name else "[unnamed]"
        except (FaultError, AttributeError):
            fields[f"{prefix}VMA"] = "Unavailable"
        if prog.platform.arch == Architecture.X86_64:
            pte_entries = _walk_page_table(mm, vaddr)
            for j, (level, pa, val) in enumerate(pte_entries):
                if not (val & 1):
                    fields[f"{prefix}{level}"] = "not present"
                elif j == len(pte_entries) - 1:
                    flags = '|'.join(n for n, m in _PTE_FLAGS if val & m)
                    fields[f"{prefix}{level}"] = f"{hex(pa)} => {hex(val)} ({flags})"
                else:
                    fields[f"{prefix}{level}"] = f"{hex(pa)} => {hex(val)}"


def _vma_fields(mm, addr, fields):
    """Collect VMA fields for a user-space address lookup."""
    try:
        vma = vma_find(mm, addr)
        fields["Page VMA"] = hex(vma.value_())
        fields["VMA Start"] = hex(vma.vm_start.value_())
        fields["VMA End"] = hex(vma.vm_end.value_())
        flags_hex = hex(vma.vm_flags.value_())
        flags_readable = format_vma_flags(vma.vm_flags.value_())
        fields["VMA Flags"] = f"{flags_hex} ({flags_readable})"
        name = vma_name(vma)
        fields["VMA Name"] = name.decode() if name else "[unnamed]"
    except (FaultError, AttributeError) as e:
        for key in ("Page VMA", "VMA Start", "VMA End", "VMA Flags"):
            fields[key] = "Unavailable"
        print(f"Error retrieving VMA information: {e}")


# ---------------------------------------------------------------------------
# Main page state display
# ---------------------------------------------------------------------------

def show_page_state(page, addr=0, mm=None, pid=None, task=None):
    """Display detailed information about a page."""
    try:
        if pid is not None and task is not None:
            comm = task.comm.string_().decode()
            print(f"PID: {pid} Comm: {comm} mm: {hex(mm)}")
        print(format_page_data(page))

        head = compound_head(page) if PageCompound(page) else page

        fields = _base_fields(page, head)
        fields["Page Status"] = _page_status(head)

        if PageSlab(head):
            _slab_fields(head, fields)
        else:
            _page_fields(head, fields)

        _compound_fields(page, head, fields)
        _memcg_fields(head, fields)
        _rmap_fields(page, head, fields, show_detail=(mm is None))

        if mm is not None:
            _vma_fields(mm, addr, fields)
            _pte_fields(mm, addr, fields)

        _print_fields(fields)
    except FaultError as e:
        print(f"Error accessing page state: {e}")
    except Exception as e:
        print(f"Unexpected error: {e}")


# ---------------------------------------------------------------------------
# Input validation
# ---------------------------------------------------------------------------

def _validate_pfn(pfn):
    """Validate PFN is within the valid range."""
    max_pfn = prog["max_pfn"].value_()
    if pfn < 0 or pfn >= max_pfn:
        sys.exit(f"PFN {hex(pfn)} out of range (valid: 0 - {hex(max_pfn - 1)})")
    return pfn


def _validate_page_addr(addr):
    """Validate page struct address is within vmemmap range."""
    try:
        base = prog["vmemmap"].value_()
    except KeyError:
        sys.exit("Cannot determine vmemmap base address")
    page_size = prog.type("struct page").size
    max_pfn = prog["max_pfn"].value_()
    end = base + max_pfn * page_size
    if addr < base or addr >= end:
        sys.exit(f"Page address {hex(addr)} out of vmemmap range "
                 f"({hex(base)} - {hex(end - 1)})")
    if (addr - base) % page_size != 0:
        nearest = base + ((addr - base) // page_size) * page_size
        sys.exit(f"Page address {hex(addr)} not aligned to struct page size "
                 f"({page_size}), nearest valid: {hex(nearest)}")
    return addr


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _count_distinct_memcg(page, stride, mask):
    """Count distinct memcg pointers in a slab page's obj_exts array."""
    base = page.memcg_data.value_() & ~mask
    if base == 0:
        return 0
    try:
        n = cast("struct slab *", page).objects.value_()
    except (AttributeError, FaultError):
        return 0
    return len(_collect_memcg_ptrs(base, n, stride))


def _scan_page_types():
    """Scan all pages and categorize by type. Returns {type: page} dict."""
    from drgn.helpers.linux.mm import for_each_page

    objexts_flag = prog.constant("MEMCG_DATA_OBJEXTS").value_()
    mask = prog.constant('__NR_MEMCG_DATA_FLAGS').value_() - 1

    found = {}
    needed = ["anon", "file", "compound_anon", "compound_file",
              "slab_multi", "slab_single", "slab_no_objcg", "uncharged"]

    for page in for_each_page(prog):
        try:
            head = compound_head(page) if PageCompound(page) else page

            if PageSlab(head):
                if head.memcg_data.read_() & objexts_flag:
                    try:
                        slab = cast("struct slab *", head)
                        if slab.obj_exts_needs_objcg.value_():
                            slab_stride = _slab_obj_ext_stride(slab)
                            n = _count_distinct_memcg(head, slab_stride, mask)
                            if n >= 2 and "slab_multi" not in found:
                                found["slab_multi"] = page
                            elif n == 1 and "slab_single" not in found:
                                found["slab_single"] = page
                        elif "slab_no_objcg" not in found:
                            found["slab_no_objcg"] = page
                    except (AttributeError, FaultError):
                        pass
                continue

            if PageCompound(page):
                mapping = head.mapping.value_()
                if mapping & 0x1 and "compound_anon" not in found:
                    found["compound_anon"] = page
                elif mapping and not (mapping & 0x1) and "compound_file" not in found:
                    found["compound_file"] = page
                continue

            if page.memcg_data.read_() == 0:
                if "uncharged" not in found:
                    found["uncharged"] = page
                continue

            mapping = head.mapping.value_()
            if mapping == 0:
                continue
            mapcount = head._mapcount.counter.value_()
            if mapping & 0x1:
                if "anon" not in found and mapcount >= 0:
                    found["anon"] = page
            else:
                if "file" not in found and mapcount >= 0:
                    found["file"] = page
        except FaultError:
            continue

        if all(k in found for k in needed):
            break

    return found


def run_self_test():
    """Scan for various page types and verify show_page_state output."""
    print("Scanning pages...")
    found = _scan_page_types()

    labels = [
        ("anon", "Single Anonymous page"),
        ("file", "Single File-backed page"),
        ("compound_anon", "Compound Anonymous page"),
        ("compound_file", "Compound File-backed page"),
        ("slab_multi", "Slab page with multiple memcgs"),
        ("slab_single", "Slab page with single memcg"),
        ("slab_no_objcg", "Slab page without objcg (codetag only)"),
        ("uncharged", "Uncharged page (memcg_data == 0)"),
    ]

    test_num = 1
    for key, label in labels:
        print(f"\n{'='*60}")
        if key in found:
            print(f"Test {test_num}: {label}")
            print(f"PFN: {hex(page_to_pfn(found[key]).value_())}")
            print(f"{'='*60}")
            show_page_state(found[key])
        else:
            print(f"Test {test_num}: {label}")
            print(f"{'='*60}")
            print(f"[SKIP] No {label} found")
        test_num += 1

    # Input validation tests
    print(f"\n{'='*60}")
    print(f"Test {test_num}: Invalid PFN (out of range)")
    print(f"{'='*60}")
    try:
        _validate_pfn(prog["max_pfn"].value_() + 1)
    except SystemExit as e:
        print(f"Correctly rejected: {e}")
    test_num += 1

    print(f"\n{'='*60}")
    print(f"Test {test_num}: Invalid page address (unaligned)")
    print(f"{'='*60}")
    try:
        _validate_page_addr(prog["vmemmap"].value_() + 1)
    except SystemExit as e:
        print(f"Correctly rejected: {e}")

    print(f"\n{'='*60}")
    print(f"All {test_num} tests completed!")
    print(f"{'='*60}")


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

def main():
    """Parse arguments and display page state."""
    parser = argparse.ArgumentParser(
        description=DESC, formatter_class=argparse.RawTextHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--pfn', metavar='PFN', type=lambda x: int(x, 0),
                       help='Page Frame Number (decimal or hex, e.g. 0x12345)')
    group.add_argument('--page', metavar='ADDR', type=lambda x: int(x, 0),
                       help='Page struct address (decimal or hex, e.g. 0xffffea0001234000)')
    group.add_argument('--user', nargs=2, metavar=('PID', 'VADDR'),
                       help='User-space lookup: PID and virtual address')
    group.add_argument('--selftest', action='store_true',
                       help='Run self-test: scan for various page types')
    args = parser.parse_args()

    try:
        if args.selftest:
            run_self_test()
        elif args.pfn is not None:
            show_page_state(pfn_to_page(prog, _validate_pfn(args.pfn)))
        elif args.page is not None:
            show_page_state(Object(prog, "struct page *",
                                   _validate_page_addr(args.page)))
        elif args.user is not None:
            pid = int(args.user[0], 0)
            vaddr = int(args.user[1], 0)
            task = find_task(pid)
            mm = task.mm
            page = follow_page(mm, vaddr)
            if page:
                show_page_state(page, vaddr, mm, pid, task)
            else:
                sys.exit(f"Address {hex(vaddr)} is not mapped.")
    except FaultError as e:
        sys.exit(f"Error accessing task or memory: {e}")
    except Exception as e:
        sys.exit(f"Unexpected error: {e}")


if __name__ == "__main__":
    main()
