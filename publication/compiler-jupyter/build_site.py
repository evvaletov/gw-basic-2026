#!/usr/bin/env python3
"""Keep the original article-build command pointing to the complete site build."""
from pathlib import Path
import runpy

if __name__ == "__main__":
    runpy.run_path(str(Path(__file__).resolve().parents[1] / "build_site.py"),
                  run_name="__main__")
