import sys

if len(sys.argv) < 4:
    sys.exit(1)

spv_path = sys.argv[1]
header_path = sys.argv[2]
var_name = sys.argv[3].replace('.', '_').replace('-', '_')

with open(spv_path, 'rb') as f:
    data = f.read()

words = [int.from_bytes(data[i:i+4], 'little') for i in range(0, len(data), 4)]

with open(header_path, 'w') as f:
    f.write("#pragma once\n#include <cstdint>\n#include <cstddef>\n\n")
    f.write(f"static const uint32_t {var_name}_spv[] = {{\n")
    for i in range(0, len(words), 8):
        chunk = words[i:i+8]
        f.write("    " + ", ".join(f"0x{w:08x}" for w in chunk) + ",\n")
    f.write("};\n\n")
    f.write(f"static const size_t {var_name}_spv_size = sizeof({var_name}_spv);\n")
