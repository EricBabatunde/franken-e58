#!/usr/bin/env bash
# Confirms every CONFIG_ line in the defaults file(s) survived into the generated sdkconfig.
# Unknown option names are silently dropped by Kconfig, so this check is mandatory.
# Usage: tools/verify_sdkconfig.sh [generated_file [defaults_file ...]]
#   default:     sdkconfig.esp32s3 checked against sdkconfig.defaults
#   bench (M7b): tools/verify_sdkconfig.sh sdkconfig.esp32s3_bench sdkconfig.defaults sdkconfig.defaults.bench
set -u
gen="${1:-sdkconfig.esp32s3}"
shift || true
defaults=("$@")
[ ${#defaults[@]} -eq 0 ] && defaults=(sdkconfig.defaults)
[ -f "$gen" ] || { echo "Generated file $gen not found. Run 'pio run' first."; exit 2; }
# Later files override earlier ones, as in the build (SDKCONFIG_DEFAULTS order): only the last
# value of each option is checked.
declare -A val_of src_of
order=()
for d in "${defaults[@]}"; do
  [ -f "$d" ] || { echo "Defaults file $d not found."; exit 2; }
  while IFS= read -r line; do
    key="${line%%=*}"
    [ -z "${val_of[$key]+x}" ] && order+=("$key")
    val_of[$key]="${line#*=}"
    src_of[$key]="$d"
  done < <(grep -E '^CONFIG_' "$d")
done
bad=0
n=0
for key in "${order[@]}"; do
  val="${val_of[$key]}"
  line="$key=$val"
  if [ "$val" = "n" ]; then want="# $key is not set"; else want="$line"; fi
  n=$((n + 1))
  if grep -qxF "$want" "$gen"; then
    echo "OK        $line"
  else
    echo "MISMATCH  $line   (${src_of[$key]})"
    bad=1
  fi
done
if [ $bad -eq 0 ]; then echo "verify_sdkconfig: $gen: $n/$n OK"; else echo "verify_sdkconfig: $gen: MISMATCH"; fi
exit $bad
