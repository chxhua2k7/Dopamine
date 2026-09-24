#!/bin/sh
# Builds and runs the Linux test harness for BaseBin/launchdhook/src/bootlog.c.
# Run from the Dopamine repo root (after applying the verbose boot patch):
#   sh harness/run.sh
# Needs clang + libblocksruntime-dev (apt-get install libblocksruntime-dev).
set -e
SRC=BaseBin/launchdhook/src
HERE=$(dirname "$0")
mkdir -p /var/mobile/Library/Logs/Dopamine /var/jb/basebin/LaunchDaemons
touch /var/jb/basebin/LaunchDaemons/com.opa334.Dopamine.bootlog.plist   # "daemon installed" case; rm it to test the fallback
clang -std=gnu11 -fblocks -Wall -Wextra -Wno-unused-parameter -I"$HERE/stubs" -I"$SRC" \
  -o "$HERE/test_driver" "$HERE/test_driver.c" "$SRC/bootlog.c" -lBlocksRuntime -lpthread
# args: rotation width height spawnCount ; FOREIGN=1 simulates backboardd presenting a frame
"$HERE/test_driver" 0 1179 2556 40
FOREIGN=1 "$HERE/test_driver" 0 1179 2556 40
"$HERE/test_driver" 90 2048 1536 20
tail -3 /var/mobile/Library/Logs/Dopamine/bootlog.txt
echo "framebuffer dumps: out_<rotation>.ppm (convert with: python3 -c 'from PIL import Image; Image.open(\"out_0.ppm\").save(\"out_0.png\")')"
