#!/bin/sh
# Fetch Nuvoton's MS51 BSP (Apache-2.0) for the register headers.
set -e
cd "$(dirname "$0")"
mkdir -p bsp
if [ ! -d bsp/MS51_BSP ]; then
  git clone --depth 1 https://github.com/OpenNuvoton/MS51_BSP bsp/MS51_BSP
fi
echo "BSP ready in bsp/MS51_BSP"
