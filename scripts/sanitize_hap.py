#!/usr/bin/env python3
"""Create an unsigned HAP release copy without source maps or build-host paths."""
from __future__ import annotations

import argparse
import pathlib
import re
import zipfile

HOST_PATH = re.compile(rb"/Users/date/[^\x00]*")
DROP_ENTRIES = {"ets/sourceMaps.map", "pkgContextInfo.json"}


def scrub_native(data: bytes) -> tuple[bytes, int]:
    out = bytearray(data)
    matches = list(HOST_PATH.finditer(data))
    for match in reversed(matches):
        old = match.group(0)
        basename = old.rsplit(b"/", 1)[-1]
        replacement = b"/source-redacted/" + basename
        if len(replacement) > len(old):
            replacement = b"/src/" + basename
        if len(replacement) > len(old):
            raise ValueError(f"cannot safely scrub path literal {old!r}")
        out[match.start():match.end()] = replacement + b"\0" * (len(old) - len(replacement))
    return bytes(out), len(matches)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    paths_scrubbed = 0
    with zipfile.ZipFile(args.input, "r") as source, zipfile.ZipFile(args.output, "w") as target:
        for info in source.infolist():
            if info.filename in DROP_ENTRIES:
                continue
            payload = source.read(info.filename)
            if info.filename == "libs/arm64-v8a/libhps3core.so":
                payload, paths_scrubbed = scrub_native(payload)
            target.writestr(info, payload)

    with zipfile.ZipFile(args.output, "r") as result:
        bad = result.testzip()
        if bad:
            raise SystemExit(f"invalid ZIP entry after sanitization: {bad}")
        if any(name in result.namelist() for name in DROP_ENTRIES):
            raise SystemExit("source map or package context unexpectedly remains")
        for info in result.infolist():
            if HOST_PATH.search(result.read(info.filename)):
                raise SystemExit(f"host path remains in {info.filename}")
    print(f"sanitized {args.output}: removed {len(DROP_ENTRIES)} metadata files; scrubbed {paths_scrubbed} host paths")


if __name__ == "__main__":
    main()
