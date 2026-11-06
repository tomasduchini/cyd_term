#!/usr/bin/env python3
# Genera main/font8x16.h a partir de unscii-16.hex
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "unscii-16.hex"
DST = "main/font8x16.h"

# byte en la tabla -> codepoint Unicode
MAP = {c: c for c in range(0x20, 0x7F)}
MAP.update({0x80: 0x2588, 0x81: 0x2593, 0x82: 0x2592, 0x83: 0x2591})  # █ ▓ ▒ ░
MAP.update({0x84: 0x250C, 0x85: 0x2510, 0x86: 0x2514, 0x87: 0x2518,   # ┌ ┐ └ ┘
            0x88: 0x2500, 0x89: 0x2502, 0x8A: 0x251C, 0x8B: 0x2524})  # ─ │ ├ ┤
MAP.update({0x8C: 0x2580, 0x8D: 0x2584})  # ▀ ▄
MAP.update({0x8E + i: 0x2581 + i for i in range(7)})  # ▁ ▂ ▃ ▄ ▅ ▆ ▇
# Iconos del clima: 16x16 en ASCII, escalados x2 a 32x32 = 4x2 celdas de 8x16.
# El orden define el indice en C: tile = ICON_BASE + indice*8 + fila*4 + columna
ICON_BASE = 0xA0
ICONS = [
    ("sol", [
        ".......##.......",
        ".......##.......",
        "..##........##..",
        "..###......###..",
        ".....######.....",
        "....########....",
        "....########....",
        "##.##########.##",
        "##.##########.##",
        "....########....",
        "....########....",
        ".....######.....",
        "..###......###..",
        "..##........##..",
        ".......##.......",
        ".......##.......",
    ]),
    ("luna", [
        ".....######.....",
        "...#####........",
        "..####..........",
        ".####...........",
        ".###............",
        "####............",
        "####............",
        "####............",
        "####............",
        "####............",
        ".###............",
        ".####.......##..",
        "..####....####..",
        "...##########...",
        ".....######.....",
        "................",
    ]),
    ("parcial", [
        "...#............",
        ".#.#.#..........",
        "..###......###..",
        "#######...#####.",
        "..###....#######",
        ".#.#.#..########",
        "...#....########",
        "........########",
        "...#############",
        "..##############",
        ".###############",
        ".###############",
        "..#############.",
        "................",
        "................",
        "................",
    ]),
    ("nube", [
        "................",
        "................",
        ".......####.....",
        ".....########...",
        "...###########..",
        "..#############.",
        ".##############.",
        "################",
        "################",
        "################",
        ".##############.",
        "..############..",
        "................",
        "................",
        "................",
        "................",
    ]),
    ("niebla", [
        "................",
        "................",
        ".##############.",
        "................",
        "...############.",
        "................",
        ".###########....",
        "................",
        "..#############.",
        "................",
        ".##########.....",
        "................",
        "...###########..",
        "................",
        "................",
        "................",
    ]),
    ("lluvia", [
        ".......####.....",
        ".....########...",
        "...###########..",
        "..#############.",
        ".###############",
        "################",
        ".##############.",
        "................",
        "..#...#...#...#.",
        ".#...#...#...#..",
        "................",
        "...#...#...#....",
        "..#...#...#.....",
        "................",
        ".#...#...#...#..",
        "#...#...#...#...",
    ]),
    ("nieve", [
        ".......####.....",
        ".....########...",
        "...###########..",
        "..#############.",
        ".###############",
        "################",
        ".##############.",
        "................",
        "..#.......#.....",
        ".###.....###....",
        "..#.......#.....",
        "......#.......#.",
        ".....###.....###",
        "......#.......#.",
        "................",
        "................",
    ]),
    ("tormenta", [
        ".......####.....",
        ".....########...",
        "...###########..",
        "..#############.",
        ".###############",
        "################",
        ".##############.",
        "................",
        ".......###......",
        "......###.......",
        ".....######.....",
        ".......###......",
        "......###.......",
        ".....##.........",
        "....#...........",
        "................",
    ]),
]

# Iconos de las apps (mismo formato que los del clima). El orden tiene que coincidir con
# la tabla s_apps de page_apps.c.
APP_ICONS = [
    ("cronometro", [
        "......####......",
        ".......##.......",
        ".....######.....",
        "...##......##...",
        "..#..........#..",
        ".#.....#......#.",
        ".#.....#......#.",
        "#......#.......#",
        "#......####....#",
        "#..............#",
        ".#............#.",
        ".#............#.",
        "..#..........#..",
        "...##......##...",
        ".....######.....",
        "................",
    ]),
    ("temporizador", [
        ".##############.",
        ".##############.",
        "..#..........#..",
        "..#..........#..",
        "...#.######.#...",
        "....#.####.#....",
        ".....#.##.#.....",
        "......#..#......",
        "......#..#......",
        ".....#....#.....",
        "....#..##..#....",
        "...#..####..#...",
        "..#.########.#..",
        "..#..........#..",
        ".##############.",
        ".##############.",
    ]),
    ("calendario", [
        "................",
        "...##......##...",
        ".##############.",
        ".##############.",
        ".#............#.",
        ".#.#.#.#.#.#..#.",
        ".#............#.",
        ".#.#.#.#.#.#..#.",
        ".#............#.",
        ".#.#.#.#.#.#..#.",
        ".#............#.",
        ".#.#.#.#.#.#..#.",
        ".#............#.",
        ".##############.",
        "................",
        "................",
    ]),
    ("musica", [
        "................",
        "......########..",
        "......########..",
        "......#......#..",
        "......#......#..",
        "......#......#..",
        "......#......#..",
        "......#......#..",
        "......#......#..",
        "......#......#..",
        "...####...####..",
        "..#####..#####..",
        "..#####..#####..",
        "...###....###...",
        "................",
        "................",
    ]),
    ("juegos", [
        "................",
        "................",
        "................",
        "................",
        "...##########...",
        "..############..",
        ".#####.#####.##.",
        ".###...###.####.",
        ".#####.#####.##.",
        ".##############.",
        ".#####....#####.",
        ".####......####.",
        "..##........##..",
        "................",
        "................",
        "................",
    ]),
]

# Codigos libres para los tiles de las apps: 0x01..0x1F son caracteres de control (nunca se
# imprimen como texto) y 0x95..0x9F no los usa nadie
APP_CODES = list(range(0x01, 0x20)) + list(range(0x95, 0xA0))

def icon_tiles(name, art):
    if len(art) != 16 or any(len(row) != 16 for row in art):
        sys.exit(f"Icono '{name}': tiene que ser de 16x16")
    # escalado x2: cada fila y cada columna se duplican -> 32x32
    px = [[c == "#" for c in row for _ in (0, 1)] for row in art for _ in (0, 1)]
    tiles = []
    for ty in range(2):
        for tx in range(4):
            rows = []
            for y in range(16):
                b = 0
                for x in range(8):
                    if px[ty * 16 + y][tx * 8 + x]:
                        b |= 0x80 >> x
                rows.append(b)
            tiles.append(bytes(rows))
    return tiles

# Glifos de una celda generados por código (8x16, bit7 = columa izquierda)
CELL_BASE = 0xE0

def vbar(level):  # barra vertical angosta de 6 px; level 1..8 = 2 px por niuvel desde abajo
    return bytes(0x7E if y >= 16 -2 * level else 0 for y in range(16))

def hbar(level):  # barra hosrizontal fina de 4 px de alto; level 1..8 px desde la izquierda
    mask = (0xFF << (8 - level)) & 0xFF
    return bytes(mask if 6 <= y <= 9 else 0 for y in range(16))

def dot(filled):   # cuadradito de 6x6 centrado
    rows = []
    for y in range(16):
        if 5 <= y <= 10:
            rows.append(0x7E if filled or y in (5, 10) else 0x42)
        else:
            rows.append(0)
    return bytes(rows)

def hbar_r(level):
    mask = (1 << level) - 1
    return bytes(mask if 6 <= y <= 9 else 0 for y in range(16))

def marker():
    return bytes(0xFF if 6 <= y <= 9 else (0x18 if 2 <= y <= 13 else 0) for y in range(16))

# 0xE0..0xE7 vbar | 0xE8..0xEF hbar izq | 0xF0/0xF1 puntos | 0xF2..0xF8 hbar der | 0xF9 marca
CELLS = ([vbar(l) for l in range(1, 9)] + [hbar(l) for l in range(1, 9)] +
         [dot(True), dot(False)] + [hbar_r(l) for l in range(1, 8)] + [marker()])

glyphs = {}
with open(SRC) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        cp, bits = line.split(":")
        if len(bits) == 32:  # solo glifos de 8x16
            glyphs[int(cp, 16)] = bytes.fromhex(bits)

table = [bytes(16)] * 256
for code, cp in MAP.items():
    if cp not in glyphs:
        sys.exit(f"Falta U+{cp:04X} en {SRC}")
    table[code] = glyphs[cp]
    
for i, (name, art) in enumerate(ICONS):
    for t, tile in enumerate(icon_tiles(name, art)):
        table[ICON_BASE + i * 8 + t] = tile

for i, g in enumerate(CELLS):
    table[CELL_BASE + i] = g

app_codes = []
for i, (name, art) in enumerate(APP_ICONS):
    codes = APP_CODES[i * 8:(i + 1) * 8]
    if len(codes) < 8:
        sys.exit("No quedan codigos libres para mas iconos de apps")
    for code, tile in zip(codes, icon_tiles(name, art)):
        table[code] = tile
    app_codes.append(codes)

with open("main/app_icons.h", "w") as f:
    f.write("// Generado por tools/gen_font.py: tiles de los iconos de apps (4x2, por filas)\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write(f"#define APP_ICON_COUNT {len(APP_ICONS)}\n\n")
    f.write("static const uint8_t app_icon_tiles[APP_ICON_COUNT][8] = {\n")
    for (name, _), codes in zip(APP_ICONS, app_codes):
        f.write("    {" + ", ".join(f"0x{c:02X}" for c in codes) + f"}},  // {name}\n")
    f.write("};\n")
print("main/app_icons.h generado")

with open(DST, "w") as f:
    f.write("// Generado por tools/gen_font.py desde unscii-16 (dominio publico)\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write("static const uint8_t font8x16[256][16] = {\n")
    for code, g in enumerate(table):
        f.write("    {" + ", ".join(f"0x{b:02X}" for b in g) + f"}}, // 0x{code:02X}\n")
    f.write("};\n")
print(f"{DST} generado")