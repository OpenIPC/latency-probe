#!/bin/sh
# Copy the probe to a camera, run it, fetch the result and score it.
#   ./run.sh CAMERA TAG [latprobe options...]
# e.g. ./run.sh root@192.168.1.10 lowdelay -d 45
# Needs ssh access to the camera and the latency-probe docker image (make image).
set -e
cam=$1; tag=$2; shift 2
mkdir -p results
ssh "$cam" 'cat > /tmp/latprobe && chmod +x /tmp/latprobe' < probe/latprobe
ssh "$cam" "/tmp/latprobe -o /tmp/latprobe.es $*" > "results/$tag.txt"
ssh "$cam" 'cat /tmp/latprobe.es; rm -f /tmp/latprobe.es /tmp/latprobe' > "results/$tag.es"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/w" latency-probe \
	python host/analyze.py "results/$tag.txt" "results/$tag.es" --json "results/$tag.json"
