#!/bin/bash

trim() {
    local var="$*"
    # remove leading whitespace characters
    var="${var#"${var%%[![:space:]]*}"}"
    # remove trailing whitespace characters
    var="${var%"${var##*[![:space:]]}"}"
    printf '%s' "$var"
}

 > hostfile

NODELIST="$(scontrol show hostname $MYSLURM_SLAVES | paste -d -s)"
for node in $NODELIST; do
        echo $node
        nodeip=$(ssh $node hostname -I)
	ip=$(trim $nodeip)
	echo "$ip" >> hostfile
done;

