import struct, sys

def esp_rom_crc32_le(init, data_bytes):
    """ESP32 ROM CRC32 (polynomial 0xEDB88320, no final XOR)"""
    crc = init
    for byte in data_bytes:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
    return crc & 0xFFFFFFFF

data = bytearray(8192)
for off in (0, 32):
    struct.pack_into('<I', data, off, 1)  # ota_seq=1 → ota_0 boots
    data[off+4:off+24] = b'ota_0\x00'.ljust(20, b'\x00')
    c = esp_rom_crc32_le(0xFFFFFFFF, data[off:off+4])
    c = esp_rom_crc32_le(c, data[off+4:off+24])
    struct.pack_into('<I', data, off+24, c)

path = sys.argv[1] if len(sys.argv) > 1 else 'otadata.bin'
with open(path, 'wb') as f:
    f.write(data)
print(f'{path} ({len(data)} bytes)')
