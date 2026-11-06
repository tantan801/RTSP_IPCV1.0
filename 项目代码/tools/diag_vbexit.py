from board import Board
import time, sys

b = Board()

def ps():
    return b.raw("ps | grep -E 'sample_venc' | grep -v grep | head -3", 6)

def tail_log(n=12):
    return b.raw("tail -%d /mnt/sample_venc.log" % n, 6)

for rnd in range(3):
    print("======= ROUND %d stop" % (rnd+1))
    print(b.raw("/etc/init.d/S99rtsp stop", 25))
    time.sleep(4)
    print("--- ps after stop:")
    print(ps() or "(empty)")
    print("--- log tail:")
    print(tail_log(6))
    print("======= ROUND %d start" % (rnd+1))
    print(b.raw("/etc/init.d/S99rtsp start", 20))
    time.sleep(8)
    print("--- ps after start:")
    print(ps() or "(empty)")
    print("--- log tail:")
    print(tail_log(8))
    print()

b.close()
print("DIAG_DONE")
