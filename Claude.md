#  -- 
## OVERVIEW
We want to port the packet processing benchmark in ~/netbench/ for OSv. The implementation in this dir serves as a reference.
## STRUCTURE

```
modules keeps osv modules (actual applications)
modules/microbench microbenchmark for packet processing 
include/api/minidpdk minidpdk implementation providing a dpdk like direct NIC access interface for apps
include/api/minidpdk/dev.hh contains device functions for configuration/packet submission/retrieval and RSS setup
include/api/minidpdk/lcore.hh lcore setup binding OSV-Cores
include/api/minidpdk/rss.hh RSS type definitions and structures
include/api/minidpdk/time.hh Timer cycles info and delay/sleep functionioality
```

## WHERE TO LOOK

| Reference | File | Notes |
|------|------|-------|
|Receive side|~/netbench/pong.cc|DPDK implementation for recv and pong|
|DPDK-reference port setup |~/netbench/port.(h\|cc)|dpdk setup process for the port)|
|Utility functions for setup |~/netbench/util.h| Setup utilities eg to avoid false sharing|

## ANTI-PATTERNS

- **Do not copy one by one** 
- **Do not rewrite anything in the reference or outside of microbench**
- **Do not include any third party libs like hdr_hist from the reference (Ignore them)**

## NOTES
MiniDPDK is a shim API-layer for DPDK. 

## Building and Testing
You cannot run the app on this system however building is possible: 

``` 
podman exec osv-compiler bash -lc './scripts/build image=microbench -j 8'
```
