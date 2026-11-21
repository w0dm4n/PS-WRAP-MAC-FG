#!/bin/bash
# PS-WRAP-MAC-FG: build libplacebo v7.360.1 (the version the official PS-WRAP-MAC 0.14.0 bundles) into _deps/libplacebo-7.360
# needs: brew shaderc little-cms2 vulkan-headers vulkan-loader ninja · python with meson + jinja2 on PATH (e.g. .venv)
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
[ -d _deps/libplacebo-src ] || git clone -q https://github.com/haasn/libplacebo.git _deps/libplacebo-src
cd _deps/libplacebo-src
git checkout -q v7.360.1 && git submodule update --init -q
rm -rf build
PKG_CONFIG_PATH=$(brew --prefix)/lib/pkgconfig meson setup build --prefix="$ROOT/_deps/libplacebo-7.360" --buildtype=release \
	-Ddemos=false -Dtests=false -Dvulkan=enabled -Dshaderc=enabled -Dglslang=disabled -Dlcms=enabled -Dopengl=enabled -Dd3d11=disabled \
	-Dvk-proc-addr=enabled -Dvulkan-registry="$(brew --prefix)/opt/vulkan-headers/share/vulkan/registry/vk.xml"
ninja -C build install
