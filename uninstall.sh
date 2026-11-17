#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Idempotent Uninstaller for AVerMedia Live Gamer Extreme (GC550) Kernel Driver
#
# Usage:
#   ./uninstall.sh                 # Safe uninstallation of target GC550 DKMS version (default)
#   ./uninstall.sh --all-versions  # Unregister and remove ALL registered GC550 versions and legacy packages
#   ./uninstall.sh --help          # Show help message

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
	PKG_VER="0.5.0"
fi

# Validate derived version format
if [[ ! "${PKG_VER}" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?(-[a-zA-Z0-9.]+)?$ ]]; then
	printf "\033[1;31m[ERR ]\033[0m Invalid or malformed GC550 driver version: '%s'\n" "${PKG_VER}" >&2
	exit 1
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

# Strictly enumerate registered versions for a module from DKMS status.
# Validates every non-empty line belonging to the requested module.
# Returns:
#   0 = Validation succeeded; populated array 'validated_versions'
#   1 = Validation failed (command error, malformed lines, invalid versions); cleanup must be aborted
get_registered_module_versions() {
	local mod="$1"
	local raw_out
	local exit_code=0
	raw_out="$(dkms status -m "${mod}" 2>&1)" || exit_code=$?

	if [[ ${exit_code} -ne 0 ]]; then
		warn "DKMS status query failed for ${mod} (exit code ${exit_code}): ${raw_out}"
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
			found_vers+=("${BASH_REMATCH[2]}")
		elif [[ "${line}" =~ ${re_added} ]]; then
			found_vers+=("${BASH_REMATCH[2]}")
		elif [[ "${line}" =~ ${re_prefix} ]]; then
			warn "Malformed DKMS status line for ${mod}: '${line}'"
			return 1
		else
			warn "Unexpected line in DKMS status query for ${mod}: '${line}'"
			return 1
		fi
	done <<< "${raw_out}"

	validated_versions=()
	if [[ ${#found_vers[@]} -gt 0 ]]; then
		while IFS= read -r v; do
			[[ -n "${v}" ]] && validated_versions+=("${v}")
		done < <(printf '%s\n' "${found_vers[@]}" | sort -u)
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

main() {
	local purge_all=false
	local custom_ver=""

	# Handle --help before requiring root
	for arg in "$@"; do
		if [[ "${arg}" == "--help" || "${arg}" == "-h" ]]; then
			echo "AVerMedia Live Gamer Extreme (GC550) DKMS Uninstaller"
			echo ""
			echo "Usage: $0 [OPTION]"
			echo "  (no args)               Safely remove target version (${PKG_NAME}/${PKG_VER}) only (default)"
			echo "  --version, -v <version> Specify target version to remove (default: ${PKG_VER})"
			echo "  --all-versions, -a      Unregister and remove ALL registered GC550 versions and legacy packages"
			echo "  --help, -h              Show this help message"
			exit 0
		fi
	done

	require_root "$@"

	while [[ $# -gt 0 ]]; do
		case "$1" in
			--all-versions|--all|-a)
				purge_all=true
				shift
				;;
			--version|-v)
				if [[ $# -lt 2 || -z "$2" ]]; then
					error "Option --version requires an argument."
				fi
				custom_ver="$2"
				shift 2
				;;
			*)
				error "Unknown argument: $1. Use --help for usage."
				;;
		esac
	done

	if [[ -n "${custom_ver}" ]]; then
		PKG_VER="${custom_ver}"
	fi

	# Validate target version format
	if [[ ! "${PKG_VER}" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?(-[a-zA-Z0-9.]+)?$ ]]; then
		error "Invalid or malformed GC550 driver version: '${PKG_VER}'"
	fi
	DKMS_SRC_DIR="/usr/src/${PKG_NAME}-${PKG_VER}"

	if [[ "${purge_all}" == "true" ]]; then
		info "=== Uninstalling ALL GC550 DKMS Registrations & Versions ==="
	else
		info "=== Uninstalling ${PKG_NAME}/${PKG_VER} ==="
	fi

	# 1. Check if module is currently loaded
	if lsmod | grep "^${MOD_NAME}[[:space:]]" >/dev/null; then
		warn "Module '${MOD_NAME}' is currently LOADED in kernel memory."
		warn "Per safety rules, running modules will NOT be forcefully unloaded."
		warn "If video/audio capture is active, please close applications first."
		warn "You may unload it manually when idle: sudo modprobe -r ${MOD_NAME}"
	else
		ok "Module '${MOD_NAME}' is not loaded."
	fi

	# 2. DKMS Removal
	if command -v dkms >/dev/null 2>&1; then
		if [[ "${purge_all}" == "true" ]]; then
			# 1. Purge all registered versions of gc550
			local -a validated_versions=()
			if ! get_registered_module_versions "${PKG_NAME}"; then
				error "DKMS status output for ${PKG_NAME} is ambiguous or malformed. Aborting uninstallation to protect system integrity."
			fi

			if [[ ${#validated_versions[@]} -eq 0 ]]; then
				info "No registered versions found for ${PKG_NAME} in DKMS."
			fi
			for v in "${validated_versions[@]}"; do
				info "Removing DKMS module ${PKG_NAME}/${v} across all kernels..."
				if dkms remove -m "${PKG_NAME}" -v "${v}" --all; then
					ok "DKMS module ${PKG_NAME}/${v} removal command completed."
					# Fail-closed post-removal status verification across all kernels
					if check_dkms_unregistered "${PKG_NAME}" "${v}"; then
						safe_remove_dkms_source_dir "/usr/src/${PKG_NAME}-${v}" "${PKG_NAME}" "${v}"
					else
						warn "Post-removal verification failed or registrations remain for ${PKG_NAME}/${v}; source tree preserved."
					fi
				else
					warn "DKMS removal reported an error for ${PKG_NAME}/${v}; source tree preserved."
				fi
			done

			# 2. Purge legacy avermedia-gc550 package registrations if present
			local -a legacy_versions=()
			validated_versions=()
			if ! get_registered_module_versions "avermedia-gc550"; then
				warn "DKMS status output for legacy package avermedia-gc550 is ambiguous or malformed; skipping legacy cleanup to protect system integrity."
			else
				legacy_versions=("${validated_versions[@]}")
				for lv in "${legacy_versions[@]}"; do
					info "Removing legacy DKMS module avermedia-gc550/${lv} across all kernels..."
					if dkms remove -m "avermedia-gc550" -v "${lv}" --all; then
						ok "Legacy DKMS module avermedia-gc550/${lv} removal command completed."
						# Fail-closed post-removal status verification across all kernels
						if check_dkms_unregistered "avermedia-gc550" "${lv}"; then
							safe_remove_dkms_source_dir "/usr/src/avermedia-gc550-${lv}" "avermedia-gc550" "${lv}"
						else
							warn "Post-removal verification failed or registrations remain for legacy package avermedia-gc550/${lv}; source tree preserved."
						fi
					else
						warn "DKMS removal reported an error for legacy package avermedia-gc550/${lv}; source tree preserved."
					fi
				done
			fi
		else
			# Strict default: remove ONLY target version ${PKG_NAME}/${PKG_VER}
			local pre_check_rc=0
			check_dkms_unregistered "${PKG_NAME}" "${PKG_VER}" >/dev/null 2>&1 || pre_check_rc=$?
			if [[ ${pre_check_rc} -eq 1 ]]; then
				# Active registration confirmed in DKMS
				info "Removing DKMS module ${PKG_NAME}/${PKG_VER} across all kernels..."
				if dkms remove -m "${PKG_NAME}" -v "${PKG_VER}" --all; then
					ok "DKMS package ${PKG_NAME}/${PKG_VER} removal command completed."
					# Fail-closed post-removal verification
					if check_dkms_unregistered "${PKG_NAME}" "${PKG_VER}"; then
						safe_remove_dkms_source_dir "${DKMS_SRC_DIR}" "${PKG_NAME}" "${PKG_VER}"
					else
						local post_rc=$?
						if [[ ${post_rc} -eq 1 ]]; then
							warn "DKMS status indicates registrations still remain for ${PKG_NAME}/${PKG_VER}; source tree preserved."
						else
							warn "Could not verify unregistration of ${PKG_NAME}/${PKG_VER} due to DKMS status query failure; source tree preserved."
						fi
					fi
				else
					error "DKMS removal failed for ${PKG_NAME}/${PKG_VER}. Aborting source tree deletion to avoid DKMS database corruption."
				fi
			elif [[ ${pre_check_rc} -eq 0 ]]; then
				info "DKMS package ${PKG_NAME}/${PKG_VER} is not currently registered in DKMS."
				# Confirmed zero registrations remain; safe to clean unmanaged directory if present
				safe_remove_dkms_source_dir "${DKMS_SRC_DIR}" "${PKG_NAME}" "${PKG_VER}"
			else
				error "DKMS status check failed for ${PKG_NAME}/${PKG_VER}. Aborting uninstallation to avoid unsafe operations."
			fi
		fi
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
