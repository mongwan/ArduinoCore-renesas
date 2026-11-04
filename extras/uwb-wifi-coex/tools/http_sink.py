#!/usr/bin/env python3
"""HTTP receiver for CoexAnchorUdp built with COEX_HTTP_TEST=1 (TCP test).

The board POSTs {"seq":N,"t":ms} to /ingest every COEX_HTTP_PERIOD_MS
(default 1 s), one TCP connection per request. This server answers 200 and
closes, and measures delivery and gaps.

  python3 http_sink.py --port 8080 --duration 300

Criteria checked at the end (all adjustable):
  * delivery: received / (seq span) >= --min-delivery
  * no stall: no gap between requests longer than --stall-s
Exit code 0 when both pass, 1 otherwise, 2 when nothing arrived within
--wait-s (wrong SERVER_IP, firmware without COEX_HTTP_TEST, or a firewall).
"""
import argparse
import csv
import json
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--duration", type=float, default=300.0,
                   help="seconds to run after the first request (default 300)")
    p.add_argument("--wait-s", type=float, default=120.0,
                   help="give up if no request arrives within this many seconds")
    p.add_argument("--stall-s", type=float, default=3.0,
                   help="a gap longer than this between requests counts as a stall")
    p.add_argument("--min-delivery", type=float, default=0.99)
    p.add_argument("--report-s", type=float, default=10.0)
    p.add_argument("--csv", help="write wall,seq,t_ms for every request")
    p.add_argument("--selftest", action="store_true",
                   help="post synthetic requests to ourselves and check the math")
    return p.parse_args(argv)


class Stats:
    def __init__(self, stall_s):
        self.lock = threading.Lock()
        self.stall_s = stall_s
        self.rows = []            # (wall, seq, t_ms)
        self.seqs = set()
        self.dupes = 0
        self.bad = 0
        self.min_seq = None
        self.max_seq = None
        self.first = None
        self.last = None
        self.max_gap = 0.0
        self.stalls = []
        self.restarts = 0

    def on_request(self, body, now):
        try:
            msg = json.loads(body)
            seq = int(msg["seq"])
            t_ms = msg.get("t")
        except (ValueError, KeyError, TypeError):
            with self.lock:
                self.bad += 1
            return
        with self.lock:
            if self.max_seq is not None and seq + 100 < self.max_seq:
                self.restarts += 1       # board rebooted: new seq window
                self.min_seq = seq
                self.max_seq = seq
                self.seqs = set()
            if seq in self.seqs:
                self.dupes += 1
                return
            self.seqs.add(seq)
            self.min_seq = seq if self.min_seq is None else min(self.min_seq, seq)
            self.max_seq = seq if self.max_seq is None else max(self.max_seq, seq)
            if self.last is not None:
                gap = now - self.last
                self.max_gap = max(self.max_gap, gap)
                if gap > self.stall_s:
                    self.stalls.append((self.last, gap))
            else:
                self.first = now
            self.last = now
            self.rows.append((now, seq, t_ms))

    def produced(self):
        return 0 if self.min_seq is None else self.max_seq - self.min_seq + 1

    def delivery(self):
        p = self.produced()
        return len(self.seqs) / p if p else 0.0

    def line(self, t0, now):
        with self.lock:
            return ("t=%7.1fs rx=%d produced=%d delivery=%6.2f%% max_gap=%5.2fs stalls=%d restarts=%d bad=%d"
                    % (now - t0, len(self.seqs), self.produced(), 100 * self.delivery(),
                       self.max_gap, len(self.stalls), self.restarts, self.bad))


def make_handler(stats):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n) if n > 0 else b""
            stats.on_request(body, time.monotonic())
            reply = b"ok\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(reply)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(reply)

        def log_message(self, fmt, *args):
            pass

    return Handler


def run(args, server, stats):
    th = threading.Thread(target=server.serve_forever, daemon=True)
    th.start()
    print("listening on %s:%d for %.0fs after the first request (first-request wait %.0fs)"
          % (args.bind, server.server_address[1], args.duration, args.wait_s), flush=True)
    t_start = time.monotonic()
    while stats.first is None:
        if args.wait_s > 0 and time.monotonic() - t_start > args.wait_s:
            server.shutdown()
            print("RESULT: NOTHING RECEIVED within %.0fs (check SERVER_IP, COEX_HTTP_TEST=1 build, firewall)"
                  % args.wait_s)
            return 2
        time.sleep(0.05)
    t0 = stats.first
    end = t0 + args.duration
    next_report = t0 + args.report_s
    while True:
        now = time.monotonic()
        if now >= end:
            break
        if now >= next_report:
            print(stats.line(t0, now), flush=True)
            next_report += args.report_s
        time.sleep(0.05)
    server.shutdown()
    with stats.lock:
        if stats.last is not None and end - stats.last > args.stall_s:
            stats.stalls.append((stats.last, end - stats.last))
            stats.max_gap = max(stats.max_gap, end - stats.last)
    print(stats.line(t0, end))
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["wall", "seq", "t_ms"])
            for wall, seq, t_ms in stats.rows:
                w.writerow(["%.3f" % (wall - t0), seq, t_ms])
    for start, gap in stats.stalls[:10]:
        print("  stall at +%.1fs for %.2fs" % (start - t0, gap))
    ok_d = stats.delivery() >= args.min_delivery
    ok_s = not stats.stalls
    print("RESULT: stall %s (max gap %.2fs, limit %.1fs, %d stalls) | delivery %s (%.2f%% >= %.0f%%)"
          % ("PASS" if ok_s else "FAIL", stats.max_gap, args.stall_s, len(stats.stalls),
             "PASS" if ok_d else "FAIL", 100 * stats.delivery(), 100 * args.min_delivery))
    return 0 if ok_d and ok_s else 1


def selftest():
    """Post 40 requests, skip seq 7 and 8, pause 3.5 s once: expect FAIL."""
    args = parse_args(["--selftest", "--port", "0", "--duration", "8", "--wait-s", "5",
                       "--report-s", "2", "--stall-s", "3"])
    stats = Stats(args.stall_s)
    server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(stats))
    port = server.server_address[1]

    def client():
        time.sleep(0.2)
        for seq in range(40):
            if seq in (7, 8):
                continue
            if seq == 20:
                time.sleep(3.5)
            body = json.dumps({"seq": seq, "t": seq * 100}).encode()
            req = urllib.request.Request("http://127.0.0.1:%d/ingest" % port, data=body,
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=2) as r:
                r.read()
            time.sleep(0.05)

    threading.Thread(target=client, daemon=True).start()
    code = run(args, server, stats)
    good = (code == 1 and len(stats.seqs) == 38 and stats.produced() == 40
            and len(stats.stalls) >= 1)
    print("SELFTEST %s (expected FAIL with 38/40 and a stall; got %d/%d, stalls %d)"
          % ("OK" if good else "BROKEN", len(stats.seqs), stats.produced(), len(stats.stalls)))
    return 0 if good else 1


def main():
    args = parse_args()
    if args.selftest:
        return selftest()
    stats = Stats(args.stall_s)
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(stats))
    try:
        return run(args, server, stats)
    except KeyboardInterrupt:
        server.shutdown()
        print("interrupted")
        return 1


if __name__ == "__main__":
    sys.exit(main())
