#!/usr/bin/env bash
# Download one day of NASDAQ TotalView-ITCH 5.0 and decompress it for replay.
#
#   scripts/fetch_itch.sh [MMDDYYYY]        default: 01302019 (4.8 GB, 11.2 GB unpacked)
#
# Integrity: NASDAQ's .md5sum links for the 2019 files return 404, so the
# check is gzip's own CRC32 and length trailer, verified while decompressing
# (gzip exits non-zero on any corruption). A resumed download (-C -) only
# appends, so a corrupted partial file must be deleted by hand.
set -euo pipefail

day="${1:-01302019}"
name="${day}.NASDAQ_ITCH50"
url="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${name}.gz"
dir="data/itch"

mkdir -p "$dir"
cd "$dir"
if [[ ! -f "$name" ]]; then
    curl --fail --location --retry 10 --retry-all-errors -C - -o "${name}.gz" "$url"
    gzip -dc "${name}.gz" > "${name}.partial"   # fails, and stops here, on a bad CRC
    mv "${name}.partial" "$name"
fi
ls -l "$name"
