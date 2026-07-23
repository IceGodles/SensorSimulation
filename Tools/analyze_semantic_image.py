import json
import sys
from collections import Counter
from pathlib import Path

from PIL import Image


image_path = Path(sys.argv[1])
report_path = Path(sys.argv[2])
expected_width = int(sys.argv[3])
expected_height = int(sys.argv[4])
expected_ids = {0, 10, 20, 100, 200}

image = Image.open(image_path).convert("RGBA")
counts = Counter(image.getdata())
red_counts = Counter()
invalid_values = []
for rgba, count in sorted(counts.items()):
    red_counts[rgba[0]] += count
    if rgba[0] not in expected_ids or rgba[1:] != (0, 0, 255):
        invalid_values.append({"rgba": list(rgba), "count": count})

missing_object_ids = sorted(expected_ids.difference({0}).difference(red_counts))
report = {
    "image": str(image_path.resolve()),
    "size": list(image.size),
    "expected_size": [expected_width, expected_height],
    "mode": image.mode,
    "expected_semantic_ids": sorted(expected_ids),
    "red_channel_counts": {str(key): value for key, value in sorted(red_counts.items())},
    "unique_rgba_count": len(counts),
    "missing_object_ids": missing_object_ids,
    "invalid_rgba_values": invalid_values,
    "invalid_pixel_count": sum(item["count"] for item in invalid_values),
    "passed": (
        image.size == (expected_width, expected_height)
        and not missing_object_ids
        and not invalid_values
    ),
}

report_path.parent.mkdir(parents=True, exist_ok=True)
report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(report, ensure_ascii=False, indent=2))
