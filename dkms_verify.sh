#!/bin/bash

# DKMS verification script for snd-usb-motu
# Shows which kernels have the module built and installed

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== snd-usb-motu DKMS Status ===${NC}"
echo ""

# Get DKMS status for our module
DKMS_OUTPUT=$(dkms status -m snd-usb-motu 2>/dev/null || true)

if [ -z "$DKMS_OUTPUT" ]; then
    echo -e "${YELLOW}Module not found in DKMS.${NC}"
    echo "Install it first: sudo ./dkms_install.sh"
    exit 0
fi

# Count totals
TOTAL=$(echo "$DKMS_OUTPUT" | grep -c "x86_64.*: installed" || true)
echo -e "${YELLOW}Total kernels with module installed: $TOTAL${NC}"
echo ""

# Show each kernel
echo "$DKMS_OUTPUT" | while IFS= read -r line; do
    if echo "$line" | grep -q ": installed"; then
        echo -e "${GREEN}✓ $(echo "$line" | sed 's/, x86_64.*installed//')${NC}"
    fi
done

echo ""
echo "To build for missing kernels:"
echo "  sudo ./dkms_install.sh"
echo "  or"
echo "  make dkms_install"
echo ""
