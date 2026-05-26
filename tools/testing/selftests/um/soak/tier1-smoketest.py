#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Phase J Tier 1 smoke test — host-installed Python C-extension exercise.
#
# Spec: Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
#       phase-J-design-2026-05-07.md §3.1.
#
# The memo's literal proposal (`pytest --pyargs requests.tests`) doesn't
# work as-is — Debian/Ubuntu's `python3-requests` / `python3-cryptography`
# / `python3-numpy` apt packages do NOT include their upstream `tests/`
# submodules. Adapting per the memo's "favour breadth over depth" guidance:
# this script exercises one C-extension-heavy code path per library
# deterministically, in a single 1-2 s wall-clock window, without network.
#
# Targets (one bounded check per library):
#   requests       — PreparedRequest URL + JSON-body + header handling
#                    (URL parsing C-extension via charset_normalizer +
#                    urllib3's parser).
#   cryptography   — AES-256-CBC encrypt/decrypt round-trip, 4 KiB block.
#                    Exercises the OpenSSL bindings — the original
#                    "Python C-extension import" P0 (issue #274) path.
#   numpy          — 64x64 matrix inverse round-trip + FFT/iFFT round-trip.
#                    Exercises the BLAS + FFT C-extensions.
#
# Exits 0 on ALL_OK, 1 on any test failure. Intended to run inside UML
# under Phase J soak rotation; the daemon's classifier looks for
# `REPRO_DONE rc=0` per pilot/daemon convention.

import os
import sys


def test_requests() -> bool:
	import requests
	from requests.models import PreparedRequest

	pr = PreparedRequest()
	pr.prepare(method="POST", url="http://example.com/api/v1/x?q=1",
		   headers={"X-Test": "phaseJ"},
		   json={"k": "v", "n": [1, 2, 3]})
	assert pr.method == "POST"
	assert pr.url == "http://example.com/api/v1/x?q=1"
	assert pr.body == b'{"k": "v", "n": [1, 2, 3]}'
	assert pr.headers["Content-Type"] == "application/json"
	assert pr.headers["X-Test"] == "phaseJ"
	# Also exercise URL-rebuild path.
	from requests.utils import urlparse, urlunparse
	parts = urlparse(pr.url)
	assert parts.scheme == "http"
	assert urlunparse(parts) == pr.url
	print("OK: requests version=%s" % requests.__version__)
	return True


def test_cryptography() -> bool:
	from cryptography.hazmat.backends import default_backend
	from cryptography.hazmat.primitives.ciphers import (
		Cipher, algorithms, modes,
	)

	key = b"\x00" * 32   # deterministic for reproducibility
	iv = b"\x11" * 16
	plaintext = (b"the quick brown fox jumps over the lazy dog\n") * 64
	# PKCS7 padding to 16-byte block.
	pad = 16 - (len(plaintext) % 16)
	padded = plaintext + bytes([pad]) * pad
	enc = Cipher(algorithms.AES(key), modes.CBC(iv),
		     backend=default_backend()).encryptor()
	ct = enc.update(padded) + enc.finalize()
	assert len(ct) == len(padded)
	dec = Cipher(algorithms.AES(key), modes.CBC(iv),
		     backend=default_backend()).decryptor()
	pt = dec.update(ct) + dec.finalize()
	# Strip PKCS7 padding.
	pad_len = pt[-1]
	assert pt[-pad_len:] == bytes([pad_len]) * pad_len
	assert pt[:-pad_len] == plaintext
	import cryptography
	print("OK: cryptography version=%s" % cryptography.__version__)
	return True


def test_numpy() -> bool:
	import numpy as np

	rng = np.random.default_rng(seed=20260507)
	a = rng.random((64, 64))
	# Make a well-conditioned matrix: a @ a.T + eye is positive-definite.
	m = a @ a.T + np.eye(64)
	inv = np.linalg.inv(m)
	identity = m @ inv
	np.testing.assert_allclose(identity, np.eye(64), atol=1e-9)

	x = rng.random(1024)
	x_round = np.fft.ifft(np.fft.fft(x)).real
	np.testing.assert_allclose(x_round, x, atol=1e-9)
	print("OK: numpy version=%s" % np.__version__)
	return True


TESTS = {
	"requests": test_requests,
	"cryptography": test_cryptography,
	"numpy": test_numpy,
}


def main() -> int:
	# Single-test mode: argv[1] selects which library to exercise. Used by
	# the Phase J Tier 1 template — each library runs in its own fresh
	# Python process to avoid C-extension cross-contamination (we hit a
	# kvm-v2 import-chain pathology when requests + cryptography + numpy
	# share a process; tracked separately, see soak/README.md).
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
		print("TIER1_SMOKE: %s OK" % name)
		return 0

	# All-in-one mode: kept for host-side smoke + ad-hoc invocation.
	# Will trip the kvm-v2 import-chain pathology in-guest; use the per
	# -test invocation in soak templates.
	failed = []
	for name, fn in TESTS.items():
		try:
			fn()
		except Exception as e:
			print("FAIL: %s: %s: %s" % (name, type(e).__name__, e))
			failed.append(name)
	if failed:
		print("TIER1_SMOKE: FAIL %s" % ",".join(failed))
		return 1
	print("TIER1_SMOKE: ALL_OK requests+cryptography+numpy")
	return 0


if __name__ == "__main__":
	sys.exit(main())
