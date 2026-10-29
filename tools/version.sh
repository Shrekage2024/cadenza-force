#!/bin/sh
# Prints a version from versions.json: tools/version.sh force (the plugin, as packaged for the Force)
exec python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$(dirname "$0")/../versions.json" "${1:?usage: tools/version.sh force}"
