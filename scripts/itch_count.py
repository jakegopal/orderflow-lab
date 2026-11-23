"""Count ITCH 5.0 messages by type, independently of the C++ parser.

A second implementation of the framing rule (2-byte big-endian length, then
the message, whose first byte is its type), in a different language with no
shared code. Its counts must equal `itch_stats` exactly. It also checks every
message's length against the spec table, written out again here rather than
imported, so a typo in one table can't hide in both.

    python scripts/itch_count.py data/itch/01302019.NASDAQ_ITCH50
"""

import mmap
import sys
from collections import Counter
from pathlib import Path

SPEC_LENGTH = {
    "S": 12, "R": 39, "H": 25, "Y": 20, "L": 26, "V": 35, "W": 12, "K": 28, "J": 35,
    "h": 21, "A": 36, "F": 40, "E": 31, "C": 36, "X": 23, "D": 19, "U": 35, "P": 44,
    "Q": 40, "B": 19, "I": 50, "N": 20, "O": 48,
}


def count(path: Path) -> tuple[Counter[str], int, bool]:
    """Per-type counts, number of length mismatches, and whether the file is cut short."""
    counts: Counter[str] = Counter()
    bad = 0
    with path.open("rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        pos, end = 0, len(data)
        while pos + 2 <= end:
            length = int.from_bytes(data[pos : pos + 2], "big")
            if pos + 2 + length > end:
                return counts, bad, True
            kind = chr(data[pos + 2])
            counts[kind] += 1
            if SPEC_LENGTH.get(kind) != length:
                bad += 1
            pos += 2 + length
        return counts, bad, pos != end


def main() -> None:
    counts, bad, truncated = count(Path(sys.argv[1]))
    for kind in sorted(counts):
        print(f"{kind} {counts[kind]:12d}")
    print(f"total {sum(counts.values())} messages, {bad} malformed, "
          f"truncated: {'yes' if truncated else 'no'}")


if __name__ == "__main__":
    main()
