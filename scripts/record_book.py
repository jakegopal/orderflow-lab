"""Record the Coinbase level2 feed for one product to daily JSONL files.

One job, done minimally: capture each raw message plus our receive-time,
append one line per message, start a new file each UTC day, and reconnect
with backoff when the connection drops. No parsing beyond a JSON round-trip
that validates the message and normalises it to a single line -- book
reconstruction happens later, offline, where a bug is re-runnable.
"""

import json
import sys
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import TextIO

from websockets.exceptions import ConnectionClosed
from websockets.sync.client import connect

URI = "wss://advanced-trade-ws.coinbase.com"
PRODUCT = "BTC-USD"
DATA_DIR = Path("data")
SUBSCRIBE = {"type": "subscribe", "product_ids": [PRODUCT], "channel": "level2"}


def log(message: str) -> None:
    """Timestamped line to stderr, so it never lands in the data files."""
    stamp = datetime.now(UTC).isoformat()
    print(f"{stamp}  {message}", file=sys.stderr, flush=True)


def path_for(day: str) -> Path:
    """Data file for a given UTC date string, e.g. data/coinbase_BTC-USD_2026-08-21.jsonl."""
    return DATA_DIR / f"coinbase_{PRODUCT}_{day}.jsonl"


def open_for_today(current: TextIO | None, current_day: str) -> tuple[TextIO, str]:
    """Return an open file for today (UTC), rotating if the day changed.

    Reuses `current` when the day is unchanged; otherwise closes it and opens
    the new day's file. Always returns a real file, never None.
    """
    day = datetime.now(UTC).strftime("%Y-%m-%d")
    if day == current_day and current is not None:
        return current, current_day
    if current is not None:
        current.close()
    path = path_for(day)
    log(f"writing to {path}")
    return path.open("a", encoding="utf-8"), day


def record_forever() -> None:
    """Outer loop: keep a connection alive, reconnecting with backoff on drop."""
    DATA_DIR.mkdir(exist_ok=True)
    delay = 1.0
    out: TextIO | None = None
    current_day = ""

    try:
        while True:
            try:
                log(f"connecting to {URI}")
                with connect(URI, max_size=None) as ws:
                    ws.send(json.dumps(SUBSCRIBE))
                    log(f"subscribed to level2 {PRODUCT}")
                    delay = 1.0

                    for raw in ws:
                        recv_ns = time.time_ns()
                        out, current_day = open_for_today(out, current_day)
                        record = {"recv_ns": recv_ns, "raw": json.loads(raw)}
                        out.write(json.dumps(record) + "\n")

            except (ConnectionClosed, OSError) as exc:
                log(f"disconnected: {exc}; reconnecting in {delay:.0f}s")
                time.sleep(delay)
                delay = min(delay * 2, 30.0)
    except KeyboardInterrupt:
        log("stopped by user")
    finally:
        if out is not None:
            out.close()


if __name__ == "__main__":
    record_forever()
