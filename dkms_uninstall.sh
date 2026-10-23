#!/bin/bash

# DKMS uninstallation script for snd-usb-motu

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${RED}=== snd-usb-motu DKMS Uninstallation Script ===${NC}"
echo ""

# Check if running as root or with sudo
if [ "$EUID" -ne 0 ] && [ "$(whoami)" != "root" ]; then
    SUDO="sudo"
else
    SUDO=""
fi

echo -e "${YELLOW}Removing DKMS module...${NC}"
$SUDO dkms remove snd-usb-motu/1.0 --all
if [ $? -ne 0 ]; then
    echo -e "${YELLOW}Module may not be installed via DKMS.${NC}"
fi
echo -e "${GREEN}✓ DKMS module removed${NC}"

echo ""
echo -e "${GREEN}=== Uninstallation Complete ===${NC}"
echo ""
echo "The module has been removed from DKMS."
echo "It will no longer be automatically rebuilt for new kernels."
echo ""

