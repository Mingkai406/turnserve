#!/usr/bin/env python3
"""Download the pinned smoke-test model; weights remain outside git."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

REVISION = "09816acd5d99df7be770d85ea30822623dab342c"
FILENAME = "SmolLM2-135M-Instruct-Q4_K_M.gguf"
SHA256 = "2e8040ceae7815abe0dcb3540b9995eaa1fa0d2ca9e797d0a635ae4433c68c2d"
URL = f"https://huggingface.co/bartowski/SmolLM2-135M-Instruct-GGUF/resolve/{REVISION}/{FILENAME}"

def checksum(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path("models"))
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    target = args.directory / FILENAME
    if target.exists() and checksum(target) == SHA256:
        print(target)
        return
    partial = target.with_suffix(".partial")
    try:
        with urllib.request.urlopen(URL, timeout=60) as response, partial.open("wb") as output:
            for block in iter(lambda: response.read(1024 * 1024), b""):
                output.write(block)
        if checksum(partial) != SHA256:
            raise RuntimeError("model checksum mismatch")
        partial.replace(target)
    finally:
        partial.unlink(missing_ok=True)
    print(target)

if __name__ == "__main__":
    main()
