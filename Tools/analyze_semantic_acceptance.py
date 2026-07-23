import json
import sys
from collections import Counter
from pathlib import Path

from PIL import Image


image_path = Path(sys.argv[1])
report_path = Path(sys.argv[2])
expected_ids = {0, 10, 20, 100, 200}

image = Image.open(image_path).convert("RGBA")
pixels = list(image.getdata())
counts = Counter(pixels)
red_counts = Counter(pixel[0] for pixel in pixels)

invalid_pixels = [
    {"rgba": list(rgba), "count": count}
    for rgba, count in sorted(counts.items())
    if rgba[0] not in expected_ids or rgba[1] != 0 or rgba[2] != 0 or rgba[3] != 255
]

report = {
    "image": str(image_path.resolve()),
    "size": list(image.size),
    "mode": image.mode,
    "expected_semantic_ids": sorted(expected_ids),
    "red_channel_counts": {str(key): value for key, value in sorted(red_counts.items())},
    "unique_rgba_count": len(counts),
    "invalid_rgba_values": invalid_pixels,
    "invalid_pixel_count": sum(item["count"] for item in invalid_pixels),
    "passed": image.size == (1280, 720) and not invalid_pixels,
}

report_path.parent.mkdir(parents=True, exist_ok=True)
report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(report, ensure_ascii=False, indent=2))
