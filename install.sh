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

# Validate derived version format
if [[ ! "${PKG_VER}" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?(-[a-zA-Z0-9.]+)?$ ]]; then
	printf "\033[1;31m[ERR ]\033[0m Invalid or malformed GC550 driver version derived from kernel/gc550.h: '%s'\n" "${PKG_VER}" >&2
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
	ok "All 10 required source manifest files are present."

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
	if dkms status -m "${PKG_NAME}" 2>/dev/null | grep "${PKG_NAME}" >/dev/null; then
		info "Found existing DKMS registration for ${PKG_NAME}:"
		dkms status -m "${PKG_NAME}"
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

	# 10. Check target DKMS source directory destination safety
	info "Verifying DKMS destination path safety: ${DKMS_SRC_DIR}..."
	if ! validate_target_dkms_source_dir "${DKMS_SRC_DIR}" "${PKG_NAME}" "${PKG_VER}"; then
		error "Preflight safety check failed: target destination ${DKMS_SRC_DIR} is invalid or unsafe."
	fi
	ok "Target source destination verified safe: ${DKMS_SRC_DIR}"

	echo ""
}

# Fail-closed validation helper: queries DKMS status for module/version.
# Returns:
#   0 = Confirmed: zero registrations remain across all kernels (NONE)
#   1 = Active registrations remain in DKMS (ACTIVE - source directory must be preserved)
#   2 = Status query failed or returned malformed/ambiguous output (ERROR - fail-closed)
check_dkms_unregistered() {
	local mod="$1"
	local ver="$2"

	if [[ -z "${mod}" || -z "${ver}" ]]; then
		warn "check_dkms_unregistered called with missing module or version arguments."
		return 2
	fi

	local mod_esc ver_esc
	mod_esc="$(printf '%s\n' "${mod}" | sed 's/[.[\*^$()+?{|]/\\&/g')"
	ver_esc="$(printf '%s\n' "${ver}" | sed 's/[.[\*^$()+?{|]/\\&/g')"

	local raw_out
	local exit_code=0
	raw_out="$(dkms status -m "${mod}" -v "${ver}" 2>&1)" || exit_code=$?

	if [[ ${exit_code} -ne 0 ]]; then
		warn "DKMS status query failed for ${mod}/${ver} (exit code ${exit_code}): ${raw_out}"
		return 2
	fi

	local re_kernel_record="^${mod_esc}(/|,)[[:space:]]*${ver_esc}(/|,)[[:space:]]*([0-9]+\\.[0-9]+[-_+a-zA-Z0-9.]*)[[:space:]]*,[[:space:]]*([a-zA-Z0-9_]+):[[:space:]]*(installed|built)$"
	local re_added="^${mod_esc}(/|,)[[:space:]]*${ver_esc}:[[:space:]]*(added|broken)$"
	local active_count=0
	local active_details=()

	while IFS= read -r line || [[ -n "${line}" ]]; do
		line="${line#"${line%%[![:space:]]*}"}"
		line="${line%"${line##*[![:space:]]}"}"
		[[ -z "${line}" ]] && continue

		if [[ "${line}" =~ ${re_kernel_record} ]]; then
			((active_count++))
			active_details+=("${BASH_REMATCH[3]} (${BASH_REMATCH[5]})")
		elif [[ "${line}" =~ ${re_added} ]]; then
			((active_count++))
			active_details+=("state: ${BASH_REMATCH[2]}")
		else
			warn "DKMS status query for ${mod}/${ver} returned unexpected or malformed output: '${line}'"
			return 2
		fi
	done <<< "${raw_out}"

	if [[ ${active_count} -eq 0 ]]; then
		return 0
	else
		local joined_details
		joined_details="$(printf '%s, ' "${active_details[@]}" | sed 's/, $//')"
		info "Preserving source tree for ${mod}/${ver} (still registered in DKMS: ${joined_details:-active})."
		return 1
	fi
}

# Helper: Extracts a scalar variable from a dkms.conf file with strict validation.
# Rejects missing, duplicate, malformed, or ambiguous declarations.
extract_dkms_conf_var() {
	local conf_file="$1"
	local var_name="$2"
	local -a matches=()
	local line

	while IFS= read -r line || [[ -n "${line}" ]]; do
		[[ "${line}" =~ ^[[:space:]]*# ]] && continue
		if [[ "${line}" =~ ^[[:space:]]*${var_name}[[:space:]]*= ]]; then
			matches+=("${line}")
		fi
	done < "${conf_file}"

	if [[ ${#matches[@]} -eq 0 ]]; then
		return 1 # Missing
	fi
	if [[ ${#matches[@]} -gt 1 ]]; then
		return 2 # Duplicate or ambiguous
	fi

	local target_line="${matches[0]}"
	local re_double="^[[:space:]]*${var_name}[[:space:]]*=[[:space:]]*\"([^\"]*)\"([[:space:]]*(#.*)?)?$"
	local re_single="^[[:space:]]*${var_name}[[:space:]]*=[[:space:]]*'([^']*)'([[:space:]]*(#.*)?)?$"
	local re_unquoted="^[[:space:]]*${var_name}[[:space:]]*=[[:space:]]*([^[:space:]\"'\`$#]+)([[:space:]]*(#.*)?)?$"

	if [[ "${target_line}" =~ ${re_double} ]]; then
		printf "%s" "${BASH_REMATCH[1]}"
	elif [[ "${target_line}" =~ ${re_single} ]]; then
		printf "%s" "${BASH_REMATCH[1]}"
	elif [[ "${target_line}" =~ ${re_unquoted} ]]; then
		printf "%s" "${BASH_REMATCH[1]}"
	else
		return 3 # Malformed syntax
	fi
	return 0
}

# Strictly enumerate superseded versions registered for TARGET_KVER from DKMS.
# Validates module, version, kernel, architecture, and status fields before any removal.
# Returns:
#   0 = Validation succeeded; populated array 'superseded_vers'
#   1 = Validation failed (command error, malformed lines, invalid fields); retirement must be skipped
get_superseded_versions_for_kernel() {
	local mod="$1"
	local target_kver="$2"
	local current_ver="${3:-}"

	local raw_out
	local exit_code=0
	raw_out="$(dkms status -m "${mod}" -k "${target_kver}" 2>&1)" || exit_code=$?

	if [[ ${exit_code} -ne 0 ]]; then
		warn "DKMS status query failed for ${mod} on kernel ${target_kver} (exit code ${exit_code}): ${raw_out}"
		return 1
	fi

	local mod_esc
	mod_esc="$(printf '%s\n' "${mod}" | sed 's/[.[\*^$()+?{|]/\\&/g')"

	local re_kernel_record="^${mod_esc}(/|,)[[:space:]]*([0-9]+\\.[0-9]+(\\.[0-9]+)?(-[a-zA-Z0-9.]+)?)(/|,)[[:space:]]*([0-9]+\\.[0-9]+[-_+a-zA-Z0-9.]*)[[:space:]]*,[[:space:]]*([a-zA-Z0-9_]+):[[:space:]]*(installed|built)$"
	local re_added="^${mod_esc}(/|,)[[:space:]]*([0-9]+\\.[0-9]+(\\.[0-9]+)?(-[a-zA-Z0-9.]+)?):[[:space:]]*(added|broken)$"
	local re_prefix="^${mod_esc}(/|,)"

	local -a found_vers=()
	local line

	while IFS= read -r line || [[ -n "${line}" ]]; do
		line="${line#"${line%%[![:space:]]*}"}"
		line="${line%"${line##*[![:space:]]}"}"
		[[ -z "${line}" ]] && continue

		if [[ "${line}" =~ ${re_kernel_record} ]]; then
			local v="${BASH_REMATCH[2]}"
			local k="${BASH_REMATCH[6]}"
			if [[ "${k}" == "${target_kver}" ]]; then
				if [[ -n "${current_ver}" && "${v}" == "${current_ver}" ]]; then
					: # Current version on target kernel; not superseded
				else
					found_vers+=("${v}")
				fi
			fi
		elif [[ "${line}" =~ ${re_added} ]]; then
			: # Valid added/broken state; not built on target kernel
		elif [[ "${line}" =~ ${re_prefix} ]]; then
			warn "Malformed DKMS status line for ${mod}: '${line}'"
			return 1
		else
			warn "Unexpected line in DKMS status query for ${mod}: '${line}'"
			return 1
		fi
	done <<< "${raw_out}"

	superseded_vers=()
	if [[ ${#found_vers[@]} -gt 0 ]]; then
		while IFS= read -r v; do
			[[ -n "${v}" ]] && superseded_vers+=("${v}")
		done < <(printf '%s\n' "${found_vers[@]}" | sort -u)
	fi

	return 0
}

# Validates destination DKMS source directory safety before creating or writing files.
# Prevents symlink traversal, destination escapes, and overwriting mismatched packages.
#
# Arguments:
#   $1: target_dir (e.g. /usr/src/gc550-0.5.0)
#   $2: expected_pkg (e.g. gc550)
#   $3: expected_ver (e.g. 0.5.0)
#   $4: [expected_parent] (optional, defaults to /usr/src)
#
# Returns:
#   0 on safe destination validation
#   1 on validation failure (fail closed)
validate_target_dkms_source_dir() {
	local target_dir="$1"
	local expected_pkg="$2"
	local expected_ver="$3"
	local expected_parent="${4:-/usr/src}"

	if [[ -z "${target_dir}" || -z "${expected_pkg}" || -z "${expected_ver}" ]]; then
		warn "validate_target_dkms_source_dir: Missing required parameters."
		return 1
	fi

	# 1. Path component symlink check: No component from root to leaf may be a symlink
	local cur=""
	local IFS='/'
	local -a comp_parts
	read -ra comp_parts <<< "${target_dir}"
	for comp in "${comp_parts[@]}"; do
		[[ -z "${comp}" ]] && continue
		cur="${cur}/${comp}"
		if [[ -L "${cur}" ]]; then
			warn "Destination path component '${cur}' is a symbolic link; refusing to write."
			return 1
		fi
	done

	# 2. Strict basename check: leaf directory must match expected <pkg>-<ver> exactly
	local expected_basename="${expected_pkg}-${expected_ver}"
	local actual_basename
	actual_basename="$(basename "${target_dir}")"
	if [[ "${actual_basename}" != "${expected_basename}" ]]; then
		warn "Destination basename '${actual_basename}' does not match expected '${expected_basename}'."
		return 1
	fi

	# 3. Parent directory check: raw parent must equal expected parent
	local raw_parent
	raw_parent="$(dirname "${target_dir}")"
	local canonical_expected_parent
	canonical_expected_parent="$(realpath -m "${expected_parent}" 2>/dev/null || echo "${expected_parent}")"

	# 4. Resolved canonical path check: resolved destination must reside directly under canonical parent
	local resolved_target
	resolved_target="$(realpath -m "${target_dir}")"
	local resolved_parent
	resolved_parent="$(dirname "${resolved_target}")"

	if [[ "${resolved_parent}" != "${canonical_expected_parent}" ]]; then
		warn "Resolved destination '${resolved_target}' does not reside directly under '${canonical_expected_parent}'."
		return 1
	fi

	# 5. Existing destination checks: real directory, symlink check, and package identity check
	if [[ -e "${target_dir}" || -L "${target_dir}" ]]; then
		if [[ -L "${target_dir}" ]]; then
			warn "Destination '${target_dir}' is a symbolic link; refusing to write."
			return 1
		fi
		if [[ ! -d "${target_dir}" ]]; then
			warn "Destination '${target_dir}' exists but is not a directory; refusing to write."
			return 1
		fi

		# 5a. Protect dkms.conf: Reject symlinks, require regular file, validate exact identity
		if [[ -e "${target_dir}/dkms.conf" || -L "${target_dir}/dkms.conf" ]]; then
			if [[ -L "${target_dir}/dkms.conf" ]]; then
				warn "Destination dkms.conf is a symbolic link; refusing to read or overwrite: ${target_dir}/dkms.conf"
				return 1
			fi
			if [[ ! -f "${target_dir}/dkms.conf" ]]; then
				warn "Destination dkms.conf exists but is not a regular file; refusing to write: ${target_dir}/dkms.conf"
				return 1
			fi
			local existing_name existing_ver
			if ! existing_name="$(extract_dkms_conf_var "${target_dir}/dkms.conf" "PACKAGE_NAME")"; then
				warn "Existing destination contains invalid or unparseable PACKAGE_NAME in dkms.conf: ${target_dir}"
				return 1
			fi
			if ! existing_ver="$(extract_dkms_conf_var "${target_dir}/dkms.conf" "PACKAGE_VERSION")"; then
				warn "Existing destination contains invalid or unparseable PACKAGE_VERSION in dkms.conf: ${target_dir}"
				return 1
			fi
			if [[ "${existing_name}" != "${expected_pkg}" ]]; then
				warn "Existing destination PACKAGE_NAME '${existing_name}' does not match expected '${expected_pkg}': ${target_dir}"
				return 1
			fi
			if [[ "${existing_ver}" != "${expected_ver}" ]]; then
				warn "Existing destination PACKAGE_VERSION '${existing_ver}' does not match expected '${expected_ver}': ${target_dir}"
				return 1
			fi
		elif [[ -n "$(ls -A "${target_dir}" 2>/dev/null)" ]]; then
			# Existing non-empty directory lacking dkms.conf is ambiguous; fail closed
			warn "Existing destination '${target_dir}' is non-empty but lacks a valid dkms.conf; refusing to overwrite."
			return 1
		fi

		# 5b. Protect kernel subdirectory: Reject symlinks, require real directory if present
		if [[ -e "${target_dir}/kernel" || -L "${target_dir}/kernel" ]]; then
			if [[ -L "${target_dir}/kernel" ]]; then
				warn "Destination kernel path '${target_dir}/kernel' is a symbolic link; refusing to write."
				return 1
			fi
			if [[ ! -d "${target_dir}/kernel" ]]; then
				warn "Destination kernel path '${target_dir}/kernel' exists but is not a directory; refusing to write."
				return 1
			fi
		fi
	fi

	return 0
}

# Strict validation helper before any source tree deletion
safe_remove_dkms_source_dir() {
	local dir="$1"
	local expected_name="$2"
	local expected_ver="${3:-}"

	if [[ ! -d "${dir}" ]]; then
		return 0
	fi

	# Security: Reject symlinks to prevent directory traversal
	if [[ -L "${dir}" ]]; then
		warn "Refusing to delete symlink source directory: ${dir}"
		return 1
	fi

	# Canonical realpath check: must be strictly located within /usr/src/
	local real_path
	real_path="$(realpath "${dir}" 2>/dev/null || true)"
	if [[ -z "${real_path}" || "${real_path}" != /usr/src/* || "${real_path}" == "/usr/src" || "${real_path}" == "/usr/src/" ]]; then
		warn "Refusing to delete directory outside /usr/src: ${dir} (resolved: ${real_path})"
		return 1
	fi

	# Package ownership check: dkms.conf must exist and be a genuine regular file (not a symlink)
	if [[ -L "${dir}/dkms.conf" || ! -f "${dir}/dkms.conf" ]]; then
		warn "Refusing to delete directory lacking genuine dkms.conf: ${dir}"
		return 1
	fi

	# Package ownership check: PACKAGE_NAME must equal expected name exactly
	local actual_name
	if ! actual_name="$(extract_dkms_conf_var "${dir}/dkms.conf" "PACKAGE_NAME")"; then
		warn "Refusing to delete directory: cannot establish unambiguous PACKAGE_NAME in ${dir}/dkms.conf"
		return 1
	fi
	if [[ "${actual_name}" != "${expected_name}" ]]; then
		warn "Refusing to delete directory: dkms.conf PACKAGE_NAME '${actual_name}' does not match expected '${expected_name}': ${dir}"
		return 1
	fi

	# Version check: if expected_ver provided, PACKAGE_VERSION must equal expected version exactly
	if [[ -n "${expected_ver}" ]]; then
		local actual_ver
		if ! actual_ver="$(extract_dkms_conf_var "${dir}/dkms.conf" "PACKAGE_VERSION")"; then
			warn "Refusing to delete directory: cannot establish unambiguous PACKAGE_VERSION in ${dir}/dkms.conf"
			return 1
		fi
		if [[ "${actual_ver}" != "${expected_ver}" ]]; then
			warn "Refusing to delete directory: dkms.conf PACKAGE_VERSION '${actual_ver}' does not match expected '${expected_ver}': ${dir}"
			return 1
		fi
	fi

	info "Removing verified DKMS source tree: ${dir}..."
	rm -rf "${dir}"
	ok "Source tree ${dir} removed."
	return 0
}

# --- DKMS Installation ---
install_dkms_package() {
	require_root "$@"

	info "=== Beginning DKMS Package Installation ==="

	# Step 1: Validate DKMS source destination safety before any filesystem operations
	info "Validating DKMS source destination: ${DKMS_SRC_DIR}..."
	if ! validate_target_dkms_source_dir "${DKMS_SRC_DIR}" "${PKG_NAME}" "${PKG_VER}"; then
		error "Destination validation failed for ${DKMS_SRC_DIR}. Aborting installation to ensure filesystem integrity."
	fi

	# Step 2: Create DKMS source tree without following symlinks
	info "Creating DKMS source directory: ${DKMS_SRC_DIR}..."
	if [[ -L "${DKMS_SRC_DIR}" ]]; then
		error "Destination ${DKMS_SRC_DIR} is a symbolic link."
	fi
	if [[ ! -d "${DKMS_SRC_DIR}" ]]; then
		mkdir -m 0755 "${DKMS_SRC_DIR}"
	fi

	# Inspect and create kernel subdirectory without following symlinks
	if [[ -L "${DKMS_SRC_DIR}/kernel" ]]; then
		error "Destination kernel path ${DKMS_SRC_DIR}/kernel is a symbolic link."
	fi
	if [[ -e "${DKMS_SRC_DIR}/kernel" && ! -d "${DKMS_SRC_DIR}/kernel" ]]; then
		error "Destination kernel path ${DKMS_SRC_DIR}/kernel exists but is not a directory."
	fi
	if [[ ! -d "${DKMS_SRC_DIR}/kernel" ]]; then
		mkdir -m 0755 "${DKMS_SRC_DIR}/kernel"
	fi

	# Post-creation integrity recheck
	if [[ -L "${DKMS_SRC_DIR}" || ! -d "${DKMS_SRC_DIR}" ]]; then
		error "Integrity check failed: ${DKMS_SRC_DIR} is not a valid directory."
	fi
	if [[ -L "${DKMS_SRC_DIR}/kernel" || ! -d "${DKMS_SRC_DIR}/kernel" ]]; then
		error "Integrity check failed: ${DKMS_SRC_DIR}/kernel is not a valid directory."
	fi

	# Step 3: Copy strictly approved source manifest files
	info "Copying manifest files to ${DKMS_SRC_DIR}..."
	for f in "${DKMS_MANIFEST_ROOT[@]}"; do
		local dest_file="${DKMS_SRC_DIR}/${f}"
		if [[ -L "${dest_file}" ]]; then
			error "Destination file ${dest_file} is a symbolic link; refusing to overwrite."
		fi
		if [[ -e "${dest_file}" && ! -f "${dest_file}" ]]; then
			error "Destination path ${dest_file} exists but is not a regular file; refusing to overwrite."
		fi
		install -m 0644 "${SCRIPT_DIR}/${f}" "${dest_file}"
	done

	# Revalidate kernel directory immediately before copying files into it
	if [[ -L "${DKMS_SRC_DIR}/kernel" || ! -d "${DKMS_SRC_DIR}/kernel" ]]; then
		error "Integrity check failed: ${DKMS_SRC_DIR}/kernel was modified or is a symbolic link."
	fi

	for f in "${DKMS_MANIFEST_KERNEL[@]}"; do
		local dest_file="${DKMS_SRC_DIR}/kernel/${f}"
		if [[ -L "${dest_file}" ]]; then
			error "Destination kernel file ${dest_file} is a symbolic link; refusing to overwrite."
		fi
		if [[ -e "${dest_file}" && ! -f "${dest_file}" ]]; then
			error "Destination kernel path ${dest_file} exists but is not a regular file; refusing to overwrite."
		fi
		install -m 0644 "${SCRIPT_DIR}/kernel/${f}" "${dest_file}"
	done
	ok "Strict source manifest populated in ${DKMS_SRC_DIR}"

	# Step 4: Register with DKMS (dkms add)
	local reg_check=0
	check_dkms_unregistered "${PKG_NAME}" "${PKG_VER}" >/dev/null 2>&1 || reg_check=$?
	if [[ ${reg_check} -eq 0 ]]; then
		info "Registering module with DKMS (dkms add)..."
		dkms add -m "${PKG_NAME}" -v "${PKG_VER}"
		ok "DKMS package ${PKG_NAME}/${PKG_VER} registered."
	elif [[ ${reg_check} -eq 1 ]]; then
		info "DKMS package ${PKG_NAME}/${PKG_VER} already registered."
	else
		error "DKMS status check failed for ${PKG_NAME}/${PKG_VER}. Aborting installation."
	fi

	# Step 4: Build with DKMS (dkms build)
	info "Building module for kernel ${TARGET_KVER} (dkms build)..."
	if ! dkms build -m "${PKG_NAME}" -v "${PKG_VER}" -k "${TARGET_KVER}"; then
		error "DKMS build failed for kernel ${TARGET_KVER}. Existing driver installation (if any) was left intact.
See build log in /var/lib/dkms/${PKG_NAME}/${PKG_VER}/build/make.log"
	fi
	ok "DKMS build succeeded for kernel ${TARGET_KVER}."

	# Step 5: Install with DKMS (dkms install)
	info "Installing module for kernel ${TARGET_KVER} (dkms install)..."
	if ! dkms install -m "${PKG_NAME}" -v "${PKG_VER}" -k "${TARGET_KVER}" --force; then
		error "DKMS install failed for kernel ${TARGET_KVER}."
	fi
	ok "DKMS module installed for kernel ${TARGET_KVER}."

	# Step 6: Post-installation retirement of superseded DKMS versions
	# Crucial safety rule: Only executed AFTER Step 5 has successfully built and installed the new module on disk

	# 6a. Kernel-scoped retirement of superseded GC550 versions
	local -a superseded_vers=()
	if ! get_superseded_versions_for_kernel "${PKG_NAME}" "${TARGET_KVER}" "${PKG_VER}"; then
		warn "DKMS status output for ${PKG_NAME} is ambiguous or malformed; skipping retirement and preserving source directories."
	else
		for ov in "${superseded_vers[@]}"; do
			info "Retiring superseded DKMS version for kernel ${TARGET_KVER}: ${PKG_NAME}/${ov}..."
			if dkms remove -m "${PKG_NAME}" -v "${ov}" -k "${TARGET_KVER}"; then
				ok "Superseded DKMS version ${PKG_NAME}/${ov} retired for kernel ${TARGET_KVER}."
				# Fail-closed post-removal status verification across all kernels
				if check_dkms_unregistered "${PKG_NAME}" "${ov}"; then
					safe_remove_dkms_source_dir "/usr/src/${PKG_NAME}-${ov}" "${PKG_NAME}" "${ov}"
				fi
			else
				warn "Could not cleanly unregister superseded version ${PKG_NAME}/${ov} for kernel ${TARGET_KVER}; source tree preserved."
			fi
		done
	fi

	# 6b. Kernel-scoped retirement of legacy avermedia-gc550 package
	local -a legacy_superseded_vers=()
	superseded_vers=()
	if ! get_superseded_versions_for_kernel "avermedia-gc550" "${TARGET_KVER}" ""; then
		warn "DKMS status output for legacy package avermedia-gc550 is ambiguous or malformed; skipping retirement and preserving source directories."
	else
		legacy_superseded_vers=("${superseded_vers[@]}")
		for lv in "${legacy_superseded_vers[@]}"; do
			info "Retiring legacy DKMS package avermedia-gc550/${lv} for kernel ${TARGET_KVER}..."
			if dkms remove -m "avermedia-gc550" -v "${lv}" -k "${TARGET_KVER}"; then
				ok "Legacy DKMS package avermedia-gc550/${lv} retired for kernel ${TARGET_KVER}."
				# Fail-closed post-removal status verification across all kernels
				if check_dkms_unregistered "avermedia-gc550" "${lv}"; then
					safe_remove_dkms_source_dir "/usr/src/avermedia-gc550-${lv}" "avermedia-gc550" "${lv}"
				fi
			else
				warn "Could not cleanly unregister legacy package avermedia-gc550/${lv} for kernel ${TARGET_KVER}; source tree preserved."
			fi
		done
	fi

	# Step 7: Run depmod
	info "Updating kernel module dependencies (depmod)..."
	depmod -a "${TARGET_KVER}"
	ok "depmod completed for ${TARGET_KVER}."

	# Step 8: Install udev rule
	if [[ -f "${SCRIPT_DIR}/${UDEV_RULE}" ]]; then
		info "Installing udev rule to ${UDEV_DEST}..."
		install -m 0644 "${SCRIPT_DIR}/${UDEV_RULE}" "${UDEV_DEST}"
		udevadm control --reload-rules && udevadm trigger || true
		ok "Udev rule installed and reloaded."
	fi

	# Step 9: Post-Installation Status & Verification
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
	local -a uninstall_opts=()

	while [[ $# -gt 0 ]]; do
		case "$1" in
			--check|--dry-run|-c)
				action="check"
				shift
				;;
			--uninstall|-u|uninstall)
				action="uninstall"
				shift
				;;
			--all-versions|-a)
				uninstall_opts+=("$1")
				shift
				;;
			--version|-v)
				if [[ $# -lt 2 || -z "$2" ]]; then
					error "Option --version requires an argument."
				fi
				uninstall_opts+=("$1" "$2")
				shift 2
				;;
			--help|-h)
				echo "AVerMedia Live Gamer Extreme (GC550) DKMS Installer"
				echo ""
				echo "Usage: $0 [OPTION]"
				echo "  --check, -c              Run preflight and dependency checks without modifying system"
				echo "  --uninstall, -u [OPTS]   Remove GC550 DKMS package and udev rules (delegates to uninstall.sh)"
				echo "  --help, -h               Show this help message"
				echo ""
				echo "Default action: Build and install GC550 DKMS package for $(uname -r)"
				exit 0
				;;
			*)
				error "Unknown argument: $1. Use --help for usage."
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
				exec "${SCRIPT_DIR}/uninstall.sh" "${uninstall_opts[@]}"
			else
				error "uninstall.sh not found in ${SCRIPT_DIR}."
			fi
			;;
	esac
}

main "$@"
