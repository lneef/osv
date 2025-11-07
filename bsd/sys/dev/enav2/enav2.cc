/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015-2021 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cerrno>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/smp.h>
#include <bsd/sys/sys/socket.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/in_cksum.h>
#include <machine/resource.h>

//#define ENA_LOG_ENABLE 1
//#define ENA_LOG_IO_ENABLE 1

#include "enav2.h"
#include <api/bypass/pktbuf.hh>
#include <api/bypass/net_eth.hh>
#include "ena_datapath.h"

#include <osv/mmu.hh>
#include <osv/mempool.hh>
#include <osv/aligned_new.hh>
#include <osv/msi.hh>
#include <osv/sched.hh>
#include <osv/trace.hh>


int ena_log_level = ENA_DBG;

static inline void critical_enter()  { sched::preempt_disable(); }
static inline void critical_exit() { sched::preempt_enable(); }

#include <sys/buf_ring.h>

/*********************************************************
 *  Function prototypes
 *********************************************************/
static void ena_intr_msix_mgmnt(void *);
static void ena_free_pci_resources(ena_adapter *);
static inline void ena_alloc_counters(counter_u64_t *, int);
static inline void ena_free_counters(counter_u64_t *, int);
static inline void ena_reset_counters(counter_u64_t *, int);
static void ena_init_rings(ena_adapter *);
static int ena_setup_tx_resources(struct ena_adapter *, int);
static void ena_free_tx_resources(struct ena_adapter *, int);
static int ena_setup_rx_resources(struct ena_adapter *, unsigned int);
static void ena_free_rx_resources(struct ena_adapter *, unsigned int);
static inline int ena_alloc_rx_pbuf(struct ena_adapter *, struct ena_ring *,
    struct ena_rx_buffer *);
static void ena_free_rx_bufs(struct ena_adapter *, unsigned int);
static void ena_free_tx_bufs(struct ena_adapter *, unsigned int);
static void ena_destroy_all_tx_queues(struct ena_adapter *);
static void ena_destroy_all_rx_queues(struct ena_adapter *);
static void ena_destroy_all_io_queues(struct ena_adapter *);
static int ena_enable_msix(struct ena_adapter *);
static void ena_setup_mgmnt_intr(struct ena_adapter *);
static int ena_request_mgmnt_irq(struct ena_adapter *);
static void ena_free_mgmnt_irq(struct ena_adapter *);
static void ena_disable_msix(struct ena_adapter *);
static int ena_up_complete(struct ena_adapter *);
static ena_offloads ena_get_dev_offloads(struct ena_com_dev_get_features_ctx *);
static int ena_set_queues_placement_policy(pci::device *, struct ena_com_dev *,
    struct ena_admin_feature_llq_desc *, struct ena_llq_configurations *);
static uint32_t ena_calc_max_io_queue_num(pci::device *, struct ena_com_dev *,
    struct ena_com_dev_get_features_ctx *);
static int ena_calc_io_queue_size(struct ena_calc_queue_size_ctx *);
static void ena_config_host_info(struct ena_com_dev *, pci::device*);
static int ena_device_init(struct ena_adapter *, pci::device *,
    struct ena_com_dev_get_features_ctx *, int *);
static int ena_enable_msix_and_set_admin_interrupts(struct ena_adapter *);
static void ena_update_on_link_change(void *, struct ena_admin_aenq_entry *);
static void unimplemented_aenq_handler(void *, struct ena_admin_aenq_entry *);
static void ena_timer_service(void *);
static int ena_create_io_queue(ena_eth_dev *data, ena_ring *ring);


#ifdef ENA_LOG_ENABLE
static char ena_version[] = ENA_DEVICE_NAME ENA_DRV_MODULE_NAME
    " v" ENA_DRV_MODULE_VERSION;
#endif

static ena_vendor_info_t ena_vendor_info_array[] = {
	{ PCI_VENDOR_ID_AMAZON, PCI_DEV_ID_ENA_PF, 0 },
	{ PCI_VENDOR_ID_AMAZON, PCI_DEV_ID_ENA_PF_RSERV0, 0 },
	{ PCI_VENDOR_ID_AMAZON, PCI_DEV_ID_ENA_VF, 0 },
	{ PCI_VENDOR_ID_AMAZON, PCI_DEV_ID_ENA_VF_RSERV0, 0 },
	/* Last entry */
	{ 0, 0, 0 }
};

static __inline __pure2 int
flsl(long mask)
{
	return (mask == 0 ? 0 :
	    8 * sizeof(mask) - __builtin_clzl((u_long)mask));
}

struct sx ena_global_lock;


int
ena_dma_alloc(device_t dmadev, bus_size_t size, ena_mem_handle_t *dma,
    int mapflags, bus_size_t alignment, int domain)
{
	dma->vaddr = (caddr_t)memory::alloc_phys_contiguous_aligned(size, mmu::page_size);
	if (!dma->vaddr) {
		ena_log(pdev, ERR, "memory::alloc_phys_contiguous_aligned failed!", 1);
		dma->vaddr = 0;
		dma->paddr = 0;
		return ENA_COM_NO_MEM;
	}

	dma->paddr = mmu::virt_to_phys(dma->vaddr);

	return (0);
}

static void
ena_free_pci_resources(ena_adapter *adapter)
{
	if (adapter->registers != NULL) {
		adapter->registers->unmap();
	}
}

bool
ena_probe(pci::device* pdev)
{
	ena_vendor_info_t *ent = ena_vendor_info_array;
	while (ent->vendor_id != 0) {
		if (pdev->get_id() == hw_device_id(ent->vendor_id, ent->device_id)) {
			ena_log_raw(DBG, "vendor=%x device=%x", ent->vendor_id,
			    ent->device_id);

			return true;
		}
		ent++;
	}

	return false;
}

/*
static int
ena_change_mtu(ena_eth_dev* dev, int new_mtu)
{
	ena_adapter *adapter = static_cast<ena_adapter*>(dev->adapter);
	int rc;

	if ((new_mtu > adapter->max_mtu) || (new_mtu < ENA_MIN_MTU)) {
		ena_log(pdev, ERR, "Invalid MTU setting. new_mtu: %d max mtu: %d min mtu: %d",
		    new_mtu, adapter->max_mtu, ENA_MIN_MTU);
		return (EINVAL);
	}

	rc = ena_com_set_dev_mtu(adapter->ena_dev, new_mtu);
	if (likely(rc == 0)) {
		ena_log(pdev, DBG, "set MTU to %d", new_mtu);
		dev->mtu = new_mtu;
	} else {
		ena_log(pdev, ERR, "Failed to set MTU to %d", new_mtu);
	}

	return (rc);
}

*/

//Later todo - Disable counters for now
static inline void
ena_alloc_counters(counter_u64_t *begin, int size)
{
/*	counter_u64_t *end = (counter_u64_t *)((char *)begin + size);

	for (; begin < end; ++begin)
		*begin = counter_u64_alloc(M_WAITOK);*/
}

static inline void
ena_free_counters(counter_u64_t *begin, int size)
{
/*	counter_u64_t *end = (counter_u64_t *)((char *)begin + size);

	for (; begin < end; ++begin)
		counter_u64_free(*begin);*/
}

static inline void
ena_reset_counters(counter_u64_t *begin, int size)
{
/*	counter_u64_t *end = (counter_u64_t *)((char *)begin + size);

	for (; begin < end; ++begin)
		counter_u64_zero(*begin);*/
}

/**
 * ena_setup_tx_resources - allocate Tx resources (Descriptors)
 * @adapter: network interface device structure
 * @qid: queue index
 *
 * Returns 0 on success, otherwise on failure.
 **/
static int
ena_setup_tx_resources(struct ena_adapter *adapter, int qid)
{
	ena_ring *tx_ring = &adapter->tx_ring[qid];
	int size, i;

	size = sizeof(struct ena_tx_buffer) * tx_ring->ring_size;

	tx_ring->tx_buffer_info = static_cast<ena_tx_buffer*>(aligned_alloc(alignof(ena_tx_buffer), size));
	if (unlikely(tx_ring->tx_buffer_info == NULL))
		return (ENOMEM);
	bzero(tx_ring->tx_buffer_info, size);

	size = sizeof(uint16_t) * tx_ring->ring_size;
	tx_ring->free_tx_ids = static_cast<uint16_t*>(malloc(size, M_DEVBUF, M_NOWAIT | M_ZERO));
	if (unlikely(tx_ring->free_tx_ids == NULL))
		goto err_buf_info_free;

	size = tx_ring->tx_max_header_size;
	tx_ring->push_buf_intermediate_buf = static_cast<uint8_t*>(malloc(size, M_DEVBUF,
	    M_NOWAIT | M_ZERO));
	if (unlikely(tx_ring->push_buf_intermediate_buf == NULL))
		goto err_tx_ids_free;

	/* Req id stack for TX OOO completions */
	for (i = 0; i < tx_ring->ring_size; i++)
		tx_ring->free_tx_ids[i] = i;

	/* Reset TX statistics. */
	ena_reset_counters((counter_u64_t *)&tx_ring->tx_stats,
	    sizeof(tx_ring->tx_stats));

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;
	tx_ring->acum_pkts = 0;

	return (0);

err_tx_ids_free:
	free(tx_ring->free_tx_ids, M_DEVBUF);
	tx_ring->free_tx_ids = NULL;
err_buf_info_free:
	free(tx_ring->tx_buffer_info, M_DEVBUF);
	tx_ring->tx_buffer_info = NULL;

	return (ENOMEM);
}

/**
 * ena_free_tx_resources - Free Tx Resources per Queue
 * @adapter: network interface device structure
 * @qid: queue index
 *
 * Free all transmit software resources
 **/
static void
ena_free_tx_resources(ena_adapter *adapter, int qid)
{
	ena_ring *tx_ring = &adapter->tx_ring[qid];
  pkt_buf *p;

	/* Free pbufs */
	for (int i = 0; i < tx_ring->ring_size; i++) {
    p = tx_ring->tx_buffer_info[i].pbuf;  
		p->pool->free_bulk(&p, 1);
		tx_ring->tx_buffer_info[i].pbuf = NULL;
	}

	/* And free allocated memory. */
	free(tx_ring->tx_buffer_info, M_DEVBUF);
	tx_ring->tx_buffer_info = NULL;

	free(tx_ring->free_tx_ids, M_DEVBUF);
	tx_ring->free_tx_ids = NULL;

	free(tx_ring->push_buf_intermediate_buf, M_DEVBUF);
	tx_ring->push_buf_intermediate_buf = NULL;
}

static void ena_free_all_tx_resources(ena_adapter *adapter){
    ena_eth_dev *data = adapter->eth_dev;
    uint16_t i;
    for(i = 0; i < data->nb_tx_queue; ++i){
        if(!adapter->tx_ring[i].configured)
            continue;
        ena_free_tx_resources(adapter, i);
    }
}

/**
 * ena_setup_rx_resources - allocate Rx resources (Descriptors)
 * @adapter: network interface device structure
 * @qid: queue index
 *
 * Returns 0 on success, otherwise on failure.
 **/
static int
ena_setup_rx_resources(ena_adapter *adapter, unsigned int qid)
{
  ena_ring *rx_ring = &adapter->rx_ring[qid];
	int size, i;

	size = sizeof(struct ena_rx_buffer) * rx_ring->ring_size;

	/*
	 * Alloc extra element so in rx path
	 * we can always prefetch rx_info + 1
	 */
	size += sizeof(struct ena_rx_buffer);

	rx_ring->rx_buffer_info = static_cast<ena_rx_buffer*>(aligned_alloc(alignof(ena_rx_buffer), size));
	bzero(rx_ring->rx_buffer_info, size);

	size = sizeof(uint16_t) * rx_ring->ring_size;
	rx_ring->free_rx_ids = static_cast<uint16_t*>(malloc(size, M_DEVBUF, M_WAITOK));

	for (i = 0; i < rx_ring->ring_size; i++)
		rx_ring->free_rx_ids[i] = i;

	/* Reset RX statistics. */
	ena_reset_counters((counter_u64_t *)&rx_ring->rx_stats,
	    sizeof(rx_ring->rx_stats));

	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;

	return (0);
}

/**
 * ena_free_rx_resources - Free Rx Resources
 * @adapter: network interface device structure
 * @qid: queue index
 *
 * Free all receive software resources
 **/
static void
ena_free_rx_resources(struct ena_adapter *adapter, unsigned int qid)
{
	struct ena_ring *rx_ring = &adapter->rx_ring[qid];
  pkt_buf *p;
	/* Free buffer DMA maps, */
	for (int i = 0; i < rx_ring->ring_size; i++) {
    p = rx_ring->rx_buffer_info[i].pbuf; 
		p->pool->free_bulk(&p, 1);
		rx_ring->rx_buffer_info[i].pbuf = NULL;
	}

	/* free allocated memory */
	free(rx_ring->rx_buffer_info, M_DEVBUF);
	rx_ring->rx_buffer_info = NULL;

	free(rx_ring->free_rx_ids, M_DEVBUF);
	rx_ring->free_rx_ids = NULL;
}

static void ena_free_all_rx_resources(ena_adapter *adapter){
    eth_dev *data = adapter->eth_dev;
    uint16_t i;
    for(i = 0; i < data->nb_tx_queue; ++i){
        if(!adapter->rx_ring[i].configured)
            continue;
        ena_free_rx_resources(adapter, i);
    }
}

static inline int
ena_alloc_rx_pbuf(struct ena_adapter *adapter, struct ena_ring *rx_ring,
    struct ena_rx_buffer *rx_info)
{
	struct ena_com_buf *ena_buf;

	/* if previous allocated frag is not used */
	if (unlikely(rx_info->pbuf != NULL))
		return (0);

	/* Get pbuf using UMA allocator */
 rx_ring->pool->alloc_bulk(&rx_info->pbuf, 1);

	if (unlikely(rx_info->pbuf == NULL)) 
			return (ENOMEM);

	/* Set pbuf length*/
	ena_buf = &rx_info->ena_buf;
	ena_buf->paddr = mmu::virt_to_phys(rx_info->pbuf->buf);
	ena_buf->len = rx_info->pbuf->buf_len;

	return (0);
}

static void
ena_free_rx_pbuf(struct ena_adapter *adapter, struct ena_ring *rx_ring,
    struct ena_rx_buffer *rx_info)
{
	if (rx_info->pbuf == NULL) {
		ena_log(adapter->pdev, WARN,
		    "Trying to free unallocated buffer");
		return;
	}
  rx_info->pbuf->pool->free_bulk(&rx_info->pbuf, 1);
	rx_info->pbuf = NULL;
}

/**
 * ena_refill_rx_bufs - Refills ring with descriptors
 * @rx_ring: the ring which we want to feed with free descriptors
 * @num: number of descriptors to refill
 * Refills the ring with newly allocated DMA-mapped pbufs for receiving
 **/
int
ena_refill_rx_bufs(struct ena_ring *rx_ring, uint32_t num)
{
	struct ena_adapter *adapter = rx_ring->adapter;
	uint16_t next_to_use, req_id;
	uint32_t i;
	int rc;

	ena_log_io(adapter->pdev, INFO, "refill qid: %d", rx_ring->qid);

	next_to_use = rx_ring->next_to_use;

	for (i = 0; i < num; i++) {
		struct ena_rx_buffer *rx_info;

		ena_log_io(pdev, DBG, "RX buffer - next to use: %d",
			next_to_use);

		req_id = rx_ring->free_rx_ids[next_to_use];
		rx_info = &rx_ring->rx_buffer_info[req_id];

		rc = ena_alloc_rx_pbuf(adapter, rx_ring, rx_info);
		if (unlikely(rc != 0)) {
			ena_log_io(pdev, WARN,
			    "failed to alloc buffer for rx queue %d",
			    rx_ring->qid);
			break;
		}
		rc = ena_com_add_single_rx_desc(rx_ring->ena_com_io_sq,
		    &rx_info->ena_buf, req_id);
		if (unlikely(rc != 0)) {
			ena_log_io(pdev, WARN,
			    "failed to add buffer for rx queue %d",
			    rx_ring->qid);
			break;
		}
		next_to_use = ENA_RX_RING_IDX_NEXT(next_to_use,
		    rx_ring->ring_size);
	}
	ena_log_io(pdev, INFO,
	    "allocated %d RX BUFs", num);

	if (unlikely(i < num)) {
		counter_u64_add(rx_ring->rx_stats.refil_partial, 1);
		ena_log_io(pdev, WARN,
		    "refilled rx qid %d with only %d mbufs (from %d)",
		    rx_ring->qid, i, num);
	}

	if (likely(i != 0))
		ena_com_write_sq_doorbell(rx_ring->ena_com_io_sq);

	rx_ring->next_to_use = next_to_use;
	return (i);
}

static void
ena_free_rx_bufs(struct ena_adapter *adapter, unsigned int qid)
{
	struct ena_ring *rx_ring = &adapter->rx_ring[qid];
	unsigned int i;

	for (i = 0; i < rx_ring->ring_size; i++) {
		struct ena_rx_buffer *rx_info = &rx_ring->rx_buffer_info[i];

		if (rx_info->pbuf != NULL)
			ena_free_rx_pbuf(adapter, rx_ring, rx_info);
	}
}

/**
 * ena_free_tx_bufs - Free Tx Buffers per Queue
 * @adapter: network interface device structure
 * @qid: queue index
 **/
static void
ena_free_tx_bufs(struct ena_adapter *adapter, unsigned int qid)
{
	bool print_once = true;
	struct ena_ring *tx_ring = &adapter->tx_ring[qid];

	for (int i = 0; i < tx_ring->ring_size; i++) {
		struct ena_tx_buffer *tx_info = &tx_ring->tx_buffer_info[i];

		if (tx_info->pbuf == NULL)
			continue;

		if (print_once) {
			ena_log(adapter->pdev, WARN,
			    "free uncompleted tx pbuf qid %d idx 0x%x", qid,
			    i);
			print_once = false;
		} else {
			ena_log(adapter->pdev, DBG,
			    "free uncompleted tx pbuf qid %d idx 0x%x", qid,
			    i);
		}

    tx_info->pbuf->pool->free_bulk(&tx_info->pbuf, 1);
		tx_info->pbuf = NULL;
	}
}

static void
ena_destroy_all_tx_queues(struct ena_adapter *adapter)
{
	uint16_t ena_qid;
	int i;
  auto *data = static_cast<eth_dev*>(adapter->eth_dev);

	for (i = 0; i < data->nb_tx_queue; i++) {
    if(!adapter->tx_ring[i].configured)  
        continue;
		ena_qid = ENA_IO_TXQ_IDX(i);
		ena_com_destroy_io_queue(adapter->ena_dev, ena_qid);
	}
}

static void
ena_destroy_all_rx_queues(struct ena_adapter *adapter)
{
	uint16_t ena_qid;
	int i;
  auto *data = static_cast<eth_dev*>(adapter->eth_dev);

	for (i = 0; i < data->nb_rx_queue; i++) {
    if(!adapter->rx_ring[i].configured)
        continue;
		ena_qid = ENA_IO_RXQ_IDX(i);
		ena_com_destroy_io_queue(adapter->ena_dev, ena_qid);
	}
}

static void
ena_destroy_all_io_queues(struct ena_adapter *adapter)
{
	ena_destroy_all_tx_queues(adapter);
	ena_destroy_all_rx_queues(adapter);
}

/*********************************************************************
 *
 *  MSIX & Interrupt Service routine
 *
 **********************************************************************/

/**
 * ena_intr_msix_mgmnt - MSIX Interrupt Handler for admin/async queue
 * @arg: interrupt number
 **/
static void
ena_intr_msix_mgmnt(void *arg)
{
	struct ena_adapter *adapter = (struct ena_adapter *)arg;

	ena_com_admin_q_comp_intr_handler(adapter->ena_dev);
	if (likely(ENA_FLAG_ISSET(ENA_FLAG_DEVICE_RUNNING, adapter)))
		ena_com_aenq_intr_handler(adapter->ena_dev, arg);
}

static int
ena_enable_msix(ena_adapter *adapter)
{
	pci::device *dev = adapter->pdev;

	if (ENA_FLAG_ISSET(ENA_FLAG_MSIX_ENABLED, adapter)) {
		ena_log(dev, ERR, "Error, MSI-X is already enabled");
		return (EINVAL);
	}

	/* Reserved the max msix vectors we might need */
  /* Right now only polling */
	int msix_vecs = ENA_MAX_MSIX_VEC(0);

	ena_log(dev, DBG, "trying to enable MSI-X, vectors: %d", msix_vecs);

	dev->set_bus_master(true);
	dev->msix_enable();
	assert(dev->is_msix());
  ena_log(dev, DBG, "%u\n", dev->msix_get_num_entries());

	if (msix_vecs > dev->msix_get_num_entries()) {
		if (msix_vecs == ENA_ADMIN_MSIX_VEC) {
			ena_log(dev, ERR,
			    "Not enough number of MSI-x allocated: %d",
			    msix_vecs);
			dev->msix_disable();
			return ENOSPC;
		}
		ena_log(dev, ERR,
		    "Enable only %d MSI-x (out of %d), reduce "
		    "the number of queues",
		    msix_vecs, dev->msix_get_num_entries());
	}

	adapter->msix_vecs = msix_vecs;
	ENA_FLAG_SET_ATOMIC(ENA_FLAG_MSIX_ENABLED, adapter);

	return (0);
}

static void
ena_setup_mgmnt_intr(struct ena_adapter *adapter)
{
	adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].data = adapter;
	adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].vector = ENA_MGMNT_IRQ_IDX;
	adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].mvector = nullptr;
}

static int
ena_request_mgmnt_irq(struct ena_adapter *adapter)
{
	interrupt_manager _msi(adapter->pdev);

	std::vector<msix_vector*> assigned = _msi.request_vectors(1);
	if (assigned.size() != 1) {
		_msi.free_vectors(assigned);
		ena_log(pdev, ERR, "could not request MGMNT irq vector: %d", ENA_MGMNT_IRQ_IDX);
		return (ENXIO);
	}

	auto vec = assigned[0];
	if (!_msi.assign_isr(vec, [adapter]() { ena_intr_msix_mgmnt(adapter); })) {
		_msi.free_vectors(assigned);
		ena_log(pdev, ERR, "could not assign MGMNT irq vector isr: %d", ENA_MGMNT_IRQ_IDX);
		return (ENXIO);
	}

	if (!_msi.setup_entry(ENA_MGMNT_IRQ_IDX, vec)) {
		_msi.free_vectors(assigned);
		ena_log(pdev, ERR, "could not setup MGMNT irq vector entry: %d", ENA_MGMNT_IRQ_IDX);
		return (ENXIO);
	}

	//Save assigned msix vector
	adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].mvector = assigned[0];
	_msi.unmask_interrupts(assigned);

	return 0;
}

static void
ena_free_mgmnt_irq(struct ena_adapter *adapter)
{
	ena_irq *irq = &adapter->irq_tbl[ENA_MGMNT_IRQ_IDX];
	if (irq->mvector) {
		delete irq->mvector;
		irq->mvector = nullptr;
	}
}

static void
ena_free_irqs(struct ena_adapter *adapter)
{
	ena_free_mgmnt_irq(adapter);
	ena_disable_msix(adapter);
}

static void
ena_disable_msix(struct ena_adapter *adapter)
{
	if (ENA_FLAG_ISSET(ENA_FLAG_MSIX_ENABLED, adapter)) {
		ENA_FLAG_CLEAR_ATOMIC(ENA_FLAG_MSIX_ENABLED, adapter);
		adapter->pdev->msix_disable();
	}

	adapter->msix_vecs = 0;
}

static int
ena_up_complete(struct ena_adapter *adapter)
{
  auto* eth_dev = adapter->eth_dev;  
  for(uint16_t i = 0; i < eth_dev->nb_rx_queue; ++i)
      ena_refill_rx_bufs(&adapter->rx_ring[i], adapter->rx_ring[i].ring_size);
	ena_reset_counters((counter_u64_t *)&adapter->hw_stats,
	    sizeof(adapter->hw_stats));

	return (0);
}

void ena_get_dev_info(eth_dev_info *info, eth_dev *data){
    auto *adapter = static_cast<ena_adapter*>(data->adapter);
    auto offloads = adapter->offload_cap;
    info->rx_offload_capa = offloads.rx_offloads;
    info->tx_offload_capa = offloads.tx_offloads;
}

int ena_start(ena_adapter *adapter){
    int rc = 0;
    uint16_t i;
    ena_eth_dev *data = adapter->eth_dev;
	  if (ENA_FLAG_ISSET(ENA_FLAG_DEV_UP, adapter))
		    return (0);

	  ena_log(adapter->pdev, INFO, "device is going UP");

	  ena_log(adapter->pdev, INFO,
	        "Creating %u Tx, %u RxIO queues. Rx queue size: %d, Tx queue size: %d, LLQ is %s",
	      adapter->eth_dev->nb_tx_queue,
        adapter->eth_dev->nb_rx_queue,
	      adapter->requested_rx_ring_size,
	      adapter->requested_tx_ring_size,
	      (adapter->ena_dev->tx_mem_queue_type ==
    ENA_ADMIN_PLACEMENT_POLICY_DEV) ? "ENABLED" : "DISABLED");

	counter_u64_add(adapter->dev_stats.interface_up, 1);

	ENA_FLAG_SET_ATOMIC(ENA_FLAG_DEV_UP, adapter);

    for(i = 0; i < data->nb_tx_queue; ++i){
        rc = ena_create_io_queue(data, &adapter->tx_ring[i]);
        if(unlikely(rc))
            goto err_start_complete;
    }
    for(i = 0; i < data->nb_rx_queue; ++i){
        rc = ena_create_io_queue(data, &adapter->rx_ring[i]);
        if(unlikely(rc))
            goto err_start_complete;
    }
    rc = ena_up_complete(adapter);
    if(unlikely(rc != 0))
        goto err_start_complete;

    return 0;
err_start_complete:
    ena_destroy_all_io_queues(adapter);
    return rc;
}


static ena_offloads 
ena_get_dev_offloads(ena_com_dev_get_features_ctx *feat)
{
	ena_offloads offloads;

	if ((feat->offload.tx &
	  ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_FULL_MASK) != 0)
		offloads.tx_offloads |= PBUF_OFFLOAD_IPV4_CKSUM;
	if ((feat->offload.tx &
	     ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L3_CSUM_IPV4_MASK) != 0)
		offloads.tx_offloads |= PBUF_OFFLOAD_UDP_CKSUM;

	if ((feat->offload.rx_supported &
	    ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV4_CSUM_MASK) != 0)
		offloads.rx_offloads |= PBUF_OFFLOAD_IPV4_CKSUM;

	if ((feat->offload.rx_supported &
	    ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L3_CSUM_IPV4_MASK) != 0)
		offloads.rx_offloads |= PBUF_OFFLOAD_UDP_CKSUM;

	return offloads;
}

void ena_stop(ena_eth_dev *data, ena_adapter *adapter){
	int rc;

	if (!ENA_FLAG_ISSET(ENA_FLAG_DEV_UP, adapter))
		return;

	ena_log(adapter->pdev, INFO, "device is going DOWN");

	ENA_FLAG_CLEAR_ATOMIC(ENA_FLAG_DEV_UP, adapter);

	if (ENA_FLAG_ISSET(ENA_FLAG_TRIGGER_RESET, adapter)) {
		rc = ena_com_dev_reset(adapter->ena_dev, adapter->reset_reason);
		if (unlikely(rc != 0))
			ena_log(adapter->pdev, ERR, "Device reset failed");
	}

	ena_destroy_all_io_queues(adapter);
  for(uint16_t i = 0; i < data->nb_rx_queue; ++i)
      ena_free_rx_bufs(adapter, i);
  for(uint16_t i = 0; i < data->nb_tx_queue; ++i)
      ena_free_tx_bufs(adapter, i);
	counter_u64_add(adapter->dev_stats.interface_down, 1);

}

static uint32_t
ena_calc_max_io_queue_num(pci::device *pdev, struct ena_com_dev *ena_dev,
    struct ena_com_dev_get_features_ctx *get_feat_ctx)
{
	uint32_t io_tx_sq_num, io_tx_cq_num, io_rx_num, max_num_io_queues;

	/* Regular queues capabilities */
	if (ena_dev->supported_features & BIT(ENA_ADMIN_MAX_QUEUES_EXT)) {
		struct ena_admin_queue_ext_feature_fields *max_queue_ext =
		    &get_feat_ctx->max_queue_ext.max_queue_ext;
		io_rx_num = min_t(int, max_queue_ext->max_rx_sq_num,
		    max_queue_ext->max_rx_cq_num);

		io_tx_sq_num = max_queue_ext->max_tx_sq_num;
		io_tx_cq_num = max_queue_ext->max_tx_cq_num;
	} else {
		struct ena_admin_queue_feature_desc *max_queues =
		    &get_feat_ctx->max_queues;
		io_tx_sq_num = max_queues->max_sq_num;
		io_tx_cq_num = max_queues->max_cq_num;
		io_rx_num = min_t(int, io_tx_sq_num, io_tx_cq_num);
	}

	max_num_io_queues = min_t(uint32_t, mp_ncpus, ENA_MAX_NUM_IO_QUEUES);
	max_num_io_queues = min_t(uint32_t, max_num_io_queues, io_rx_num);
	max_num_io_queues = min_t(uint32_t, max_num_io_queues, io_tx_sq_num);
	max_num_io_queues = min_t(uint32_t, max_num_io_queues, io_tx_cq_num);
	/* 1 IRQ for mgmnt and 1 IRQ for each TX/RX pair */
	max_num_io_queues = min_t(uint32_t, max_num_io_queues, pdev->msix_get_num_entries() - 1);

	return (max_num_io_queues);
}

static int
ena_set_queues_placement_policy(pci::device *pdev, struct ena_com_dev *ena_dev,
    struct ena_admin_feature_llq_desc *llq,
    struct ena_llq_configurations *llq_default_configurations)
{
	//We do NOT support LLQ
	ena_dev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;

	ena_log(pdev, INFO,
		"LLQ is not supported. Using the host mode policy.");
	return (0);
}

static int
ena_calc_io_queue_size(struct ena_calc_queue_size_ctx *ctx)
{
	struct ena_com_dev *ena_dev = ctx->ena_dev;
	uint32_t tx_queue_size = ENA_DEFAULT_RING_SIZE;
	uint32_t rx_queue_size = ENA_DEFAULT_RING_SIZE;
	uint32_t max_tx_queue_size;
	uint32_t max_rx_queue_size;

	if (ena_dev->supported_features & BIT(ENA_ADMIN_MAX_QUEUES_EXT)) {
		struct ena_admin_queue_ext_feature_fields *max_queue_ext =
		    &ctx->get_feat_ctx->max_queue_ext.max_queue_ext;
		max_rx_queue_size = min_t(uint32_t,
		    max_queue_ext->max_rx_cq_depth,
		    max_queue_ext->max_rx_sq_depth);
		max_tx_queue_size = max_queue_ext->max_tx_cq_depth;

		max_tx_queue_size = min_t(uint32_t, max_tx_queue_size,
		    max_queue_ext->max_tx_sq_depth);

		ctx->max_tx_sgl_size = min_t(uint16_t, ENA_PKT_MAX_BUFS,
		    max_queue_ext->max_per_packet_tx_descs);
		ctx->max_rx_sgl_size = min_t(uint16_t, ENA_PKT_MAX_BUFS,
		    max_queue_ext->max_per_packet_rx_descs);
	} else {
		struct ena_admin_queue_feature_desc *max_queues =
		    &ctx->get_feat_ctx->max_queues;
		max_rx_queue_size = min_t(uint32_t, max_queues->max_cq_depth,
		    max_queues->max_sq_depth);
		max_tx_queue_size = max_queues->max_cq_depth;

		max_tx_queue_size = min_t(uint32_t, max_tx_queue_size,
		    max_queues->max_sq_depth);

		ctx->max_tx_sgl_size = min_t(uint16_t, ENA_PKT_MAX_BUFS,
		    max_queues->max_packet_tx_descs);
		ctx->max_rx_sgl_size = min_t(uint16_t, ENA_PKT_MAX_BUFS,
		    max_queues->max_packet_rx_descs);
	}

	/* round down to the nearest power of 2 */
	max_tx_queue_size = 1 << (flsl(max_tx_queue_size) - 1);
	max_rx_queue_size = 1 << (flsl(max_rx_queue_size) - 1);

	tx_queue_size = clamp_val(tx_queue_size, ENA_MIN_RING_SIZE,
	    max_tx_queue_size);
	rx_queue_size = clamp_val(rx_queue_size, ENA_MIN_RING_SIZE,
	    max_rx_queue_size);

	tx_queue_size = 1 << (flsl(tx_queue_size) - 1);
	rx_queue_size = 1 << (flsl(rx_queue_size) - 1);

	ctx->max_tx_queue_size = max_tx_queue_size;
	ctx->max_rx_queue_size = max_rx_queue_size;
	ctx->tx_queue_size = tx_queue_size;
	ctx->rx_queue_size = rx_queue_size;

	return (0);
}

static void
ena_config_host_info(struct ena_com_dev *ena_dev, pci::device* dev)
{
	struct ena_admin_host_info *host_info;
	int rc;

	/* Allocate only the host info */
	rc = ena_com_allocate_host_info(ena_dev);
	if (unlikely(rc != 0)) {
		ena_log(dev, ERR, "Cannot allocate host info");
		return;
	}

	host_info = ena_dev->host_attr.host_info;

	u8 bus, slot, func;
	dev->get_bdf(bus, slot, func);
	host_info->bdf = (bus << 8) | (slot << 3) | func;

	host_info->os_type = ENA_ADMIN_OS_FREEBSD;
	host_info->kernel_ver = 0;

	//Maybe down the road put some OSv specific info
	host_info->kernel_ver_str[0] = '\0';
	host_info->os_dist = 0;
	host_info->os_dist_str[0] = '\0';

	host_info->driver_version = (ENA_DRV_MODULE_VER_MAJOR) |
	    (ENA_DRV_MODULE_VER_MINOR << ENA_ADMIN_HOST_INFO_MINOR_SHIFT) |
	    (ENA_DRV_MODULE_VER_SUBMINOR << ENA_ADMIN_HOST_INFO_SUB_MINOR_SHIFT);
	host_info->num_cpus = mp_ncpus;
	host_info->driver_supported_features = ENA_ADMIN_HOST_INFO_RX_OFFSET_MASK;

	rc = ena_com_set_host_attributes(ena_dev);
	if (unlikely(rc != 0)) {
		if (rc == EOPNOTSUPP)
			ena_log(dev, WARN, "Cannot set host attributes");
		else
			ena_log(dev, ERR, "Cannot set host attributes");

		goto err;
	}

	return;

err:
	ena_com_delete_host_info(ena_dev);
}

static int
ena_enable_msix_and_set_admin_interrupts(struct ena_adapter *adapter)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	int rc;

	rc = ena_enable_msix(adapter);
	if (unlikely(rc != 0)) {
		ena_log(adapter->pdev, ERR, "Error with MSI-X enablement");
		return (rc);
	}

	ena_setup_mgmnt_intr(adapter);

	rc = ena_request_mgmnt_irq(adapter);
	if (unlikely(rc != 0)) {
		ena_log(adapter->pdev, ERR, "Cannot setup mgmnt queue intr");
		goto err_disable_msix;
	}

	ena_com_set_admin_polling_mode(ena_dev, false);

	ena_com_admin_aenq_enable(ena_dev);

	return (0);

err_disable_msix:
	ena_disable_msix(adapter);

	return (rc);
}

/* Function called on ENA_ADMIN_KEEP_ALIVE event */
static void
ena_keep_alive_wd(void *adapter_data, struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_adapter *adapter = (struct ena_adapter *)adapter_data;
	struct ena_admin_aenq_keep_alive_desc *desc;
	uint64_t rx_drops;
	uint64_t tx_drops;

	desc = (struct ena_admin_aenq_keep_alive_desc *)aenq_e;

	rx_drops = ((uint64_t)desc->rx_drops_high << 32) | desc->rx_drops_low;
	tx_drops = ((uint64_t)desc->tx_drops_high << 32) | desc->tx_drops_low;
	counter_u64_zero(adapter->hw_stats.rx_drops);
	counter_u64_add(adapter->hw_stats.rx_drops, rx_drops);
	counter_u64_zero(adapter->hw_stats.tx_drops);
	counter_u64_add(adapter->hw_stats.tx_drops, tx_drops);

	u64 uptime = osv::clock::uptime::now().time_since_epoch().count();
	adapter->keep_alive_timestamp.store(uptime, std::memory_order_release);
}

/* Check for keep alive expiration */
static void
check_for_missing_keep_alive(struct ena_adapter *adapter)
{
	if (adapter->wd_active == 0)
		return;

	if (adapter->keep_alive_timeout == ENA_HW_HINTS_NO_TIMEOUT)
		return;

	u64 timestamp = adapter->keep_alive_timestamp.load(std::memory_order_acquire);
	u64 now = osv::clock::uptime::now().time_since_epoch().count() - timestamp;
	if (unlikely(now > timestamp + adapter->keep_alive_timeout)) {
		ena_log(adapter->pdev, ERR, "Keep alive watchdog timeout.");
		counter_u64_add(adapter->dev_stats.wd_expired, 1);
		ena_trigger_reset(adapter, ENA_REGS_RESET_KEEP_ALIVE_TO);
	}
}

/* Check if admin queue is enabled */
static void
check_for_admin_com_state(struct ena_adapter *adapter)
{
	if (unlikely(ena_com_get_admin_running_state(adapter->ena_dev) == false)) {
		ena_log(adapter->pdev, ERR,
		    "ENA admin queue is not in running state!");
		counter_u64_add(adapter->dev_stats.admin_q_pause, 1);
		ena_trigger_reset(adapter, ENA_REGS_RESET_ADMIN_TO);
	}
}

static int
check_missing_comp_in_tx_queue(struct ena_adapter *adapter,
    struct ena_ring *tx_ring)
{
	struct ena_tx_buffer *tx_buf;
	uint32_t missed_tx = 0;
	int i, rc = 0;

	u64 curtime = osv::clock::uptime::now().time_since_epoch().count();

	for (i = 0; i < tx_ring->ring_size; i++) {
		tx_buf = &tx_ring->tx_buffer_info[i];

		if (tx_buf->timestamp == 0)
			continue;

		u64 time_offset = curtime - tx_buf->timestamp;

		/* Check again if packet is still waiting */
		if (unlikely(time_offset > adapter->missing_tx_timeout)) {

			if (tx_buf->print_once) {
				ena_log(pdev, WARN,
				    "Found a Tx that wasn't completed on time, qid %d, index %d.",
				    tx_ring->qid, i);
			}

			tx_buf->print_once = false;
			missed_tx++;
		}
	}

	if (unlikely(missed_tx > adapter->missing_tx_threshold)) {
		ena_log(pdev, ERR,
		    "The number of lost tx completion is above the threshold "
		    "(%d > %d). Reset the device",
		    missed_tx, adapter->missing_tx_threshold);
		ena_trigger_reset(adapter, ENA_REGS_RESET_MISS_TX_CMPL);
		rc = EIO;
	}

	counter_u64_add(tx_ring->tx_stats.missing_tx_comp, missed_tx);

	return (rc);
}

/*
 * Check for TX which were not completed on time.
 * Timeout is defined by "missing_tx_timeout".
 * Reset will be performed if number of incompleted
 * transactions exceeds "missing_tx_threshold".
 */
static void
check_for_missing_completions(ena_adapter *adapter)
{
	struct ena_ring *tx_ring;
	int i, budget, rc;

	/* Make sure the driver doesn't turn the device in other process */
	rmb();

	if (!ENA_FLAG_ISSET(ENA_FLAG_DEV_UP, adapter))
		return;

	if (ENA_FLAG_ISSET(ENA_FLAG_TRIGGER_RESET, adapter))
		return;

	if (adapter->missing_tx_timeout == ENA_HW_HINTS_NO_TIMEOUT)
		return;

	budget = adapter->missing_tx_max_queues;
  eth_dev *data = adapter->eth_dev;

	for (i = adapter->next_monitored_tx_qid; i < data->nb_tx_queue; i++) {
		tx_ring = &adapter->tx_ring[i];

		rc = check_missing_comp_in_tx_queue(adapter, tx_ring);
		if (unlikely(rc != 0))
			return;

		budget--;
		if (budget == 0) {
			i++;
			break;
		}
	}

	adapter->next_monitored_tx_qid = i % data->nb_tx_queue;;
}

static void
ena_update_hints(struct ena_adapter *adapter,
    struct ena_admin_ena_hw_hints *hints)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;

	if (hints->admin_completion_tx_timeout)
		ena_dev->admin_queue.completion_timeout =
		    hints->admin_completion_tx_timeout * 1000;

	if (hints->mmio_read_timeout)
		/* convert to usec */
		ena_dev->mmio_read.reg_read_to = hints->mmio_read_timeout * 1000;

	if (hints->missed_tx_completion_count_threshold_to_reset)
		adapter->missing_tx_threshold =
		    hints->missed_tx_completion_count_threshold_to_reset;

	if (hints->missing_tx_completion_timeout) {
		if (hints->missing_tx_completion_timeout ==
		    ENA_HW_HINTS_NO_TIMEOUT)
			adapter->missing_tx_timeout = ENA_HW_HINTS_NO_TIMEOUT;
		else
			adapter->missing_tx_timeout = NANOSECONDS_IN_MSEC *
			    hints->missing_tx_completion_timeout; //In ms
	}

	if (hints->driver_watchdog_timeout) {
		if (hints->driver_watchdog_timeout == ENA_HW_HINTS_NO_TIMEOUT)
			adapter->keep_alive_timeout = ENA_HW_HINTS_NO_TIMEOUT;
		else
			adapter->keep_alive_timeout = NANOSECONDS_IN_MSEC *
			    hints->driver_watchdog_timeout; //In ms
	}
}

static void
ena_timer_service(void *data)
{
	struct ena_adapter *adapter = (struct ena_adapter *)data;

	check_for_missing_keep_alive(adapter);

	check_for_admin_com_state(adapter);

	check_for_missing_completions(adapter);

	if (unlikely(ENA_FLAG_ISSET(ENA_FLAG_TRIGGER_RESET, adapter))) {
		/*
		 * Timeout when validating version indicates that the device
		 * became unresponsive. If that happens skip the reset and
		 * reschedule timer service, so the reset can be retried later.
		 */
		if (ena_com_validate_version(adapter->ena_dev) ==
		    ENA_COM_TIMER_EXPIRED) {
			ena_log(adapter->pdev, WARN,
			    "FW unresponsive, skipping reset");
			ENA_TIMER_RESET(adapter);
			return;
		}
		ena_log(adapter->pdev, WARN, "Trigger reset is on");
		return;
	}

	/*
	 * Schedule another timeout one second from now.
	 */
	ENA_TIMER_RESET(adapter);
}

void
ena_destroy_device(struct ena_adapter *adapter, bool graceful)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	bool dev_up;

	if (!ENA_FLAG_ISSET(ENA_FLAG_DEVICE_RUNNING, adapter))
		return;

	ENA_TIMER_DRAIN(adapter);

	dev_up = ENA_FLAG_ISSET(ENA_FLAG_DEV_UP, adapter);
	if (dev_up)
		ENA_FLAG_SET_ATOMIC(ENA_FLAG_DEV_UP_BEFORE_RESET, adapter);

	if (!graceful)
		ena_com_set_admin_running_state(ena_dev, false);

	/*
	 * Stop the device from sending AENQ events (if the device was up, and
	 * the trigger reset was on, ena_stop already performs device reset)
	 */
	if (!(ENA_FLAG_ISSET(ENA_FLAG_TRIGGER_RESET, adapter) && dev_up))
		ena_com_dev_reset(adapter->ena_dev, adapter->reset_reason);

	ena_free_mgmnt_irq(adapter);

	ena_disable_msix(adapter);

	/*
	 * IO rings resources should be freed because `ena_restore_device()`
	 * calls (not directly) `ena_enable_msix()`, which re-allocates MSIX
	 * vectors. The amount of MSIX vectors after destroy-restore may be
	 * different than before. Therefore, IO rings resources should be
	 * established from scratch each time.
	 */

	ena_com_abort_admin_commands(ena_dev);

	ena_com_wait_for_abort_completion(ena_dev);

	ena_com_admin_destroy(ena_dev);

	ena_com_mmio_reg_read_request_destroy(ena_dev);

	adapter->reset_reason = ENA_REGS_RESET_NORMAL;

	ENA_FLAG_CLEAR_ATOMIC(ENA_FLAG_TRIGGER_RESET, adapter);
	ENA_FLAG_CLEAR_ATOMIC(ENA_FLAG_DEVICE_RUNNING, adapter);
}

static void
ena_free_stats(struct ena_adapter *adapter)
{
	ena_free_counters((counter_u64_t *)&adapter->hw_stats,
	    sizeof(struct ena_hw_stats));
	ena_free_counters((counter_u64_t *)&adapter->dev_stats,
	    sizeof(struct ena_stats_dev));

}

static void
free_adapter(ena_adapter *adapter)
{
	if (adapter) {
		adapter->~ena_adapter();
		free(adapter);
	}
}

static uint16_t instance = 0;
/**
 * ena_attach - Device Initialization Routine
 * @pdev: device information struct
 *
 * Returns 0 on success, otherwise on failure.
 *
 * ena_attach initializes an adapter identified by a device structure.
 * The OS initialization, configuring of the adapter private structure,
 * and a hardware reset occur.
 **/
int
ena_attach(pci::device* pdev, ena_adapter **_adapter)
{
	struct ena_adapter *adapter;
	struct ena_com_dev_get_features_ctx get_feat_ctx;
	struct ena_calc_queue_size_ctx calc_queue_ctx = { 0 };
	static int version_printed;
	struct ena_com_dev *ena_dev = NULL;
	uint32_t max_num_io_queues;
	int rc;

	adapter = aligned_new<ena_adapter>();
	adapter->pdev = pdev;
	adapter->first_bind = -1;

	/*
	 * Set up the timer service - driver is responsible for avoiding
	 * concurrency, as the callout won't be using any locking inside.
	 */
	ENA_TIMER_INIT(adapter);
	adapter->keep_alive_timeout = ENA_DEFAULT_KEEP_ALIVE_TO;
	adapter->missing_tx_timeout = ENA_DEFAULT_TX_CMP_TO;
	adapter->missing_tx_max_queues = ENA_DEFAULT_TX_MONITORED_QUEUES;
	adapter->missing_tx_threshold = ENA_DEFAULT_TX_CMP_THRESHOLD;

	if (version_printed++ == 0)
		ena_log(pdev, INFO, "%s", ena_version);

	/* Allocate memory for ena_dev structure */
	ena_dev = static_cast<ena_com_dev*>(malloc(sizeof(struct ena_com_dev), M_DEVBUF,
	    M_WAITOK | M_ZERO));

	adapter->ena_dev = ena_dev;
	ena_dev->dmadev = pdev;
  adapter->eth_dev = static_cast<ena_eth_dev*>(malloc(sizeof(ena_eth_dev), M_DEVBUF, M_WAITOK | M_ZERO));
  new (adapter->eth_dev) ena_eth_dev(adapter);

  ports.register_port(instance++, adapter->eth_dev);
  ena_log(pdev, INFO, "Registered port with id %d\n", instance - 1);
	adapter->registers = pdev->get_bar(ENA_REG_BAR + 1);
	if (unlikely(adapter->registers == NULL)) {
		ena_log(pdev, ERR,
		    "unable to allocate bus resource: registers!");
		rc = ENOMEM;
		goto err_dev_free;
	}
	adapter->registers->map();

	ena_dev->bus = malloc(sizeof(struct ena_bus), M_DEVBUF,
	    M_WAITOK | M_ZERO);

	/* Store register resources */
	((struct ena_bus *)(ena_dev->bus))->reg_bar = adapter->registers;

	if (unlikely(((struct ena_bus *)(ena_dev->bus))->reg_bar == 0)) {
		ena_log(pdev, ERR, "failed to pmap registers bar");
		rc = ENXIO;
		goto err_bus_free;
	}

	/* Initially clear all the flags */
	ENA_FLAG_ZERO(adapter);

	/* Device initialization */
	rc = ena_device_init(adapter, pdev, &get_feat_ctx, &adapter->wd_active);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR, "ENA device init failed! (err: %d)", rc);
		rc = ENXIO;
		goto err_bus_free;
	}

	adapter->keep_alive_timestamp = osv::clock::uptime::now().time_since_epoch().count();
	adapter->offload_cap = ena_get_dev_offloads(&get_feat_ctx);

	memcpy(adapter->mac_addr, get_feat_ctx.dev_attr.mac_addr, ETHER_ADDR_LEN);

	calc_queue_ctx.pdev = pdev;
	calc_queue_ctx.ena_dev = ena_dev;
	calc_queue_ctx.get_feat_ctx = &get_feat_ctx;

	/* Calculate initial and maximum IO queue number and size */
	max_num_io_queues = ena_calc_max_io_queue_num(pdev, ena_dev,
	    &get_feat_ctx);
	rc = ena_calc_io_queue_size(&calc_queue_ctx);
	if (unlikely((rc != 0) || (max_num_io_queues <= 0))) {
		rc = EFAULT;
		goto err_com_free;
	}

	adapter->requested_tx_ring_size = calc_queue_ctx.tx_queue_size;
	adapter->requested_rx_ring_size = calc_queue_ctx.rx_queue_size;
	adapter->max_tx_ring_size = calc_queue_ctx.max_tx_queue_size;
	adapter->max_rx_ring_size = calc_queue_ctx.max_rx_queue_size;
	adapter->max_tx_sgl_size = calc_queue_ctx.max_tx_sgl_size;
	adapter->max_rx_sgl_size = calc_queue_ctx.max_rx_sgl_size;

	adapter->max_num_io_queues = max_num_io_queues;
	ena_log(pdev, INFO, "ena_attach: set max_num_io_queues to %d", max_num_io_queues);

	adapter->max_mtu = get_feat_ctx.dev_attr.max_mtu;

	adapter->reset_reason = ENA_REGS_RESET_NORMAL;

	/*
	 * The amount of requested MSIX vectors is equal to
	 * adapter::max_num_io_queues (see `ena_enable_msix()`), plus a constant
	 * number of admin queue interrupts. The former is initially determined
	 * by HW capabilities (see `ena_calc_max_io_queue_num())` but may not be
	 * achieved if there are not enough system resources. By default, the
	 * number of effectively used IO queues is the same but later on it can
	 * be limited by the user using sysctl interface.
	 */
	rc = ena_enable_msix_and_set_admin_interrupts(adapter);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR,
		    "Failed to enable and set the admin interrupts");
		goto err_com_free;
	}

	/* initialize rings basic information */
	ena_init_rings(adapter);

	/* Initialize statistics */
	ena_alloc_counters((counter_u64_t *)&adapter->dev_stats,
	    sizeof(struct ena_stats_dev));
	ena_alloc_counters((counter_u64_t *)&adapter->hw_stats,
	    sizeof(struct ena_hw_stats));

	/* setup network interface */
	ENA_FLAG_SET_ATOMIC(ENA_FLAG_DEVICE_RUNNING, adapter);

	/* Run the timer service */
	ENA_TIMER_RESET(adapter);

	*_adapter = adapter;
	return (0);

err_com_free:
	ena_com_admin_destroy(ena_dev);
	ena_com_delete_host_info(ena_dev);
	ena_com_mmio_reg_read_request_destroy(ena_dev);
err_bus_free:
	free(ena_dev->bus, M_DEVBUF);
	ena_free_pci_resources(adapter);
err_dev_free:
  free(adapter->eth_dev, M_DEVBUF);
	free(ena_dev, M_DEVBUF);

	free_adapter(adapter);
	return (rc);
}

/**
 * ena_detach - Device Removal Routine
 * @pdev: device information struct
 *
 * ena_detach is called by the device subsystem to alert the driver
 * that it should release a PCI device.
 **/
int
ena_detach(ena_adapter *adapter)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	/* Stop timer service */
	ENA_LOCK_LOCK();
	ENA_TIMER_DRAIN(adapter);
	ENA_LOCK_UNLOCK();

  ena_free_all_tx_resources(adapter);
	ena_free_all_rx_resources(adapter);

	ENA_LOCK_LOCK();
	ena_destroy_device(adapter, true);
	ENA_LOCK_UNLOCK();

	ena_free_stats(adapter);

	ena_free_irqs(adapter);

	ena_free_pci_resources(adapter);

	if (adapter->rss_indir != NULL)
		free(adapter->rss_indir, M_DEVBUF);

	if (likely(ENA_FLAG_ISSET(ENA_FLAG_RSS_ACTIVE, adapter)))
		ena_com_rss_destroy(ena_dev);

	ena_com_delete_host_info(ena_dev);

	free(ena_dev->bus, M_DEVBUF);

	free(ena_dev, M_DEVBUF);

	free_adapter(adapter);
	return 0;
}

/******************************************************************************
 ******************************** AENQ Handlers *******************************
 *****************************************************************************/
/**
 * ena_update_on_link_change:
 * Notify the network interface about the change in link status
 **/
static void
ena_update_on_link_change(void *adapter_data,
    struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_adapter *adapter = (struct ena_adapter *)adapter_data;
	struct ena_admin_aenq_link_change_desc *aenq_desc;
	int status;

	aenq_desc = (struct ena_admin_aenq_link_change_desc *)aenq_e;
	status = aenq_desc->flags &
	    ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK;

	if (status != 0) {
		ena_log(adapter->pdev, INFO, "link is UP");
		ENA_FLAG_SET_ATOMIC(ENA_FLAG_LINK_UP, adapter);
	} else {
		ena_log(adapter->pdev, INFO, "link is DOWN");
		ENA_FLAG_CLEAR_ATOMIC(ENA_FLAG_LINK_UP, adapter);
	}
}

static void
ena_notification(void *adapter_data, struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_adapter *adapter = (struct ena_adapter *)adapter_data;
	struct ena_admin_ena_hw_hints *hints;

	ENA_WARN(aenq_e->aenq_common_desc.group != ENA_ADMIN_NOTIFICATION,
	    adapter->ena_dev, "Invalid group(%x) expected %x",
	    aenq_e->aenq_common_desc.group, ENA_ADMIN_NOTIFICATION);

	switch (aenq_e->aenq_common_desc.syndrome) {
	case ENA_ADMIN_UPDATE_HINTS:
		hints =
		    (struct ena_admin_ena_hw_hints *)(&aenq_e->inline_data_w4);
		ena_update_hints(adapter, hints);
		break;
	default:
		ena_log(adapter->pdev, ERR,
		    "Invalid aenq notification link state %d",
		    aenq_e->aenq_common_desc.syndrome);
	}
}

/**
 * This handler will called for unknown event group or unimplemented handlers
 **/
static void
unimplemented_aenq_handler(void *adapter_data,
    struct ena_admin_aenq_entry *aenq_e)
{
	ena_log(adapter->pdev, ERR,
	    "Unknown event was received or event with unimplemented handler");
}

/*
 * Contains pointers to event handlers, e.g. link state chage.
 */
static struct ena_aenq_handlers aenq_handlers = {
    handlers : {
	    [ENA_ADMIN_LINK_CHANGE] = ena_update_on_link_change,
	    [ENA_ADMIN_FATAL_ERROR] = nullptr,
	    [ENA_ADMIN_WARNING] = nullptr,
	    [ENA_ADMIN_NOTIFICATION] = ena_notification,
	    [ENA_ADMIN_KEEP_ALIVE] = ena_keep_alive_wd,
    },
    unimplemented_handler : unimplemented_aenq_handler
};

static int
ena_device_init(struct ena_adapter *adapter, pci::device *pdev,
    struct ena_com_dev_get_features_ctx *get_feat_ctx, int *wd_active)
{
	ena_com_dev *ena_dev = adapter->ena_dev;
	bool readless_supported;
	uint32_t aenq_groups;
	int rc;

	rc = ena_com_mmio_reg_read_request_init(ena_dev);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR, "failed to init mmio read less");
		return (rc);
	}

	/*
	 * The PCIe configuration space revision id indicate if mmio reg
	 * read is disabled
	 */
	readless_supported = !(pdev->get_revision_id() & ENA_MMIO_DISABLE_REG_READ);
	ena_com_set_mmio_read_mode(ena_dev, readless_supported);

	rc = ena_com_dev_reset(ena_dev, ENA_REGS_RESET_NORMAL);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR, "Can not reset device");
		goto err_mmio_read_less;
	}

	rc = ena_com_validate_version(ena_dev);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR, "device version is too low");
		goto err_mmio_read_less;
	}

	/* ENA admin level init */
	rc = ena_com_admin_init(ena_dev, &aenq_handlers);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR,
		    "Can not initialize ena admin queue with device");
		goto err_mmio_read_less;
	}

	/*
	 * To enable the msix interrupts the driver needs to know the number
	 * of queues. So the driver uses polling mode to retrieve this
	 * information
	 */
	ena_com_set_admin_polling_mode(ena_dev, true);

	ena_config_host_info(ena_dev, pdev);

	/* Get Device Attributes */
	rc = ena_com_get_dev_attr_feat(ena_dev, get_feat_ctx);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR,
		    "Cannot get attribute for ena device rc: %d", rc);
		goto err_admin_init;
	}

	aenq_groups = BIT(ENA_ADMIN_LINK_CHANGE) |
	    BIT(ENA_ADMIN_FATAL_ERROR) |
	    BIT(ENA_ADMIN_WARNING) |
	    BIT(ENA_ADMIN_NOTIFICATION) |
	    BIT(ENA_ADMIN_KEEP_ALIVE);

	aenq_groups &= get_feat_ctx->aenq.supported_groups;
	rc = ena_com_set_aenq_config(ena_dev, aenq_groups);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR, "Cannot configure aenq groups rc: %d", rc);
		goto err_admin_init;
	}

	*wd_active = !!(aenq_groups & BIT(ENA_ADMIN_KEEP_ALIVE));

	rc = ena_set_queues_placement_policy(pdev, ena_dev, &get_feat_ctx->llq, nullptr);
	if (unlikely(rc != 0)) {
		ena_log(pdev, ERR, "Failed to set placement policy");
		goto err_admin_init;
	}

	return (0);

err_admin_init:
	ena_com_delete_host_info(ena_dev);
	ena_com_admin_destroy(ena_dev);
err_mmio_read_less:
	ena_com_mmio_reg_read_request_destroy(ena_dev);

	return (rc);
}

static int ena_tx_queue_setup(eth_dev *dev, uint16_t qid, uint16_t nb_desc, unsigned int socket, uint64_t offloads){
    int rc;
    ena_adapter *adapter = static_cast<ena_adapter*>(dev->adapter);
    ena_ring *txq = &adapter->tx_ring[qid];
    if(txq->configured)
        return EEXIST;
    if(!powerof2(nb_desc)){
        ena_log_io(nullptr, ERR, "%u is not a power of 2\n", nb_desc);
        return EINVAL;
    }
    txq->ena_dev = adapter->ena_dev;
    txq->qid = qid;
    txq->ring_size = nb_desc;
    txq->numa_domain = socket;
    txq->next_to_use = 0;
    txq->next_to_clean = 0;
    txq->pkts_without_db = false;

    rc = ena_setup_tx_resources(adapter, qid);
    if(unlikely(rc))
        return rc;
    txq->offload = offloads;
    txq->configured = 1;
    return 0;
}

static int ena_rx_queue_setup(eth_dev *dev, uint16_t qid, uint16_t nb_desc, unsigned socket, uint64_t offloads, pbuf_pool *pool){
    int rc;
    ena_adapter *adapter = static_cast<ena_adapter*>(dev->adapter);
    ena_ring *rxq = &adapter->rx_ring[qid];
    if(rxq->configured)
        return EEXIST;
    if(!powerof2(nb_desc)){
        ena_log_io(nullptr, ERR, "%u is not a power of 2\n", nb_desc);
        return EINVAL;
    }
    rxq->ena_dev = adapter->ena_dev;
    rxq->qid = qid;
    rxq->ring_size = nb_desc;
    rxq->numa_domain = socket;
    rxq->pool = pool;
    rxq->next_to_use = 0;
    rxq->next_to_clean = 0;
    rxq->pkts_without_db = false;

    rc = ena_setup_rx_resources(adapter, qid);
    if(unlikely(rc))
        return rc;
    rxq->offload = offloads;
    rxq->configured = 1;
    return 0;
}


static void ena_init_rings(ena_adapter *adapter){
    uint32_t i;
    for(i = 0; i < adapter->max_num_io_queues; ++i){
        ena_ring *ring = &adapter->tx_ring[i];
        ring->configured = 0;
        ring->adapter = adapter;
        ring->ring_type = ENA_RING_TYPE_TX;
        ring->qid = i;
        ring->tx_mem_queue_type = adapter->ena_dev->tx_mem_queue_type;
        ring->tx_max_header_size = adapter->ena_dev->tx_max_header_size;
        ring->sgl_size = adapter->max_tx_sgl_size;
    }

    for(i = 0; i < adapter->max_num_io_queues; ++i){
        ena_ring *ring = &adapter->rx_ring[i];
        ring->configured = 0;
        ring->adapter = adapter;
        ring->ring_type = ENA_RING_TYPE_RX;
        ring->qid = i;
        ring->sgl_size = adapter->max_rx_sgl_size;
    }
}

static int
ena_create_io_queue(ena_eth_dev *data, ena_ring *ring)
{
  ena_adapter *adapter = ring->adapter;  
	ena_com_dev *ena_dev = adapter->ena_dev;
	ena_com_create_io_ctx ctx{};
	uint16_t ena_qid;
	int rc, i;

  ctx.msix_vector = -1; /* used by dpdk for polling */
  if(ring->ring_type == ENA_RING_TYPE_TX){
      ena_qid = ENA_IO_TXQ_IDX(ring->qid);
      ctx.direction = ENA_COM_IO_QUEUE_DIRECTION_TX;
      ctx.mem_queue_type = ena_dev->tx_mem_queue_type;
      for(i = 0; i < ring->ring_size; ++i)
          ring->free_tx_ids[i] = i;
  }else{
      ena_qid = ENA_IO_RXQ_IDX(ring->qid);
      ctx.direction = ENA_COM_IO_QUEUE_DIRECTION_RX;
      for(i = 0; i < ring->ring_size; ++i)
          ring->free_rx_ids[i] = i;
  }

  ctx.queue_size = ring->ring_size;
  ctx.qid = ena_qid;
  ctx.numa_node = ring->numa_domain;


  rc = ena_com_create_io_queue(ena_dev, &ctx);
  if(rc != 0){
    ena_log(adapter->pdev, ERR,
			"Failed to create IO queue[%d] (qid:%d), rc: %d",
			ring->qid, ena_qid, rc);
		return rc;
  }
  rc = ena_com_get_io_handlers(ena_dev, ena_qid, &ring->ena_com_io_sq, &ring->ena_com_io_cq);
		if (rc != 0) {
			ena_log(adapter->pdev, ERR,
			    "Failed to get IO queue handlers. IO queue num"
			    " %d rc: %d",
			    i, rc);
			ena_com_destroy_io_queue(ena_dev, ena_qid);
			goto err;
		}

		if (ctx.numa_node >= 0) {
			ena_com_update_numa_node(ring->ena_com_io_cq,
			    ctx.numa_node);
		}

	return (0);

err:
  ena_com_destroy_io_queue(ena_dev, ena_qid);
	return (ENXIO);
}

/*
 * ena dev setup
 */
int ena_eth_dev::setup_device(uint16_t nb_tx, uint16_t nb_rx){
    nb_tx_queue = nb_tx;
    nb_rx_queue = nb_rx;
    return 0;
}

int ena_eth_dev::setup_tx_queue(uint16_t qid, uint16_t nb_desc, unsigned int socket, uint64_t offloads){
    return ena_tx_queue_setup(this, qid, nb_desc, socket, offloads);
}

int ena_eth_dev::setup_rx_queue(uint16_t qid, uint16_t nb_desc, unsigned int socket, uint64_t offloads, pbuf_pool *pool){
    return ena_rx_queue_setup(this, qid, nb_desc, socket, offloads, pool);
}

int ena_eth_dev::eth_start(){
    return ena_start(static_cast<ena_adapter*>(adapter));
}

void ena_eth_dev::eth_stop(){
    ena_stop(this, static_cast<ena_adapter*>(adapter));
}

void ena_eth_dev::get_dev_info(eth_dev_info &info){
    ena_get_dev_info(&info, this); 
}

void ena_eth_dev::get_mac_addr(char *addr){
    auto* dev_adapter = static_cast<ena_adapter*>(adapter);
    memcpy(addr, dev_adapter->mac_addr, sizeof(dev_adapter->mac_addr));
}
