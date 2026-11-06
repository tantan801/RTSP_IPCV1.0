from board import Board
import time
b = Board()
CMD = "awk '/1920/ && /1080/ {print $6; exit}' /proc/umap/venc"
s1 = b.raw(CMD, 15).strip()
time.sleep(6)
s2 = b.raw(CMD, 15).strip()
print("T0:", s1)
print("T6:", s2)
print("T0:", s1); print("T6:", s2)
try:
    a = int(s1.split()[-1]); c = int(s2.split()[-1])
    print("delta = %d frames / 6s" % (c - a))
except Exception as e:
    print("raw lines:", repr(s1), repr(s2))
