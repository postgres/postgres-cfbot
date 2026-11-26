# Copyright (c) 2021-2026, PostgreSQL Global Development Group

"""Port of src/test/modules/test_json_parser/t/002_inline.pl.

Test success or failure of the incremental (table-driven) JSON parser
for a variety of small inputs.
"""

import re
import subprocess

import pytest
from pypg.util import run

EXES = [
    ("test_json_parser_incremental",),
    ("test_json_parser_incremental", "-o"),
    ("test_json_parser_incremental_shlib",),
    ("test_json_parser_incremental_shlib", "-o"),
]

# (name, input, error-regex-or-None). Inputs are bytes; backslash-heavy and
# non-UTF-8 inputs are spelled out explicitly to avoid escaping ambiguity.
CASES = [
    ("number", b"12345", None),
    ("string", b'"hello"', None),
    ("false", b"false", None),
    ("true", b"true", None),
    ("null", b"null", None),
    ("empty object", b"{}", None),
    ("empty array", b"[]", None),
    ("array with number", b"[12345]", None),
    ("array with numbers", b"[12345,67890]", None),
    ("array with null", b"[null]", None),
    ("array with string", b'["hello"]', None),
    ("array with boolean", b"[false]", None),
    ("single pair", b'{"key": "value"}', None),
    ("heavily nested array", b"[" * 3200 + b"]" * 3200, None),
    ("serial escapes", b'"' + b"\\" * 8 + b'"', None),
    (
        "interrupted escapes",
        b'"' + b"\\" * 3 + b'"' + b"\\" * 5 + b'"' + b"\\" * 2 + b'"',
        None,
    ),
    ("whitespace", b'     ""     ', None),
    ("unclosed empty object", b"{", r"input string ended unexpectedly"),
    ("bad key", b"{{", r'Expected string or "}", but found "\{"'),
    ("bad key", b"{{}", r'Expected string or "}", but found "\{"'),
    ("numeric key", b"{1234: 2}", r'Expected string or "}", but found "1234"'),
    (
        "second numeric key",
        b'{"a": "a", 1234: 2}',
        r'Expected string, but found "1234"',
    ),
    (
        "unclosed object with pair",
        b'{"key": "value"',
        r"input string ended unexpectedly",
    ),
    ("missing key value", b'{"key": }', r'Expected JSON value, but found "}"'),
    ("missing colon", b'{"key" 12345}', r'Expected ":", but found "12345"'),
    (
        "missing comma",
        b'{"key": 12345 12345}',
        r'Expected "," or "}", but found "12345"',
    ),
    ("overnested array", b"[" * 6401, r"maximum permitted depth is 6400"),
    ("overclosed array", b"[]]", r'Expected end of input, but found "]"'),
    (
        "unexpected token in array",
        b"[ }}} ]",
        r'Expected array element or "]", but found "}"',
    ),
    ("junk punctuation", b"[ ||| ]", r'Token "\|" is invalid'),
    ("missing comma in array", b"[123 123]", r'Expected "," or "]", but found "123"'),
    ("misspelled boolean", b"tru", r'Token "tru" is invalid'),
    ("misspelled boolean in array", b"[tru]", r'Token "tru" is invalid'),
    ("smashed top-level scalar", b"12zz", r'Token "12zz" is invalid'),
    ("smashed scalar in array", b"[12zz]", r'Token "12zz" is invalid'),
    (
        "unknown escape sequence",
        b'"hello\\vworld"',
        r'Escape sequence "\\v" is invalid',
    ),
    (
        "unescaped control",
        b'"hello\tworld"',
        r"Character with value 0x09 must be escaped",
    ),
    (
        "incorrect escape count",
        b'"' + b"\\" * 7 + b'"',
        r'Token ""' + r"\\" * 7 + r'"" is invalid',
    ),
    # Case with three bytes: double-quote, backslash and <f5>.
    # Both invalid-token and invalid-escape are possible errors, because for
    # smaller chunk sizes the incremental parser skips the string parsing when
    # it cannot find an ending quote.
    (
        "incomplete UTF-8 sequence",
        b'"\\\xf5',
        r'(Token|Escape sequence) ""?\\\xf5" is invalid',
    ),
]


def split_nul(data):
    """Mimic Perl's unpack("(Z*)*"): split on NUL into strings, dropping the
    single trailing empty string left by a trailing NUL terminator.
    """
    parts = data.split(b"\0")
    if parts and parts[-1] == b"":
        parts.pop()
    return [p.decode("latin-1") for p in parts]


@pytest.mark.parametrize("exe", EXES, ids=[" ".join(e) for e in EXES])
@pytest.mark.parametrize(
    "data,error",
    [(data, error) for _, data, error in CASES],
    ids=[name for name, _, _ in CASES],
)
def test_inline(exe, data, error, tmp_path):
    """Each input, against each program flavor, at every chunk size.

    Parametrized rather than looped over so a failure names the input and the
    remaining inputs still run; the case names become the test ids.
    """
    # Test the input with chunk sizes from max(input_size, 64) down to 1
    chunk = min(len(data), 64)

    fname = tmp_path / "input.json"
    fname.write_bytes(data)

    # The -r mode runs the parser in a loop, with output separated by nulls.
    # Unpack that as a list of null-terminated ASCII strings (Z*) and check that
    # each run produces the same result.
    r = run(
        *exe,
        "-r",
        chunk,
        fname,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    stdout = split_nul(r.stdout)
    stderr = split_nul(r.stderr)

    assert len(stdout) == chunk, "stdout has correct number of entries"
    assert len(stderr) == chunk, "stderr has correct number of entries"

    for i, size in enumerate(range(chunk, 0, -1)):
        if error is not None:
            assert "SUCCESS" not in stdout[i], f"chunk size {size}: fails"
            assert re.search(error, stderr[i]), f"chunk size {size}: {stderr[i]!r}"
        else:
            assert "SUCCESS" in stdout[i], f"chunk size {size}: succeeds"
            assert stderr[i] == "", f"chunk size {size}: no error output"
