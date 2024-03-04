#!/bin/sh -e
rm -rf bin.alpine
mkdir bin.alpine
podman build -f Containerfile.alpine -v $PWD/bin.alpine:/out:Z
