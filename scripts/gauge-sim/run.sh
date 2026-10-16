#!/bin/sh
# Re-cut the gauge math from the driver and run the host simulation.
set -e
cd "$(dirname "$0")"
D=../../device/linux-lenovo-tabe7/mt6392_charger.c
{ sed -n '/^ \* Battery model from the vendor profile/,/^#define MT6392_FULL_CURRENT_MA/p' $D | sed '1s/^/\/*/'
  sed -n '/^\/\* Charge left (uAh) for an open-circuit voltage/,/^static bool mt6392_charging/p' $D | sed '$d'; } > model.inc
cc -O1 -Wall -o sim sim.c && ./sim
