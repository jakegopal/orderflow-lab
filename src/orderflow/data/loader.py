"""Turn raw venue recordings into a canonical event stream.

Format and venue translation. Read the recorder's JSONL,
parse each raw update into a BookUpdate, emit them in order grouped into whole
messages, and flag where the message sequence is broken. Nothing downstream
should know the bytes came from Coinbase.

Three conversions happen here, each a deliberate choice:
  price_level  "76289.31"  -> int ticks 7628931   (exact, hashable, guarded)
  new_quantity "0.2611"    -> float 0.2611         (a magnitude, never a key)
  event_time   ISO-8601    -> int nanoseconds      (the schema's `ts`)
"""
import json
import warnings
from collections.abc import Iterator
from datetime import datetime
from pathlib import Path
from typing import Any

from orderflow.data.schema import BookMessage, BookUpdate, Side

TICKS_PER_UNIT = 100  # USD cents; Coinbase BTC-USD tick size is $0.01


def price_to_ticks(price_level: str) -> int:
    """Convert a decimal price string to integer ticks.

    Raises ValueError if the price is finer than the tick size -- a loud failure
    beats silently truncating sub-cent prices and corrupting the book.
    """
    scaled = float(price_level) * TICKS_PER_UNIT
    ticks = round(scaled)
    if abs(scaled - ticks) > 1e-6:
        raise ValueError(f"price {price_level!r} is finer than 1/{TICKS_PER_UNIT} tick")
    return ticks


def iso_to_ns(event_time: str) -> int:
    """Convert an ISO-8601 UTC timestamp to exact integer nanoseconds since epoch.

    Parses the sub-second digits as an integer rather than routing through a
    float (datetime.timestamp() loses precision past ~microseconds at 2026
    magnitudes). Splits on the decimal point, scales the fractional digits to
    nanoseconds exactly, and adds them to the whole-seconds part.
    """
    # Normalise the trailing 'Z' to an explicit UTC offset datetime can parse.
    text = event_time.replace("Z", "+00:00")
    if "." in text:
        whole, frac = text.split(".")
        # frac looks like "509738" or "509738+00:00"; peel off any offset.
        digits = frac
        offset = ""
        for marker in ("+", "-"):
            if marker in digits:
                digits, offset = digits.split(marker, 1)
                offset = marker + offset
                break
        whole = whole + offset
    else:
        whole, digits = text, ""

    dt = datetime.fromisoformat(whole)
    whole_seconds_ns = int(dt.timestamp()) * 1_000_000_000

    # Scale fractional digits to exactly 9 places (nanoseconds), no float.
    digits = (digits + "000000000")[:9]
    frac_ns = int(digits) if digits else 0
    return whole_seconds_ns + frac_ns


# Coinbase's names for the two sides, mapped to the schema's. Coinbase calls the
# sell side "offer"; the rest of this codebase says "ask".
COINBASE_SIDES: dict[str, Side] = {"bid": "bid", "offer": "ask"}


def parse_side(side: str) -> Side:
    """Map a Coinbase side to the schema's, rejecting anything unexpected.

    Strict on purpose: a side the code doesn't recognise must stop the run, not
    be silently filed under the ask side.
    """
    try:
        return COINBASE_SIDES[side]
    except KeyError:
        raise ValueError(f"unknown book side {side!r}") from None


def parse_update(update: dict[str, Any], seq: int, is_snapshot: bool = False) -> BookUpdate:
    """Parse one raw level-2 update dict into a BookUpdate."""
    return BookUpdate(
        ts=iso_to_ns(update["event_time"]),
        side=parse_side(update["side"]),
        price_ticks=price_to_ticks(update["price_level"]),
        size=float(update["new_quantity"]),
        seq=seq,
        is_snapshot=is_snapshot,
    )


def iter_updates(raw_message: dict[str, Any]) -> Iterator[BookUpdate]:
    """Yield one BookUpdate per level change in a single recorded message.

    Tags each update with its message's seq and whether its event was a snapshot.
    """
    seq = raw_message["sequence_num"]
    for event in raw_message["events"]:
        is_snapshot = event["type"] == "snapshot"
        for update in event["updates"]:
            yield parse_update(update, seq, is_snapshot)


def load_messages(path: Path) -> Iterator[BookMessage]:
    """Yield each book message from a recorder JSONL file, flagging discontinuities.

    Gap detection lives here because this is the only stage that sees every message
    on the connection. Coinbase's sequence_num counts every message across all
    channels, so a subscriptions ack between two book messages takes a seq of its
    own. Dropping it before checking continuity invents a gap that never happened.
    So every message advances the count, and only l2_data messages are yielded.

    Any break in seq + 1 continuity sets gap_before on the next book message: a
    skip, a repeat, or a reset to 0 on reconnect. The first message sets it too,
    because nothing before it proves continuity.

    Each line is one recorded message: {"recv_ns": ..., "raw": {...}}. A damaged
    line is one that is not valid JSON. The recorder leaves one behind when it is
    killed mid-write (a truncated line), or when the machine shuts down before the
    write reaches disk (a run of NUL bytes). The message on that line is lost
    either way, and crashing would also throw away every good message before it.
    So skip it, warn with the file and line number so the loss is visible, and
    count it as a gap, since we can't know which seq it carried.
    """
    gap_pending = True
    last_seq: int | None = None
    with path.open(encoding="utf-8") as f:
        for line_no, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                warnings.warn(
                    f"{path}:{line_no}: skipped damaged line ({len(line)} chars)",
                    stacklevel=2,
                )
                gap_pending = True
                continue
            raw = record["raw"]
            seq = raw["sequence_num"]
            if last_seq is not None and seq != last_seq + 1:
                gap_pending = True
            last_seq = seq
            if raw["channel"] != "l2_data":
                continue
            yield BookMessage(
                seq=seq,
                ts=iso_to_ns(raw["timestamp"]),
                is_snapshot=any(e["type"] == "snapshot" for e in raw["events"]),
                gap_before=gap_pending,
                updates=tuple(iter_updates(raw)),
            )
            gap_pending = False
