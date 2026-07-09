#!/usr/bin/env python3
"""Import known Lenia organisms from the official Chakazul/Lenia repository
into our Presets/organisms/*.json schema (docs/specs/phase11_organisms.md).

WHY THIS FILE EXISTS (data provenance — do not hand-edit the outputs)
-----------------------------------------------------------------------
Lenia creature cell patterns are extremely sensitive: a single wrong float
in the `cells` grid and the "organism" dies instead of gliding. The spec
explicitly forbids writing cell data from memory. This script instead reads
the values from the official catalogue file and decodes them with a
byte-for-byte port of the official RLE decoder, so the numbers in
Presets/organisms/*.json are always literally what Chakazul/Lenia ships.

USAGE
-----
    git clone --depth 1 https://github.com/Chakazul/Lenia /tmp/lenia_official
    python3 tools/import_lenia_organism.py \
        --repo /tmp/lenia_official --out-dir Presets/organisms

Requires the clone above (not vendored — it's ~50MB with the full pattern
catalogue and unrelated Android/Matlab/R ports we don't need). Pure stdlib,
no pip installs required. Re-running regenerates byte-identical output
(decoding is a pure function of the catalogue's text — no randomness
anywhere in this pipeline), which is itself part of the phase11 completion
checklist (§5 "tools/import_lenia_organism.py を再実行すると同一の
orbium.json が再生成される").

RLE DECODER PROVENANCE
-----------------------
The functions in the "ported RLE decoder" section below are a faithful,
line-by-line port of:

    Repository: https://github.com/Chakazul/Lenia
    Commit:     adfc542939266de7f4bb7ebb552e8499701ee107 (2022-03-16)
    File:       Python/LeniaND.py
    Class:      Board (staticmethods)
      - DIM_DELIM              line 31
      - Board.ch2val            lines 108-112
      - Board._append_stack     lines 135-139
      - Board._recur_get_max_lens  lines 142-146
      - Board._recur_cubify     lines 149-156
      - Board.rle2arr           lines 169-191
      - Board.st2fracs          lines 198-199
      - Board.from_data (cells/params extraction contract)  lines 77-89

LeniaND.py implements this generically for N dimensions (DIM_DELIM has
entries up to 4D+); Chakazul/Lenia's own 2D catalogue (Python/animals.json,
which is what we consume — see also the JavaScript viewer's "Lenia
Expanded Universe" catalogue) only ever uses DIM=2 (row delimiter '$'), so
this port fixes DIM=2 and drops the unused 3D+ delimiter branches ('%',
'#', '@A'..'@F'). This was verified NOT to change behavior for 2D data: at
DIM=2 the original's top-level cubify padding (`more = max_lens[0] -
len(A)`) is always 0 (max_lens[0] is computed as exactly len(A)), so the
original's `list1.extend([[]] * more)` — which aliases the same empty-list
object `more` times — never actually executes for our case; only the
row-level padding (`list1.extend([0] * more)`, extending with immutable
ints) is ever exercised. This port was cross-checked against a literal
(numpy-based) transcription of the original run on both organisms this
script exports, byte-for-byte identical results (see scratch verification,
not part of this repo).

Known caveat (kernel/growth function family — see docs/specs comments in
Modules/FieldModules/Lenia.cpp): the official catalogue also carries `kn`/
`gn` (kernel-core / growth-function family selectors; both entries this
script exports use the default kn=1, gn=1 = "polynomial (quad4)" family).
Our engine's Lenia implementation only ever had one kernel/growth family
(a truncated-Gaussian bump, used by every existing preset). This script
still faithfully records kn/gn/betas for provenance, but LeniaModule does
not select a kernel family from them — only R, T, mu, sigma and the cells
bitmap feed the simulation (docs/specs/phase11_organisms.md's explicit
"useOrganismParams" override list). This is a deliberate, documented scope
boundary, not a data error.
"""

import argparse
import json
import os
import sys
from fractions import Fraction

# ---------------------------------------------------------------------------
# Ported RLE decoder (see module docstring for exact source provenance).
# Fixed to DIM=2 (row-major 2D cells, the only shape animals.json uses).
# ---------------------------------------------------------------------------

_ROW_DELIM = '$'  # DIM_DELIM[DIM-1] for DIM=2 (source line 31)


def ch2val(c):
    """Port of Board.ch2val (LeniaND.py:108-112)."""
    if c in '.b':
        return 0
    elif c == 'o':
        return 255
    elif len(c) == 1:
        return ord(c) - ord('A') + 1
    else:
        return (ord(c[0]) - ord('p')) * 24 + (ord(c[1]) - ord('A') + 25)


def _append_stack(list1, list2, count, is_repeat):
    """Port of Board._append_stack (LeniaND.py:135-139)."""
    list1.append(list2)
    if count != '':
        repeated = list2 if is_repeat else []
        list1.extend([repeated] * (int(count) - 1))


def rle2arr(st):
    """Port of Board.rle2arr (LeniaND.py:169-191), specialized to DIM=2.

    stacks[0] accumulates the current row's cell values (run-length
    expanded); '$' folds a completed row into stacks[1] (the row list).
    Trailing dead cells before '$'/'!' are omitted in the source encoding
    (classic RLE convention) — rows are right-padded with 0 to the widest
    row after decoding, exactly as the original's _recur_get_max_lens /
    _recur_cubify does.
    """
    row_stack = []  # stacks[0]
    rows = []       # stacks[1] == stacks[DIM-1]
    last, count = '', ''
    st = st.rstrip('!') + _ROW_DELIM
    for ch in st:
        if ch.isdigit():
            count += ch
        elif ch in 'pqrstuvwxy@':
            last = ch
        else:
            token = last + ch
            if token != _ROW_DELIM:
                _append_stack(row_stack, ch2val(token) / 255.0, count, is_repeat=True)
            else:
                # dim = delims.index('$') == 1 in the general algorithm ->
                # for d in range(1): fold stacks[0] into stacks[1].
                _append_stack(rows, row_stack, count, is_repeat=False)
                row_stack = []
            last, count = '', ''

    max_len = max((len(r) for r in rows), default=0)
    for r in rows:
        more = max_len - len(r)
        if more > 0:
            r.extend([0] * more)  # right-pad with dead cells (int 0, not aliased)
    return rows


def parse_betas(b_field):
    """Port of Board.st2fracs (LeniaND.py:198-199) + float conversion.

    b_field is normally a comma-separated string of fractions ("1",
    "1/2,1", ...) per animals.json; defensively also accept a JSON array
    of numbers in case a future catalogue entry stores it that way.
    """
    if isinstance(b_field, (list, tuple)):
        return [float(v) for v in b_field]
    fracs = [Fraction(tok) for tok in str(b_field).split(',')]
    return [float(f) for f in fracs]


def cells_field_to_string(cells):
    """Port of the from_data join step (LeniaND.py:86-87): some catalogue
    entries store `cells` as a list of line-strings instead of one string.
    """
    if isinstance(cells, (list, tuple)):
        return ''.join(cells)
    return cells


# ---------------------------------------------------------------------------
# animals.json extraction
# ---------------------------------------------------------------------------

def load_catalogue(repo_dir):
    path = os.path.join(repo_dir, 'Python', 'animals.json')
    if not os.path.isfile(path):
        sys.exit(
            'error: {} not found.\n'
            'This script needs a clone of the official Lenia repo:\n'
            '  git clone --depth 1 https://github.com/Chakazul/Lenia {}\n'
            .format(path, repo_dir)
        )
    with open(path, 'r', encoding='utf-8') as f:
        return json.load(f)


def find_by_code(entries, code):
    matches = [e for e in entries if e.get('code') == code and 'cells' in e and 'params' in e]
    if not matches:
        sys.exit('error: no catalogue entry with code={!r} (and cells+params) found'.format(code))
    if len(matches) > 1:
        print('warning: {} catalogue entries share code={!r}; using the first '
              '({!r})'.format(len(matches), code, matches[0].get('name')), file=sys.stderr)
    return matches[0]


def decode_organism(entry, repo_label):
    p = entry['params']
    cells_str = cells_field_to_string(entry['cells'])
    cells = rle2arr(cells_str)
    height = len(cells)
    width = len(cells[0]) if height else 0

    # Sanity check straight out of decode (not a fabrication risk -- this
    # only asserts properties of OUR OWN decode function, catches a broken
    # port rather than trusting it silently).
    for row in cells:
        assert len(row) == width, 'rle2arr produced a ragged array (decoder bug)'
        for v in row:
            assert 0.0 <= v <= 1.0, 'decoded cell value out of [0,1] (decoder bug)'

    return {
        'name': entry.get('name', ''),
        'code': entry.get('code', ''),
        'cname': entry.get('cname', ''),
        'source': 'Chakazul/Lenia Python/animals.json (code={}), {}'.format(
            entry.get('code', ''), repo_label),
        'R': p.get('R'),
        'T': p.get('T'),
        'mu': p.get('m'),
        'sigma': p.get('s'),
        'betas': parse_betas(p.get('b', '1')),
        'kn': p.get('kn', 1),
        'gn': p.get('gn', 1),
        'width': width,
        'height': height,
        'cells': cells,
    }


# Default export set (docs/specs/phase11_organisms.md: "Orbium（必須）+
# もう1〜2種（回転系 or 大型を任意選定）"). Orbium unicaudatus is the
# canonical glider; Gyrorbium gyrans is the suggested "回転系" (rotator)
# pick — same R/T as Orbium, chiral/asymmetric body that spins in place
# rather than translating.
DEFAULT_EXPORTS = [
    ('O2u', 'orbium.json'),
    ('OG2g', 'gyrorbium.json'),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', default='/tmp/lenia_official',
                     help='path to a clone of github.com/Chakazul/Lenia (default: %(default)s)')
    ap.add_argument('--out-dir', default='Presets/organisms',
                     help='output directory for organism JSON files (default: %(default)s)')
    ap.add_argument('--code', action='append', dest='codes', default=None,
                     help='catalogue "code" to export (repeatable). Default: Orbium + Gyrorbium.')
    args = ap.parse_args()

    entries = load_catalogue(args.repo)
    os.makedirs(args.out_dir, exist_ok=True)

    if args.codes:
        exports = [(code, code.lower().lstrip('0123456789') + '.json') for code in args.codes]
    else:
        exports = DEFAULT_EXPORTS

    for code, filename in exports:
        entry = find_by_code(entries, code)
        organism = decode_organism(entry, 'https://github.com/Chakazul/Lenia')
        out_path = os.path.join(args.out_dir, filename)
        with open(out_path, 'w', encoding='utf-8') as f:
            json.dump(organism, f, indent=2, ensure_ascii=False)
            f.write('\n')
        print('wrote {} ({}, {}x{}, R={} T={} mu={} sigma={})'.format(
            out_path, organism['name'], organism['width'], organism['height'],
            organism['R'], organism['T'], organism['mu'], organism['sigma']))


if __name__ == '__main__':
    main()
