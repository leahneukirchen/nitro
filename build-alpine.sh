#!/bin/sh -e
rm -rf bin.alpine
docker build -f Dockerfile.alpine --progress=plain --iidfile=id.alpine .
docker save $(cat id.alpine) | bsdtar xO '*/layer.tar' | bsdtar xv --ignore-zeros bin.alpine/nitro bin.alpine/nitroctl
