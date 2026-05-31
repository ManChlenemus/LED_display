import os
import glob
from PIL import Image, ImageFont, ImageDraw

FONTS_DIR = "."
FONT_SIZE = 18
OUTPUT_FILE = "fonts.h"

CHARS = " ?!\"',-_./0123456789@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]abcdefghijklmnopqrstuvwxyzАБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдеёжзийклмнопрстуфхцчшщъыьэюя"

font_files = []
for root, dirs, files in os.walk(FONTS_DIR):
    for file in files:
        if file.lower().endswith((".otf", ".ttf")):
            font_files.append(os.path.join(root, file))

if not font_files:
    print(f"Ошибка: Не найдено ни одного шрифта в папке {FONTS_DIR}!")
    exit(1)

font_files.sort()

cpp_code = "#pragma once\n#include <Arduino.h>\n\n"
cpp_code += "struct Glyph {\n    uint8_t width;\n    const uint8_t* data;\n};\n\n"
cpp_code += f"// Сгенерировано {len(font_files)} шрифтов:\n"

for i, f in enumerate(font_files):
    cpp_code += f"// Индекс {i}: {os.path.basename(f)}\n"
cpp_code += "\n"

all_mappings = {}

print(f"Найдено шрифтов: {len(font_files)}. Начинаем генерацию...")

for font_index, font_path in enumerate(font_files):
    font_name = os.path.basename(font_path).split(".")[0].replace("-", "_")
    print(f"[{font_index}] Обработка: {font_name}...")

    try:
        font = ImageFont.truetype(font_path, FONT_SIZE)
    except Exception as e:
        print(f"  -> Пропуск (ошибка загрузки): {e}")
        continue

    ascent, descent = font.getmetrics()
    total_height = ascent + descent
    y_offset = (24 - total_height) // 2
    baseline = y_offset + ascent

    font_mapping = []

    for char_index, char in enumerate(CHARS):
        bbox = font.getmask(char).getbbox()
        width = bbox[2] if bbox else int(font.getlength(char))
        if width <= 0:
            width = 6

        img = Image.new("1", (width, 24), 0)
        draw = ImageDraw.Draw(img)
        draw.text((0, baseline), char, font=font, fill=1, anchor="ls")

        bytes_array = []
        for x in range(width):
            b0 = b1 = b2 = 0
            for y in range(8):
                if img.getpixel((x, y)):
                    b0 |= 1 << y
            for y in range(8, 16):
                if img.getpixel((x, y)):
                    b1 |= 1 << (y - 8)
            for y in range(16, 24):
                if img.getpixel((x, y)):
                    b2 |= 1 << (y - 16)
            bytes_array.extend([b0, b1, b2])

        hex_str = ", ".join([f"0x{b:02X}" for b in bytes_array])
        array_name = f"g_{font_index}_{char_index}"

        cpp_code += f"const uint8_t {array_name}[] PROGMEM = {{ {hex_str} }};\n"

        try:
            byte_code = char.encode("cp1251")[0]
        except UnicodeEncodeError:
            byte_code = ord(char)

        font_mapping.append((byte_code, array_name, width))

    all_mappings[font_index] = font_mapping
    cpp_code += "\n"

cpp_code += "const Glyph getGlyph(uint8_t code, uint8_t fontIndex) {\n"
cpp_code += "    switch(fontIndex) {\n"

for font_index, mapping in all_mappings.items():
    cpp_code += f"        case {font_index}:\n"
    cpp_code += "            switch(code) {\n"
    for code, array_name, width in mapping:
        cpp_code += (
            f"                case {code}: return {{ {width}, {array_name} }};\n"
        )
    cpp_code += "                default: return { 0, nullptr };\n"
    cpp_code += "            }\n"

cpp_code += "        default: return { 0, nullptr };\n"
cpp_code += "    }\n}\n"

cpp_code += f"\nconstexpr uint8_t CUSTOM_FONT_COUNT = {len(all_mappings)};\n"

with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
    f.write(cpp_code)

print(f"\nГотово! Файл {OUTPUT_FILE} содержит {len(all_mappings)} шрифтов.")
