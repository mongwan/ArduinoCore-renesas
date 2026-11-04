#!/usr/bin/env python3
"""Receive-direction test for CoexAnchorUdp built with COEX_RX_ECHO=1.

Sends numbered datagrams from the PC to the board at a fixed rate; the board
sends each one back. Measures loss, round-trip time and gaps.

  python3 udp_echo_test.py --board 192.168.0.42 --port 9001 --rate 10 --duration 300

The board IP is printed on the board's serial port ("COEX_RX_ECHO: echo on
<ip>:9001"). Run udp_rx_stats.py at the same time to watch the normal
board -> PC stream (it uses port 9000, this tool uses its own socket).

Criteria checked at the end (all adjustable):
  * loss: (sent - echoed) / sent <= --max-loss  (percent)
  * no stall: no gap between echoes longer than --stall-s
Exit code 0 when both pass, 1 otherwise, 2 when no echo came back at all
(wrong IP/port, firmware without COEX_RX_ECHO, or a firewall).
"""
import argparse
import csv
import socket
import sys
import threading
import time


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--board", help="board IP address (required unless --selftest)")
    p.add_argument("--port", type=int, default=9001, help="board echo port (COEX_ECHO_PORT)")
    p.add_argument("--bind", default="0.0.0.0", help="local address for our socket")
    p.add_argument("--local-port", type=int, default=0, help="local port (0 = any)")
    p.add_argument("--rate", type=float, default=10.0, help="datagrams per second (default 10)")
    p.add_argument("--size", type=int, default=48,
                   help="payload bytes, 24..128 (the board drops larger ones)")
    p.add_argument("--duration", type=float, default=300.0, help="seconds of sending (default 300)")
    p.add_argument("--grace-s", type=float, default=2.0,
                   help="keep listening this long after the last send")
    p.add_argument("--stall-s", type=float, default=2.0,
                   help="a gap longer than this between echoes counts as a stall")
    p.add_argument("--max-loss", type=float, default=1.0, help="percent (default 1.0)")
    p.add_argument("--report-s", type=float, default=10.0)
    p.add_argument("--csv", help="write seq,send_time,rtt_ms for every datagram")
    p.add_argument("--selftest", action="store_true",
                   help="run against a local echo server that drops some datagrams")
    args = p.parse_args(argv)
    if not args.selftest and not args.board:
        p.error("--board is required")
    if not 24 <= args.size <= 128:
        p.error("--size must be 24..128")
    return args


def make_payload(seq, size):
    head = ("E%010d " % seq).encode()
    return head + b"." * (size - len(head))


def parse_seq(data):
    if len(data) < 11 or data[:1] != b"E":
        return None
    try:
        return int(data[1:11])
    except ValueError:
        return None


class Stats:
    def __init__(self, stall_s):
        self.stall_s = stall_s
        self.sent = {}            # seq -> send time
        self.rtt = {}             # seq -> rtt seconds
        self.dupes = 0
        self.bad = 0
        self.foreign = 0          # datagrams from another address
        self.reordered = 0
        self.last_seq_back = -1
        self.first_echo = None
        self.last_echo = None
        self.max_gap = 0.0
        self.stalls = []          # (start time, gap)

    def on_echo(self, seq, now):
        if seq not in self.sent:
            self.bad += 1
            return
        if seq in self.rtt:
            self.dupes += 1
            return
        self.rtt[seq] = now - self.sent[seq]
        if seq < self.last_seq_back:
            self.reordered += 1
        self.last_seq_back = max(self.last_seq_back, seq)
        if self.last_echo is not None:
            gap = now - self.last_echo
            if gap > self.max_gap:
                self.max_gap = gap
            if gap > self.stall_s:
                self.stalls.append((self.last_echo, gap))
        else:
            self.first_echo = now
        self.last_echo = now

    def loss_pct(self):
        if not self.sent:
            return 0.0
        return 100.0 * (len(self.sent) - len(self.rtt)) / len(self.sent)

    def rtt_ms(self):
        return sorted(v * 1000.0 for v in self.rtt.values())

    def line(self, t0, now):
        r = self.rtt_ms()
        med = r[len(r) // 2] if r else float("nan")
        return ("[%6.0fs] sent=%d echoed=%d loss=%.2f%% rtt_med=%.1fms max_gap=%.2fs stalls=%d"
                % (now - t0, len(self.sent), len(self.rtt), self.loss_pct(), med,
                   self.max_gap, len(self.stalls)))


def pct(sorted_vals, q):
    if not sorted_vals:
        return float("nan")
    i = min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1))))
    return sorted_vals[i]


def run(args, sock, target):
    st = Stats(args.stall_s)
    period = 1.0 / args.rate
    t0 = time.monotonic()
    end_send = t0 + args.duration
    end_all = end_send + args.grace_s
    next_send = t0
    next_report = t0 + args.report_s
    seq = 0
    sock.settimeout(0.0)
    print("sending %.1f/s x %d B to %s:%d for %.0fs"
          % (args.rate, args.size, target[0], target[1], args.duration))
    while True:
        now = time.monotonic()
        if now >= end_all:
            break
        if now >= next_send and now < end_send:
            st.sent[seq] = now
            try:
                sock.sendto(make_payload(seq, args.size), target)
            except OSError as e:
                print("send error:", e)
            seq += 1
            next_send += period
            if next_send < now - 1.0:   # PC was suspended: do not burst
                next_send = now + period
        if now >= next_report:
            print(st.line(t0, now), flush=True)
            next_report += args.report_s
        wait = min(next_send if now < end_send else end_all, next_report) - time.monotonic()
        sock.settimeout(max(0.0005, min(wait, 0.05)))
        try:
            data, addr = sock.recvfrom(2048)
        except (socket.timeout, BlockingIOError):
            continue
        except ConnectionResetError:
            # Windows: ICMP port unreachable from an earlier send
            continue
        now = time.monotonic()
        if addr[0] != target[0]:
            st.foreign += 1
            continue
        s = parse_seq(data)
        if s is None:
            st.bad += 1
            continue
        st.on_echo(s, now)
    # silence at the end also counts as a stall
    if st.last_echo is not None and end_send - st.last_echo > args.stall_s:
        st.stalls.append((st.last_echo, end_send - st.last_echo))
        st.max_gap = max(st.max_gap, end_send - st.last_echo)
    return st, t0


def report(args, st, t0):
    r = st.rtt_ms()
    print()
    print("=== echo test result ===")
    print("sent        %d" % len(st.sent))
    print("echoed      %d  (dupes %d, reordered %d, bad %d, foreign %d)"
          % (len(st.rtt), st.dupes, st.reordered, st.bad, st.foreign))
    print("loss        %.2f%%  (limit %.2f%%)" % (st.loss_pct(), args.max_loss))
    if r:
        print("rtt ms      min %.1f  median %.1f  p95 %.1f  p99 %.1f  max %.1f"
              % (r[0], pct(r, 0.5), pct(r, 0.95), pct(r, 0.99), r[-1]))
    print("max gap     %.2fs  (stall limit %.2fs)" % (st.max_gap, args.stall_s))
    for start, gap in st.stalls[:10]:
        print("  stall at +%.1fs for %.2fs" % (start - t0, gap))
    if not st.rtt:
        print("RESULT: NO ECHO (check board IP/port, COEX_RX_ECHO=1 build, firewall)")
        return 2
    ok_loss = st.loss_pct() <= args.max_loss
    ok_stall = not st.stalls
    print("loss  %s" % ("PASS" if ok_loss else "FAIL"))
    print("stall %s" % ("PASS" if ok_stall else "FAIL"))
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["seq", "send_s", "rtt_ms"])
            for s in sorted(st.sent):
                rtt = st.rtt.get(s)
                w.writerow([s, "%.4f" % (st.sent[s] - t0), "" if rtt is None else "%.2f" % (rtt * 1000)])
    print("RESULT: %s" % ("PASS" if ok_loss and ok_stall else "FAIL"))
    return 0 if ok_loss and ok_stall else 1


def selftest():
    """Local echo server that drops every 20th datagram and pauses once."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    stop = threading.Event()

    def server():
        srv.settimeout(0.1)
        n = 0
        while not stop.is_set():
            try:
                data, addr = srv.recvfrom(2048)
            except socket.timeout:
                continue
            n += 1
            if n % 20 == 0:
                continue                 # 5 % loss
            if n == 30:
                time.sleep(2.5)          # one stall
            srv.sendto(data, addr)

    th = threading.Thread(target=server, daemon=True)
    th.start()
    args = parse_args(["--selftest", "--rate", "50", "--duration", "6", "--grace-s", "1",
                       "--report-s", "2", "--max-loss", "1"])
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    st, t0 = run(args, sock, ("127.0.0.1", port))
    stop.set()
    code = report(args, st, t0)
    loss = st.loss_pct()
    good = code == 1 and 3.0 <= loss <= 8.0 and len(st.stalls) >= 1
    print("SELFTEST %s (expected FAIL with ~5%% loss and a stall; got loss %.1f%%, stalls %d)"
          % ("OK" if good else "BROKEN", loss, len(st.stalls)))
    return 0 if good else 1


def main():
    args = parse_args()
    if args.selftest:
        return selftest()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.local_port))
    try:
        st, t0 = run(args, sock, (args.board, args.port))
    except KeyboardInterrupt:
        print("interrupted")
        return 1
    return report(args, st, t0)


if __name__ == "__main__":
    sys.exit(main())
