/***************************************************************
 *
 * (C) 2011-14 Nicola Bonelli <nicola@pfq.io>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#include <pragma/diagnostic_push>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>

#include <pragma/diagnostic_pop>

#include <pf_q-sock.h>
#include <pf_q-memory.h>

/* vector of pointers to pfq_sock */

static atomic_t      pfq_sock_count;

atomic_long_t pfq_sock_vector[Q_MAX_ID];


static void
pfq_sock_init_once(void)
{
#ifdef PFQ_USE_SKB_POOL
	pfq_skb_pool_enable(true);
#endif
}


static void
pfq_sock_finish_once(void)
{
#ifdef PFQ_USE_SKB_POOL
	pfq_skb_pool_enable(false);
#endif
}


pfq_id_t
pfq_get_free_id(struct pfq_sock * so)
{
        int n = 0;

        for(; n < (__force int)Q_MAX_ID; n++)
        {
                if (!atomic_long_cmpxchg(pfq_sock_vector + n, 0, (long)so)) {
			if(atomic_inc_return(&pfq_sock_count) == 1)
				pfq_sock_init_once();
			return (__force pfq_id_t)n;
                }
        }
        return (__force pfq_id_t)-ENOMEM;
}


int pfq_get_sock_count(void)
{
        return atomic_read(&pfq_sock_count);
}


struct pfq_sock *
pfq_get_sock_by_id(pfq_id_t id)
{
        struct pfq_sock *so;
        if (unlikely((__force int)id >= Q_MAX_ID)) {
                pr_devel("[PFQ] pfq_get_sock_by_id: bad id=%d!\n", id);
                return NULL;
        }
	so = (struct pfq_sock *)atomic_long_read(&pfq_sock_vector[(__force int)id]);
	smp_read_barrier_depends();
	return so;
}


void pfq_release_sock_id(pfq_id_t id)
{
        if (unlikely((__force int)id >= Q_MAX_ID ||
		     (__force int)id < 0)) {
                pr_devel("[PFQ] pfq_release_sock_by_id: bad id=%d!\n", id);
                return;
        }

        atomic_long_set(pfq_sock_vector + (__force int)id, 0);
        if (atomic_dec_return(&pfq_sock_count) == 0)
		pfq_sock_finish_once();
}


void pfq_sock_opt_init(struct pfq_sock_opt *that, size_t caplen, size_t maxlen)
{
        /* the queue is allocate later, when the socket is enabled */
        int n;

        atomic_long_set(&that->rxq.addr, 0);

        that->rxq.base_addr = NULL;

        /* disable tiemstamping by default */
        that->tstamp = false;

	/* Rx queue setup */

        /* set slots and caplen default values */

        that->caplen = caplen;
        that->rx_queue_len = 0;
        that->rx_slot_size = 0;

        /* initialize waitqueue */

        init_waitqueue_head(&that->waitqueue);

	/* Tx queues setup */

        that->tx_queue_len  = 0;
        that->tx_slot_size  = Q_QUEUE_SLOT_SIZE(maxlen);
	that->tx_num_queues = 0;

	for(n = 0; n < Q_MAX_TX_QUEUES; ++n)
	{
		atomic_long_set(&that->txq[n].addr, 0);

		that->txq[n].base_addr = NULL;
		that->txq[n].if_index  = -1;
		that->txq[n].queue     = -1;
		that->txq[n].cpu       = -1;
		that->txq[n].task      = NULL;
	}
}


void pfq_sock_init(struct pfq_sock *so, int id)
{
	so->id = id;

        /* memory mapped queues are allocated later, when the socket is enabled */

	so->egress_type   = pfq_endpoint_socket;
	so->egress_index  = 0;
	so->egress_queue  = 0;

	/* default weight */

	so->weight = 1;

        so->shmem.addr = NULL;
        so->shmem.size = 0;
        so->shmem.kind = 0;
        so->shmem.hugepages = NULL;
        so->shmem.npages = 0;

        /* reset stats */

        sparse_set(&so->stats.recv, 0);
        sparse_set(&so->stats.lost, 0);
        sparse_set(&so->stats.drop, 0);
        sparse_set(&so->stats.sent, 0);
        sparse_set(&so->stats.disc, 0);
}

