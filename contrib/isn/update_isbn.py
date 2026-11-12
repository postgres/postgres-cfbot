#!/usr/bin/env python3

# Update ISBN.h from primary source.

import re
from urllib.request import Request, urlopen
import xml.etree.ElementTree as ET

SOURCE_URL = "https://www.isbn-international.org/export_rangemessage.xml"
GS1_PREFIXES = ("978", "979")

def generate_table(root, gs1_prefix):
  source = root.find("MessageSource").text
  date = root.find("MessageDate").text
  first_digit_index = [-1] * 10
  first_digit_count = [0] * 10
  table_lines = ["\t/* Source: %s */" % source, "\t/* Fetched: %s */" % date]
  index = 0
  for registration_group in root.iter("RegistrationGroups"):
    for group in registration_group.iter("Group"):

      # Example prefix: 978-631, first part is GS1, second part identifies one
      # of several prefixes that arecontrolled by Argentina.
      prefix = group.find("Prefix").text
      prefix_split = prefix.split("-")

      # Filter out GS1 prefixes we're not interested in.
      if prefix_split[0] != gs1_prefix:
        continue

      # Include a comment showing which language or country each prefix
      # corresponds to.
      agency = group.find("Agency").text
      table_lines.append("")
      table_lines.append("\t/* %s */" % agency)

      # Length is where the hyphen goes between publisher number and the serial
      # number assigned by the publisher.
      for rule in group.iter("Rule"):
        range_str = rule.find("Range").text
        length = int(rule.find("Length").text)

        # Our table's way of representing ranges (ie truncating the rest of the
        # digits and assuming they were /0*/ and /9*/) doesn't work for 0 but
        # that seems to be OK in practice: we add no hyphen if we can't find
        # the range in the table, so just skip this entry.
        if length == 0:
          continue

        # Maintain index of the range of table indexes for each first digit.
        first_digit = int(prefix_split[1][0])
        if first_digit_index[first_digit] == -1:
          first_digit_index[first_digit] = index
          first_digit_count[first_digit] = 1
        elif first_digit_index[first_digit] + first_digit_count[first_digit] != index:
          raise RuntimeError("input out of order: first digit %d, which began at index %d, my index %d, first_digit_index = %s, first_digit_count = %s" % (first_digit, first_digit_index[first_digit], index, first_digit_index, first_digit_count))
        else:
          first_digit_count[first_digit] += 1
        index += 1

        # Chop the ranges off at the point where the hyphen appears.
        range_split = range_str.split("-")
        table_lines.append("""\t{"%s", "%s"},""" %
                           (prefix_split[1] + "-" + range_split[0][0:length],
                            prefix_split[1] + "-" + range_split[1][0:length]))

  index_lines = []
  for i in range(10):
    if first_digit_index[i] == -1:
      index_lines.append("\t{0, 0},")
    else:
      index_lines.append("\t{%d, %d}," % (first_digit_index[i], first_digit_count[i]))

  return table_lines, index_lines

def fetch_and_parse_source_data():
   with urlopen(SOURCE_URL) as response:
        data = response.read()
        return ET.fromstring(data)

def merge_isbn(input_lines):
  root = fetch_and_parse_source_data()
  indexes = {}
  tables = {}
  for gs1_prefix in GS1_PREFIXES:
    tables[gs1_prefix], indexes[gs1_prefix] = generate_table(root, gs1_prefix)

  echo = True
  for line in input_lines:
    if echo:
        yield line
        if groups := re.search(r"@BEGIN (index|table):([0-9]+).*@", line):
          echo = False
          if groups.group(1) == "index":
            yield from indexes[groups.group(2)]
          elif groups.group(1) == "table":
            yield from tables[groups.group(2)]
    elif re.search(r"@END.*@", line):
      yield line
      echo = True

if __name__ == "__main__":
  input_lines = []
  with open("ISBN.h", "r") as f:
    for line in f.readlines():
      input_lines.append(line.rstrip())
  output_lines = merge_isbn(input_lines)
  with open("ISBN.h", "w") as f:
    for line in output_lines:
      f.write(line + "\n")
