#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Idempotent Uninstaller for AVerMedia Live Gamer Extreme (GC550) Kernel Driver
#
# Usage:
#   ./uninstall.sh           # Safe uninstallation of GC550 DKMS module and udev rule
#   ./uninstall.sh --help    # Show help message

set -euo pipefail

PKG_NAME="gc550"
MOD_NAME="gc550"
UDEV_RULE="99-avermedia-gc550.rules"
UDEV_DEST="/etc/udev/rules.d/${UDEV_RULE}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Dynamically derive project version from kernel/gc550.h
if [[ -f "${SCRIPT_DIR}/kernel/gc550.h" ]]; then
	PKG_VER="$(grep -E '#define[[:space:]]+GC550_DRIVER_VERSION' "${SCRIPT_DIR}/kernel/gc550.h" | awk -F'"' '{print $2}')"
else
	PKG_VER="0.1.3"
fi

DKMS_SRC_DIR="/usr/src/${PKG_NAME}-${PKG_VER}"
TARGET_KVER="${KVER:-$(uname -r)}"

info()  { printf "\033[1;34m[INFO]\033[0m %s\n" "$*"; }
ok()    { printf "\033[1;32m[ OK ]\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33m[WARN]\033[0m %s\n" "$*"; }
error() { printf "\033[1;31m[ERR ]\033[0m %s\n" "$*" >&2; exit 1; }

require_root() {
	if [[ "${EUID}" -ne 0 ]]; then
		info "Elevating privileges to root with sudo..."
		exec sudo "$0" "$@"
	fi
}

main() {
	if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
		echo "AVerMedia Live Gamer Extreme (GC550) DKMS Uninstaller"
		echo ""
		echo "Usage: $0"
		echo "Removes the GC550 DKMS module and udev rules without touching unrelated packages."
		exit 0
	fi

	require_root "$@"

	info "=== Uninstalling ${PKG_NAME}/${PKG_VER} ==="

	# 1. Check if module is currently loaded
	if lsmod | grep "^${MOD_NAME}[[:space:]]" >/dev/null; then
		warn "Module '${MOD_NAME}' is currently LOADED in kernel memory."
		warn "Per safety rules, running modules will NOT be forcefully unloaded."
		warn "If video/audio capture is active, please close applications first."
		warn "You may unload it manually when idle: sudo modprobe -r ${MOD_NAME}"
	else
		ok "Module '${MOD_NAME}' is not loaded."
	fi

	# 2. Remove DKMS registration and built binaries for gc550
	if command -v dkms >/dev/null 2>&1; then
		if dkms status -m "${PKG_NAME}" -v "${PKG_VER}" 2>/dev/null | grep "${PKG_NAME}" >/dev/null; then
			info "Removing DKMS module ${PKG_NAME}/${PKG_VER}..."
			dkms remove -m "${PKG_NAME}" -v "${PKG_VER}" --all || true
			ok "DKMS package ${PKG_NAME}/${PKG_VER} removed."
		else
			info "DKMS package ${PKG_NAME}/${PKG_VER} is not currently registered."
		fi
	fi

	# 3. Remove DKMS source directory (strictly only /usr/src/gc550-<version>)
	if [[ -d "${DKMS_SRC_DIR}" && "${DKMS_SRC_DIR}" == /usr/src/gc550-* ]]; then
		info "Removing source tree: ${DKMS_SRC_DIR}..."
		rm -rf "${DKMS_SRC_DIR}"
		ok "Source tree ${DKMS_SRC_DIR} removed."
	fi

	# 4. Remove udev rule
	if [[ -f "${UDEV_DEST}" ]]; then
		info "Removing udev rule: ${UDEV_DEST}..."
		rm -f "${UDEV_DEST}"
		udevadm control --reload-rules && udevadm trigger || true
		ok "Udev rule removed."
	fi

	# 5. Run depmod to update module indices
	info "Updating kernel module dependencies (depmod)..."
	depmod -a "${TARGET_KVER}"
	ok "depmod completed."

	echo ""
	ok "GC550 uninstallation complete."
	info "Unrelated kernel modules (adv7604, adv7511, media controller) were preserved."
}

main "$@"
