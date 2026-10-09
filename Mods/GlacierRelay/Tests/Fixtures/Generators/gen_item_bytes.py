"""Generate Tests/Fixtures/B0ItemBytes.h from the B0 native log.

Usage:
    python3 gen_item_bytes.py <native log> <out header>

Input:  the same B0 native log as gen_items_fixture.py (read only; never modified or regenerated here).
Output: the C++ header with the seven item definition ids as 16-byte little-endian ZRepositoryID images
        (data1/data2/data3 least significant byte first, data4 in order), derived from the dashed strings the
        engine's JSON writer printed, each checked by rendering the image back to the same string; the images in
        first-seen order; and the image of each of the 24 events' RepositoryId by B0Items.h index.
        To verify a committed header, generate into a temporary path and compare byte for byte.

No identifiers are carried: only the item definition ids and item names (engine display strings) appear.
"""
import json, re, sys

log, out = sys.argv[1], sys.argv[2]
NAMES = ("ItemPickedUp", "ItemThrown", "ItemRemovedFromInventory")
rows = []
for line in open(log, encoding="utf-8", errors="replace"):
    m = re.search(r"sent #(\d+) \(index (\d+), frame (\d+)\): (\{.*\})\s*$", line)
    if not m:
        continue
    e = json.loads(m.group(4))
    if e.get("Name") in NAMES:
        rows.append(e)
assert len(rows) == 24

defs = []
for e in rows:
    if e["Value"]["RepositoryId"] not in defs:
        defs.append(e["Value"]["RepositoryId"])
assert len(defs) == 7

def image(dashed):
    a, b, c, d, e = dashed.split("-")
    d1 = int(a, 16); d2 = int(b, 16); d3 = int(c, 16)
    data4 = bytes.fromhex(d + e)
    return list(d1.to_bytes(4, "little")) + list(d2.to_bytes(2, "little")) + list(d3.to_bytes(2, "little")) + list(data4)

def render(dashed):
    # Independent check of the expected rendering from the image (the inverse of image()).
    b = image(dashed)
    d1 = int.from_bytes(bytes(b[0:4]), "little"); d2 = int.from_bytes(bytes(b[4:6]), "little"); d3 = int.from_bytes(bytes(b[6:8]), "little")
    return "%08x-%04x-%04x-%s-%s" % (d1, d2, d3, bytes(b[8:10]).hex(), bytes(b[10:16]).hex())

names = {}
for e in rows:
    names.setdefault(e["Value"]["RepositoryId"], e["Value"]["ItemName"])

with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("#pragma once\n\n")
    f.write("// The seven item definition ids of the B0 corpus (Tests/Fixtures/B0Items.h) in the form the engine passes a\n")
    f.write("// ZRepositoryID at runtime (M2 design, section 34): the 16-byte little-endian memory image of its GUID fields\n")
    f.write("// (data1, data2, data3 least significant byte first; data4 in order). Each image was derived from the dashed\n")
    f.write("// string the engine's own JSON writer printed in B0; the string beside it is that B0 text and is the expected\n")
    f.write("// rendering. Whether the item object's RepositoryId is a ZRepositoryID or a ZString at runtime is NOT known\n")
    f.write("// (section 38.1 C): these images let a test exercise the production renderer on the item ids either way.\n")
    f.write("// Generated from the B0 corpus; do not edit by hand.\n\n")
    f.write("#include <array>\n#include <cstddef>\n#include <cstdint>\n\n#include \"B0DisguiseBytes.h\"\n\nnamespace B0Fixtures\n{\n")
    for i, d in enumerate(defs):
        assert render(d) == d
        f.write(f"    // {d}: {names[d]}\n")
        f.write(f"    inline constexpr RepositoryIdImage k_ItemImage{i + 1} = {{\n        \"{d}\",\n        {{{', '.join('0x%02x' % x for x in image(d))}}},\n    }};\n\n")
    f.write("    // The seven definition images in first-seen order (the k_ItemDefinition<n> order of B0Items.h).\n")
    f.write("    inline constexpr const RepositoryIdImage* k_ItemDefinitionImages[] = {\n        " + ", ".join(f"&k_ItemImage{i + 1}" for i in range(7)) + "\n    };\n\n")
    f.write("    // The image of each event's RepositoryId, by k_Items index.\n")
    f.write("    inline constexpr const RepositoryIdImage* k_ItemImages[] = {\n")
    for e in rows:
        f.write(f"        &k_ItemImage{defs.index(e['Value']['RepositoryId']) + 1}, // {e['Name']} {e['Value']['ItemName']}\n")
    f.write("    };\n}\n")
print("wrote", out)
for d in defs:
    print(d, names[d], " ".join("%02x" % x for x in image(d)))
