#!/bin/sh

set -e

if [ -d .git ] && [ "$(git tag)" ] ; then
    version="$(git describe --tags --abbrev=0)"
elif [ -f version ] ; then
    version="$(cat version)"
else
    version="unknown"
fi

NEW_CONTENT="#define BABELD_VERSION \"$version\""

if [ ! -f version.h ] || [ "$(cat version.h)" != "$NEW_CONTENT" ]; then
    echo "$NEW_CONTENT" > version.h
fi
