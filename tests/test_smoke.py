"""Smoke test: the package is installed and importable."""

import orderflow


def test_package_exposes_version() -> None:
    assert orderflow.__version__ == "0.1.0"
