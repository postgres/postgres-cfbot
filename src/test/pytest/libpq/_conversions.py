# Copyright (c) 2025, PostgreSQL Global Development Group

"""
Conversion between PostgreSQL text values and Python objects.

Two directions live here: the OID-keyed registry that turns a column's text
value into a typed Python object (``register_type_info`` / ``_convert_pg_value``
/ ``_parse_array``), and ``_build_params``, which encodes Python parameters into
the C array libpq's extended-protocol functions expect. ``simplify_query_results``
is the small ergonomic helper that unwraps single-row/single-column results.
"""

from __future__ import annotations

import ctypes
import datetime
import decimal
import json
import uuid
from collections.abc import Callable
from typing import Any

# PostgreSQL type OIDs and conversion system
# Type registry - maps OID to converter function
_type_converters: dict[int, Callable[[str], Any]] = {}
_array_to_elem_map: dict[int, int] = {}


def register_type_info(
    name: str, oid: int, array_oid: int, converter: Callable[[str], Any]
):
    """
    Register a PostgreSQL type with its OID, array OID, and conversion function.

    Usage:
        register_type_info("bool", 16, 1000, lambda v: v == "t")
    """
    _type_converters[oid] = converter
    if array_oid is not None:
        _array_to_elem_map[array_oid] = oid


def _parse_array(value: str, elem_oid: int) -> list[Any]:
    """Parse PostgreSQL array syntax into nested Python lists."""
    stack: list[list[Any]] = []
    current_element: list[str] = []
    in_quotes = False
    was_quoted = False
    pos = 0

    while pos < len(value):
        char = value[pos]

        if in_quotes:
            if char == "\\":
                next_char = value[pos + 1]
                if next_char not in '"\\':
                    raise NotImplementedError('Only \\" and \\\\ escapes are supported')
                current_element.append(next_char)
                pos += 2
                continue
            elif char == '"':
                in_quotes = False
            else:
                current_element.append(char)
        elif char == '"':
            in_quotes = True
            was_quoted = True
        elif char == "{":
            stack.append([])
        elif char in ",}":
            if current_element or was_quoted:
                elem = "".join(current_element)
                if not was_quoted and elem == "NULL":
                    stack[-1].append(None)
                else:
                    stack[-1].append(_convert_pg_value(elem, elem_oid))
                current_element = []
                was_quoted = False
            if char == "}":
                completed = stack.pop()
                if not stack:
                    return completed
                stack[-1].append(completed)
        elif char != " ":
            current_element.append(char)
        pos += 1

    raise ValueError(f"Malformed array literal: {value}")


# Register standard PostgreSQL types that we'll likely encounter in tests
register_type_info("bool", 16, 1000, lambda v: v == "t")
register_type_info("int2", 21, 1005, int)
register_type_info("int4", 23, 1007, int)
register_type_info("int8", 20, 1016, int)
register_type_info("float4", 700, 1021, float)
register_type_info("float8", 701, 1022, float)
register_type_info("numeric", 1700, 1231, decimal.Decimal)
register_type_info("text", 25, 1009, str)
register_type_info("varchar", 1043, 1015, str)
register_type_info("date", 1082, 1182, datetime.date.fromisoformat)
register_type_info("time", 1083, 1183, datetime.time.fromisoformat)
register_type_info("timestamp", 1114, 1115, datetime.datetime.fromisoformat)
register_type_info("timestamptz", 1184, 1185, datetime.datetime.fromisoformat)
register_type_info("uuid", 2950, 2951, uuid.UUID)
register_type_info("json", 114, 199, json.loads)
register_type_info("jsonb", 3802, 3807, json.loads)


def _convert_pg_value(value: str, type_oid: int) -> Any:
    """
    Convert PostgreSQL string value to appropriate Python type based on OID.
    Uses the registered type converters from register_type_info().
    """
    # Check if it's an array type
    if type_oid in _array_to_elem_map:
        elem_oid = _array_to_elem_map[type_oid]
        return _parse_array(value, elem_oid)

    # Use registered converter if available
    converter = _type_converters.get(type_oid)
    if converter:
        return converter(value)

    # Unknown types - return as string
    return value


def simplify_query_results(results: list[tuple[Any, ...]]) -> Any:
    """
    Simplify the results of a query so that the caller doesn't have to unpack
    lists and tuples of length 1.
    """
    if len(results) == 1:
        row = results[0]
        if len(row) == 1:
            # If there's only a single cell, just return the value
            return row[0]
        # If there's only a single row, just return that row
        return row

    if len(results) != 0 and len(results[0]) == 1:
        # If there's only a single column, return an array of values
        return [row[0] for row in results]

    # if there are multiple rows and columns, return the results as is
    return results


# Type OIDs for the Python types whose SQL type is unambiguous. Sending these
# saves the caller writing casts into the query: without a type the server sees
# an "unknown" parameter, which it cannot resolve when a function is overloaded
# (generate_series($1, $2) being the common example).
_BOOL_OID = 16
_INT8_OID = 20
_INT4_OID = 23
_FLOAT8_OID = 701
_NUMERIC_OID = 1700

# The ranges of the integer types we can declare. Python has no equivalent of
# INT32_MAX to borrow -- sys.maxsize is the pointer width, not int4 -- so name
# them here rather than spelling powers of two inline.
_INT4_MIN, _INT4_MAX = -(2**31), 2**31 - 1
_INT8_MIN, _INT8_MAX = -(2**63), 2**63 - 1

# Strings are deliberately absent: leaving them unknown is what lets a Python
# str stand in for whatever the context wants -- a regclass argument, an enum,
# a pg_lsn -- without the test spelling out the type.


def _param_oid(value: Any) -> int:
    """The OID to declare for ``value``, or 0 to leave it to the server."""
    if isinstance(value, bool):  # before int: bool is a subclass of it
        return _BOOL_OID
    if isinstance(value, int):
        # The narrowest type that holds it, so a parameter can be passed to a
        # function or compared against a column expecting a plain integer.
        # Python ints are unbounded, so fall back to numeric beyond int8.
        if _INT4_MIN <= value <= _INT4_MAX:
            return _INT4_OID
        if _INT8_MIN <= value <= _INT8_MAX:
            return _INT8_OID
        return _NUMERIC_OID
    if isinstance(value, float):
        return _FLOAT8_OID
    return 0


def _build_params(params: tuple[Any, ...]) -> tuple[int, Any, Any]:
    """Build the (nParams, paramTypes, paramValues) triple libpq's
    extended-protocol functions expect from a tuple of Python parameter values.
    Values are passed in text format; ``None`` becomes a SQL NULL."""
    if not params:
        return 0, None, None
    values = (ctypes.c_char_p * len(params))()
    types = (ctypes.c_uint * len(params))()
    for i, p in enumerate(params):
        values[i] = None if p is None else str(p).encode()
        types[i] = _param_oid(p)
    return len(params), types, values
