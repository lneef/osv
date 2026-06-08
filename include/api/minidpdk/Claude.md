#  -- 
## OVERVIEW
MiniDPDK: DPDK like API layer for direct NIC hardware access.
MiniDPDK assumes a per core shared nothing architecture where each core has its local storage structures.
## STRUCTURE

```
bit.hh bit operation helpers
defs.hh definitions for offloading codes/packet types
dev.hh ethernet device interface
lcore.hh shim wrapper for DPDK's lcore API
mem.hh shim layer for DPDK's memopools
mem_pool.hh packet pool similar to rte_pktmbuf_pool relying on OSv primitives
net.hh packet headers
rss.hh RSS offloading definitions and setup structures
stack.hh stack implemenation safe to run on a single SMP core
time.hh tsc retrieval and delay/sleep
util.hh utility functions(atomic wrapper, prefetching, branch prediction, spinlock)
```
| Task | File | Notes |
|------|------|-------|
|mbuf allocation| mem_pool.hh mem.hh| DPDK shim layer forwards allocations to underlying MiniDPDK mem\_pool|
|Packet forwarding| stack.hh | Allows for thread safe forwarding on a single SMP core(IRQ->Cleanup). Not save for usage across multiple cores|

## ANTI-PATTERNS

- **Changing implemenation of primitives like atomics and barriers**
- **Passing packets across SMP-cores**
