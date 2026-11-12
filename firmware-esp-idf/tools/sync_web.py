#!/usr/bin/env python3
"""Copy the shared web UI into firmware-esp-idf/spiffs.

The ESP-IDF build embeds this directory as the SPIFFS "storage" partition.
Run after editing shared/web/*.
"""
from __future__ import annotations

import shutil
from pathlib import Path

FILES = [
    "index.html",
    "settings.html",
    "setup.html",
    "login.html",
    "locked.html",
    "style.css",
    "app.js",
]


def main() -> None:
    here = Path(__file__).resolve().parent
    repo = here.parent.parent
    src = repo / "shared" / "web"
    dst = repo / "firmware-esp-idf" / "spiffs"
    dst.mkdir(parents=True, exist_ok=True)
    total = 0
    for name in FILES:
        path = src / name
        if not path.exists():
            raise SystemExit(f"missing asset: {path}")
        shutil.copy2(path, dst / name)
        total += path.stat().st_size
        print(f"copied {name}")
    print(f"synced {len(FILES)} files ({total} bytes) -> {dst}")


if __name__ == "__main__":
    main()
