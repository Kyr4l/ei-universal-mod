#!/bin/bash

nameToNid() {
    local name="$1"
    local nidHash=0
    local nidBase=1000000000

    # change characters to uppercase
    name=$(echo "$name" | tr '[:lower:]' '[:upper:]')

    local len=${#name}

    for ((i=1; i<=len; i++)); do
        # ascii character code
        local ch=$(printf "%d" "'${name:i-1:1}")

        # 5 ^ (len - i)
        local exponent=$((len - i))
        local temp=$(echo "5^$exponent" | bc)

        # uint32 overflow simulation
        temp=$((temp % 4294967296))

        # ch * temp with overflow
        temp=$(((ch * temp) % 4294967296))

        # add hash with overflow
        nidHash=$(((nidHash + temp) % 4294967296))
    done

    # modulo 1_000_000_000
    if (( nidHash > nidBase )); then
        nidHash=$((nidHash % nidBase))
    fi

    # final NID
    echo $((nidHash + nidBase))
}

#  usage
if [ -z "$1" ]; then
    echo "Usage: $0 <NPC Name>"
    echo "Example: $0 Merc1"
else
    nid=$(nameToNid "$1")
    echo "Name: $1"
    echo "NID: $nid"
fi
