#!/usr/bin/env bash
# Hydraw DAW — install system dependencies (Debian/Ubuntu)
set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
    libgtk-3-dev \
    libwebkit2gtk-4.1-dev \
    libasound2-dev
