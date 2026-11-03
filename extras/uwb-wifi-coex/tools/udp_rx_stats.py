#!/usr/bin/env python3
"""Receive the JSON lines sent by CoexAnchorUdp / ReproIssue12 and check the
success criteria of the plan.

  python3 udp_rx_stats.py --port 9000 --duration 600 --csv run1.csv

Criteria checked at the end (defaults follow the plan, all adjustable):
  * no stall: no gap between ranging packets longer than --stall-s
  * delivery: received / produced >= --min-delivery (produced = seq span)
Exit code 0 when both pass, 1 otherwise, 2 when nothing was received.
"""
import argparse
import csv
import json
import socket
import sys
import time


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--port", type=int, default=9000)
    p.add_argument("--duration", type=float, default=600.0,
                   help="seconds to run after the first packet (default 600 = 10 min)")
    p.add_argument("--stall-s", type=float, default=2.0,
                   help="a gap longer than this between ranging packets counts as a stall")
    p.add_argument("--min-delivery", type=float, default=0.99)
    p.add_argument("--report-s", type=float, default=10.0)
    p.add_argument("--csv", help="write every ranging packet to this CSV file")
    p.add_argument("--selftest", action="store_true",
                   help="send synthetic packets to ourselves and check the math")
    return p.parse_args(argv)


class Stats:
    def __init__(self, stall_s):
        self.stall_s = stall_s
        self.first_wall = None
        self.last_wall = None
        self.seqs = set()
        self.dupes = 0
        self.min_seq = None
        self.max_seq = None
        self.max_gap = 0.0
        self.stalls = []          # (start_wall, gap_s)
        self.bad_lines = 0
        self.last_status = None
        self.restarts = 0         # seq went backwards a lot = board reset

    def on_ranging(self, msg, now):
        seq = int(msg["seq"])
        if self.max_seq is not None and seq + 1000 < self.max_seq:
            # board rebooted: start a new seq window, keep the old counts
            self.restarts += 1
            self.min_seq = seq
            self.max_seq = seq
        if self.last_wall is not None:
            gap = now - self.last_wall
            self.max_gap = max(self.max_gap, gap)
            if gap > self.stall_s:
                self.stalls.append((self.last_wall, gap))
        if self.first_wall is None:
            self.first_wall = now
        self.last_wall = now
        key = (self.restarts, seq)
        if key in self.seqs:
            self.dupes += 1
        self.seqs.add(key)
        self.min_seq = seq if self.min_seq is None else min(self.min_seq, seq)
        self.max_seq = seq if self.max_seq is None else max(self.max_seq, seq)

    def produced(self):
        if self.max_seq is None:
            return 0
        # per restart window: span of seq values seen
        windows = {}
        for r, s in self.seqs:
            lo, hi = windows.get(r, (s, s))
            windows[r] = (min(lo, s), max(hi, s))
        return sum(hi - lo + 1 for lo, hi in windows.values())

    def received(self):
        return len(self.seqs)

    def delivery(self):
        p = self.produced()
        return self.received() / p if p else 0.0

    def line(self, now):
        run = (now - self.first_wall) if self.first_wall else 0.0
        return (f"t={run:7.1f}s rx={self.received()} produced={self.produced()} "
                f"delivery={self.delivery()*100:6.2f}% max_gap={self.max_gap:5.2f}s "
                f"stalls={len(self.stalls)} restarts={self.restarts} bad={self.bad_lines}"
                + (f" | board {self.last_status}" if self.last_status else ""))


def run(args, sock):
    st = Stats(args.stall_s)
    writer = None
    fh = None
    if args.csv:
        fh = open(args.csv, "w", newline="")
        writer = csv.writer(fh)
        writer.writerow(["wall", "seq", "t_ms", "mac", "d_cm"])
    sock.settimeout(0.5)
    next_report = None
    try:
        while True:
            now = time.monotonic()
            if st.first_wall is not None:
                if now - st.first_wall >= args.duration:
                    break
                # a silence that is still ongoing also counts
                if st.last_wall is not None:
                    st.max_gap = max(st.max_gap, now - st.last_wall)
                if next_report is None:
                    next_report = now + args.report_s
                if now >= next_report:
                    print(st.line(now), flush=True)
                    next_report = now + args.report_s
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout:
                continue
            now = time.monotonic()
            for raw in data.decode("utf-8", "replace").splitlines():
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    msg = json.loads(raw)
                except ValueError:
                    st.bad_lines += 1
                    continue
                if msg.get("type") == "r":
                    st.on_ranging(msg, now)
                    if writer:
                        writer.writerow([f"{now:.3f}", msg.get("seq"), msg.get("t"),
                                         msg.get("mac"), msg.get("d")])
                elif msg.get("type") == "s":
                    st.last_status = " ".join(f"{k}={v}" for k, v in msg.items() if k != "type")
    except KeyboardInterrupt:
        pass
    finally:
        if fh:
            fh.close()

    now = time.monotonic()
    if st.last_wall is not None and st.first_wall is not None:
        # trailing silence up to the end of the run
        tail = min(now, st.first_wall + args.duration) - st.last_wall
        if tail > args.stall_s:
            st.stalls.append((st.last_wall, tail))
            st.max_gap = max(st.max_gap, tail)
    print(st.line(now))
    if st.received() == 0:
        print("RESULT: no ranging packets received")
        return 2, st
    ok_stall = len(st.stalls) == 0
    ok_deliv = st.delivery() >= args.min_delivery
    print(f"RESULT: stall {'PASS' if ok_stall else 'FAIL'} (max gap {st.max_gap:.2f}s, "
          f"limit {args.stall_s}s, {len(st.stalls)} stalls) | delivery "
          f"{'PASS' if ok_deliv else 'FAIL'} ({st.delivery()*100:.2f}% >= "
          f"{args.min_delivery*100:.0f}%)")
    return (0 if ok_stall and ok_deliv else 1), st


def selftest():
    """Loopback: 300 packets with 3 dropped and one 2.5 s pause."""
    import threading
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    port = rx.getsockname()[1]
    args = parse_args(["--port", str(port), "--duration", "5", "--report-s", "2"])

    def sender():
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for seq in range(300):
            if seq in (10, 11, 200):
                continue
            if seq == 150:
                time.sleep(2.5)
            line = json.dumps({"type": "r", "seq": seq, "t": seq * 10,
                               "mac": "2222", "d": 100}) + "\n"
            tx.sendto(line.encode(), ("127.0.0.1", port))
            time.sleep(0.005)
        tx.sendto(b'{"type":"s","seq":300,"acq":1}\n', ("127.0.0.1", port))

    threading.Thread(target=sender, daemon=True).start()
    code, st = run(args, rx)
    assert st.received() == 297, st.received()
    assert st.produced() == 300, st.produced()
    assert len(st.stalls) == 1 and 2.3 < st.stalls[0][1] < 3.5, st.stalls
    assert code == 1  # one stall and 99.0% delivery -> stall FAIL
    print("selftest OK")
    return 0


def main():
    args = parse_args()
    if args.selftest:
        return selftest()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    print(f"listening on {args.bind}:{args.port} for {args.duration:.0f}s "
          f"after the first packet", flush=True)
    code, _ = run(args, sock)
    return code


if __name__ == "__main__":
    sys.exit(main())
