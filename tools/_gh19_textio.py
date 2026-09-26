#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Canonical text I/O for the GH#19 PR-gate tools (task #122).

Why this module exists
----------------------
Python text mode with no ``encoding=`` uses the *process locale* encoding, not UTF-8. On the
Windows CI runner that locale is cp1252, which decodes almost any byte pair, so the same root
cause has two faces:

  * a read with no ``errors=`` raises UnicodeDecodeError. Inside a CMake custom command that
    surfaces only as ``error MSB8066 ... exited with code 1``, which says nothing about encoding.
    This is exactly how tools/stage_gh19_s6_shadow.py reading core/include/lunar24/core/vco.h
    failed on the Windows runner for PR #46.
  * a read carrying ``errors="replace"`` does not raise at all: the offending byte becomes U+FFFD
    and a mojibaked line reaches a gate that is reading *evidence*. Nothing in a gate may
    substitute a byte silently.

Both faces are handled in one place here, so the policy exists once rather than once per call site:

  * reads and writes pin ``encoding="utf-8"``;
  * writes additionally pin ``newline="\\n"``, so a report produced on Windows has the same bytes
    as one produced on macOS/Linux -- the pinned evidence artifacts are compared across platforms;
  * a byte that is not valid UTF-8 raises Gh19TextIOError: a *named* refusal carrying the path and
    the byte offset. A gate that reads evidence must either read it exactly or say it could not.

Non-codec errors (FileNotFoundError, PermissionError, IsADirectoryError) are deliberately NOT
wrapped: they keep their own types, because gates and callers already distinguish those.

Usage
-----
    from _gh19_textio import open_text, read_text, write_text

    text = read_text(path)                       # whole-file read
    write_text(path, text)                       # whole-file write, LF line ends
    with open_text(path) as fh:                  # streaming / line iteration
        for line in fh:
            ...
    with open_text(path, "w") as fh:             # streaming write
        fh.write(...)

The streaming form yields a real file object, so line-iteration semantics are exactly those of
``open(path, encoding="utf-8")`` -- in particular do NOT rewrite such loops as
``read_text(path).splitlines()``: str.splitlines() also breaks on \\v, \\f, \\x1c-\\x1e, \\x85 and
U+2028/2029, which file iteration does not, so it is not a drop-in replacement.
"""

import contextlib

__all__ = ["ENCODING", "NEWLINE", "Gh19TextIOError", "read_text", "write_text", "open_text"]

ENCODING = "utf-8"
NEWLINE = "\n"


class Gh19TextIOError(Exception):
    """A text file could not be read or written as UTF-8.

    Named refusal rather than a silent substitution (never ``errors="replace"``). Carries the
    path, the byte offset where the codec stopped, and the codec's own reason.
    """

    def __init__(self, path, reason, offset=None, exc=None):
        self.path = str(path)
        self.reason = reason
        self.offset = offset
        self.exc = exc
        where = "" if offset is None else " at byte %d" % offset
        super().__init__("%s: %s%s" % (self.path, reason, where))


def _named(path, exc):
    """Build the named refusal for a codec failure (decode or encode)."""
    if isinstance(exc, UnicodeDecodeError):
        return Gh19TextIOError(path, "not valid UTF-8 (%s)" % exc.reason, exc.start, exc)
    return Gh19TextIOError(path, "not encodable as UTF-8 (%s)" % exc.reason, exc.start, exc)


def read_text(path):
    """Read the whole file as UTF-8. Bad bytes raise Gh19TextIOError, never substitute."""
    try:
        with open(path, encoding=ENCODING) as fh:
            return fh.read()
    except (UnicodeDecodeError, UnicodeEncodeError) as exc:
        raise _named(path, exc) from exc


def write_text(path, text):
    """Write text as UTF-8 with LF line ends. Unencodable text raises Gh19TextIOError."""
    try:
        with open(path, "w", encoding=ENCODING, newline=NEWLINE) as fh:
            fh.write(text)
    except (UnicodeDecodeError, UnicodeEncodeError) as exc:
        raise _named(path, exc) from exc


@contextlib.contextmanager
def open_text(path, mode="r", newline=None):
    """Yield a UTF-8 text file object opened in `mode`; codec failures become Gh19TextIOError.

    The yielded object is the real file object, so iteration and `with` semantics are unchanged
    from `open(path, mode, encoding="utf-8")`. Decode/encode failures raised anywhere in the body
    are converted, which is what lets a line-iterating loop fail loudly instead of midway.

    `newline` is None for this module's policy: universal newlines on a read, LF on a write. Pass
    it explicitly only where the caller's own line handling is the point -- csv is the case that
    exists here, and it asks to be given a file opened with newline="". Passed explicitly on a
    read it replaces universal-newline translation rather than adding to it, which is exactly the
    difference such a caller wants; on a write it replaces the LF policy.
    """
    if "b" in mode:
        raise ValueError("open_text is text-only; mode %r asks for binary" % mode)
    kwargs = {"encoding": ENCODING}
    if any(c in mode for c in "wax+"):
        kwargs["newline"] = NEWLINE if newline is None else newline
    elif newline is not None:
        kwargs["newline"] = newline
    fh = open(path, mode, **kwargs)
    try:
        yield fh
    except (UnicodeDecodeError, UnicodeEncodeError) as exc:
        raise _named(path, exc) from exc
    finally:
        fh.close()
