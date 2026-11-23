"""Tests for BookState: applying updates and reading the top of book."""

from builders import make
from orderflow.book.state import BookState, TopOfBook


def test_empty_book_has_no_best() -> None:
    book = BookState()
    assert book.best_bid() is None
    assert book.best_ask() is None


def test_apply_adds_levels() -> None:
    book = BookState()
    book.apply(make("bid", 10000, 1.5))
    book.apply(make("bid", 9999, 2.0))
    book.apply(make("ask", 10002, 0.8))
    assert book.best_bid() == 10000     # the higher of the two bids
    assert book.best_ask() == 10002
    assert book.bids[9999] == 2.0


def test_zero_size_removes_level() -> None:
    book = BookState()
    book.apply(make("bid", 10000, 1.5))
    book.apply(make("bid", 10000, 0.0))   # size 0 -> remove
    assert 10000 not in book.bids
    assert book.best_bid() is None


def test_removing_absent_level_is_harmless() -> None:
    book = BookState()
    book.apply(make("ask", 10002, 0.0))   # remove something never added
    assert book.best_ask() is None        # no crash, book still empty


def test_resizing_a_level_overwrites() -> None:
    book = BookState()
    book.apply(make("bid", 10000, 1.5))
    book.apply(make("bid", 10000, 3.0))   # same level, new size
    assert book.bids[10000] == 3.0        # overwritten, not added


def test_top_of_book_reads_best_levels() -> None:
    book = BookState()
    book.apply(make("bid", 10000, 1.5))
    book.apply(make("bid", 9999, 2.0))
    book.apply(make("ask", 10002, 0.8))
    assert book.top_of_book() == TopOfBook(bid_px=10000, bid_sz=1.5, ask_px=10002, ask_sz=0.8)


def test_top_of_book_needs_both_sides() -> None:
    book = BookState()
    book.apply(make("bid", 10000, 1.5))
    assert book.top_of_book() is None
