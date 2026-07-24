"""Renders an ICU .dat file as the C source genccode would emit.

A pure-Python re-implementation of `writeCCode` in ICU's
source/tools/toolutil/pkg_genc.cpp (the `genccode` tool), so the build can
embed the ICU data statically without first building genccode itself. The
output is byte-compatible in structure: a struct whose leading double forces
alignment, then the file's bytes, under the entry-point symbol
`<name>_dat`.

Usage: dat_to_c.py <input.dat> <entry-name> <output.c>
e.g.:  dat_to_c.py icudt76l.dat icudt76 icudt76_dat.c
"""

import sys


def main() -> None:
    dat_path, entry_name, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(dat_path, "rb") as f:
        data = f.read()

    with open(out_path, "w") as out:
        out.write(
            "#ifndef IN_GENERATED_CCODE\n"
            "#define IN_GENERATED_CCODE\n"
            "#define U_DISABLE_RENAMING 1\n"
            '#include "unicode/umachine.h"\n'
            "#endif\n"
            "U_CDECL_BEGIN\n"
            "const struct {\n"
            "    double bogus;\n"
            f"    uint8_t bytes[{len(data)}]; \n"
            f"}} {entry_name}_dat={{ 0.0, {{\n"
        )
        # 16 bytes per line keeps the file editor-safe; the exact wrapping
        # genccode uses does not matter to the compiler.
        for i in range(0, len(data), 16):
            out.write(",".join(str(b) for b in data[i : i + 16]))
            out.write(",\n" if i + 16 < len(data) else "\n")
        out.write("}\n};\nU_CDECL_END\n")


if __name__ == "__main__":
    main()
