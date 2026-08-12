import socket, time

def http_get(host, port, path, timeout=8):
    addr = socket.getaddrinfo(host, port)[0][4]
    s = socket.socket()
    s.settimeout(timeout)
    s.connect(addr)
    req = b"GET " + path.encode() + b" HTTP/1.1\r\nHost: " + \
        host.encode() + b"\r\nConnection: close\r\n\r\n"
    s.sendall(req)
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ")[1])
    return status, body
