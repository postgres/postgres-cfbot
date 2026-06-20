#!/usr/bin/env bash

tail -n 100 pg.log | grep ' elapsed' | tail -n 1 | awk '{print $2 " " $5}'
