include ../common.gmk

CXXFLAGS=-O2 -g -Wall -std=c++11 -fPIC $(COMMON) 
SRCS=pingpong.cc

.PHONY: module
module: pingpong.so

quiet = $(if $V, $1, @echo " $2"; $1)

pingpong.so: $(SRCS)
	$(call quiet, $(CXX) $(CXXFLAGS) -shared -o $@ $(SRCS), LINK $@)

clean:
	rm -f pingpong.so pingpong.d pingpong-stripped.so
