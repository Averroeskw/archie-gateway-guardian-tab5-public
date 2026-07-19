#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

if find firmware/main -maxdepth 1 -name '*.local.h' -print -quit | grep -q .; then
    echo "Refusing to package while a local firmware header exists; build from a clean checkout." >&2
    exit 1
fi

merged="${1:-firmware/build/archie-gateway-guardian-tab5-merged.bin}"
version="${2:-dev}"
if [[ ! -f "$merged" ]]; then
    echo "Merged image not found: $merged" >&2
    echo "Build a clean release image first, then rerun this script." >&2
    exit 1
fi

rm -rf site
mkdir -p site
cp web/index.html site/index.html
cp assets/archie_guardian_reference.png site/archie_guardian_reference.png
cp "$merged" site/archie-gateway-guardian-tab5-merged.bin
sed "s/__VERSION__/${version#v}/" web/manifest.template.json > site/manifest.json
sha256sum site/archie-gateway-guardian-tab5-merged.bin > site/SHA256SUMS
echo "Prepared site/ without credentials. Serve it with: python3 -m http.server --directory site 8000"
