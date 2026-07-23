import json
import shutil
import sys
from collections import Counter
from pathlib import Path

from PIL import Image


dataset = Path(sys.argv[1])
report_path = Path(sys.argv[2])
sample_dir = Path(sys.argv[3])
required = {"rgb.png", "semantic.png", "frame_info.json", "groundtruth.json"}
expected_ids = {0, 10, 20, 100, 200}

frames = []
for frame_dir in sorted(dataset.glob("frame_*")):
    try:
        frame_id = int(frame_dir.name.removeprefix("frame_"))
    except ValueError:
        continue
    files = {path.name for path in frame_dir.iterdir() if path.is_file()}
    frames.append((frame_id, frame_dir, files))

ids = [item[0] for item in frames]
missing_ids = (
    sorted(set(range(min(ids), max(ids) + 1)).difference(ids))
    if ids
    else []
)
missing_required = {
    str(frame_id): sorted(required.difference(files))
    for frame_id, _frame_dir, files in frames
    if not required.issubset(files)
}
complete = [item for item in frames if required.issubset(item[2])]

frame_info_mismatches = []
for frame_id, frame_dir, _files in complete:
    info = json.loads((frame_dir / "frame_info.json").read_text(encoding="utf-8-sig"))
    try:
        info_frame_id = int(info.get("frame_id"))
    except (TypeError, ValueError):
        info_frame_id = None
    if (
        info_frame_id != frame_id
        or info.get("image_count") != 2
        or info.get("object_count") != 4
    ):
        frame_info_mismatches.append(
            {
                "directory_frame_id": frame_id,
                "frame_id": info_frame_id,
                "image_count": info.get("image_count"),
                "object_count": info.get("object_count"),
            }
        )

sample_results = []
if complete:
    indexes = sorted({0, len(complete) // 2, len(complete) - 1})
    sample_dir.mkdir(parents=True, exist_ok=True)
    for index in indexes:
        frame_id, frame_dir, _files = complete[index]
        semantic_path = frame_dir / "semantic.png"
        image = Image.open(semantic_path).convert("RGBA")
        counts = Counter(image.getdata())
        invalid_count = sum(
            count
            for rgba, count in counts.items()
            if rgba[0] not in expected_ids or rgba[1:] != (0, 0, 255)
        )
        present_ids = {rgba[0] for rgba in counts}
        copied_path = sample_dir / f"frame_{frame_id:06d}_semantic.png"
        shutil.copy2(semantic_path, copied_path)
        sample_results.append(
            {
                "frame_id": frame_id,
                "size": list(image.size),
                "unique_rgba_count": len(counts),
                "missing_object_ids": sorted(expected_ids.difference({0}).difference(present_ids)),
                "invalid_pixel_count": invalid_count,
                "copied_image": str(copied_path.resolve()),
            }
        )

report = {
    "dataset": str(dataset.resolve()),
    "frame_directory_count": len(frames),
    "min_frame_id": min(ids) if ids else None,
    "max_frame_id": max(ids) if ids else None,
    "missing_frame_id_count": len(missing_ids),
    "missing_frame_ids": missing_ids,
    "complete_frame_count": len(complete),
    "incomplete_frame_count": len(missing_required),
    "missing_required_files": missing_required,
    "frame_info_mismatch_count": len(frame_info_mismatches),
    "frame_info_mismatches": frame_info_mismatches,
    "semantic_samples": sample_results,
    "passed_content_checks": (
        bool(frames)
        and not missing_required
        and not frame_info_mismatches
        and all(
            not sample["missing_object_ids"] and sample["invalid_pixel_count"] == 0
            for sample in sample_results
        )
    ),
    "passed_continuity_check": not missing_ids,
}

report_path.parent.mkdir(parents=True, exist_ok=True)
report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(report, ensure_ascii=False, indent=2))
