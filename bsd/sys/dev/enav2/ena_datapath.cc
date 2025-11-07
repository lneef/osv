/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015-2020 Amazon.com, Inc. or its affiliates.
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
__FBSDID("$FreeBSD$");
#include "osv/mmu.hh"
#include "osv/virt_to_phys.hh"
#include <algorithm>
#include <cstdint>
// #define ENA_LOG_ENABLE 1
// #define ENA_LOG_IO_ENABLE 1
#include "ena_comv1/ena_com.h"
#include "ena_comv1/ena_eth_com.h"
#include "ena_comv1/ena_plat.h"
#include "enav2.h"

#include <osv/sched.hh>
#include <osv/trace.hh>

static inline void critical_enter() { sched::preempt_disable(); }
static inline void critical_exit() { sched::preempt_enable(); }

#include <sys/buf_ring.h>

// #include <netinet6/ip6_var.h>

/*********************************************************************
 *  Static functions prototypes
 *********************************************************************/

static int ena_tx_cleanup(struct ena_ring *);
static inline int ena_get_tx_req_id(struct ena_ring *tx_ring,
                                    struct ena_com_io_cq *io_cq,
                                    uint16_t *req_id);
/*********************************************************************
 *  Core Tx/Rx
 *********************************************************************/
/* sets flags according to cksum config */
static inline void ena_prepare_rx_pbuf(struct ena_ring* rx_ring, struct pkt_buf* pbuf, struct ena_com_rx_ctx *ena_rx_ctx){
    uint64_t o_flags = PBUF_OFFLOAD_NONE;
    uint16_t p_type = PACKET_NONE;
    auto packet_type = ena_rx_ctx->l4_proto;
    switch(ena_rx_ctx->l3_proto){
        case ENA_ETH_IO_L3_PROTO_IPV4:
            p_type |= PACKET_IPV4;
            if(unlikely(ena_rx_ctx->l3_csum_err))
                o_flags |= PBUF_CKSUM_L3_BAD;
            else
                o_flags |= PBUF_CKSUM_L3_OK;
            break;
        default:
            break;
    }
    switch(ena_rx_ctx->l4_proto){
        case ENA_ETH_IO_L4_PROTO_UDP:
            p_type |= PACKET_UDP;
            break;
    default:
            break;
    }

    if((packet_type & (ENA_ETH_IO_L4_PROTO_UDP)) && !ena_rx_ctx->frag){
        if(ena_rx_ctx->l4_csum_checked){
            if(unlikely(ena_rx_ctx->l4_csum_err))
                o_flags |= PBUF_CKSUM_L4_OK;
            else
                o_flags |= PBUF_CKSUM_L4_BAD;
        }else{
            o_flags |= PBUF_CKSUM_L4_UNKONW;

        }
    }
    pbuf->olflags = o_flags;
    pbuf->packet_type = p_type;
}

static inline void ena_tx_pbuf_prepare(struct pkt_buf* pbuf, struct ena_com_tx_ctx *ena_tx_ctx) {
    struct ena_com_tx_meta* ena_meta = &ena_tx_ctx->ena_meta;
    if(pbuf->olflags != PBUF_OFFLOAD_NONE){
        ena_tx_ctx->l3_csum_enable = pbuf->olflags & PBUF_OFFLOAD_IPV4_CKSUM;
        if(pbuf->olflags & PBUF_OFFLOAD_IPV4_CKSUM){
            ena_tx_ctx->l4_csum_enable = true;
            ena_tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
        }
    
    }
    ena_meta->l3_hdr_len = pbuf->l3_len;
    ena_meta->l3_hdr_offset = pbuf->l2_len;
    ena_meta->l4_hdr_len = pbuf->l4_len;
    ena_meta->mss = 0;
    ena_tx_ctx->meta_valid = true;
}

static inline int ena_get_tx_req_id(struct ena_ring *tx_ring,
                                    struct ena_com_io_cq *io_cq,
                                    uint16_t *req_id) {
  struct ena_adapter *adapter = tx_ring->adapter;
  int rc;

  rc = ena_com_tx_comp_req_id_get(io_cq, req_id);
  if (rc == ENA_COM_TRY_AGAIN)
    return (EAGAIN);

  if (unlikely(rc != 0)) {
    ena_log(adapter->pdev, ERR, "Invalid req_id %hu in qid %hu", *req_id,
            tx_ring->qid);
    //counter_u64_add(tx_ring->tx_stats.bad_req_id, 1);
    goto err;
  }

  if (tx_ring->tx_buffer_info[*req_id].pbuf != NULL)
    return (0);

  ena_log(adapter->pdev, ERR,
          "tx_info doesn't have valid mbuf. req_id %hu qid %hu", *req_id,
          tx_ring->qid);
err:
  ena_trigger_reset(adapter, ENA_REGS_RESET_INV_TX_REQ_ID);

  return (EFAULT);
}

TRACEPOINT(trace_ena_tx_cleanup, "qid=%d pkts=%d", int, int);

/**
 * ena_tx_cleanup - clear sent packets and corresponding descriptors
 * @tx_ring: ring for which we want to clean packets
 *
 * Once packets are sent, we ask the device in a loop for no longer used
 * descriptors. We find the related mbuf chain in a map (index in an array)
 * and free it, then update ring state.
 * This is performed in "endless" loop, updating ring pointers every
 * TX_COMMIT. The first check of free descriptor is performed before the actual
 * loop, then repeated at the loop end.
 **/
static int ena_tx_cleanup(struct ena_ring *tx_ring) {
  struct ena_adapter *adapter;
  struct ena_com_io_cq *io_cq;
  uint16_t next_to_clean;
  uint16_t ena_qid;
  unsigned int cleanup_budget = 0;
  unsigned int total_tx_desc = 0;
  unsigned int total_tx_pkts = 0;
  int rc;

  cleanup_budget = tx_ring->ring_size - 1;

  adapter = tx_ring->adapter;
  ena_qid = ENA_IO_TXQ_IDX(tx_ring->qid);
  io_cq = &adapter->ena_dev->io_cq_queues[ena_qid];
  next_to_clean = tx_ring->next_to_clean;
  while(likely(total_tx_pkts < cleanup_budget)){
      uint16_t req_id;
      ena_tx_buffer *tx_info;
      pkt_buf *pbuf;
      rc = ena_get_tx_req_id(tx_ring, io_cq, &req_id);
      if(unlikely(rc != 0))
          break;
      tx_info = &tx_ring->tx_buffer_info[req_id];
      pbuf = tx_info->pbuf;
      pbuf->pool->free_bulk(&pbuf, 1);

      tx_info->pbuf = nullptr;
      total_tx_desc += tx_info->tx_descs;
      ++total_tx_pkts;
      tx_ring->free_tx_ids[next_to_clean] = req_id;

      next_to_clean = ENA_TX_RING_IDX_NEXT(next_to_clean, tx_ring->ring_size);
  }

  if(likely(total_tx_desc > 0 )){
      tx_ring->next_to_clean = next_to_clean;
      ena_com_comp_ack(tx_ring->ena_com_io_sq, total_tx_desc);
      ena_com_update_dev_comp_head(tx_ring->ena_com_io_cq);
  }

  /*
   * Need to make the rings circular update visible to
   * ena_xmit_mbuf() before checking for tx_ring->running.
   */
  mb();

  tx_ring->tx_last_cleanup_ticks = bsd_ticks;

  return total_tx_pkts;
}

/**
 * ena_rx_mbuf - assemble mbuf from descriptors
 * @rx_ring: ring for which we want to clean packets
 * @ena_bufs: buffer info
 * @ena_rx_ctx: metadata for this packet(s)
 * @next_to_clean: ring pointer, will be updated only upon success
 *
 **/
static struct pkt_buf *ena_rx_mbuf(struct ena_ring *rx_ring,
                                struct ena_com_rx_buf_info *ena_bufs,
                                struct ena_com_rx_ctx *ena_rx_ctx,
                                uint16_t *next_to_clean) {
  struct pkt_buf *pbuf, *pbuf_head;
  struct ena_rx_buffer *rx_info;
  struct ena_adapter *adapter;
  unsigned int descs = ena_rx_ctx->descs;
  uint16_t ntc, len, req_id, buf = 0;

  ntc = *next_to_clean;
  adapter = rx_ring->adapter;

  len = ena_bufs[buf].len;
  req_id = ena_bufs[buf].req_id;
  rx_info = &rx_ring->rx_buffer_info[req_id];
  if (unlikely(rx_info->pbuf == NULL)) {
    ena_log(adapter->pdev, ERR, "NULL mbuf in rx_info");
    return (NULL);
  }

  ena_log_io(adapter->pdev, DBG, "rx_info %p, mbuf %p, paddr %jx", rx_info,
             rx_info->pbuf, (uintmax_t)rx_info->ena_buf.paddr);
  pbuf = rx_info->pbuf;
  pbuf_head = pbuf;
  pbuf_head->nb_segs = descs;
  pbuf_head->pkt_len = len;
  rx_info->pbuf = nullptr;
  rx_ring->free_rx_ids[ntc] = req_id;
  ntc = ENA_RX_RING_IDX_NEXT(ntc, rx_ring->ring_size);
  ena_log_io(adapter->pdev, DBG, "Mbuf data offset=%u", ena_rx_ctx->pkt_offset);

  ena_log_io(adapter->pdev, DBG, "rx mbuf 0x%p, flags=0x%x, len: %d", pbuf,
             pbuf->olflags, pbuf->pkt_len);

  /*
   * While we have more than 1 descriptors for one rcvd packet, append
   * other mbufs to the main one
   */
  while (--descs) {
    ++buf;
    len = ena_bufs[buf].len;
    req_id = ena_bufs[buf].req_id;
    rx_info = &rx_ring->rx_buffer_info[req_id];

    if (unlikely(rx_info->pbuf == NULL)) {
      ena_log(adapter->pdev, ERR, "NULL mbuf in rx_info");
      /*
       * If one of the required mbufs was not allocated yet,
       * we can break there.
       * All earlier used descriptors will be reallocated
       * later and not used mbufs can be reused.
       * The next_to_clean pointer will not be updated in case
       * of an error, so caller should advance it manually
       * in error handling routine to keep it up to date
       * with hw ring.
       */
      rx_info->pbuf->pool->free_bulk(&rx_info->pbuf, 1);
      return (NULL);
    }else{
        pbuf->next = rx_info->pbuf;
        pbuf = pbuf->next;
        pbuf_head->pkt_len += rx_info->pbuf->pkt_len; 
    }

    ena_log_io(adapter->pdev, INFO, "rx pbuf updated. len %d",
               pbuf->pkt_len);

    rx_info->pbuf = NULL;
    rx_ring->free_rx_ids[ntc] = req_id;
    ntc = ENA_RX_RING_IDX_NEXT(ntc, rx_ring->ring_size);
  }

  *next_to_clean = ntc;

  return (pbuf);
}

/**
 * ena_rx_cleanup - handle rx irq
 * @arg: ring for which irq is being handled
 **/
static uint16_t ena_rx_recv(struct ena_ring *rx_ring, pkt_buf** pkts, uint16_t nb_pkts) {
  struct ena_adapter *adapter;
  struct pkt_buf *pbuf;
  struct ena_com_rx_ctx ena_rx_ctx;
  struct ena_com_io_cq *io_cq;
  struct ena_com_io_sq *io_sq;
  enum ena_regs_reset_reason_types reset_reason;
  uint16_t ena_qid;
  uint16_t descs_in_use;
  uint16_t completed;
  uint16_t next_to_clean;
  uint32_t refill_required;
  uint32_t refill_threshold;
  unsigned int qid;
  int rc, i;

  adapter = rx_ring->adapter;
  qid = rx_ring->qid;
  ena_qid = ENA_IO_RXQ_IDX(qid);
  io_cq = &adapter->ena_dev->io_cq_queues[ena_qid];
  io_sq = &adapter->ena_dev->io_sq_queues[ena_qid];
  next_to_clean = rx_ring->next_to_clean;
  descs_in_use = rx_ring->ring_size - ena_com_free_q_entries(rx_ring->ena_com_io_sq) - 1;
  nb_pkts = min_t(uint16_t, nb_pkts, descs_in_use); 
  ena_log_io(adapter->pdev, INFO, "rx: qid %d", qid);
  for(completed = 0; completed < nb_pkts; ++completed){
    ena_rx_ctx.ena_bufs = rx_ring->ena_bufs;
    ena_rx_ctx.max_bufs = adapter->max_rx_sgl_size;
    ena_rx_ctx.descs = 0;
    ena_rx_ctx.pkt_offset = 0;

    rc = ena_com_rx_pkt(io_cq, io_sq, &ena_rx_ctx);
    if (unlikely(rc != 0)) {
      if (rc == ENA_COM_NO_SPACE) {
        //counter_u64_add(rx_ring->rx_stats.bad_desc_num, 1);
        reset_reason = ENA_REGS_RESET_TOO_MANY_RX_DESCS;
      } else {
        //counter_u64_add(rx_ring->rx_stats.bad_req_id, 1);
        reset_reason = ENA_REGS_RESET_INV_RX_REQ_ID;
      }
      ena_trigger_reset(adapter, reset_reason);
      return (0);
    } 

    if (unlikely(ena_rx_ctx.descs == 0))
      break;

    ena_log_io(adapter->pdev, DBG,
               "rx: q %d got packet from ena. descs #: %d l3 proto %d l4 proto "
               "%d hash: %x",
               rx_ring->qid, ena_rx_ctx.descs, ena_rx_ctx.l3_proto,
               ena_rx_ctx.l4_proto, ena_rx_ctx.hash);

    /* Receive mbuf from the ring */
    pbuf = ena_rx_mbuf(rx_ring, rx_ring->ena_bufs, &ena_rx_ctx, &next_to_clean);
    /* Exit if we failed to retrieve a buffer */
    if (unlikely(pbuf == NULL)) {
      for (i = 0; i < ena_rx_ctx.descs; ++i) {
        rx_ring->free_rx_ids[next_to_clean] = rx_ring->ena_bufs[i].req_id;
        next_to_clean = ENA_RX_RING_IDX_NEXT(next_to_clean, rx_ring->ring_size);
      }
      break;
    }
    
  } 

  rx_ring->next_to_clean = next_to_clean;

  refill_required = ena_com_free_q_entries(io_sq);
  refill_threshold =
      min_t(int, rx_ring->ring_size / ENA_RX_REFILL_THRESH_DIVIDER,
            ENA_RX_REFILL_THRESH_PACKET);

  if (refill_required > refill_threshold) {
    ena_refill_rx_bufs(rx_ring, refill_required);
  }

  return completed;
}

static void ena_tx_map_mbuf(struct ena_ring *tx_ring,
                            struct ena_tx_buffer *tx_info, struct pkt_buf *pbuf,
                            void **push_header, u16 *header_len) {
  struct ena_com_buf *ena_buf;
  uint16_t delta = 0, seg_len, push_len;
  seg_len = pbuf->data_len;

  tx_info->pbuf = pbuf;
  ena_buf = tx_info->bufs;

  if (tx_ring->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
    push_len = std::min<uint32_t>(pbuf->pkt_len, tx_ring->tx_max_header_size);
    *header_len = push_len;

    if (push_len <= seg_len) {
      *push_header = pbuf->buf;
    } else {
      // TODO make sure this is valid
      pbuf->copy_data(push_len, tx_ring->push_buf_intermediate_buf);
      *push_header = tx_ring->push_buf_intermediate_buf;
      delta = push_len - seg_len;
    }

  } else {
    *push_header = nullptr;
    *header_len = 0;
    push_len = 0;
  }

  if (seg_len > push_len) {
    ena_buf->paddr = mmu::virt_to_phys(pbuf->buf) + push_len;
    ena_buf->len = seg_len - push_len;
    ena_buf++;
    tx_info->num_of_bufs++;
  }

  while ((pbuf = pbuf->next) != nullptr) {
    seg_len = pbuf->data_len;

    /* Skip mbufs if whole data is pushed as a header */
    if (delta > seg_len) {
      delta -= seg_len;
      continue;
    }

    ena_buf->paddr = mmu::virt_to_phys(pbuf->buf) + delta;
    ena_buf->len = seg_len - delta;
    ena_buf++;
    tx_info->num_of_bufs++;

    delta = 0;
  }
}

static int ena_xmit_mbuf(struct ena_ring *tx_ring, struct pkt_buf *pbuf) {
  struct ena_adapter *adapter;
  struct ena_tx_buffer *tx_info;
  struct ena_com_dev *ena_dev;
  struct ena_com_io_sq *io_sq;
  struct ena_com_tx_ctx ena_tx_ctx{};
  uint16_t next_to_use, header_len, req_id;
  uint16_t ena_qid;
  void *push_header;
  int nb_hw_desc;
  int rc;

  /* Checking for space for 2 additional metadata descriptors due to
   * possible header split and metadata descriptor
   */
  if (!ena_com_sq_have_enough_space(tx_ring->ena_com_io_sq,
                                    pbuf->nb_segs + 2)) {
    return ENA_COM_NO_MEM;
  }

  ena_qid = ENA_IO_TXQ_IDX(tx_ring->qid);
  adapter = tx_ring->adapter;
  ena_dev = adapter->ena_dev;
  io_sq = &ena_dev->io_sq_queues[ena_qid];
  next_to_use = tx_ring->next_to_use;

  req_id = tx_ring->free_tx_ids[next_to_use];
  tx_info = &tx_ring->tx_buffer_info[req_id];
  tx_info->num_of_bufs = 0;

  ena_tx_map_mbuf(tx_ring, tx_info, pbuf, &push_header, &header_len);

  ena_tx_ctx.ena_bufs = tx_info->bufs;
  ena_tx_ctx.push_header = push_header;
  ena_tx_ctx.num_bufs = tx_info->num_of_bufs;
  ena_tx_ctx.req_id = req_id;
  ena_tx_ctx.header_len = header_len;

  /* Set Tx offloads flags, if applicable */
  ena_tx_pbuf_prepare(pbuf, &ena_tx_ctx);

  if (unlikely(
          ena_com_is_doorbell_needed(tx_ring->ena_com_io_sq, &ena_tx_ctx))) {
    ena_ring_tx_doorbell(tx_ring);
    tx_ring->tx_stats.doorbells++;
  }

  /* prepare the packet's descriptors to dma engine */
  rc = ena_com_prepare_tx(tx_ring->ena_com_io_sq, &ena_tx_ctx, &nb_hw_desc);
  if (unlikely(rc)) {
    ++tx_ring->tx_stats.prepare_ctx_err;
    ena_trigger_reset(tx_ring->adapter, ENA_REGS_RESET_DRIVER_INVALID_STATE);
    return rc;
  }

  tx_info->tx_descs = nb_hw_desc;
  tx_info->timestamp = osv::clock::uptime::now().time_since_epoch().count();

  tx_ring->tx_stats.cnt++;
  tx_ring->tx_stats.bytes += pbuf->pkt_len;
  tx_ring->next_to_use = ENA_TX_RING_IDX_NEXT(next_to_use, tx_ring->ring_size);
  return 0;
}

static uint16_t ena_xmit_pkts(ena_ring *tx_queue, struct pkt_buf **pbufs,
                                  uint16_t nb_pkts) {
  struct ena_ring *tx_ring = (struct ena_ring *)(tx_queue);
  int available_desc;
  uint16_t sent_idx = 0;

  available_desc = ena_com_free_q_entries(tx_ring->ena_com_io_sq);
  if (available_desc < nb_pkts) {
    ena_tx_cleanup(tx_ring);
  }
  for (; sent_idx < nb_pkts; ++sent_idx) {
    if (ena_xmit_mbuf(tx_ring, pbufs[sent_idx]))
      break;
    tx_ring->pkts_without_db = true;
  }
  if (likely(tx_ring->pkts_without_db)) {
    ena_com_write_sq_doorbell(tx_ring->ena_com_io_sq);
    ena_com_update_dev_comp_head(tx_ring->ena_com_io_cq);
    tx_ring->pkts_without_db = false;
  }
  return sent_idx;
}


int ena_eth_dev::submit_pkts_burst(uint16_t qid, pkt_buf **pkts, uint16_t nb_pkts){
    auto* dev_adapter = static_cast<ena_adapter*>(adapter);
    return ena_xmit_pkts(&dev_adapter->tx_ring[qid], pkts, nb_pkts);
}

int ena_eth_dev::retrieve_pkts_burst(uint16_t qid, pkt_buf **pkts, uint16_t nb_pkts){
    auto* dev_adapter = static_cast<ena_adapter*>(adapter);
    return ena_rx_recv(&dev_adapter->rx_ring[qid], pkts, nb_pkts);
}
