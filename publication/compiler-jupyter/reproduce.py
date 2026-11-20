#!/usr/bin/env python3
"""Run the article examples against an already built v0.18.2 checkout."""
import argparse
import base64
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess

import nbformat
from nbclient import NotebookClient

REVISION = "12131e52fff5b7bc1afbbc50e90dd2ac3bb79ec7"


def run(args, cwd):
    return subprocess.run(args, cwd=cwd, input=b"", capture_output=True,
                          check=True, timeout=60).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkout", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    checkout = args.checkout.resolve()
    output = args.output.resolve()
    revision = run(["git", "rev-parse", "HEAD"], checkout).decode().strip()
    if revision != REVISION:
        parser.error(f"Expected release {REVISION}, got {revision}")
    if run(["git", "status", "--porcelain", "--untracked-files=no"], checkout):
        parser.error("The release checkout has tracked modifications")
    if importlib.metadata.version("gwbasickernel") != "0.1.0":
        parser.error("The article uses gwbasickernel 0.1.0")
    output.mkdir(parents=True, exist_ok=True)
    build = checkout / "build"
    interpreted = run([str(build / "gwbasic"), "examples/squares.bas"], checkout)
    shutil.copyfile(checkout / "examples/squares.bas", build / "squares.bas")
    run([str(build / "gwbasic-compile"), "build/squares.bas", "-c",
         "--keep-c", "--runtime", "."], checkout)
    native = run([str(build / "squares")], checkout)
    if interpreted != native:
        raise RuntimeError("Interpreter and native output differ")
    expected = ["N", "N SQUARED"] + [str(v) for n in range(1, 9) for v in (n, n*n)]
    if native.decode().replace("N SQUARED", "N_SQUARED").split() != [
            x.replace("N SQUARED", "N_SQUARED") for x in expected]:
        raise RuntimeError("The squares table differs from the arithmetic oracle")
    (output / "squares-output.txt").write_bytes(native)
    shutil.copyfile(build / "squares.bas.c", output / "squares.bas.c")
    circles = (checkout / "examples/circles.bas").read_text(encoding="utf-8").strip()
    cells = [
        nbformat.v4.new_markdown_cell(
            "# GW-BASIC 2026: persistent state and graphics\n\n"
            "Executed with v0.18.2 and gwbasickernel 0.1.0. "
            "Run the cells in order. The graphics cell clears the stored program.\n\n"
            "Article: https://valetov.com/articles/gw-basic-2026/", id="introduction"),
        nbformat.v4.new_code_cell("A=6", id="set-variable"),
        nbformat.v4.new_code_cell("PRINT A*7", id="read-variable"),
        nbformat.v4.new_code_cell("NEW\n" + circles + "\nRUN", id="draw-circles"),
        nbformat.v4.new_code_cell('SCREEN 0\nPRINT "DONE"', id="finish"),
    ]
    notebook = nbformat.v4.new_notebook(cells=cells, metadata={
        "kernelspec": {"name": "gwbasic", "display_name": "GW-BASIC 2026", "language": "basic"},
        "language_info": {"name": "basic", "file_extension": ".bas"},
    })
    os.environ["GWBASIC"] = str(build / "gwbasic")
    NotebookClient(notebook, timeout=60, kernel_name="gwbasic", record_timing=False,
                   resources={"metadata": {"path": str(output)}}).execute()
    state_output = "".join(o.get("text", "") for o in notebook.cells[2].outputs).strip()
    if state_output != "42":
        raise RuntimeError(f"Persistent state check failed: {state_output!r}")
    finished = "".join(o.get("text", "") for o in notebook.cells[4].outputs).strip()
    if finished != "DONE":
        raise RuntimeError(f"Notebook did not finish: {finished!r}")
    frames = [base64.b64decode(o["data"]["image/png"])
              for o in notebook.cells[3].outputs if "image/png" in o.get("data", {})]
    if not frames:
        raise RuntimeError("Graphics cell produced no PNG frames")
    dimensions = [struct.unpack(">II", frame[16:24]) for frame in frames]
    if any(w != 320 or h not in (200, 204) for w, h in dimensions):
        raise RuntimeError(f"Unexpected frame dimensions: {dimensions}")
    (output / "graphics.png").write_bytes(frames[-1])
    nbformat.write(notebook, output / "examples.ipynb")
    files = ("squares-output.txt", "squares.bas.c", "graphics.png", "examples.ipynb")
    record = {
        "source_revision": revision,
        "source_tag": "v0.18.2",
        "platform": platform.system() + " " + platform.machine(),
        "python": platform.python_version(),
        "gcc": run(["gcc", "--version"], checkout).decode().splitlines()[0],
        "packages": {name: importlib.metadata.version(name) for name in
                     ("gwbasickernel", "jupyterlab", "nbclient")},
        "interpreter_native_byte_match": True,
        "squares_arithmetic_oracle": "8 rows: n and n*n for n=1..8",
        "persistent_state_result": state_output,
        "notebook_finished": finished,
        "png_frames": len(frames),
        "final_png_dimensions": dimensions[-1],
        "sha256": {name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                   for name in files},
    }
    (output / "verification.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
