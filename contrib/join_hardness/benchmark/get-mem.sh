#!/usr/bin/env bash

tail -n 100 pg.log | grep 'max resident size' | tail -n 1 | awk '{print $2}'
