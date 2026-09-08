#!/usr/bin/env bash

# print ccp + timing
tail -n 100 pg.log | grep 'standard_join_search' | tail -n 1 | sed 's/.* count //' | awk '{print $1}'
