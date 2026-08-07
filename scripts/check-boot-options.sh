#!/bin/sh
# Report the guest's NVRAM boot options, and refuse if the entry we intend
# to boot is not among them.
#
# Why this exists: a boot that lands in the UEFI shell looks identical to a
# loader that ran and failed, and costs a reboot to tell the two apart.
# Twice the firmware went to the shell not because anything failed but
# because there was no boot option to try - BdsDxe did not even log an
# attempt. The variable store says so before the machine is started, so
# read it first rather than finding out from a screen.
VARS=${ZPP_VARS:-/home/tc/vm/RELEASEX64_OVMF_VARS.fd}
WANT=${ZPP_BOOT_ENTRY:-zpp}

if [ ! -f "$VARS" ]; then
    echo "FAIL: no variable store at $VARS"
    exit 1
fi

SIZE=$(wc -c < "$VARS")
echo "variable store: $VARS ($SIZE bytes)"

# Descriptions are UTF-16LE. Dropping the nulls makes them greppable
# without a real variable-store parser, which busybox has no tool for.
TEXT=$(tr -d '\000' < "$VARS" | strings -n 3 2>/dev/null)

echo "--- boot entries present ---"
echo "$TEXT" | grep -E '^Boot[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]$' \
    | sort -u | while read -r entry; do
    desc=$(echo "$TEXT" | grep -A1 "^$entry\$" | tail -1)
    case "$desc" in
        Boot*|"") desc="(no description)" ;;
    esac
    echo "  $entry  $desc"
done

echo "--- required entry: something matching '$WANT' ---"
if echo "$TEXT" | grep -qi "$WANT"; then
    echo "OK: a boot option mentioning '$WANT' is present"
    echo
    echo "NOTE: present is not the same as will be tried. BootOrder is not"
    echo "      a complete list of the options that exist, and this machine"
    echo "      has already demonstrated it: the real NVMe is sitting in"
    echo "      NVRAM as Boot0001 and BdsDxe went to the shell without"
    echo "      logging a single attempt at it. Adding with 'bcfg boot"
    echo "      add 0' puts the entry at the front of BootOrder as well as"
    echo "      creating it, which is why the position argument matters."
    exit 0
fi

echo "FAIL: no boot option mentions '$WANT'."
echo "      The firmware will fall straight through to the EFI shell,"
echo "      without logging a failed attempt. Add one from the shell:"
echo '        bcfg boot add 0 FS0:\EFI\zpp\zpp_loader.efi "zpp loader"'
exit 1
