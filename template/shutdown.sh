#!/bin/bash


BINDIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
PARENT=$(dirname ${BINDIR})
TMPDIR=$BINDIR/tmp

if [ -d "$TMPDIR" ]; then
    pidToKill=$(head -n 1 ./tmp/pid-port-*)
    if [[ $pidToKill == *"=="* ]]; then   
	echo "ERROR: There is more than 1 pid file in ./tmp"
	exit -1
    else
	kill -s 15 $pidToKill > /dev/null 2>&1;
	sleep 2
	kill -s 9 $pidToKill > /dev/null 2>&1;
    fi
       
fi
