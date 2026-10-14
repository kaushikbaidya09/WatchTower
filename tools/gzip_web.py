#!/usr/bin/env python3
"""Gzip-compress one web console file for embedding in the firmware.

Usage: gzip_web.py <source file> <output file>

The output has no timestamp, so the same source always gives the same bytes.
"""
import gzip
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = gzip.compress(f.read(), compresslevel=9, mtime=0)
with open(dst, "wb") as f:
    f.write(data)
