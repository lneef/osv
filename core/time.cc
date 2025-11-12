#include "osv/clock.hh"
#include <api/bypass/time.hh>
#include <cstdint>
#include <unistd.h>

uint64_t rte_get_timer_cycles(){
    return osv::clock::uptime::now().time_since_epoch().count();
}

uint64_t rte_get_timer_hz(){
    return NS_PER_S;
}

void rte_delay_us_block(unsigned int us){
    auto end = rte_get_timer_cycles() + us * NS_PER_US;
    while(rte_get_timer_cycles() < end)
#ifdef __x86_64__
		    __asm __volatile("pause");
#endif
#ifdef __aarch64__
		    __asm __volatile("isb sy");
#endif
}

void rte_delay_us_sleep(unsigned int us){
    usleep(us);
}
