#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Phase J Tier 2 smoke test — uv-managed pip C-extension exercise.
#
# Spec: phase-J-design-2026-05-07.md §3.2 (post-uv-swap, commit
# bcd791f5a369). The Tier 1 pattern from tier1-smoketest.py is
# extended to packages NOT pre-installed on the host — fetched via
# `uv run --with <pkg>` from the warmed offline cache.
#
# Targets (one bounded check per library, all without network):
#
#   httpx     — async HTTP client, urllib3-shaped URL+JSON+headers
#               round-trip (PreparedRequest equivalent). Same path
#               as Tier 1's `requests` test but on a different
#               C-extension stack.
#   pyyaml    — round-trip a multi-document YAML stream through
#               yaml.safe_load + yaml.safe_dump (libyaml C
#               extension under the hood).
#   pendulum  — datetime arithmetic + timezone conversion (pure
#               Python now in 3.x but historically a Rust-via-
#               PyO3 extension; exercises the import + the
#               datetime ABI).
#   numpy     — same matrix-inverse + FFT round-trip as
#               tier1-smoketest.py. Run here to confirm `uv`'s
#               isolated venv resolves the import-chain bug
#               (#17) where numpy fails to import after
#               cryptography in a shared site-packages.
#
# Invocation patterns:
#   Per-library:   ./tier2-uv-smoketest.py <httpx|pyyaml|pendulum|numpy>
#   All-in-one:    ./tier2-uv-smoketest.py
#
# In the Tier 2 templates each library is run as its OWN `uv run
# --with <pkg>` invocation — fresh isolated venv, no chain
# contamination. The all-in-one mode is kept for host-side smoke
# and ad-hoc debugging.
#
# Exits 0 on success, 1 on any failure, 2 on unknown test name.

import os
import sys


def test_httpx() -> None:
	import httpx

	req = httpx.Request("POST",
			    "http://example.com/api/v1/x?q=1",
			    headers={"X-Test": "phaseJ"},
			    json={"k": "v", "n": [1, 2, 3]})
	assert req.method == "POST"
	assert str(req.url) == "http://example.com/api/v1/x?q=1"
	# httpx serializes JSON compact (no spaces between separators).
	assert req.content == b'{"k":"v","n":[1,2,3]}'
	assert req.headers["Content-Type"] == "application/json"
	assert req.headers["X-Test"] == "phaseJ"
	print("OK: httpx version=%s" % httpx.__version__)


def test_pyyaml() -> None:
	import yaml

	docs = [
		{"name": "alpha", "kind": "ping", "n": 42},
		{"name": "beta", "kind": "pong", "n": -1, "deep": {"a": [1, 2, 3]}},
		{"name": "gamma", "kind": "config", "n": 0, "list": ["x", "y"]},
	]
	# safe_dump_all -> safe_load_all round-trip.
	stream = yaml.safe_dump_all(docs, default_flow_style=False)
	parsed = list(yaml.safe_load_all(stream))
	assert parsed == docs
	# Re-dump the parsed docs and confirm idempotent.
	stream2 = yaml.safe_dump_all(parsed, default_flow_style=False)
	assert stream == stream2
	print("OK: pyyaml version=%s" % yaml.__version__)


def test_pendulum() -> None:
	import pendulum

	# UTC + timezone conversion.
	utc = pendulum.datetime(2026, 5, 14, 12, 0, 0, tz="UTC")
	ny = utc.in_timezone("America/New_York")
	assert ny.hour == 8	# 12:00 UTC = 08:00 EDT in May
	assert ny.timezone_name == "America/New_York"
	# Duration arithmetic.
	later = utc.add(hours=3, minutes=30)
	delta = later - utc
	assert delta.total_seconds() == 3.5 * 3600
	# ISO 8601 round-trip.
	iso = utc.isoformat()
	parsed = pendulum.parse(iso)
	assert parsed.timestamp() == utc.timestamp()
	# Pendulum 3.x removed __version__; pull from importlib.
	import importlib.metadata
	print("OK: pendulum version=%s" %
	      importlib.metadata.version("pendulum"))


def test_numpy() -> None:
	import numpy as np

	rng = np.random.default_rng(seed=20260514)
	a = rng.random((64, 64))
	m = a @ a.T + np.eye(64)
	inv = np.linalg.inv(m)
	identity = m @ inv
	np.testing.assert_allclose(identity, np.eye(64), atol=1e-9)

	x = rng.random(1024)
	x_round = np.fft.ifft(np.fft.fft(x)).real
	np.testing.assert_allclose(x_round, x, atol=1e-9)
	print("OK: numpy version=%s" % np.__version__)


TESTS = {
	"httpx": test_httpx,
	"pyyaml": test_pyyaml,
	"pendulum": test_pendulum,
	"numpy": test_numpy,
}


def main() -> int:
	if len(sys.argv) == 2:
		name = sys.argv[1]
		fn = TESTS.get(name)
		if fn is None:
			print("FAIL: unknown test %s; valid: %s" %
			      (name, ",".join(TESTS)))
			return 2
		try:
			fn()
		except Exception as e:
			print("FAIL: %s: %s: %s" % (name, type(e).__name__, e))
			return 1
		print("TIER2_SMOKE: %s OK" % name)
		return 0

	failed = []
	for name, fn in TESTS.items():
		try:
			fn()
		except Exception as e:
			print("FAIL: %s: %s: %s" % (name, type(e).__name__, e))
			failed.append(name)
	if failed:
		print("TIER2_SMOKE: FAIL %s" % ",".join(failed))
		return 1
	print("TIER2_SMOKE: ALL_OK %s" % "+".join(TESTS))
	return 0


if __name__ == "__main__":
	sys.exit(main())
