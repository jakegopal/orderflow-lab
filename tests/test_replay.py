"""Tests for replay: snapshot-anchored segments, per-message state, never-cross."""

from pathlib import Path

import pytest

from builders import msg
from orderflow.book.replay import CrossedBookError, replay
from orderflow.data.loader import load_messages


def test_replay_builds_book_from_snapshot() -> None:
    messages = [
        msg(0, ("bid", 10000, 1.0), ("ask", 10002, 1.0), snap=True, gap=True),
        msg(1, ("bid", 10001, 0.5)),   # a better bid arrives via a delta
    ]
    states = list(replay(messages))
    final = states[-1][1]
    assert final.best_bid() == 10001
    assert final.best_ask() == 10002


def test_yields_once_per_message_not_per_update() -> None:
    messages = [
        msg(0, ("bid", 10000, 1.0), ("ask", 10002, 1.0), snap=True, gap=True),
        msg(1, ("bid", 9999, 1.0), ("bid", 9998, 1.0), ("ask", 10003, 1.0)),
    ]
    assert len(list(replay(messages))) == 2


def test_messages_before_any_snapshot_are_ignored() -> None:
    # No snapshot yet -> nothing is trustworthy -> nothing is yielded.
    messages = [msg(5, ("bid", 10000, 1.0), gap=True), msg(6, ("ask", 10002, 1.0))]
    assert list(replay(messages)) == []


def test_gap_invalidates_until_next_snapshot() -> None:
    messages = [
        msg(0, ("bid", 10000, 1.0), snap=True, gap=True),   # segment 0 opens
        msg(1, ("bid", 10001, 1.0)),                        # ok
        msg(3, ("bid", 10002, 1.0), gap=True),              # loader saw a gap: 0 dies
        msg(4, ("bid", 10003, 1.0)),                        # still invalid, skipped
        msg(0, ("bid", 10000, 2.0), snap=True, gap=True),   # reconnect: segment 1
        msg(1, ("ask", 10005, 1.0)),
    ]
    seen = [(book.best_bid(), seg) for _msg, book, seg in replay(messages)]
    assert seen == [(10000, 0), (10001, 0), (10000, 1), (10000, 1)]


def test_snapshot_replaces_book_instead_of_merging() -> None:
    """A mid-connection snapshot (no gap) must not leave the old levels behind."""
    messages = [
        msg(0, ("bid", 10000, 1.0), ("ask", 10005, 1.0), snap=True, gap=True),
        msg(1, ("bid", 9990, 1.0), ("ask", 9995, 1.0), snap=True),
    ]
    *_, (_msg, book, seg) = replay(messages)
    assert book.bids == {9990: 1.0}   # merged, the stale 10000 bid would cross 9995
    assert book.asks == {9995: 1.0}
    assert seg == 1


def test_transient_cross_inside_one_message_does_not_raise() -> None:
    """The bid moves up before the ask does, within one message.

    Partway through, bid 10003 >= ask 10002, but that state never existed on the
    exchange. Only the state after the whole message is checked.
    """
    messages = [
        msg(0, ("bid", 10000, 1.0), ("ask", 10002, 1.0), snap=True, gap=True),
        msg(1, ("bid", 10003, 1.0), ("ask", 10002, 0.0), ("ask", 10004, 1.0)),
    ]
    *_, (_msg, book, _seg) = replay(messages)
    assert (book.best_bid(), book.best_ask()) == (10003, 10004)


def test_crossed_book_at_message_end_raises() -> None:
    messages = [
        msg(0, ("ask", 10000, 1.0), snap=True, gap=True),
        msg(1, ("bid", 10001, 1.0)),   # bidding above the ask -> crossed
    ]
    with pytest.raises(CrossedBookError, match="seq 1"):
        list(replay(messages))


CAPTURE = Path("data/coinbase_BTC-USD_2026-08-21.jsonl")


@pytest.mark.skipif(not CAPTURE.exists(), reason="capture file not present")
def test_replay_never_crosses_on_real_capture() -> None:
    """Reconstruct from real recorded data; a valid segment must never cross.

    Completing without raising CrossedBookError IS the assertion -- the check
    fires after every message. 2,000 messages is the opening snapshot plus
    1,999 live messages, and the top of book takes 443 distinct values across
    them. So the check covers a moving live book, not only snapshot loading.
    """
    count = 0
    tops = set()
    for _msg, book, _seg in replay(load_messages(CAPTURE)):
        count += 1
        tops.add((book.best_bid(), book.best_ask()))
        if count >= 2000:
            break
    assert count == 2000
    assert len(tops) == 443


@pytest.mark.skipif(not CAPTURE.exists(), reason="capture file not present")
def test_whole_real_capture_is_replayed() -> None:
    """The original bug kept 4 snapshots plus 6 small messages out of 53,474.

    08-21 has 4 connections and no dropped message, so every book message must be
    yielded, in one segment per connection. Each segment's size is its
    connection's message count minus the one subscriptions ack. The cross check is
    off here (it's an O(n) scan per message); the test above covers it.
    """
    per_segment: dict[int, int] = {}
    with pytest.warns(UserWarning, match="skipped damaged line"):
        for _msg, _book, seg in replay(load_messages(CAPTURE), check_crossed=False):
            per_segment[seg] = per_segment.get(seg, 0) + 1
    assert per_segment == {0: 2765, 1: 9729, 2: 14113, 3: 26867}
