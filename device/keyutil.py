_DEC = (("%2F", "/"), ("%2B", "+"), ("%3D", "="), ("%25", "%"))


def decode_key(k):
    for enc, dec in _DEC:
        k = k.replace(enc, dec)
    return k
