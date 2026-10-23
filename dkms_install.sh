#!/bin/bash

# DKMS installation script for snd-usb-motu
# Builds for all installed kernels individually
# Continues even if some kernels fail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== snd-usb-motu DKMS Installation Script ===${NC}"
echo ""

# Check if running as root or with sudo
if [ "$EUID" -ne 0 ] && [ "$(whoami)" != "root" ]; then
    SUDO="sudo"
else
    SUDO=""
fi

echo -e "${YELLOW}Checking prerequisites...${NC}"

# Check if DKMS is installed
if ! command -v dkms &> /dev/null; then
    echo -e "${RED}DKMS is not installed.${NC}"
    echo "Please install it first:"
    echo "  sudo apt update && sudo apt install dkms"
    exit 1
fi
echo -e "${GREEN}✓ DKMS is installed${NC}"

# Find all installed kernels with build headers
KERNEL_DIRS=$(ls -d /lib/modules/*/build 2>/dev/null | sort)

if [ -z "$KERNEL_DIRS" ]; then
    echo -e "${RED}No kernels with build headers found!${NC}"
    echo "Install kernels with headers first:"
    echo "  sudo mhwd-kernel -i linux66"
    echo "  sudo mhwd-kernel -i linux71"
    echo "  sudo mhwd-kernel -i linux73"
    exit 1
fi

KERNEL_COUNT=$(echo "$KERNEL_DIRS" | wc -l)
echo -e "${GREEN}✓ Found $KERNEL_COUNT kernel(s) with build headers:${NC}"
echo "$KERNEL_DIRS" | while read -r dir; do
    echo "  - $(dirname "$dir" | xargs basename)"
done
echo ""

# Remove existing module if loaded
$SUDO rmmod snd-usb-motu 2>/dev/null || true
$SUDO rmmod snd-usb-audio 2>/dev/null || true
$SUDO rmmod motu 2>/dev/null || true

echo -e "${YELLOW}Adding source to DKMS...${NC}"
$SUDO dkms add . 2>/dev/null || echo "Source already added"

echo ""
echo -e "${YELLOW}Building for ALL installed kernels...${NC}"
echo "(Each kernel is built individually, continues on failure)"
echo ""

SUCCESS=0
FAILED=0

for kernel_dir in $KERNEL_DIRS; do
    KERNEL_VERSION=$(basename "$(dirname "$kernel_dir")")
    
    # Skip if already installed
    if dkms status -m snd-usb-motu 2>/dev/null | grep -q "$KERNEL_VERSION.*: installed"; then
        echo -e "${GREEN}✓ $KERNEL_VERSION${NC} - already installed"
        ((SUCCESS++))
        continue
    fi
    
    echo -e "${YELLOW}Processing: $KERNEL_VERSION${NC}"
    
    if $SUDO dkms install -m snd-usb-motu -v 1.0 -k "$KERNEL_VERSION" 2>/dev/null; then
        echo -e "${GREEN}✓ $KERNEL_VERSION${NC} - installed"
        ((SUCCESS++))
    else
        echo -e "${RED}✗ $KERNEL_VERSION${NC} - failed (continuing...)"
        ((FAILED++))
    fi
done

echo ""
echo -e "${GREEN}=== Installation Complete ===${NC}"
echo "Success: $SUCCESS | Failed: $FAILED"

if [ $FAILED -gt 0 ]; then
    echo -e "${YELLOW}Warning: Some kernels failed to build. Check /var/lib/dkms/snd-usb-motu/ for details.${NC}"
fi

echo ""
echo "The module is now installed as a DKMS module."
echo "It will be automatically rebuilt when you install a new kernel."
echo ""
echo "To load the module, run:"
echo "  sudo modprobe snd-usb-motu"
echo ""
echo "To verify installation:"
echo "  ./dkms_verify.sh"
echo ""
