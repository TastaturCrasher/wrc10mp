"""Decode/encode WRC 10 Common/Settings/DUMMY*.DAT files (32-byte repeating XOR)."""
import sys

KEY = b'14F5FsyDFFUC4NVANPpYggyakreWkJfy'


def xor(data: bytes) -> bytes:
    return bytes(c ^ KEY[i % len(KEY)] for i, c in enumerate(data))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: dummydat.py <in> <out>   (works both ways)')
    with open(sys.argv[1], 'rb') as f:
        data = f.read()
    with open(sys.argv[2], 'wb') as f:
        f.write(xor(data))
