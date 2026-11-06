import socket, sys

host, port, path = "192.168.1.20", 554, "/live"
try:
    s = socket.create_connection((host, port), 3)
    req = (
        f"DESCRIBE rtsp://{host}:{port}{path} RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Accept: application/sdp\r\n"
        "User-Agent: probe\r\n\r\n"
    )
    s.sendall(req.encode())
    data = s.recv(4096).decode(errors="replace")
    print(data)
    s.close()
except Exception as e:
    print("ERR:", e)
    sys.exit(1)
