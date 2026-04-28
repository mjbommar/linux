#!/usr/bin/env python3
# Class A env repro: ensurepip module imports cleanly even though the
# subprocess spawn (pip install) would fail under PID-1 init with no
# PATH and no network. Sanity check the stdlib module is intact.
import sys


def main():
    try:
        import ensurepip  # noqa: F401
        from ensurepip import _bundled  # noqa: F401
    except Exception as e:
        print("REPRO: ensurepip_check FAIL import_error=%s" % type(e).__name__)
        return 0
    print("REPRO: ensurepip_check PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
