#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Class D structural repro (Python): threading.excepthook fires for
unhandled exceptions in a worker thread.

This is the smaller test_warnings / threading-excepthook quirk: under
some UML configurations the worker-thread excepthook plumbing has
trouble because of how SIGSEGV / pthread fault interception interacts
with the seccomp stub. Pythonic version because the exception model
doesn't translate to C cleanly.
"""
import sys
import threading


def main():
    fired = threading.Event()
    captured = {}

    def hook(args):
        captured["exc_type"] = args.exc_type
        captured["exc_value"] = args.exc_value
        captured["thread_name"] = args.thread.name if args.thread else None
        fired.set()

    threading.excepthook = hook

    def worker():
        raise ValueError("structural-repro")

    t = threading.Thread(target=worker, name="repro-worker")
    t.start()
    t.join(timeout=5.0)

    if not fired.wait(timeout=2.0):
        print("REPRO: thread_excepthook FAIL hook_not_invoked")
        return 0

    if captured.get("exc_type") is ValueError and captured.get("thread_name") == "repro-worker":
        print("REPRO: thread_excepthook PASS exc=%s thread=%s" %
              (captured["exc_type"].__name__, captured["thread_name"]))
    else:
        print("REPRO: thread_excepthook FAIL captured=%r" % (captured,))
    return 0


if __name__ == "__main__":
    sys.exit(main())
