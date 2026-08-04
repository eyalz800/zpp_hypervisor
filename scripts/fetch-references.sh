#!/bin/sh
# Fetches the two references this project checks its architectural claims
# against, into .references/ (git ignored).
#
# Why this exists: recalled knowledge of the SDM is a good way to form a
# hypothesis and a bad way to settle one. Several claims that sounded right
# have been wrong here - that CD/NW are set on INIT, that a guest can be
# left runnable by writing the activity state alone - and each cost a
# debugging cycle. Both references are cheap to keep locally, so checking
# is cheap.
#
#   .references/sdm.txt   Intel SDM, all four volumes, as searchable text.
#   .references/kvm/      KVM's x86 sources, the reference implementation.
#
# Usage:
#   ./scripts/fetch-references.sh          # fetch what is missing
#   ./scripts/fetch-references.sh --force  # re-fetch everything
set -e

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.references"
force=""
[ "$1" = "--force" ] && force=1

mkdir -p "$out"

# The kernel tag to take KVM from. Pinned rather than tracking master, so a
# citation in a code comment keeps meaning what it said. Bump deliberately.
kvm_tag="v6.12"

# --- Intel SDM ------------------------------------------------------------
# The combined volume set. Intel serves it from a content endpoint by id;
# the human readable URLs under intel.com/content/dam answer 403 to curl.
sdm_url="https://cdrdv2.intel.com/v1/dl/getContent/671200"

if [ -n "$force" ] || [ ! -f "$out/sdm.txt" ]; then
    if [ -n "$force" ] || [ ! -f "$out/sdm.pdf" ]; then
        echo "fetching Intel SDM (about 26 MB)"
        curl -sSL --max-time 600 -o "$out/sdm.pdf" "$sdm_url"

        # A 403 or an error page is small; a real download is tens of MB.
        size=$(wc -c < "$out/sdm.pdf" | tr -d ' ')
        if [ "$size" -lt 5000000 ]; then
            echo "download failed - got $size bytes, not a PDF" >&2
            rm -f "$out/sdm.pdf"
            exit 1
        fi
    fi

    echo "extracting text (a few minutes, about 5000 pages)"
    python3 - "$out" <<'PY'
import sys
from pypdf import PdfReader

out = sys.argv[1]
reader = PdfReader(out + "/sdm.pdf")
total = len(reader.pages)

# Page markers, so a search result can be cited as a page number rather
# than a byte offset.
with open(out + "/sdm.txt", "w") as handle:
    for index in range(total):
        try:
            text = reader.pages[index].extract_text() or ""
        except Exception:
            text = ""
        handle.write("\n\f[[PAGE %d]]\n" % (index + 1))
        handle.write(text)
print("extracted %d pages" % total)
PY
else
    echo "sdm.txt present, skipping (use --force to re-fetch)"
fi

# --- KVM ------------------------------------------------------------------
mkdir -p "$out/kvm"
for file in \
    arch/x86/kvm/vmx/vmx.c \
    arch/x86/kvm/vmx/nested.c \
    arch/x86/kvm/vmx/vmx.h \
    arch/x86/kvm/lapic.c \
    arch/x86/kvm/x86.c \
    arch/x86/include/asm/vmx.h
do
    name=$(basename "$file")
    # vmx.c exists twice under different directories, so keep the
    # distinguishing parent in the name.
    case "$file" in
        */include/asm/vmx.h) name="asm-vmx.h" ;;
        */vmx/vmx.h) name="vmx-internal.h" ;;
    esac

    if [ -z "$force" ] && [ -f "$out/kvm/$name" ]; then
        continue
    fi

    echo "fetching $name from $kvm_tag"
    curl -sSL --max-time 120 -o "$out/kvm/$name" \
        "https://raw.githubusercontent.com/torvalds/linux/$kvm_tag/$file"
done

echo "$kvm_tag" > "$out/kvm/VERSION"
echo
echo "references ready in $out"
echo "  grep -n 'INIT' .references/sdm.txt | head"
echo "  grep -n 'kvm_vcpu_reset' .references/kvm/x86.c"
