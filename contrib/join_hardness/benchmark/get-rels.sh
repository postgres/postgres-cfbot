#!/usr/bin/env bash

p=$1

tail -n 100 pg.log | grep $p | tail -n 1 | sed 's/.*rels //' | awk '{print $1}'
