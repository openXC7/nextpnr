#!/usr/bin/env python3
"""
UF2 converter for Sonata FPGA bitstreams.
Based on the standard UF2 format with proper block structure.
"""
import struct
import argparse
import sys

SONATA_FAMILY_ID = 0x6ce29e6b
UF2_PAYLOAD_SIZE = 256
UF2_BLOCK_DATA_SIZE = 476
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
UF2_MAGIC_END = 0x0AB16F30

def blocks_from_binary(data, base_addr=0x00000000, family_id=SONATA_FAMILY_ID):
    """Convert binary data to UF2 blocks with proper header structure."""
    blocks = []
    data_len = len(data)
    block_count = (data_len + UF2_PAYLOAD_SIZE - 1) // UF2_PAYLOAD_SIZE
    
    for i in range(block_count):
        start = i * UF2_PAYLOAD_SIZE
        end = min(start + UF2_PAYLOAD_SIZE, data_len)
        block_data = data[start:end]
        block_data += b'\x00' * (UF2_BLOCK_DATA_SIZE - len(block_data))
        
        flags = UF2_FLAG_FAMILY_ID_PRESENT
        target_addr = base_addr + start
        
        # UF2 block: 32-byte header, 476-byte data area, and 4-byte end magic.
        block = struct.pack('<IIIIIIII',
            0x0A324655,      # msgStart
            0x9E5D5157,      # msgMagic
            flags,           # flags
            target_addr,     # targetAddr
            UF2_PAYLOAD_SIZE, # payloadSize
            i,               # blockNo
            block_count,     # totalBlocks
            family_id        # familyID
        ) + block_data + struct.pack('<I', UF2_MAGIC_END)
        blocks.append(block)
    
    return blocks

def main():
    parser = argparse.ArgumentParser(description='Convert bitstream to UF2 (Sonata)')
    parser.add_argument('input', help='Input .bit file')
    parser.add_argument('-o', '--output', required=True, help='Output .uf2 file')
    parser.add_argument('-b', '--base', type=lambda x: int(x, 0), default=0x00000000,
                        help='Base address (default: 0x00000000)')
    parser.add_argument('-f', '--family', type=lambda x: int(x, 0), default=SONATA_FAMILY_ID,
                        help='UF2 family ID (default: 0x6ce29e6b)')
    
    args = parser.parse_args()
    
    with open(args.input, 'rb') as f:
        bit_data = f.read()
    
    blocks = blocks_from_binary(bit_data, args.base, args.family)
    
    with open(args.output, 'wb') as f:
        for block in blocks:
            f.write(block)
    
    print(f"Wrote {len(blocks)} blocks to {args.output}")
    return 0

if __name__ == '__main__':
    sys.exit(main())

