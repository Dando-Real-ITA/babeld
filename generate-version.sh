#!/bin/sh

set -e

if [ -d .git ] && [ "$(git tag)" ] ; then
    version="$(git describe --tags --abbrev=0)"
elif [ -f version ] ; then
    version="$(cat version)"
else
    version="unknown"
fi

echo "#define BABELD_VERSION \"$version\""
