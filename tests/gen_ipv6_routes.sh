#!/bin/bash
# Generate 5000 IPv6 routes with prefix length <= 112 for babeld coalescing tests.
# Usage: sudo ./gen_ipv6_routes.sh [add|del] [interface]
#
# Routes are spread across 2001:db8::/32 (documentation prefix).
# Prefix lengths cycle through: 48, 56, 64, 80, 96, 112.

ACTION=${1:-replace}
IFACE=${2:-lo}
COUNT=5000

PREFIXES=(48 56 64 80 96 112)
NPREFIX=${#PREFIXES[@]}

# Base: 2001:db8:AAAA:BBBB:CCCC:DDDD:EEEE::/plen
# Use a per-prefix-length sequence so generated network prefixes are unique
# after canonical masking for that plen.
declare -A SEQ

added=0
fail=0

for ((i=0; i<COUNT; i++)); do
    plen=${PREFIXES[$((i % NPREFIX))]}

    k=${SEQ[$plen]:-0}
    SEQ[$plen]=$((k + 1))

    a=0; b=0; c=0; d=0; e=0
    case "$plen" in
        48)
            a=$(( k & 0xFFFF ))
            ;;
        56)
            a=$(( (k >> 8) & 0xFFFF ))
            b=$(( (k & 0xFF) << 8 ))
            ;;
        64)
            a=$(( (k >> 16) & 0xFFFF ))
            b=$(( k & 0xFFFF ))
            ;;
        80)
            a=$(( (k >> 32) & 0xFFFF ))
            b=$(( (k >> 16) & 0xFFFF ))
            c=$(( k & 0xFFFF ))
            ;;
        96)
            a=$(( (k >> 48) & 0xFFFF ))
            b=$(( (k >> 32) & 0xFFFF ))
            c=$(( (k >> 16) & 0xFFFF ))
            d=$(( k & 0xFFFF ))
            ;;
        112)
            tmp=$k
            e=$(( tmp & 0xFFFF ))
            tmp=$(( tmp >> 16 ))
            d=$(( tmp & 0xFFFF ))
            tmp=$(( tmp >> 16 ))
            c=$(( tmp & 0xFFFF ))
            tmp=$(( tmp >> 16 ))
            b=$(( tmp & 0xFFFF ))
            tmp=$(( tmp >> 16 ))
            a=$(( tmp & 0xFFFF ))
            ;;
        *)
            echo "unsupported prefix length: $plen" >&2
            (( fail++ ))
            continue
            ;;
    esac

    prefix=$(printf "2001:db8:%x:%x:%x:%x:%x::/%d" $a $b $c $d $e $plen)

    if ip -6 route "$ACTION" "$prefix" dev "$IFACE" proto 255 2>/dev/null; then
        (( added++ ))
    else
        (( fail++ ))
    fi
done

echo "$ACTION: $added succeeded, $fail failed (total attempted: $COUNT)"
