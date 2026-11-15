#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Idempotent DKMS Installer for AVerMedia Live Gamer Extreme (GC550) Kernel Driver
#
# Supported platforms: CachyOS / Arch Linux (and standard Linux with DKMS)
#
# Usage:
#   ./install.sh             # Full preflight check, DKMS build, and installation
#   ./install.sh --check     # Preflight and dependency audit only (dry-run, no changes)
#   ./install.sh --uninstall # Remove GC550 DKMS package and udev rules
#   ./install.sh --help      # Show this help message

set -euo pipefail

# --- Package Configuration ---
PKG_NAME="gc550"
MOD_NAME="gc550"
UDEV_RULE="99-avermedia-gc550.rules"
UDEV_DEST="/etc/udev/rules.d/${UDEV_RULE}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Dynamically derive project version from kernel/gc550.h
if [[ -f "${SCRIPT_DIR}/kernel/gc550.h" ]]; then
	PKG_VER="$(grep -E '#define[[:space:]]+GC550_DRIVER_VERSION' "${SCRIPT_DIR}/kernel/gc550.h" | awk -F'"' '{print $2}')"
else
	printf "\033[1;31m[ERR ]\033[0m Cannot locate %s/kernel/gc550.h to determine driver version.\n" "${SCRIPT_DIR}" >&2
	exit 1
fi

DKMS_SRC_DIR="/usr/src/${PKG_NAME}-${PKG_VER}"
TARGET_KVER="${KVER:-$(uname -r)}"
KBUILD_DIR="/lib/modules/${TARGET_KVER}/build"

# Strict Source Manifest: ONLY these files may be packaged into DKMS
DKMS_MANIFEST_ROOT=(
	"Makefile"
	"Kbuild"
	"dkms.conf"
)
DKMS_MANIFEST_KERNEL=(
	"gc550-audio.c"
	"gc550-cmd.c"
	"gc550-core.c"
	"gc550-diag.c"
	"gc550-diag.h"
	"gc550.h"
	"gc550-i2c.c"
	"gc550-regtables.h"
	"gc550-video.c"
)

# Output formatting helpers
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

# --- Preflight Checks ---
preflight_check() {
	info "=== Preflight System & Repository Audit ==="
	info "Target kernel: ${TARGET_KVER}"
	info "DKMS package:  ${PKG_NAME}/${PKG_VER}"
	info "Source path:   ${DKMS_SRC_DIR}"

	# 1. Distribution
	if [[ -f /etc/os-release ]]; then
		# shellcheck disable=SC1091
		source /etc/os-release
		info "Operating system: ${PRETTY_NAME:-Linux} (${ID:-unknown})"
	fi

	# 2. Kernel headers
	if [[ ! -d "${KBUILD_DIR}" ]]; then
		error "Kernel headers for ${TARGET_KVER} not found at ${KBUILD_DIR}.
Please install your distribution's matching kernel headers:
  - CachyOS: sudo pacman -S linux-cachyos-headers (or matching package for ${TARGET_KVER})
  - Arch:    sudo pacman -S linux-headers"
	fi
	ok "Kernel headers verified at ${KBUILD_DIR}"

	# 3. DKMS tool
	if ! command -v dkms >/dev/null 2>&1; then
		error "DKMS is not installed. Please install dkms:
  - Arch / CachyOS: sudo pacman -S dkms"
	fi
	ok "DKMS executable verified: $(command -v dkms)"

	# 4. Build toolchain
	if ! command -v make >/dev/null 2>&1; then
		error "make is not installed. Please install base-devel / build-essential."
	fi
	if grep -q "CONFIG_CC_IS_CLANG=y" "${KBUILD_DIR}/.config" 2>/dev/null; then
		if ! command -v clang >/dev/null 2>&1; then
			error "Target kernel was compiled with Clang (CONFIG_CC_IS_CLANG=y) but 'clang' is not installed."
		fi
		ok "Build toolchain verified: make + clang (LLVM=1 detected)"
	else
		if ! command -v gcc >/dev/null 2>&1; then
			error "Target kernel was compiled with GCC but 'gcc' is not installed."
		fi
		ok "Build toolchain verified: make + gcc"
	fi

	# 5. Manifest source files verification
	info "Verifying source file manifest..."
	for f in "${DKMS_MANIFEST_ROOT[@]}"; do
		if [[ ! -f "${SCRIPT_DIR}/${f}" ]]; then
			error "Required source file missing from repository: ${f}"
		fi
	done
	for f in "${DKMS_MANIFEST_KERNEL[@]}"; do
		if [[ ! -f "${SCRIPT_DIR}/kernel/${f}" ]]; then
			error "Required kernel source file missing from repository: kernel/${f}"
		fi
	done
	ok "All 12 required source manifest files are present."

	# 6. Critical Security Rule: Private document exclusion verification
	# Ensure docs/PROJECT-HANDOVER.md can NEVER enter the packaging manifest
	local private_doc="docs/PROJECT-HANDOVER.md"
	for f in "${DKMS_MANIFEST_ROOT[@]}" "${DKMS_MANIFEST_KERNEL[@]}"; do
		if [[ "${f}" == *"${private_doc}"* || "${f}" == *"PROJECT-HANDOVER"* ]]; then
			error "SECURITY VIOLATION: Private document '${private_doc}' matched in source manifest!"
		fi
	done
	if [[ -f "${SCRIPT_DIR}/${private_doc}" ]]; then
		if command -v git >/dev/null 2>&1 && git -C "${SCRIPT_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
			if ! git -C "${SCRIPT_DIR}" check-ignore -q "${private_doc}"; then
				error "SECURITY VIOLATION: Private document '${private_doc}' is NOT ignored by git!"
			fi
		fi
		ok "Private document '${private_doc}' is safely ignored and excluded from packaging."
	fi

	# 7. Subdevice dependencies audit
	info "Checking subdevice runtime dependencies for ${TARGET_KVER}..."
	local adv7604_found=false
	local adv7511_found=false

	if modinfo -k "${TARGET_KVER}" adv7604 >/dev/null 2>&1; then
		adv7604_found=true
	fi
	if modinfo -k "${TARGET_KVER}" adv7511_v4l2 >/dev/null 2>&1; then
		adv7511_found=true
	fi

	if [[ "${adv7604_found}" == "true" && "${adv7511_found}" == "true" ]]; then
		ok "Subdevice dependencies verified in kernel tree: adv7604, adv7511_v4l2 (subdev_mode=1 supported)"
	else
		warn "Subdevice module availability:"
		warn "  adv7604:      $(if [[ "${adv7604_found}" == "true" ]]; then echo "FOUND"; else echo "NOT FOUND"; fi)"
		warn "  adv7511_v4l2: $(if [[ "${adv7511_found}" == "true" ]]; then echo "FOUND"; else echo "NOT FOUND"; fi)"
		warn "Note: If either module is missing, gc550 will operate in standard mode (subdev_mode=0)."
		warn "      subdev_mode=1 cannot be used until in-tree subdevice drivers are available."
	fi

	# 8. Check existing DKMS registrations
	if dkms status -m "${PKG_NAME}" -v "${PKG_VER}" 2>/dev/null | grep "${PKG_NAME}" >/dev/null; then
		info "Found existing DKMS registration for ${PKG_NAME}/${PKG_VER}:"
		dkms status -m "${PKG_NAME}" -v "${PKG_VER}"
	fi
	if dkms status -m "avermedia-gc550" 2>/dev/null | grep "avermedia-gc550" >/dev/null; then
		info "Found legacy DKMS registration: avermedia-gc550"
		dkms status -m "avermedia-gc550"
	fi

	# 9. Check if gc550 is currently loaded
	if lsmod | grep "^${MOD_NAME}[[:space:]]" >/dev/null; then
		local active_srcversion="unknown"
		if [[ -f "/sys/module/${MOD_NAME}/srcversion" ]]; then
			active_srcversion="$(cat "/sys/module/${MOD_NAME}/srcversion")"
		fi
		warn "Module '${MOD_NAME}' is currently LOADED in kernel memory (srcversion: ${active_srcversion})."
		warn "Per project safety rules, the active module will NOT be unloaded or disrupted."
		warn "The new module will be built and installed on disk; a reload or reboot will be required."
	else
		ok "Module '${MOD_NAME}' is not currently loaded."
	fi

	echo ""
}

# --- DKMS Installation ---
install_dkms_package() {
	require_root "$@"

	info "=== Beginning DKMS Package Installation ==="

	# Step 1: Create DKMS source tree
	info "Creating DKMS source directory: ${DKMS_SRC_DIR}..."
	mkdir -p "${DKMS_SRC_DIR}/kernel"

	# Step 2: Copy strictly approved source manifest files
	info "Copying manifest files to ${DKMS_SRC_DIR}..."
	for f in "${DKMS_MANIFEST_ROOT[@]}"; do
		install -m 0644 "${SCRIPT_DIR}/${f}" "${DKMS_SRC_DIR}/${f}"
	done
	for f in "${DKMS_MANIFEST_KERNEL[@]}"; do
		install -m 0644 "${SCRIPT_DIR}/kernel/${f}" "${DKMS_SRC_DIR}/kernel/${f}"
	done
	ok "Strict source manifest populated in ${DKMS_SRC_DIR}"

	# Step 3: Register with DKMS (dkms add)
	if ! dkms status -m "${PKG_NAME}" -v "${PKG_VER}" 2>/dev/null | grep "${PKG_NAME}" >/dev/null; then
		info "Registering module with DKMS (dkms add)..."
		dkms add -m "${PKG_NAME}" -v "${PKG_VER}"
		ok "DKMS package ${PKG_NAME}/${PKG_VER} registered."
	else
		info "DKMS package ${PKG_NAME}/${PKG_VER} already registered."
	fi

	# Step 4: Build with DKMS (dkms build)
	info "Building module for kernel ${TARGET_KVER} (dkms build)..."
	if ! dkms build -m "${PKG_NAME}" -v "${PKG_VER}" -k "${TARGET_KVER}"; then
		error "DKMS build failed for kernel ${TARGET_KVER}. See build log in /var/lib/dkms/${PKG_NAME}/${PKG_VER}/build/make.log"
	fi
	ok "DKMS build succeeded for kernel ${TARGET_KVER}."

	# Step 5: Install with DKMS (dkms install)
	info "Installing module for kernel ${TARGET_KVER} (dkms install)..."
	if ! dkms install -m "${PKG_NAME}" -v "${PKG_VER}" -k "${TARGET_KVER}" --force; then
		error "DKMS install failed for kernel ${TARGET_KVER}."
	fi
	ok "DKMS module installed for kernel ${TARGET_KVER}."

	# Step 6: Run depmod
	info "Updating kernel module dependencies (depmod)..."
	depmod -a "${TARGET_KVER}"
	ok "depmod completed for ${TARGET_KVER}."

	# Step 7: Install udev rule
	if [[ -f "${SCRIPT_DIR}/${UDEV_RULE}" ]]; then
		info "Installing udev rule to ${UDEV_DEST}..."
		install -m 0644 "${SCRIPT_DIR}/${UDEV_RULE}" "${UDEV_DEST}"
		udevadm control --reload-rules && udevadm trigger || true
		ok "Udev rule installed and reloaded."
	fi

	# Step 8: Post-Installation Status & Verification
	echo ""
	info "=== Post-Installation Verification ==="
	info "DKMS status:"
	dkms status -m "${PKG_NAME}" -v "${PKG_VER}" -k "${TARGET_KVER}"

	local installed_ko
	installed_ko="$(modinfo -k "${TARGET_KVER}" -n "${MOD_NAME}" 2>/dev/null || true)"
	if [[ -n "${installed_ko}" ]]; then
		ok "Installed module path: ${installed_ko}"
		local installed_srcversion
		installed_srcversion="$(modinfo -k "${TARGET_KVER}" -F srcversion "${MOD_NAME}" 2>/dev/null || true)"
		local installed_vermagic
		installed_vermagic="$(modinfo -k "${TARGET_KVER}" -F vermagic "${MOD_NAME}" 2>/dev/null || true)"
		info "Installed vermagic:    ${installed_vermagic}"
		info "Installed srcversion: ${installed_srcversion}"

		# Check against active running module
		if lsmod | grep "^${MOD_NAME}[[:space:]]" >/dev/null; then
			local active_srcversion="unknown"
			if [[ -f "/sys/module/${MOD_NAME}/srcversion" ]]; then
				active_srcversion="$(cat "/sys/module/${MOD_NAME}/srcversion")"
			fi
			echo ""
			warn "----------------------------------------------------------------------"
			warn "IMPORTANT: The updated driver is INSTALLED on disk, but the previously"
			warn "loaded module is still ACTIVE in kernel memory."
			warn "  Active running srcversion:    ${active_srcversion}"
			warn "  Newly installed srcversion:   ${installed_srcversion}"
			warn "Per safety rules, active modules are not unloaded automatically."
			warn "To activate the new module, perform a controlled reload when idle:"
			warn "  sudo modprobe -r gc550 && sudo modprobe gc550 subdev_mode=1"
			warn "or simply reboot your system."
			warn "----------------------------------------------------------------------"
		else
			ok "Module is not currently loaded. You can load it anytime using:"
			info "  sudo modprobe gc550 subdev_mode=1"
		fi
	else
		warn "modinfo could not resolve installed path for ${MOD_NAME}."
	fi

	echo ""
	ok "GC550 DKMS installation completed successfully!"
}

# --- Standalone / CLI Entry Point ---
main() {
	local action="install"

	for arg in "$@"; do
		case "${arg}" in
			--check|--dry-run|-c)
				action="check"
				;;
			--uninstall|-u|uninstall)
				action="uninstall"
				;;
			--help|-h)
				echo "AVerMedia Live Gamer Extreme (GC550) DKMS Installer"
				echo ""
				echo "Usage: $0 [OPTION]"
				echo "  --check, -c      Run preflight and dependency checks without modifying system"
				echo "  --uninstall, -u  Remove GC550 DKMS package and udev rules"
				echo "  --help, -h       Show this help message"
				echo ""
				echo "Default action: Build and install GC550 DKMS package for $(uname -r)"
				exit 0
				;;
			*)
				error "Unknown argument: ${arg}. Use --help for usage."
				;;
		esac
	done

	preflight_check

	case "${action}" in
		check)
			ok "Preflight audit complete. No changes made to system."
			;;
		install)
			install_dkms_package "$@"
			;;
		uninstall)
			if [[ -f "${SCRIPT_DIR}/uninstall.sh" ]]; then
				exec "${SCRIPT_DIR}/uninstall.sh"
			else
				error "uninstall.sh not found in ${SCRIPT_DIR}."
			fi
			;;
	esac
}

main "$@"
