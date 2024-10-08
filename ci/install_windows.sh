#!/usr/bin/env bash

set -x
set -e

# Temporary workaround pending: https://github.com/microsoft/vcpkg-tool/pull/1501
export SystemDrive="$SYSTEMDRIVE"
export SystemRoot="$SYSTEMROOT"
export windir="$WINDIR"

# Required dependencies
VCPKG_INSTALL_CMD="vcpkg install
    zlib
    libpng
    openexr
    tbb
    gtest
    cppunit
    blosc
    glfw3
    glew
    python3
    jemalloc
    boost-iostreams
    boost-interprocess
    boost-algorithm
    pybind11
    --clean-after-build"

# Update vcpkg
vcpkg update

# Allow the vcpkg command to fail once so we can retry with the latest
set +e
$VCPKG_INSTALL_CMD
STATUS=$?

# Subsequent commands cannot fail
set -e

if [ $STATUS -ne 0 ]; then
  # Try once more with latest ports
  echo "vcpkg install failed, retrying with latest ports..."
  cd $VCPKG_INSTALLATION_ROOT && git pull && cd -
  vcpkg update
  $VCPKG_INSTALL_CMD
fi

echo "vcpkg install completed successfully"
