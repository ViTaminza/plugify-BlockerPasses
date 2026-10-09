#!/usr/bin/env python3
"""Scan a game binary for the raw byte patterns of the BlockerPasses gamedata.

Reads the binary as bytes only (never executes/interprets it). Run with:
    python3 -I plugins/BlockerPasses/tools/scan_signature.py [binary]
"""
import re
import sys

DEFAULT_BINARY = "/srv/cs2-go/game/csgo/bin/linuxsteamrt64/libserver.so"

# The pattern ABKAM's BlockerPasses uses for SetCollisionBounds
# ("48 81 C7 ? ? ? ? E9 ? ? ? ? CC CC CC CC 55 48 8D 15").
PATTERNS = {
    "SetCollisionBounds (original, with int3 padding)":
        rb"\x48\x81\xC7....\xE9....\xCC\xCC\xCC\xCC\x55\x48\x8D\x15",
    "SetCollisionBounds (without padding)":
        rb"\x48\x81\xC7....\xE9....",
}


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BINARY
    with open(path, "rb") as handle:
        data = handle.read()
    print(f"binary: {path} ({len(data)} bytes)")
    for name, pattern in PATTERNS.items():
        hits = [m.start() for m in re.finditer(pattern, data, re.DOTALL)]
        print(f"{name}: {len(hits)} hit(s) {[hex(h) for h in hits[:20]]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
