#!/usr/bin/env bash
# Confirms every CONFIG_ line in sdkconfig.defaults survived into the generated sdkconfig.
# Unknown option names are silently dropped by Kconfig, so this check is mandatory.
# Usage: tools/verify_sdkconfig.sh [generated_file]   (default: sdkconfig.esp32s3)
set -u
gen="${1:-sdkconfig.esp32s3}"
[ -f "$gen" ] || { echo "Generated file $gen not found. Run 'pio run' first."; exit 2; }
bad=0
while IFS= read -r line; do
  key="${line%%=*}"
  val="${line#*=}"
  if [ "$val" = "n" ]; then want="# $key is not set"; else want="$line"; fi
  if grep -qxF "$want" "$gen"; then
    echo "OK        $line"
  else
    echo "MISMATCH  $line"
    bad=1
  fi
done < <(grep -E '^CONFIG_' sdkconfig.defaults)
exit $bad
