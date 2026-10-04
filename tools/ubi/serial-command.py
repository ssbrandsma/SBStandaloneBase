#!/usr/bin/env python3
"""Run one bounded Linux-shell command on the Radio's serial console."""
import argparse
import re
import serial
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("command")
parser.add_argument("--port", default="COM5")
parser.add_argument("--timeout", type=float, default=20.0)
parser.add_argument("--settle", type=float, default=1.0,
                    help="seconds to wait for a newly opened console shell")
args = parser.parse_args()

marker = "__SB_SERIAL_DONE__"
wire = "{}; rc=$?; echo {}$rc\r".format(args.command, marker)
deadline = time.monotonic() + args.timeout
data = bytearray()

with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=1) as console:
    console.reset_input_buffer()
    # Opening the USB serial device may start a fresh getty shell.  Wake it and
    # allow its prompt to arrive before sending the bounded command, otherwise
    # the first command can be consumed while the shell is still starting.
    console.write(b"\r")
    console.flush()
    if args.settle > 0:
        time.sleep(args.settle)
    console.reset_input_buffer()
    console.write(wire.encode("ascii"))
    console.flush()
    while time.monotonic() < deadline:
        chunk = console.read(4096)
        if chunk:
            data.extend(chunk)
            # The echoed command ends in "$rc"; only executed output has digits.
            if re.search(marker.encode() + rb"[0-9]+", data):
                break

text = data.decode("utf-8", "replace")
sys.stdout.write(text)
if not re.search(re.escape(marker) + r"[0-9]+", text):
    raise SystemExit("serial command timed out before completion marker")
