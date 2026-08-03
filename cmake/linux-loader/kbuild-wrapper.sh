#!/usr/bin/env bash
# Runs kbuild inside a Podman container to produce zpp_loader.ko.
#
# Usage: kbuild-wrapper.sh <source_dir> <output_dir> <config>
#   source_dir  — repository root (ZPP_SOURCE_DIR)
#   output_dir  — output directory containing zpp_loader.o (ZPP_OUTPUT_DIR)
#   config      — build config (debug/release) — informational only
#
# Prerequisites:
#   - Podman installed and available on PATH.
#   - zpp_loader.o already built (run: cmake --build --preset <config> --target linux_loader).
#
# The container uses Alpine linux-headers which may differ from the host kernel.
# The resulting .ko is suitable for a kernel version matching the Alpine package.

set -euo pipefail

SOURCE_DIR="${1:?Usage: $0 <source_dir> <output_dir> <config>}"
OUTPUT_DIR="${2:?Usage: $0 <source_dir> <output_dir> <config>}"
CONFIG="${3:-debug}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

IMAGE_NAME="zpp-kbuild:latest"

if [[ ! -f "${OUTPUT_DIR}/zpp_loader.o" ]]; then
    echo "ERROR: ${OUTPUT_DIR}/zpp_loader.o not found." >&2
    echo "       Build the linux loader first: cmake --build --preset ${CONFIG} --target linux_loader" >&2
    exit 1
fi

# Build container image if not already present.
if ! podman image exists "${IMAGE_NAME}" 2>/dev/null; then
    echo "Building kbuild container image (first time only)..."
    podman build -t "${IMAGE_NAME}" -f "${SCRIPT_DIR}/Containerfile" "${SCRIPT_DIR}"
fi

# Determine the kernel version supplied by the Alpine linux-headers package.
KERNEL_VERSION=$(podman run --rm "${IMAGE_NAME}" \
    sh -c 'ls /lib/modules/ 2>/dev/null | head -1 || echo "unknown"')

if [[ "${KERNEL_VERSION}" == "unknown" ]]; then
    # Fall back: infer from linux-headers package name.
    KERNEL_VERSION=$(podman run --rm "${IMAGE_NAME}" \
        sh -c 'apk info linux-headers 2>/dev/null | grep -oP "[0-9]+\.[0-9]+\.[0-9]+" | head -1 || echo "6.6.0"')
fi

echo "Building zpp_loader.ko for kernel ${KERNEL_VERSION}..."

podman run --rm \
    -v "${SOURCE_DIR}:/src:ro" \
    -v "${OUTPUT_DIR}:/output:rw" \
    "${IMAGE_NAME}" \
    sh -c "
set -e
KERNEL_VERSION=\$(ls /lib/modules/ 2>/dev/null | head -1)
if [ -z \"\${KERNEL_VERSION}\" ]; then
    echo 'ERROR: No kernel modules directory found in container.' >&2
    exit 1
fi

mkdir -p /build/linux_loader
cp /src/linux_loader/src/main.c /build/linux_loader/main.c
cp /output/zpp_loader.o /build/linux_loader/zpp_loader_prebuilt.o

# main.c includes zpp/loader.h for the zpp_load_elf parameter struct, so the
# shared loader headers have to come along and be on the include path.
mkdir -p /build/linux_loader/include
cp -r /src/loader/include/. /build/linux_loader/include/

cat > /build/linux_loader/Makefile << 'MKEOF'
obj-m += zpp_module.o
zpp_module-objs := main.o zpp_loader_prebuilt.o
ccflags-y += -I\$(src)/include
MKEOF

make -C /lib/modules/\${KERNEL_VERSION}/build M=/build/linux_loader modules

cp /build/linux_loader/zpp_module.ko /output/zpp_loader.ko
echo 'Done: /output/zpp_loader.ko'
"

echo "Built: ${OUTPUT_DIR}/zpp_loader.ko"
