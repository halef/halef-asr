#!/bin/bash

set -e

export LOGDIR=<PATH-TO-LOGDIR>

export BASEDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CONFDIR="$BASEDIR/<PATH-TO>/conf"
iVECTORDIR="$BASEDIR/<PATH-TO>/ivector_extractor"
MODELDIR=$BASEDIR/<PATH-TO-AM>
GRAPHDIR=$MODELDIR/<PATH-TO-WFST>
TMPDIR=$BASEDIR/tmp
BINDIR=$BASEDIR/<PATH-TO-BIN>

model=$MODELDIR/final.mdl
graph=$GRAPHDIR/hclg.fst
words=$GRAPHDIR/words.txt
port=XXXX
AUDIODIR=<PATH-TO-SAVE-AUDIO-RECORDINGS>

mkdir -p $LOGDIR
mkdir -p $AUDIODIR

if [ ! -d "$TMPDIR" ]; then
	mkdir -p $TMPDIR
fi

nohup ${BINDIR}/STRM-ASR-server --port=${port} \
--pid-filename=${TMPDIR}/pid-port-${port} \
--config=${CONFDIR}/online_nnet2_decoding.conf \
--online=true --do-endpointing=false --max-active=7000 --beam=15.0 \
--lattice-beam=6.0 --acoustic-scale=0.1 \
--word-symbol-table=${words} \
--audio-dir=${AUDIODIR} \
${model} ${graph} > ${LOGDIR}/nohup${port}.out 2>&1   &
