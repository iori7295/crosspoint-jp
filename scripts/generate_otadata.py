import struct, zlib
data = bytearray(8192)
for off in (0, 32):
    struct.pack_into('<I', data, off, 1)
    data[off+4:off+24] = b'ota_0\x00'.ljust(20, b'\x00')
    c = zlib.crc32(bytes(data[off:off+24])) & 0xFFFFFFFF
    struct.pack_into('<I', data, off+24, c)
import sys
path = sys.argv[1] if len(sys.argv) > 1 else 'otadata.bin'
with open(path, 'wb') as f:
    f.write(data)
print(f'{path} ({len(data)} bytes)')
