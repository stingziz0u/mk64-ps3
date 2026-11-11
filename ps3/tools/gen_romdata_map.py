#!/usr/bin/env python3
"""Writes ps3/gen/romdata_map.c: how to rebuild, from the user's ROM, the data
tables that the N64 keeps in its code segments (main, racing, ending) and that
the port compiles in instead.

Mods change those tables (where each kart sprite, palette or course segment
is in the ROM, character stats...). With this map the port takes them from
whatever ROM it loads, as the N64 does, instead of using the stock values.

For every N64 data symbol of those segments that the PS3 binary also defines
as a global, writable symbol, the PS3 type (from the DWARF) is laid out again
with N64 sizes (pointers and longs are 4 bytes there) and turned into a list
of operations:

  COPY  n bytes, the same on both sides
  PTR   n 4-byte N64 words into 8-byte PS3 pointers; a word that is an N64 RAM
        address (0x80xxxxxx) is left alone at run time (the PS3 address of
        the same thing is already there)

Each symbol is checked against the stock ROM: the operations applied to it
must give exactly the bytes compiled into the PS3 binary. Symbols that fail
(changed for the port, unknown layout...) are left out. So with the stock
ROM the whole map changes nothing.

Only needed when the game's data tables change. Inputs: the N64 matching build
(mk64.us.elf), the PS3 build (mk64ps3.elf, with DWARF) and the stock USA ROM.

    python3 ps3/tools/gen_romdata_map.py mk64.us.elf ps3/mk64ps3.elf baserom.us.z64 > ps3/gen/romdata_map.c
"""
import sys
import struct
from elftools.elf.elffile import ELFFile

N64_ELF, PS3_ELF, ROM_PATH = sys.argv[1:4]
ROM = open(ROM_PATH, 'rb').read()

# (N64 vaddr start, end, ROM offset): the code segments with their data
SEGS = [(0x80000400, 0x800F6910, 0x00001000),
        (0x8028DF00, 0x802BA030, 0x000F7510),
        (0x80280000, 0x802874A0, 0x00123640)]


def rom_of(a):
    for s, e, r in SEGS:
        if s <= a < e:
            return a - s + r
    return None


def objects(elf):
    out = {}
    for s in elf.get_section_by_name('.symtab').iter_symbols():
        if (s['st_info']['type'] == 'STT_OBJECT' and s['st_size'] > 0
                and s['st_shndx'] not in ('SHN_UNDEF', 'SHN_ABS', 'SHN_COMMON')):
            out.setdefault(s.name, []).append(s)
    return out


n64f = open(N64_ELF, 'rb')
ps3f = open(PS3_ELF, 'rb')
n64 = ELFFile(n64f)
ps3 = ELFFile(ps3f)
nsyms = objects(n64)
psyms = objects(ps3)

# ---- DWARF: the type of every global variable --------------------------------
var_type = {}
dwarf = ps3.get_dwarf_info()
for cu in dwarf.iter_CUs():
    for die in cu.iter_DIEs():
        if die.tag != 'DW_TAG_variable' or 'DW_AT_name' not in die.attributes:
            continue
        if 'DW_AT_type' not in die.attributes:
            continue
        name = die.attributes['DW_AT_name'].value.decode()
        if die.get_parent() is None or die.get_parent().tag != 'DW_TAG_compile_unit':
            continue
        t = die.get_DIE_from_attribute('DW_AT_type')
        # a declaration may come first; keep any, a definition wins
        if name not in var_type or 'DW_AT_declaration' not in die.attributes:
            var_type[name] = t


class Bad(Exception):
    pass


def strip(t):
    while t.tag in ('DW_TAG_typedef', 'DW_TAG_const_type', 'DW_TAG_volatile_type'):
        if 'DW_AT_type' not in t.attributes:
            raise Bad('void')
        t = t.get_DIE_from_attribute('DW_AT_type')
    return t


def array_counts(t):
    n = []
    for c in t.iter_children():
        if c.tag != 'DW_TAG_subrange_type':
            continue
        if 'DW_AT_count' in c.attributes:
            n.append(c.attributes['DW_AT_count'].value)
        elif 'DW_AT_upper_bound' in c.attributes:
            n.append(c.attributes['DW_AT_upper_bound'].value + 1)
        else:
            n.append(None)
    return n


# layout(t) -> (ps3 size, n64 size, n64 align, ops)
# ops: list of (kind, ps3_off, n64_off, n) with kind 'C' (n bytes) or 'P' (n words)
memo = {}


def layout(t, total=None):
    t = strip(t)
    key = t.offset
    if total is None and key in memo:
        return memo[key]
    tag = t.tag
    if tag in ('DW_TAG_base_type', 'DW_TAG_enumeration_type'):
        sz = t.attributes['DW_AT_byte_size'].value
        name = t.attributes['DW_AT_name'].value.decode() if 'DW_AT_name' in t.attributes else ''
        if sz == 8 and 'long' in name and 'long long' not in name and 'double' not in name:
            raise Bad('long')
        r = (sz, sz, min(sz, 8), [('C', 0, 0, sz)])
    elif tag in ('DW_TAG_pointer_type', 'DW_TAG_subroutine_type'):
        r = (8, 4, 4, [('P', 0, 0, 1)])
    elif tag == 'DW_TAG_array_type':
        ps, ns, na, ops = layout(t.get_DIE_from_attribute('DW_AT_type'))
        n = 1
        for c in array_counts(t):
            if c is None:
                if total is None or ps == 0 or total % (ps * n):
                    raise Bad('flexible array')
                c = total // (ps * n)   # "T x[]" declared: the size of the symbol
            n *= c
        out = []
        for i in range(n):
            out += [(k, po + i * ps, no + i * ns, c) for k, po, no, c in ops]
        r = (ps * n, ns * n, na, out)
    elif tag in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
        if 'DW_AT_byte_size' not in t.attributes:
            raise Bad('incomplete')
        psz = t.attributes['DW_AT_byte_size'].value
        members = [c for c in t.iter_children() if c.tag == 'DW_TAG_member']
        subs = []
        for m in members:
            if 'DW_AT_bit_size' in m.attributes or 'DW_AT_data_bit_offset' in m.attributes:
                subs.append(None)
            else:
                subs.append(layout(m.get_DIE_from_attribute('DW_AT_type')))
        same = all(s is not None and s[0] == s[1] and all(k == 'C' for k, *_ in s[3]) for s in subs)
        if same or any(s is None for s in subs):
            if not same:
                # bitfields: fine only if every other member is the same on both sides
                if not all(s is None or (s[0] == s[1] and all(k == 'C' for k, *_ in s[3])) for s in subs):
                    raise Bad('bitfields with pointers')
            al = max([s[2] for s in subs if s] + [1])
            r = (psz, psz, al, [('C', 0, 0, psz)])
        elif tag == 'DW_TAG_union_type':
            raise Bad('union with pointers')
        else:
            off, al, out = 0, 1, []
            for m, s in zip(members, subs):
                ps, ns, na, ops = s
                pmo = m.attributes['DW_AT_data_member_location'].value
                off = (off + na - 1) // na * na
                out += [(k, po + pmo, no + off, c) for k, po, no, c in ops]
                off += ns
                al = max(al, na)
            nsz = (off + al - 1) // al * al
            r = (psz, nsz, al, out)
    else:
        raise Bad(tag)
    if total is None:
        memo[key] = r
    return r


def merge(ops):
    out = []
    for k, po, no, c in ops:
        if out:
            k0, p0, n0, c0 = out[-1]
            if k == k0 == 'C' and p0 + c0 == po and n0 + c0 == no:
                out[-1] = (k, p0, n0, c0 + c)
                continue
            if k == k0 == 'P' and p0 + 8 * c0 == po and n0 + 4 * c0 == no:
                out[-1] = (k, p0, n0, c0 + c)
                continue
        out.append((k, po, no, c))
    return out


sections = [ps3.get_section(i) for i in range(ps3.num_sections())]
writable = {i for i, s in enumerate(sections) if s['sh_flags'] & 1 and s['sh_type'] != 'SHT_NOBITS'}

entries, why = [], {}
for name, lst in sorted(nsyms.items()):
    if len(lst) != 1:
        continue
    na_ = lst[0]['st_value']
    nsz = lst[0]['st_size']
    rom = rom_of(na_)
    if rom is None or name not in psyms or len(psyms[name]) != 1:
        continue
    ps = psyms[name][0]
    if ps['st_info']['bind'] != 'STB_GLOBAL':
        why[name] = 'static'
        continue
    if ps['st_shndx'] not in writable:
        why[name] = 'read-only or bss'
        continue
    if name not in var_type:
        why[name] = 'no DWARF'
        continue
    try:
        psz, ns, _, ops = layout(var_type[name], ps['st_size'])
    except Bad as e:
        why[name] = 'layout: %s' % e
        continue
    if psz != ps['st_size'] or ns != nsz:
        why[name] = 'size %d/%d vs %d/%d' % (psz, ns, ps['st_size'], nsz)
        continue
    sec = sections[ps['st_shndx']]
    data = sec.data()
    pb = data[ps['st_value'] - sec['sh_addr']:][:psz]
    nb = ROM[rom:rom + nsz]
    ops = merge(ops)
    ok = True
    kept = []
    for k, po, no, c in ops:
        if k == 'C':
            if pb[po:po + c] != nb[no:no + c]:
                ok = False
                break
            kept.append((k, po, no, c))
            continue
        for i in range(c):
            v = struct.unpack('>I', nb[no + 4 * i:no + 4 * i + 4])[0]
            p = struct.unpack('>Q', pb[po + 8 * i:po + 8 * i + 8])[0]
            if (v >> 24) == 0x80 and p != 0:
                continue        # an N64 RAM pointer: the PS3 one stays, always
            if p != v:
                ok = False
                break
            kept.append(('P', po + 8 * i, no + 4 * i, 1))
        if not ok:
            break
    if not ok:
        why[name] = 'differs from the stock ROM'
        continue
    kept = merge(kept)
    if kept:
        entries.append((name, rom, nsz, kept))

print('/* Generated by ps3/tools/gen_romdata_map.py -- do not edit. */')
print('/* %d tables of the N64 code segments, rebuilt from the ROM that is loaded. */' % len(entries))
print('#include "ps3_romdata_map.h"\n')
for name, rom, nsz, ops in entries:
    print('extern char %s[];' % name)
print()
for name, rom, nsz, ops in entries:
    print('static const ps3_romdata_op OP_%s[] = { %s };' % (
        name, ', '.join('{ %d, %d, %d, %d }' % (k == 'P', po, no, c) for k, po, no, c in ops)))
print('\nconst ps3_romdata_entry gPs3RomDataMap[] = {')
for name, rom, nsz, ops in entries:
    print('    { "%s", %s, 0x%06X, %d, OP_%s, %d },' % (name, name, rom, nsz, name, len(ops)))
print('};')
print('const int gPs3RomDataMapCount = %d;' % len(entries))

reasons = {}
for n, w in why.items():
    reasons.setdefault(w if w.startswith('layout') else w.split(' ')[0], []).append(n)
sys.stderr.write('%d tables in the map\n' % len(entries))
for w, ns in sorted(reasons.items()):
    sys.stderr.write('left out (%s): %d: %s\n' % (w, len(ns), ' '.join(sorted(ns)[:40])))
