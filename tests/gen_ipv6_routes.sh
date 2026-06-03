#!/bin/bash
# Generate 5000 IPv6 routes with prefix length <= 112 for babeld coalescing tests.
# Usage: sudo ./gen_ipv6_routes.sh [add|del] [interface]
#
# Routes are spread across 2001:db8::/32 (documentation prefix).
# Prefix lengths cycle through: 48, 56, 64, 80, 96, 112.

ACTION=${1:-add}
IFACE=${2:-lo}
COUNT=5000

PREFIXES=(48 56 64 80 96 112)
NPREFIX=${#PREFIXES[@]}

# Base: 2001:0db8:AAAA:BBBB:CCCC:DDDD::/plen
# We vary the 16-bit groups AAAA..DDDD to get unique routes.

added=0
fail=0

for ((i=0; i<COUNT; i++)); do
    # Spread across four 16-bit groups so routes are unique
    a=$(( (i >> 12) & 0xFFFF ))
    b=$(( (i >>  8) & 0xFFFF ))
    c=$(( (i >>  4) & 0xFFFF ))
    d=$(( (i      ) & 0xFFFF ))

    plen=${PREFIXES[$((i % NPREFIX))]}

    # Mask the host bits beyond plen so the prefix is valid.
    # Groups: 2001:0db8 = 32 bits, then a(16) b(16) c(16) d(16) = 96 bits total → 128
    # Bit positions of each group start: a=32, b=48, c=64, d=80
    if   (( plen <= 32 )); then a=0; b=0; c=0; d=0
    elif (( plen <= 48 )); then b=0; c=0; d=0; a=$(( a & (0xFFFF << (48-plen)) & 0xFFFF ))
    elif (( plen <= 64 )); then c=0; d=0; b=$(( b & (0xFFFF << (64-plen)) & 0xFFFF ))
    elif (( plen <= 80 )); then d=0; c=$(( c & (0xFFFF << (80-plen)) & 0xFFFF ))
    elif (( plen <= 96 )); then d=$(( d & (0xFFFF << (96-plen)) & 0xFFFF ))
    # plen <= 112: all four groups kept, host nibbles in d already within prefix
    fi

    prefix=$(printf "2001:db8:%x:%x:%x:%x::/%d" $a $b $c $d $plen)

    if ip route "$ACTION" "$prefix" dev "$IFACE" proto static 2>/dev/null; then
        (( added++ ))
    else
        (( fail++ ))
    fi
done

echo "$ACTION: $added succeeded, $fail failed (total attempted: $COUNT)"
