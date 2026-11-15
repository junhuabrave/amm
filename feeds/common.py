"""Shared plumbing for venue adapters.

An adapter turns one venue's public market data into the engine's normalised
event lines on standard output (see include/amm/live/events.hpp):

    <ts_ns>,B,<symbol>,<bid>,<bid_size>,<ask>,<ask_size>
    <ts_ns>,T,<symbol>,<price>,<qty>,<B|S>          aggressor side
    <ts_ns>,C,<symbol>,<price>                      composite price

To add a venue, write a class with a `poll()` method that returns a list of
(type, symbol, fields...) tuples and hand instances to `run()`. Parsing should
live in pure functions so it can be tested against sample payloads without a
network connection (see test_feeds.py).

Standard library only: nothing to install.
"""
import json
import sys
import time
import urllib.error
import urllib.request

USER_AGENT = "amm-feed/0.1"


def http_json(url, timeout=5.0):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fmt(x):
    return repr(float(x)) if not isinstance(x, str) else x


def line(ts_ns, kind, symbol, *fields):
    return ",".join([str(int(ts_ns)), kind, symbol] + [fmt(f) for f in fields])


def run(sources, interval=0.5, duration=None, out=sys.stdout, err=sys.stderr, clock=time.time_ns, sleep=time.sleep):
    """Poll every source each `interval` seconds and write event lines.

    Timestamps are local receipt times, so the stream is monotonic across
    symbols. A source that fails is reported on stderr and retried with
    exponential back-off; the others keep running.
    """
    start = clock()
    backoff = {id(s): 0.0 for s in sources}
    retry_at = {id(s): 0 for s in sources}
    while duration is None or (clock() - start) / 1e9 < duration:
        t0 = clock()
        for s in sources:
            if t0 < retry_at[id(s)]:
                continue
            try:
                events = s.poll()
                backoff[id(s)] = 0.0
            except (urllib.error.URLError, OSError, ValueError, KeyError, IndexError) as e:
                backoff[id(s)] = min(60.0, max(1.0, 2.0 * backoff[id(s)]))
                retry_at[id(s)] = clock() + int(backoff[id(s)] * 1e9)
                print("feed %s: %s (retry in %.0fs)" % (s.name, e, backoff[id(s)]), file=err, flush=True)
                continue
            now = clock()
            for ev in events:
                out.write(line(now, ev[0], ev[1], *ev[2:]) + "\n")
            out.flush()
        spent = (clock() - t0) / 1e9
        if spent < interval:
            sleep(interval - spent)
