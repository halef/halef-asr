all:
ifndef KALDI_SRC
$(error KALDI_SRC is not set)
endif

ifndef LIBWEB_BUILD
$(error LIBWEB_BUILD is not set)
endif

EXTRA_CXXFLAGS = -Wno-sign-compare -I${KALDI_SRC} -I${LIBWEB_BUILD} -I${LIBWEB_BUILD}/../lib
include ${KALDI_SRC}/kaldi.mk

LDFLAGS += $(CUDA_LDFLAGS) -DKALDI_NO_PORTAUDIO -DLWS_WITH_SSL=0
LDLIBS += $(CUDA_LDLIBS) -lz

BINFILES = STRM-ASR-server

OBJFILES =

# Add this dependency to force cuda-compiled.o to be rebuilt when we reconfigure.
cuda-compiled.o: ${KALDI_SRC}/kaldi.mk

TESTFILES =

ADDLIBS =	${LIBWEB_BUILD}/lib/libwebsockets.a \
			${KALDI_SRC}/online2/kaldi-online2.a \
			${KALDI_SRC}/nnet2/kaldi-nnet2.a \
			${KALDI_SRC}/ivector/kaldi-ivector.a \
			${KALDI_SRC}/fstext/kaldi-fstext.a \
			${KALDI_SRC}/feat/kaldi-feat.a \
			${KALDI_SRC}/decoder/kaldi-decoder.a \
			${KALDI_SRC}/cudamatrix/kaldi-cudamatrix.a \
			${KALDI_SRC}/transform/kaldi-transform.a \
			${KALDI_SRC}/tree/kaldi-tree.a \
			${KALDI_SRC}/hmm/kaldi-hmm.a \
			${KALDI_SRC}/gmm/kaldi-gmm.a \
			${KALDI_SRC}/lat/kaldi-lat.a \
			${KALDI_SRC}/util/kaldi-util.a \
			${KALDI_SRC}/matrix/kaldi-matrix.a \
			${KALDI_SRC}/base/kaldi-base.a

include ${KALDI_SRC}/makefiles/default_rules.mk
