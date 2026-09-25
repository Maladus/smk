#!/usr/bin/env python3
"""Emit a C header embedding a Vial keyboard definition.

    vial_def_gen.py <vial.json>

Prints the header to stdout; meson's configure_file(capture:true) writes it to
the build dir. The JSON is the single source for both the served definition and
the per-board matrix-position -> stored-slot key table.

The definition is minified and XZ-compressed with `lzma.compress`, exactly like
vial-qmk's `util/vial_generate_definition.py`: Vial hosts unconditionally run
`lzma.decompress` on the bytes served by CMD_VIAL_GET_DEFINITION, so an
uncompressed definition makes them fail.
"""

import json
import lzma
import sys


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit("usage: vial_def_gen.py <vial.json>")

    src = sys.argv[1]
    with open(src, "r") as f:
        doc = json.load(f)

    # Minify: vial-qmk uses separators=(",", ":") with no trailing whitespace.
    data = json.dumps(doc, separators=(",", ":")).strip().encode("utf-8")
    data = lzma.compress(data)

    rows = doc["matrix"]["rows"]
    cols = doc["matrix"]["cols"]

    # Matrix position (row * cols + col) -> stored slot, in the order the keys
    # appear in the keymap. Empty positions get 0xFF.
    table = [0xFF] * (rows * cols)
    slot = 0
    for row in doc["layouts"]["keymap"]:
        for cell in row:
            if isinstance(cell, str) and "," in cell:
                r, c = (int(x) for x in cell.split(","))
                table[r * cols + c] = slot
                slot += 1

    out = []
    out.append("// Generated from %s - do not edit.\n" % src)
    out.append("#pragma once\n\n")
    out.append("#include <stdint.h>\n\n")

    # Counts are macros so a second translation unit can size the keymap store
    # without pulling in the definition blob (define VIAL_DEFINITION_EXTERN).
    out.append("#define VIAL_DEFINITION_SIZE %d\n" % len(data))
    out.append("#define VIAL_KEY_TABLE_SIZE %d\n" % (rows * cols))
    out.append("#define VIAL_NUM_KEYS %d\n\n" % slot)

    out.append("#ifndef VIAL_DEFINITION_EXTERN\n")
    out.append("static const __code unsigned char vial_definition[] = {\n")
    for i in range(0, len(data), 16):
        out.append("    " + " ".join("0x%02x," % b for b in data[i : i + 16]) + "\n")
    out.append("};\n\n")

    out.append("static const __code uint8_t vial_key_table[VIAL_KEY_TABLE_SIZE] = {\n")
    for i in range(0, len(table), 16):
        out.append("    " + " ".join("0x%02x," % b for b in table[i : i + 16]) + "\n")
    out.append("};\n")
    out.append("#endif\n")

    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
