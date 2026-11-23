"""Tests for the loader's three conversions, checked against hand-verified values."""
import json
from pathlib import Path
from typing import Any

import pytest

from orderflow.data.loader import (
    iso_to_ns,
    iter_updates,
    load_messages,
    parse_side,
    parse_update,
    price_to_ticks,
)


def test_price_to_ticks_exact() -> None:
    assert price_to_ticks("76289.31") == 7628931
    assert price_to_ticks("76290.00") == 7629000
    assert price_to_ticks("0.01") == 1


def test_price_to_ticks_rejects_sub_cent() -> None:
    with pytest.raises(ValueError):
        price_to_ticks("76289.315")   # finer than a cent -> must raise


def test_iso_to_ns_exact_microseconds() -> None:
    # 1 second after epoch, still exact.
    assert iso_to_ns("1970-01-01T00:00:01Z") == 1_000_000_000
    # A realistic 2026 timestamp with microseconds must land EXACTLY on
    # nanoseconds -- the last three digits are 000, not a float-rounded tail.
    ns = iso_to_ns("2026-08-21T07:37:51.509738Z")
    assert ns % 1000 == 0                      # microsecond input -> ns divisible by 1000
    assert str(ns).endswith("509738000")       # exact fractional part, no float drift


def test_parse_update_removal() -> None:
    # A real update line from the capture: new_quantity "0" means the level is gone.
    raw = {
        "side": "bid",
        "event_time": "1970-01-01T00:00:00Z",
        "price_level": "76290.12",
        "new_quantity": "0",
    }
    result = parse_update(raw, seq=1)
    assert result.price_ticks == 7629012
    assert result.size == 0.0
    assert result.side == "bid"
    assert result.seq == 1

def test_parse_side_maps_offer_to_ask_and_rejects_the_rest() -> None:
    assert parse_side("bid") == "bid"
    assert parse_side("offer") == "ask"
    with pytest.raises(ValueError, match="unknown book side 'ask'"):
        parse_side("ask")       # not a Coinbase word: refusing it beats guessing


def test_iter_updates_flattens_events() -> None:
    """One message with two updates yields two BookUpdates, sharing the seq."""
    message = {
        "sequence_num": 7,
        "events": [
            {
                "type": "update",
                "updates": [
                    {"side": "bid", "event_time": "1970-01-01T00:00:00Z",
                     "price_level": "100.00", "new_quantity": "1.5"},
                    {"side": "offer", "event_time": "1970-01-01T00:00:00Z",
                     "price_level": "101.00", "new_quantity": "0"},
                ],
            }
        ],
    }
    results = list(iter_updates(message))
    assert len(results) == 2
    assert results[0].price_ticks == 10000
    assert results[0].side == "bid"
    assert results[1].price_ticks == 10100
    assert results[1].side == "ask"            # Coinbase's "offer" in the schema's words
    assert results[1].size == 0.0
    assert all(u.seq == 7 for u in results)   # both share the message's seq

CAPTURE = Path("data/coinbase_BTC-USD_2026-08-21.jsonl")


@pytest.mark.skipif(not CAPTURE.exists(), reason="capture file not present")
def test_real_capture_updates_are_sane() -> None:
    """The loader reads real recorded bytes and yields sane BookUpdates."""
    events = (update for message in load_messages(CAPTURE) for update in message.updates)

    first = next(events)                      # pull just the first, lazily
    assert first.price_ticks > 0              # a real BTC price, in ticks
    assert first.size >= 0.0                  # sizes are non-negative; 0 = removal
    assert first.side in ("bid", "ask")
    assert first.ts > 0                        # a real nanosecond timestamp

    # Now drain a few thousand and confirm nothing throws on real data.
    count = 1
    for _, update in zip(range(5000), events, strict=False):
        assert update.price_ticks > 0
        assert update.side in ("bid", "ask")
        count += 1
    assert count > 1000                        # the file has far more than this


def _recorded_line(seq: int, channel: str = "l2_data", *, snapshot: bool = False) -> str:
    """One valid recorder line: a single-update book message, or a subscriptions ack."""
    if channel == "subscriptions":
        events: list[dict[str, Any]] = [{"subscriptions": {"level2": ["BTC-USD"]}}]
    else:
        events = [
            {
                "type": "snapshot" if snapshot else "update",
                "updates": [
                    {"side": "bid", "event_time": "1970-01-01T00:00:00Z",
                     "price_level": "100.00", "new_quantity": "1.5"},
                ],
            }
        ]
    raw = {"channel": channel, "timestamp": "1970-01-01T00:00:01.000000002Z",
           "sequence_num": seq, "events": events}
    return json.dumps({"recv_ns": 0, "raw": raw}) + "\n"


def test_skips_truncated_final_line(tmp_path: Path) -> None:
    """Recorder killed mid-write: the good lines before the cut still load."""
    path = tmp_path / "capture.jsonl"
    path.write_text(_recorded_line(0) + _recorded_line(1)[:40], encoding="utf-8")
    with pytest.warns(UserWarning, match="capture.jsonl:2: skipped damaged line") as caught:
        messages = list(load_messages(path))
    assert [m.seq for m in messages] == [0]
    assert len(caught) == 1


def test_skips_nul_padded_final_line(tmp_path: Path) -> None:
    """Unclean shutdown: the file ends in NUL bytes, which strip() does not remove."""
    path = tmp_path / "capture.jsonl"
    path.write_text(_recorded_line(0) + "\x00" * 100, encoding="utf-8")
    with pytest.warns(UserWarning, match="skipped damaged line") as caught:
        messages = list(load_messages(path))
    assert [m.seq for m in messages] == [0]
    assert len(caught) == 1


def _gap_flags(tmp_path: Path, *lines: str) -> list[tuple[int, bool]]:
    """(seq, gap_before) for each book message the loader yields from these lines."""
    path = tmp_path / "capture.jsonl"
    path.write_text("".join(lines), encoding="utf-8")
    return [(m.seq, m.gap_before) for m in load_messages(path)]


def test_subscription_message_is_not_a_gap(tmp_path: Path) -> None:
    """The original bug: the ack at seq 2 is a real message, so 1 -> 3 is continuous."""
    flags = _gap_flags(
        tmp_path,
        _recorded_line(0, snapshot=True),
        _recorded_line(1),
        _recorded_line(2, "subscriptions"),
        _recorded_line(3),
    )
    # Only the first message is flagged: nothing before it proves continuity.
    assert flags == [(0, True), (1, False), (3, False)]


def test_skipped_seq_is_a_gap(tmp_path: Path) -> None:
    flags = _gap_flags(tmp_path, _recorded_line(0), _recorded_line(1), _recorded_line(3))
    assert flags == [(0, True), (1, False), (3, True)]


def test_gap_at_non_book_message_flags_next_book_message(tmp_path: Path) -> None:
    """Seq 2 is lost just before an ack; the ack isn't yielded, so the flag carries on."""
    flags = _gap_flags(
        tmp_path,
        _recorded_line(0),
        _recorded_line(1),
        _recorded_line(3, "subscriptions"),
        _recorded_line(4),
    )
    assert flags == [(0, True), (1, False), (4, True)]


def test_repeated_seq_is_a_gap(tmp_path: Path) -> None:
    flags = _gap_flags(tmp_path, _recorded_line(0), _recorded_line(1), _recorded_line(1))
    assert flags == [(0, True), (1, False), (1, True)]


def test_seq_reset_on_reconnect_is_a_gap(tmp_path: Path) -> None:
    """A new connection restarts seq at 0 with a snapshot; continuity is broken."""
    flags = _gap_flags(
        tmp_path,
        _recorded_line(0, snapshot=True),
        _recorded_line(1),
        _recorded_line(0, snapshot=True),
    )
    assert flags == [(0, True), (1, False), (0, True)]


def test_damaged_line_is_a_gap(tmp_path: Path) -> None:
    """We can't read the damaged line's seq, so we can't prove nothing was lost."""
    with pytest.warns(UserWarning, match="skipped damaged line"):
        flags = _gap_flags(
            tmp_path, _recorded_line(0), _recorded_line(1)[:40] + "\n", _recorded_line(1)
        )
    assert flags == [(0, True), (1, True)]


def test_message_groups_updates_and_marks_snapshot(tmp_path: Path) -> None:
    path = tmp_path / "capture.jsonl"
    path.write_text(_recorded_line(0, snapshot=True) + _recorded_line(1), encoding="utf-8")
    snap, delta = load_messages(path)
    assert snap.is_snapshot and not delta.is_snapshot
    assert [u.seq for u in snap.updates] == [0]
    assert snap.ts == 1_000_000_002   # the message's own timestamp, to the ns
    assert snap.updates[0].is_snapshot


@pytest.mark.skipif(not CAPTURE.exists(), reason="capture file not present")
def test_real_capture_gaps_are_exactly_the_reconnect_snapshots() -> None:
    """08-21 holds 4 connections and no dropped message (the gap probe, all channels).

    So every message is yielded, and the only flagged ones are the 4 opening
    snapshots at seq 0. Any other flag means the loader is inventing gaps again.
    The file ends in a NUL-padded line; seq 26867 is the last intact message, so
    seeing it last proves the loader read past the damage to the end.
    """
    count = 0
    flagged = []
    with pytest.warns(UserWarning, match="skipped damaged line"):
        for message in load_messages(CAPTURE):   # streamed: ~2M updates in this file
            count += 1
            if message.gap_before:
                flagged.append((message.seq, message.is_snapshot))
    assert count == 53_474   # every l2_data message in the file
    assert flagged == [(0, True)] * 4
    assert message.seq == 26867
