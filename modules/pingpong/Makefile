include ../common.gmk

CXXFLAGS=-O2 -g -Wall -std=c++11 -fPIC $(COMMON) 
SRCS=pong.cc

.PHONY: module
module: pong.so

quiet = $(if $V, $1, @echo " $2"; $1)

pong.so: $(SRCS)
	$(call quiet, $(CXX) $(CXXFLAGS) -shared -o $@ $(SRCS), LINK $@)

clean:
	rm -f pong.so pong.d
