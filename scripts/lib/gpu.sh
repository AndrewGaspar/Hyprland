# shellcheck shell=bash
# Shared GPU render-node resolver for the HypXRland tooling.
#
# Source this file (`source scripts/lib/gpu.sh`) to get resolve_render_node.
# It has NO side effects on source (defines functions only), is safe under
# `set -euo pipefail`, and is deliberately dependency-free (plain sysfs scan) so
# it works identically on the HOST and INSIDE the container — the repo is
# bind-mounted at /src, so container code sources /src/scripts/lib/gpu.sh.
#
# resolve_render_node <spec> — echo the /dev/dri/renderD* node for a GPU spec.
#
#   spec:  amd | nvidia | intel        vendor keyword (case-insensitive), OR
#          /dev/dri/renderDNNN         an explicit render-node path.
#
#   Precedence (highest first):
#     1. explicit path         — spec begins with '/'; used verbatim if it exists.
#     2. env override          — $HYPXRLAND_<VENDOR>_NODE (AMD/NVIDIA/INTEL), if set.
#     3. vendor scan           — first /sys/class/drm/renderD*/device/vendor whose
#                                id matches (0x1002 AMD, 0x10de NVIDIA, 0x8086 Intel)
#                                AND whose /dev node is actually present.
#     4. hard error            — prints the candidate nodes + their vendor ids.
#
#   The scan reads the LOCAL /sys/class/drm. Call it host-side to pick the podman
#   --device node, and AGAIN inside the container to pick XR_GPU_NODE: CDI-injected
#   NVIDIA nodes keep their host names, but scanning in-context VERIFIES the node
#   that is actually present rather than assuming a name. The "/dev node present"
#   requirement is what makes the in-container scan land on the one GPU that was
#   injected/bind-mounted, ignoring sibling nodes still visible in shared /sys.
#
# Returns 0 and echoes the node on success; non-zero with a diagnostic on stderr
# otherwise (2 = unknown spec, 1 = not found / missing path).
resolve_render_node() {
    local spec="${1:-}"
    [[ -n $spec ]] || { echo "resolve_render_node: missing GPU spec" >&2; return 2; }

    # 1. explicit path wins outright.
    if [[ $spec == /* ]]; then
        [[ -e $spec ]] || { echo "resolve_render_node: render node not found: $spec" >&2; return 1; }
        printf '%s\n' "$spec"; return 0
    fi

    local vendor_id envvar
    case "${spec,,}" in
        amd)    vendor_id=0x1002; envvar=HYPXRLAND_AMD_NODE ;;
        nvidia) vendor_id=0x10de; envvar=HYPXRLAND_NVIDIA_NODE ;;
        intel)  vendor_id=0x8086; envvar=HYPXRLAND_INTEL_NODE ;;
        *) echo "resolve_render_node: unknown GPU spec '$spec' (want amd|nvidia|intel|/dev/dri/renderD*)" >&2; return 2 ;;
    esac

    # 2. env override (kept working as an escape hatch; must exist if set).
    local env_node="${!envvar:-}"
    if [[ -n $env_node ]]; then
        [[ -e $env_node ]] || { echo "resolve_render_node: \$$envvar=$env_node does not exist" >&2; return 1; }
        printf '%s\n' "$env_node"; return 0
    fi

    # 3. vendor scan — first matching node whose /dev entry is actually present.
    local sysnode vid dev
    for sysnode in /sys/class/drm/renderD*; do
        [[ -r $sysnode/device/vendor ]] || continue
        vid=$(cat "$sysnode/device/vendor" 2>/dev/null || true)
        dev="/dev/dri/$(basename "$sysnode")"
        if [[ $vid == "$vendor_id" && -e $dev ]]; then
            printf '%s\n' "$dev"; return 0
        fi
    done

    # 4. hard error, listing every candidate + its vendor id.
    {
        echo "resolve_render_node: no present render node for vendor $vendor_id ($spec). Candidates:"
        local any=0
        for sysnode in /sys/class/drm/renderD*; do
            [[ -r $sysnode/device/vendor ]] || continue
            any=1
            printf '    /dev/dri/%s  vendor=%s  dev-present=%s\n' \
                "$(basename "$sysnode")" \
                "$(cat "$sysnode/device/vendor" 2>/dev/null || echo '?')" \
                "$([[ -e /dev/dri/$(basename "$sysnode") ]] && echo yes || echo no)"
        done
        [[ $any -eq 1 ]] || echo "    (none — /sys/class/drm has no renderD* entries)"
        echo "  Override with \$$envvar=/dev/dri/renderDNNN or pass an explicit node path."
    } >&2
    return 1
}
