import socket, time, re, sys

HOST = '192.168.1.20'
PORT = 23


def clean(b):
    b = re.sub(rb'\xff[\xfb\xfc\xfd\xfe].', b'', b)
    return b.decode(errors='replace')


class Board:
    def __init__(self, host=HOST):
        self.s = socket.create_connection((host, PORT), timeout=5)
        self.s.settimeout(1.0)
        self.n = 0
        time.sleep(0.5)
        self.s.sendall(b'\n')
        time.sleep(0.3)
        try:
            self.s.recv(65536)
        except Exception:
            pass
        self.raw('stty -echo', 1.0)

    def raw(self, cmd, wait=3.0):
        self.n += 1
        mk = 'ZZ%dX' % self.n
        self.s.sendall((cmd + '; echo ' + mk + '\n').encode())
        out = b''
        end = time.time() + wait
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d:
                    break
                out += d
                if mk.encode() in out:
                    end = min(end, time.time() + 0.3)
            except socket.timeout:
                pass
        t = clean(out)
        t = t.replace(mk, '').strip()
        return t

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


if __name__ == '__main__':
    b = Board()
    cmds = sys.argv[1:]
    if not cmds:
        cmds = ['ps | grep -E "sample_venc|S99rtsp" | grep -v grep',
                'netstat -ln | grep 554',
                'tail -20 /mnt/sample_venc.log']
    for c in cmds:
        print('=== ' + c)
        print(b.raw(c, 6.0))
    b.close()
