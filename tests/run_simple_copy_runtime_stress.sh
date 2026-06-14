#!/usr/bin/env bash
set -euo pipefail

if [ "${ALLOW_NVMEV_RUNTIME_STRESS:-0}" != "1" ]; then
	echo "refusing: set ALLOW_NVMEV_RUNTIME_STRESS=1 to run runtime stress" >&2
	exit 2
fi

if ! grep -q 'memmap=' /proc/cmdline; then
	echo "refusing: current boot has no memmap= reservation" >&2
	exit 2
fi

if [ "$#" -ne 2 ]; then
	echo "usage: $0 /dev/nvmeXnY nsid" >&2
	echo "refusing: runtime stress requires an explicit NVMeVirt namespace" >&2
	exit 2
fi

dev="$1"
nsid="$2"
repo="$(cd "$(dirname "$0")/.." && pwd)"
out="${OUT:-$repo/tests/runtime_stress_current}"
devbase="$(basename "$dev")"
model_path="/sys/class/block/$devbase/device/model"

if [ ! -b "$dev" ]; then
	echo "refusing: $dev is not a block device" >&2
	exit 2
fi

if [ ! -r "$model_path" ] || ! grep -q 'CSL_Virt_MN_01' "$model_path"; then
	echo "refusing: $dev does not look like an NVMeVirt namespace" >&2
	exit 2
fi

if [ "$(sudo -n blockdev --getsz "$dev")" -lt 512 ]; then
	echo "refusing: $dev is too small for the runtime stress LBA map" >&2
	exit 2
fi

mkdir -p "$out"
gcc -O2 -Wall -Wextra -pthread \
	"$repo/tests/simple_copy_runtime_stress.c" \
	-o "$out/simple_copy_runtime_stress"

sudo -n timeout 60s "$out/simple_copy_runtime_stress" "$dev" "$nsid" |
	tee "$out/runtime_stress.csv"

if [ "${ALLOW_NVMEV_UNLOAD_STRESS:-0}" = "1" ]; then
	sudo timeout 20s rmmod nvmev
fi
