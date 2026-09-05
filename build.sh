#!/bin/sh
set -e

clang -Wall -o waycpy waycpy.c wlr-screencopy-client-protocol.c -lwayland-client