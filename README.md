# Cassandra ASR

There are various problems with the current implementation. It is based on very old KALDI and libwebsockets and ixes coding syles. Functionally, there is no way to safely terminate an ongoing recognition event and it only supports a single stream at a time.

## Requires:

- Kaldi 5.2
- Libwebsocket v1.5-stable

## Build

Build libwebsockets and kaldi first.

You need to set the following variables pointing to:
  - export LIBWEB_BUILD=<path-to-libwebsocket-build-dir>
  - export KALDI_SRC=<path-to-kaldi-source-dir>

Then you can run `make`.

## TODO
- Fix problems outlined above. 
- Refine build script to build into build dir. 
- Maybe move env vars into ./configure script. 
