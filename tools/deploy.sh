#!/usr/bin/env bash
# Upload dist/<TITLE_ID>/ to the console.
#
#   PS5_VULKAN=... ./tools/deploy.sh [--all]
#
# PS5_Vulkan's deploy tool uploads what changed (eboot.bin always) over the
# console's FTP server, with the address from PS5_Vulkan's .env.
#
# PPSSPP's asset tree is nearly two hundred files that never change between
# builds, so the default incremental upload is what to use; --all forces the lot.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
: "${PS5_VULKAN:?set PS5_VULKAN to a built PS5_Vulkan checkout}"

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
	"$root/sce_sys/param.json")
[[ -d $root/dist/$title_id ]] ||
	{ echo "no dist/$title_id: run tools/build.sh then tools/link-title.sh first" >&2; exit 2; }

exec python3 "$PS5_VULKAN/tools/deploy-title-folder.py" \
	--always eboot.bin "$root/dist/$title_id" "$@"
