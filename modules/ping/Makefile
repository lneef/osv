include ../common.gmk

CXXFLAGS=-O2 -g -Wall -std=c++11 -fPIC $(COMMON) 
SRCS=ping.cc

.PHONY: module
module: ping.so

quiet = $(if $V, $1, @echo " $2"; $1)

ping.so: $(SRCS)
	$(call quiet, $(CXX) $(CXXFLAGS) -shared -o $@ $(SRCS), LINK $@)

clean:
	rm -f sysinfo.so sysinfo.d

