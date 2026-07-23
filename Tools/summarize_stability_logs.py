import json
import re
import sys
from pathlib import Path


report_path = Path(sys.argv[1])
logs = [Path(value) for value in sys.argv[2:]]
patterns = {
    "readback_queue_full": r"Readback queue is full",
    "readback_rejected": r"Image readback was rejected",
    "frame_timeouts": r"timed out after",
    "export_queue_full": r"Export queue full",
    "pso_failures": r"Failed to create pipeline state",
    "fatal_errors": r"Fatal error:",
}

results = []
for log in logs:
    text = log.read_text(encoding="utf-8", errors="replace")
    result = {
        "log": str(log.resolve()),
        **{key: len(re.findall(pattern, text)) for key, pattern in patterns.items()},
        "clean_exit": "LogExit: Exiting" in text,
    }
    results.append(result)

report = {"runs": results}
report_path.parent.mkdir(parents=True, exist_ok=True)
report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(report, ensure_ascii=False, indent=2))
