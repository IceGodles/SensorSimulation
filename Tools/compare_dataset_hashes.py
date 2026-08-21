#!/usr/bin/env python3
"""计算并比较一个或多个 Session 的 committed frame 内容哈希。"""
import argparse
import hashlib
import json
import fnmatch
from pathlib import Path


def hash_session(session: Path, excludes: list[str]) -> dict:
    frames = sorted(path for path in session.glob("frame_*") if path.is_dir() and not path.name.endswith(".tmp"))
    frame_hashes = []
    root_hash = hashlib.sha256()
    for frame in frames:
        digest = hashlib.sha256()
        for path in sorted(item for item in frame.rglob("*") if item.is_file()):
            if any(fnmatch.fnmatch(path.name, pattern) for pattern in excludes):
                continue
            relative = path.relative_to(frame).as_posix().encode("utf-8")
            digest.update(len(relative).to_bytes(4, "little"))
            digest.update(relative)
            with path.open("rb") as handle:
                for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                    digest.update(chunk)
        value = digest.hexdigest()
        frame_hashes.append({"frame": frame.name, "sha256": value})
        root_hash.update(frame.name.encode("utf-8"))
        root_hash.update(bytes.fromhex(value))
    return {"session": str(session.resolve()), "frame_count": len(frames), "sha256": root_hash.hexdigest(), "frames": frame_hashes}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sessions", nargs="+")
    parser.add_argument("--report")
    parser.add_argument("--exclude", action="append", default=[], help="按文件名 glob 排除非确定性模态")
    args = parser.parse_args()
    results = [hash_session(Path(value), args.exclude) for value in args.sessions]
    reference = results[0]["sha256"] if results else ""
    passed = bool(results) and all(item["sha256"] == reference for item in results)
    report = {"passed": passed, "excluded": args.exclude, "sessions": results}
    output = json.dumps(report, ensure_ascii=False, indent=2)
    print(output)
    if args.report:
        Path(args.report).write_text(output, encoding="utf-8")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
