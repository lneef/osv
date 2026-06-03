include ../common.gmk

CXXFLAGS=-O3 -g -Wall -std=c++23 -fPIC $(COMMON) 
SRCS=$(wildcard *.cc)

.PHONY: module
module: microbench.so

quiet = $(if $V, $1, @echo " $2"; $1)

microbench.so: $(SRCS) $(TARGET)
	$(call quiet, $(CXX) $(CXXFLAGS) -shared -o $@ $(SRCS), LINK $@)

clean:
	rm -f microbench.so microbench.d microbench-stripped.so
