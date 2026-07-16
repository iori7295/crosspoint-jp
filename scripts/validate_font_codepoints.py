#!/usr/bin/env python3
"""
Validate that font files contain required Unicode codepoints.

Checks both built-in .h fonts and SD card .cpfont files for a given
set of "required" codepoints (e.g. common JIS kanji, UI labels, etc).

Usage:
  python3 scripts/validate_font_codepoints.py \\
    --cpfont path/to/font.cpfont \\
    --required 0x4E00,0x4EE3,0x8A33...
"""
import struct
import sys
import re
import os
import json
from pathlib import Path


def parse_cpfont(filepath: str) -> list:
    """Parse a .cpfont file and return list of (codepoint, style) pairs."""
    with open(filepath, 'rb') as f:
        data = f.read()
    
    # Validate magic
    if data[:8] != b'CPFONT\x00\x00':
        raise ValueError(f"Invalid magic in {filepath}")
    
    ver = struct.unpack_from('<H', data, 8)[0]
    if ver != 4:
        raise ValueError(f"Unsupported version {ver} in {filepath} (expected 4)")
    
    style_count = data[12]
    intervals = []
    
    for s in range(style_count):
        off = 32 + s * 32  # TOC starts at offset 32, each entry 32 bytes
        iv = struct.unpack_from('<I', data, off + 4)[0]
        gl = struct.unpack_from('<I', data, off + 8)[0]
        data_off = struct.unpack_from('<I', data, off + 24)[0]
        
        # Read interval table (12 bytes per entry)
        iv_off = data_off
        for i in range(iv):
            start = struct.unpack_from('<I', data, iv_off + i * 12)[0]
            end = struct.unpack_from('<I', data, iv_off + i * 12 + 4)[0]
            for cp in range(start, end + 1):
                intervals.append(cp)
    
    return intervals


def parse_builtin_h(filepath: str) -> list:
    """Parse a built-in .h font file and return list of codepoints."""
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Find interval array
    m = re.search(r'Intervals\[\] = \{(.*?)\n\};', content, re.DOTALL)
    if not m:
        raise ValueError(f"Could not find interval array in {filepath}")
    
    body = m.group(1)
    intervals_raw = re.findall(r'0x([0-9A-F]+),\s*0x([0-9A-F]+),\s*(\d+)', body)
    
    codepoints = set()
    for first_s, last_s, _ in intervals_raw:
        first = int(first_s, 16)
        last = int(last_s, 16)
        for cp in range(first, last + 1):
            codepoints.add(cp)
    
    return sorted(codepoints)


def check_required(codepoints: set, required: list, name: str) -> bool:
    """Check if all required codepoints are in the set."""
    all_ok = True
    for cp in required:
        if cp not in codepoints:
            try:
                ch = chr(cp)
            except:
                ch = '?'
            print(f"  ✗ MISSING U+{cp:04X} ({ch}) in {name}")
            all_ok = False
    return all_ok


def main():
    parser = argparse.ArgumentParser(description='Validate font codepoint coverage')
    parser.add_argument('--cpfont', help='.cpfont file to check')
    parser.add_argument('--h', dest='hfile', help='built-in .h font file to check')
    parser.add_argument('--required', type=str, 
                        help='Comma-separated hex codepoints (e.g. 0x4E00,0x4EE3)')
    parser.add_argument('--required-file', type=str,
                        help='File with one hex codepoint per line')
    parser.add_argument('--check-ui', action='store_true',
                        help='Use built-in UI required set from japanese.yaml')
    
    args = parser.parse_args()
    
    if not args.required and not args.required_file and not args.check_ui:
        print("Specify --required, --required-file, or --check-ui")
        return
    
    # Collect required codepoints
    required = set()
    if args.required:
        for s in args.required.split(','):
            required.add(int(s.strip(), 16))
    if args.required_file:
        with open(args.required_file) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    required.add(int(line, 16))
    if args.check_ui:
        # Load japanese.yaml for UI required set
        import yaml
        yaml_path = next(
            (p for p in [os.path.join(os.path.dirname(__file__), '../lib/I18n/translations/japanese.yaml'),
                          os.path.join(os.getcwd(), 'lib/I18n/translations/japanese.yaml')]
             if os.path.exists(p)), None)
        if yaml_path:
            with open(yaml_path) as f:
                data = yaml.safe_load(f)
            for key, value in data.items():
                if isinstance(value, str):
                    for ch in value:
                        cp = ord(ch)
                        if cp > 0x007F:
                            required.add(cp)
            print(f"Loaded {len(required)} UI codepoints from japanese.yaml")
    
    required = sorted(required)
    print(f"Required codepoints: {len(required)}")
    
    # Check font file
    if args.cpfont:
        cps = set(parse_cpfont(args.cpfont))
        name = os.path.basename(args.cpfont)
        print(f"\n.cpfont file: {name} ({len(cps)} codepoints)")
        ok = check_required(cps, required, name)
        print(f"Result: {'✓ ALL OK' if ok else '✗ SOME MISSING'}")
    
    if args.hfile:
        cps = set(parse_builtin_h(args.hfile))
        name = os.path.basename(args.hfile)
        print(f"\n.h file: {name} ({len(cps)} codepoints)")
        ok = check_required(cps, required, name)
        print(f"Result: {'✓ ALL OK' if ok else '✗ SOME MISSING'}")


if __name__ == '__main__':
    import argparse
    main()
