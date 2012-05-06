/***************************************************************
 *                                                
 * (C) 2011-12 Nicola Bonelli <nicola.bonelli@cnit.it>   
 *             Andrea Di Pietro <andrea.dipietro@for.unipi.it>
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

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/semaphore.h>
#include <linux/socket.h>  
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/highmem.h>
#include <linux/ioctl.h>
#include <linux/ip.h>
#include <linux/poll.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h> 

#include <linux/percpu.h>

#include <net/sock.h>
#ifdef CONFIG_INET
#include <net/inet_common.h>
#endif

#include <linux/pf_q.h>

#include <pf_q-priv.h>
#include <pf_q-devmap.h>
#include <pf_q-group.h>
#include <pf_q-queue.h>
#include <pf_q-steer.h>

#include <mpdb-queue.h>

struct net_proto_family  pfq_family_ops;
struct packet_type       pfq_prot_hook;
struct proto             pfq_proto;
struct proto_ops         pfq_ops; 

static int direct_path  = 0;
static int queue_slots  = 131072; // slots per queue
static int cap_len      = 1514;
#ifdef PFQ_USE_SKB_BATCH_QUEUE
static int batch_len 	= 16;
#endif


MODULE_LICENSE("GPL");

MODULE_AUTHOR("Nicola Bonelli <nicola.bonelli@cnit.it>");
MODULE_AUTHOR("Andrea Di Pietro <andrea.dipietro@for.unipi.it>");

MODULE_DESCRIPTION("packet catpure system for 64bit multi-core architecture");

module_param(direct_path,  int, 0644);
module_param(cap_len,      int, 0644);
module_param(queue_slots,  int, 0644);
#ifdef PFQ_USE_SKB_BATCH_QUEUE
module_param(batch_len,    int, 0644);
#endif

MODULE_PARM_DESC(direct_path, " Direct Path: 0 = classic, 1 = direct");
MODULE_PARM_DESC(cap_len,     " Default capture length (bytes)");
MODULE_PARM_DESC(queue_slots, " Queue slots (default=131072)");
#ifdef PFQ_USE_SKB_BATCH_QUEUE
MODULE_PARM_DESC(batch_len,   " Queue batch length");
#endif

/* vector of pointers to pfq_opt */

atomic_long_t pfq_vector[Q_MAX_ID]; 

/* timestamp toggle */

atomic_t timestamp_toggle;

/* per-cpu data... */

struct local_data 
{
  	unsigned long           eligible_mask;
	unsigned long           sock_mask [Q_MAX_ID];
        int                     sock_cnt;
        struct pfq_queue_skb    batch_queue;
};

struct local_data __percpu 	* cpu_data;


/* uhm okay, this is a legit form of static polymorphism */

static inline struct pfq_sock *
pfq_sk(struct sock *sk)
{
        return (struct pfq_sock *)(sk);
}


inline 
int pfq_get_free_id(struct pfq_opt * pq)
{
        int n = 0;
        for(; n < Q_MAX_ID; n++)
        {            
                if (!atomic_long_cmpxchg(pfq_vector + n, 0, (long)pq))
                        return n;         
        }
        return -1;
}


inline 
struct pfq_opt * 
pfq_get_opt(int id)
{
        struct pfq_opt * opt;
        if (unlikely(id >= Q_MAX_ID || id < 0))
        {
                printk(KERN_WARNING "[PFQ] pfq_devmap_freeid: bad id=%d\n", id);
                return NULL;
        }
        
	opt = (struct pfq_opt *)atomic_long_read(&pfq_vector[id]);  
	smp_read_barrier_depends();
	return opt;  
}


inline 
void pfq_release_id(int id)
{
        if (unlikely(id >= Q_MAX_ID || id < 0))
        {
                printk(KERN_WARNING "[PFQ] pfq_devmap_freeid: bad id=%d\n", id);
                return;
        }
        atomic_long_set(pfq_vector + id, 0);
}


inline
bool pfq_filter(const struct sk_buff *skb)
{             
        /* placeholder for future implementation */
        return true;
}


inline
bool pfq_enqueue_skb(struct sk_buff *skb, struct pfq_opt *pq)
{
        /* eventually filter the packet... */

        if (unlikely(!pfq_filter(skb)))
        {
                sparse_inc(&pq->q_stat.drop);
                return false;
        }

        /* enqueue the sk_buff: it's wait-free. */

        if (pq->q_active && mpdb_enqueue(pq, skb)) {

                /* increment recv counter */
                sparse_inc(&pq->q_stat.recv);
                return true;
        }
        else {
                sparse_inc(&pq->q_stat.lost);
                return false;
        }

        return true;
}


struct pfq_steer_cache
{
	steer_function_t fun;
	steer_ret_t ret;
};


inline steer_ret_t
pfq_memoized_call(struct pfq_steer_cache *mem, steer_function_t fun, const struct sk_buff *skb)
{
	if (mem->fun != fun) {
		mem->fun = fun;
		mem->ret = fun(skb);
	} 
	return mem->ret; 
}


/* send this packet to selected sockets */

inline
void pfq_send_skb_to_mask(struct sk_buff *skb, unsigned long sock_mask)
{
        while (sock_mask != 0)
        {        
                unsigned int zn = __builtin_ctz(sock_mask);
		struct pfq_opt * pq = pfq_get_opt(zn);
                if (pq) {
                	pfq_enqueue_skb(skb, pq);
                }
                sock_mask ^= 1 << zn;
        }
}


int 
pfq_direct_receive(struct sk_buff *skb, int __index, int __queue, bool direct)
{       
        struct local_data * local_cache = this_cpu_ptr(cpu_data);
	struct pfq_steer_cache steer_cache = { NULL, {0, 0}  };
        unsigned long group_mask, sock_mask;

#ifdef PFQ_USE_SKB_BATCH_QUEUE
#pragma message "[PFQ] using skb batch queue"
	struct pfq_queue_skb * batch_queue = &local_cache->batch_queue;
        int n;
#endif
        /* if required, timestamp this packet now */

        if (atomic_read(&timestamp_toggle) && 
                        skb->tstamp.tv64 == 0) {
                __net_timestamp(skb);
        }

        /* reset skb->data to the beginning of the packet */
        
	if (skb->pkt_type != PACKET_OUTGOING) {	
		skb_push(skb, skb->mac_len);
	}
	else {
		skb->mac_len = ETH_HLEN;
	}

#ifdef PFQ_USE_SKB_BATCH_QUEUE
	if (pfq_queue_skb_push(batch_queue, skb) == -1)
	{
                printk(KERN_INFO "[PFQ] batch_queue internal error\n");
                return -1;
        }
	if (pfq_queue_skb_size(batch_queue) < batch_len)
                return 0;

	queue_for_each(skb, n, batch_queue)
	{
#endif
        /* get the balancing groups bitmap */

        group_mask = __pfq_devmap_get_groups(skb->dev->ifindex, skb_get_rx_queue(skb));

        while (group_mask)
        {         
                int first = __builtin_ctzl(group_mask);
                steer_ret_t resp;

                steer_function_t steer_function = (steer_function_t) atomic_long_read(&pfq_groups[first].steer);

                sparse_inc(&pfq_groups[first].recv);

                sock_mask = 0;
                if (steer_function) {

                        resp = pfq_memoized_call(&steer_cache, steer_function, skb);
                        if (likely(resp.hash)) {

                                unsigned long eligible_mask = atomic_long_read(&pfq_groups[first].id_mask[resp.type]);

                                if (resp.hash == hash_clone)
                                {
                                        sock_mask |= eligible_mask;
                                        goto send_to_group;
                                }

                                if (unlikely(eligible_mask != local_cache->eligible_mask)) {

                                        local_cache->eligible_mask = eligible_mask;
                                        local_cache->sock_cnt = 0;
                                        while (eligible_mask)
                                        {
                                                int first_sock = eligible_mask & -eligible_mask;
                                                local_cache->sock_mask[local_cache->sock_cnt++] = first_sock;
                                                eligible_mask ^= first_sock;
                                        }
                                }

                                if (local_cache->sock_cnt) {
                                        sock_mask |= local_cache->sock_mask[resp.hash % local_cache->sock_cnt];
                                }
                        }
                }
                else {
                        sock_mask |= atomic_long_read(&pfq_groups[first].id_mask[Q_GROUP_DATA]);
                }
send_to_group:
                if (sock_mask)
                        pfq_send_skb_to_mask(skb, sock_mask);

                group_mask ^= 1L << first;
        }

        if (likely(direct))
		__kfree_skb(skb);
	else
		kfree_skb(skb);

#ifdef PFQ_USE_SKB_BATCH_QUEUE
        
        }       /* queue_for_each */

        pfq_queue_skb_flush(batch_queue);

#endif
        return 0;

}


/* simple HANDLER */       

int 
pfq_packet_rcv
(
    struct sk_buff *skb, struct net_device *dev,
    struct packet_type *pt
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16))
    ,struct net_device *orig_dev
#endif
    )
{
	if (skb_shared(skb)) {
                struct sk_buff *nskb = skb_clone(skb, GFP_ATOMIC);
                if (nskb == NULL) {
                        consume_skb(skb);
                        return 0;
                } 
                kfree_skb(skb);
                skb = nskb;
        }

        return pfq_direct_receive(skb, dev->ifindex, skb_get_rx_queue(skb), false);
}



static int 
pfq_ctor(struct pfq_opt *pq)
{
        /* set to 0 by default */
        memset(pq, 0, sizeof(struct pfq_opt));

        /* get a unique id for this queue */
        pq->q_id = pfq_get_free_id(pq);
        if (pq->q_id == -1)
        {
                printk(KERN_WARNING "[PFQ] no queue available!\n");
                return -EBUSY;
        }
        
        /* disable tiemstamping by default */
        pq->q_tstamp = false;

        /* queue is alloc when the socket is enabled */
        pq->q_addr = NULL; 
        pq->q_queue_mem = 0;
        
        /* set q_slots and q_caplen default values */
        
        pq->q_caplen    = cap_len;
        pq->q_offset    = 0;
        pq->q_slot_size = DBMP_QUEUE_SLOT_SIZE(cap_len);
        pq->q_slots     = queue_slots;

        /* disabled by default */
        pq->q_active = false;
        
        /* initialize waitqueue */
        init_waitqueue_head(&pq->q_waitqueue);
        
        /* reset stats */
        sparse_set(&pq->q_stat.recv, 0);
        sparse_set(&pq->q_stat.lost, 0);
        sparse_set(&pq->q_stat.drop, 0);

        return 0;
}


static void 
pfq_dtor(struct pfq_opt *pq)
{
        pfq_release_id(pq->q_id); 

        mpdb_queue_free(pq);
}


static int 
pfq_create(
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24))
    struct net *net,
#endif
    struct socket *sock, int protocol
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33))
    , int kern
#endif
    )
{
        struct pfq_opt *pq;
        struct sock *sk;
        struct pfq_sock *psk;
        int err = -ENOMEM;

        /* security and sanity check */
        if (!capable(CAP_NET_ADMIN))
                return -EPERM;
        if (sock->type != SOCK_RAW)
                return -ESOCKTNOSUPPORT;
        if (protocol != __constant_htons(ETH_P_ALL))
                return -EPROTONOSUPPORT;

#if(LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
        sk = sk_alloc(PF_Q, GFP_KERNEL, 1, NULL);
#else
#if(LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24))
        // BD: API changed in 2.6.12, ref:
        // http://svn.clkao.org/svnweb/linux/revision/?rev=28201
        sk = sk_alloc(PF_Q, GFP_ATOMIC, &q_proto, 1);
#else
        sk = sk_alloc(net, PF_INET, GFP_KERNEL, &pfq_proto);
#endif
#endif
        if (sk == NULL)
                goto out;

        sock->ops = &pfq_ops;
        sock_init_data(sock,sk);

#if(LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
        sk_set_owner(sk, THIS_MODULE);
#endif
        /* alloc memory for this pq */

        pq = (struct pfq_opt *)kmalloc(sizeof(struct pfq_opt), GFP_KERNEL);
        if (!pq) 
        {
                err = -ENOMEM;
                goto pq_err;
        }   

        /* construct pfq_opt */
        if (pfq_ctor(pq) != 0)
        {
                err = -ENOMEM;
                goto ctor_err;
        }

	smp_wmb();

        /* store the pq */
        psk = pfq_sk(sk);
	psk->opt = pq;
        return 0;

ctor_err:    
        kfree(pq);
pq_err:    
        sk_free(sk);
out:    
        return err;
}   


static int 
pfq_release(struct socket *sock)
{
        struct sock * sk = sock->sk;
        struct pfq_opt * pq;
	
	if (!sk)
		return 0;
	
	pq = pfq_sk(sk)->opt;
	
	if (pq) {
		
		pq->q_active = false;

		/* decrease the timestamp_toggle counter */
		if (pq->q_tstamp) {
			atomic_dec(&timestamp_toggle);
			printk(KERN_INFO "[PFQ|%d] timestamp_toggle => %d\n", pq->q_id, atomic_read(&timestamp_toggle));
		}

        	pfq_leave_all_groups(pq->q_id);

		wmb();

		/* Convenient way to avoid a race condition,
		 * without using rwmutexes that are very expensive 
		 */

		msleep(10 /* msec */);

		pfq_dtor(pq);

                msleep(10 /* msec */);

		kfree(pq);
	}
        
        sock_orphan(sk);
	sock->sk = NULL;
        
	sock_put(sk);
        return 0;
}


static
int pfq_getsockopt(struct socket *sock,
                   int level, int optname,
                   char __user * optval, int __user * optlen)
{
        int len;
        struct pfq_opt *pq = pfq_sk(sock->sk)->opt;

        if (pq == NULL)
                return -EFAULT;
        
	if (get_user(len, optlen))
                return -EFAULT;
        if (len < 0)
		return -EINVAL;

        switch(optname)
        {
        case SO_GET_ID: 
            {
                    if (len != sizeof(pq->q_id))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_id, sizeof(pq->q_id)))
                            return -EFAULT;
            } break;

        case SO_GET_GROUPS:
            {
                    unsigned long grps;
                    if(len != sizeof(unsigned long))
                            return -EINVAL;
                    grps = pfq_get_groups(pq->q_id);
                    if (copy_to_user(optval, &grps, sizeof(grps)))
                            return -EFAULT;
            } break;

        case SO_GET_QUEUE_MEM: 
            {
                    if (len != sizeof(pq->q_queue_mem))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_queue_mem, sizeof(pq->q_queue_mem)))
                            return -EFAULT;
            } break;

        case SO_GET_SLOTS: 
            {
                    if (len != sizeof(pq->q_slots))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_slots, sizeof(pq->q_slots)))
                            return -EFAULT;
            } break;

        case SO_GET_TSTAMP_TYPE: 
            {
                    if (len != sizeof(pq->q_tstamp))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_tstamp, sizeof(pq->q_tstamp)))
                            return -EFAULT;
            } break;

        case SO_GET_STATUS: 
            {
                    if (len != sizeof(pq->q_active)) 
                            return -EINVAL;
                    
                    if (copy_to_user(optval, &pq->q_active, sizeof(pq->q_active))) 
                            return -EFAULT;
                    
            } break;

        case SO_GET_STATS: 
            {
                    struct pfq_stats stat;
                    if (len != sizeof(struct pfq_stats))
                            return -EINVAL;

                    stat.recv = sparse_read(&pq->q_stat.recv);
                    stat.lost = sparse_read(&pq->q_stat.lost);
                    stat.drop = sparse_read(&pq->q_stat.drop);

                    if (copy_to_user(optval, &stat, sizeof(stat)))
                            return -EFAULT;
            } break;

        case SO_GET_CAPLEN: 
            {
                    if (len != sizeof(pq->q_caplen))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_caplen, sizeof(pq->q_caplen)))
                            return -EFAULT;
            } break;

        case SO_GET_OFFSET: 
            {
                    if (len != sizeof(pq->q_offset))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_offset, sizeof(pq->q_offset)))
                            return -EFAULT;
            } break;

        case SO_GROUP_JOIN: 
            {
                    struct pfq_group_join group;

                    if (len != sizeof(group)) 
                            return -EINVAL;
                    
		    if (copy_from_user(&group, optval, len)) 
                            return -EFAULT;

		    if (group.gid < Q_ANY_GROUP || group.gid >= Q_MAX_GROUP)
			    return -EINVAL;

                    if (group.gid == Q_ANY_GROUP) {
                            group.gid = pfq_join_free_group(pq->q_id, group.type, group.policy);
                            if (group.gid < 0)
                                    return -EFAULT;
                            if (copy_to_user(optval, &group, len))
                                    return -EFAULT;
                    } else
                    
                    if (pfq_join_group(group.gid, pq->q_id, group.type, group.policy) < 0) {
                    	    printk(KERN_INFO "[PFQ|%d] join error (gid:%d)\n", pq->q_id, group.gid);
                            return -EPERM;
                    }
                    printk(KERN_INFO "[PFQ|%d] join -> group id:%d type:%d\n", pq->q_id, 
				    group.gid, group.type);
            } break;

        case SO_GROUP_STATS: 
            {
                    struct pfq_stats stat;
                    int gid;

		    if (len != sizeof(stat)) 
                            return -EINVAL;
                    if (copy_from_user(&stat, optval, len)) 
                            return -EFAULT;
                    
		    gid = stat.recv;
                    if (gid < 0  || gid >= Q_MAX_GROUP)
			    return -EINVAL;

                    stat.recv = sparse_read(&pfq_groups[gid].recv);
                    stat.lost = sparse_read(&pfq_groups[gid].lost);
                    stat.drop = sparse_read(&pfq_groups[gid].drop);

                    if (copy_to_user(optval, &stat, sizeof(stat)))
                            return -EFAULT;
            } break;

        default:
            return -EFAULT;
        }

        return 0;
}


static
int pfq_setsockopt(struct socket *sock,
                   int level, int optname,
                   char __user * optval,
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,31))
                   unsigned
#endif
                   int optlen)
{
        struct pfq_opt *pq = pfq_sk(sock->sk)->opt;
        long int val;
        bool found = true;

        if (pq == NULL)
                return -EINVAL;
        if (get_user(val, (long int *)optval))
                return -EFAULT;

        switch(optname)
        {
        case SO_TOGGLE_QUEUE: 
            {
                    int active;
                    if (optlen != sizeof(active))
                            return -EINVAL;
                    if (copy_from_user(&active, optval, optlen))
                            return -EFAULT;

                    if (active)
                    {
                            if (!pq->q_addr)
                            {
                                    struct pfq_queue_descr *sq;

                                    /* alloc queue memory */
                                    pq->q_addr = mpdb_queue_alloc(pq, mpdb_queue_tot_mem(pq), &pq->q_queue_mem);
                                    if (pq->q_addr == NULL) {
                                            return -ENOMEM;
                                    }
                                    sq = (struct pfq_queue_descr *)pq->q_addr;
                                    sq->data      = (1<<28);
                                    sq->poll_wait = 0;

                                    wmb();

                                    pq->q_active = true;
                            }
                    }
                    else {
                        pq->q_active = false;
                        msleep(10);
                        wmb();
                        mpdb_queue_free(pq);
                    }

            } break;

        case SO_ADD_BINDING: 
            {
                    struct pfq_binding bind;
                    if (optlen != sizeof(struct pfq_binding)) 
                            return -EINVAL;
                    
                    if (copy_from_user(&bind, optval, optlen))
                            return -EFAULT;
                    
                    if (bind.gid < 0 || bind.gid >= Q_MAX_GROUP) {
                    	    printk(KERN_INFO "[PFQ|%d] add binding: gid:%d -> invalid group.\n", pq->q_id, bind.gid);
			    return -EINVAL; 
		    }

		    if (!__pfq_has_joined_group(bind.gid, pq->q_id)) {
                    	    printk(KERN_INFO "[PFQ|%d] add binding: gid:%d -> no permission.\n", pq->q_id, bind.gid);
			    return -EPERM;
		    }

                    pfq_devmap_update(map_set, bind.if_index, bind.hw_queue, bind.gid);
            } break;

        case SO_REMOVE_BINDING: 
            {
                    struct pfq_binding bind;
                    if (optlen != sizeof(struct pfq_binding))
                            return -EINVAL;
                    
		    if (copy_from_user(&bind, optval, optlen))
                            return -EFAULT;

                    if (bind.gid < 0 || bind.gid >= Q_MAX_GROUP) {
                    	    printk(KERN_INFO "[PFQ|%d] remove binding: gid:%d -> invalid group.\n", pq->q_id, bind.gid);
			    return -EINVAL;
		    }

		    if (!__pfq_has_joined_group(bind.gid, pq->q_id)) {
                    	    printk(KERN_INFO "[PFQ|%d] remove binding: gid:%d -> no permission.\n", pq->q_id, bind.gid);
			    return -EPERM;
		    }

                    pfq_devmap_update(map_reset, bind.if_index, bind.hw_queue, bind.gid);
            } break;

 	case SO_GROUP_STEER:
	    {
		    struct pfq_steer st;
		    
		    if (optlen != sizeof(st)) 
			    return -EINVAL;

		    if (copy_from_user(&st, optval, optlen)) 
			    return -EFAULT;
		    
                    if (st.gid < 0 || st.gid >= Q_MAX_GROUP) {
                    	    printk(KERN_INFO "[PFQ|%d] steer: gid:%d -> invalid group.\n", pq->q_id, st.gid);
			    return -EINVAL;
		    }
		    
		    if (!__pfq_has_joined_group(st.gid, pq->q_id)) {
                    	    printk(KERN_INFO "[PFQ|%d] steer: gid:%d -> no permission.\n", pq->q_id, st.gid);
			    return -EPERM;
		    }

		    if (st.name == NULL) {
			__pfq_set_steer_for_group(st.gid, NULL);
                    	printk(KERN_INFO "[PFQ|%d] steer: gid:%d -> steer NONE\n", pq->q_id, st.gid);
		    }
		    else {
                    	char name[Q_STEER_NAME_LEN]; 
			steer_function_t fun;

                    	if (strncpy_from_user(name, st.name, Q_STEER_NAME_LEN-1) < 0)
				return -EFAULT;

			name[Q_STEER_NAME_LEN-1] = '\0';
                        
			fun = pfq_get_steer_function(name);
			if (fun == NULL) {
                    		printk(KERN_INFO "[PFQ|%d] steer: gid:%d -> %s unknown function\n", pq->q_id, st.gid, name);
				return -EINVAL;
			}

			__pfq_set_steer_for_group(st.gid, fun);
                    	printk(KERN_INFO "[PFQ|%d] steer: gid:%d -> steer %s\n", pq->q_id, st.gid, name);
		    }

	    } break;
        case SO_TSTAMP_TYPE: 
            {
                    int tstamp;
                    if (optlen != sizeof(pq->q_tstamp))
                            return -EINVAL;
                    if (copy_from_user(&tstamp, optval, optlen))
                            return -EFAULT;
                    if (tstamp != 0 && tstamp != 1)
                            return -EINVAL;

                    /* update the timestamp_toggle counter */
                    atomic_add(tstamp - pq->q_tstamp, &timestamp_toggle);
                    pq->q_tstamp = tstamp;
                    printk(KERN_INFO "[PFQ|%d] timestamp_toggle => %d\n", pq->q_id, atomic_read(&timestamp_toggle));
            } break;
        
        case SO_CAPLEN: 
            {
                    if (optlen != sizeof(pq->q_caplen)) 
                            return -EINVAL;
                    if (copy_from_user(&pq->q_caplen, optval, optlen)) 
                            return -EFAULT;
                    pq->q_slot_size = DBMP_QUEUE_SLOT_SIZE(pq->q_caplen);
                    printk(KERN_INFO "[PFQ|%d] caplen:%lu -> slot_size:%lu\n", 
                                    pq->q_id, pq->q_caplen, pq->q_slot_size);
            } break;

        case SO_OFFSET: 
            {
                    if (optlen != sizeof(pq->q_offset)) 
                            return -EINVAL;
                    if (copy_from_user(&pq->q_offset, optval, optlen)) 
                            return -EFAULT;
                    printk(KERN_INFO "[PFQ|%d] offset:%lu\n", pq->q_id, pq->q_offset);
            } break;

        case SO_SLOTS: 
            {
                    if (optlen != sizeof(pq->q_slots)) 
                            return -EINVAL;
                    if (copy_from_user(&pq->q_slots, optval, optlen)) 
                            return -EFAULT;
                    printk(KERN_INFO "[PFQ|%d] queue_slots:%lu -> slot_size:%lu\n", 
                                    pq->q_id, pq->q_slots, pq->q_slot_size);
            } break;

        case SO_GROUP_LEAVE: 
            {
                    int gid;
                    if (optlen != sizeof(gid)) 
                            return -EINVAL;
                    if (copy_from_user(&gid, optval, optlen)) 
                            return -EFAULT;
                    
                    if (pfq_leave_group(gid, pq->q_id) < 0) {
                            return -EFAULT;
                    }
                    
                    printk(KERN_INFO "[PFQ|%d] leave: group id:%d\n", pq->q_id, gid);
            } break;

        default: 
            {
                    found = false; 
            } break;
        }

        return found ? 0 : sock_setsockopt(sock, level, optname, optval, optlen);
}        


static
int
pfq_memory_mmap(struct vm_area_struct *vma,
                unsigned long size, char *ptr, unsigned int flags)
{
        unsigned long start;

        vma->vm_flags |= flags;

        start = vma->vm_start;
        if (remap_vmalloc_range(vma, ptr, 0) != 0)
        {
                printk(KERN_INFO "[PFQ] remap_vmalloc_range\n");
                return -EAGAIN;
        }

        return 0;
}


static int 
pfq_mmap(struct file *file, struct socket *sock, struct vm_area_struct *vma)
{
        struct pfq_opt *pq = pfq_sk(sock->sk)->opt;
        unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);
        int ret;

        if(size % PAGE_SIZE) {
                printk(KERN_INFO "[PFQ] size not multiple of PAGE_SIZE\n");
                return -EINVAL;
        }

        if(size > pq->q_queue_mem) {
                printk(KERN_INFO "[PFQ] area too large\n");
                return -EINVAL;
        }

        if((ret = pfq_memory_mmap(vma, size, pq->q_addr, VM_LOCKED)) < 0)
                return ret;

        return 0;
}


unsigned int 
pfq_poll(struct file *file, struct socket *sock, poll_table * wait)
{
        struct sock *sk = sock->sk;
        struct pfq_sock *po = pfq_sk(sk);
        struct pfq_opt *pq;
        struct pfq_queue_descr * q;
        unsigned int mask = 0;

        pq = po->opt;
        if (pq == NULL)
                return mask;
        q = (struct pfq_queue_descr *)pq->q_addr;
        if (q == NULL)
                return mask;

        if (mpdb_queue_len(pq) >= (pq->q_slots>>1)) {
                q->poll_wait = 0; 
                mask |= POLLIN | POLLRDNORM;
        }
        else if (!q->poll_wait) {
                q->poll_wait = 1;
                poll_wait(file, &pq->q_waitqueue, wait);
        }

        return mask;
}


static
int 
pfq_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
        switch (cmd) {
#ifdef CONFIG_INET
        case SIOCGIFFLAGS:
        case SIOCSIFFLAGS:
        case SIOCGIFCONF:
        case SIOCGIFMETRIC:
        case SIOCSIFMETRIC:
        case SIOCGIFMEM:
        case SIOCSIFMEM:
        case SIOCGIFMTU:
        case SIOCSIFMTU:
        case SIOCSIFLINK:
        case SIOCGIFHWADDR:
        case SIOCSIFHWADDR:
        case SIOCSIFMAP:
        case SIOCGIFMAP:
        case SIOCSIFSLAVE:
        case SIOCGIFSLAVE:
        case SIOCGIFINDEX:
        case SIOCGIFNAME:
        case SIOCGIFCOUNT:
        case SIOCSIFHWBROADCAST:
            return(inet_dgram_ops.ioctl(sock, cmd, arg));
#endif
        default:
            return -ENOIOCTLCMD;
        }

        return 0;
}


static
void pfq_proto_ops_init(void)
{
        pfq_ops = (struct proto_ops) 
        {
                .family = PF_Q,
                .owner = THIS_MODULE,

                /* Operations that make no sense on queue sockets. */
                .connect    = sock_no_connect,
                .socketpair = sock_no_socketpair,
                .accept     = sock_no_accept,
                .getname    = sock_no_getname,
                .listen     = sock_no_listen,
                .shutdown   = sock_no_shutdown,
                .sendpage   = sock_no_sendpage,

                /* Now the operations that really occur. */
                .release    = pfq_release,    
                .bind       = sock_no_bind,         // pfq_bind,
                .mmap       = pfq_mmap,             // pfq_mmap,
                .poll       = pfq_poll,             // pfq_poll,
                .setsockopt = pfq_setsockopt,       // pfq_setsockopt,
                .getsockopt = pfq_getsockopt,       // pfq_getsockopt,
                .ioctl      = pfq_ioctl,            // pfq_ioctl,
                .recvmsg    = sock_no_recvmsg,      // pfq_recvmsg,
                .sendmsg    = sock_no_sendmsg       // pfq_sendmsg,
        };
}


static
void pfq_proto_init(void)
{
        pfq_proto = (struct proto)
        {
                .name  = "PFQ",
                .owner = THIS_MODULE,
                .obj_size = sizeof(struct pfq_sock)
        };
}


static
void pfq_net_proto_family_init(void)
{
        pfq_family_ops = (struct net_proto_family)
        {
                .family = PF_Q,
                .create = pfq_create,
                .owner = THIS_MODULE,
        };
} 


static
void register_device_handler(void)
{
        if (direct_path)
                return;
        pfq_prot_hook.func = pfq_packet_rcv;
        pfq_prot_hook.type = __constant_htons(ETH_P_ALL);
        dev_add_pack(&pfq_prot_hook);
}


static
void unregister_device_handler(void) 
{
        if (direct_path)
                return;
        dev_remove_pack(&pfq_prot_hook); /* Remove protocol hook */
}


static int __init pfq_init_module(void)
{
        int n;
        printk(KERN_WARNING "[PFQ] loading (%s)\n", Q_VERSION);

        pfq_net_proto_family_init();
        pfq_proto_ops_init();
        pfq_proto_init();

	/* create a per-cpu context */
	cpu_data = alloc_percpu(struct local_data);
	if (!cpu_data)
		return -ENOMEM;
        
        /* register pfq sniffer protocol */    
        n = proto_register(&pfq_proto, 0);
        if (n != 0)
                return n;

	/* register the pfq socket */
        sock_register(&pfq_family_ops);

        /* finally register the basic device handler */
        register_device_handler();

	/* register steer functions */

	pfq_steer_factory_init();
        
	printk(KERN_WARNING "[PFQ] ready.\n");

        return 0;
}


static void __exit pfq_exit_module(void)
{        
        int cpu;

        /* unregister the basic device handler */
        unregister_device_handler();

        /* unregister the pfq socket */
        sock_unregister(PF_Q);

        /* unregister the pfq protocol */
        proto_unregister(&pfq_proto);

        /* destroy pipeline queues (of each cpu) */

        for_each_possible_cpu(cpu) {
		
                struct local_data *local_cache = per_cpu_ptr(cpu_data, cpu);
                struct pfq_queue_skb *this_queue = &local_cache->batch_queue;
                struct sk_buff *skb;
		int n = 0;
		queue_for_each(skb, n, this_queue)
		{
                 	kfree_skb(skb);
		}
       		pfq_queue_skb_flush(this_queue);
        }

        /* free per-cpu data */
	free_percpu(cpu_data);

	/* free steer functions */
	pfq_steer_factory_free();

        printk(KERN_WARNING "[PFQ] unloaded\n");
}


/* pfq aware drivers support */

inline
int pfq_direct_capture(const struct sk_buff *skb)
{
        return direct_path && 
                __pfq_devmap_monitor_get(skb->dev->ifindex);
}


inline
int pfq_normalize_skb(struct sk_buff *skb)
{
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0)                
	skb_reset_mac_len(skb);
#else
	skb->mac_len = skb->network_header - skb->mac_header;
#endif

	if (skb->protocol == __constant_htons(ETH_P_8021Q)) {
		
		if (likely(!vlan_tx_tag_present(skb))) {
		
			struct vlan_hdr *vhdr;
			if (unlikely(!pskb_may_pull(skb, VLAN_HLEN)))
			{
				__kfree_skb(skb);
				return -1;
			}
			vhdr = (struct vlan_hdr *) skb->data;
			skb->vlan_tci = ntohs(vhdr->h_vlan_TCI);

		}
	}

#ifdef PFQ_USE_SKB_LINEARIZE
#pragma message "[PFQ] using skb_linearize"
	if(skb_linearize(skb) < 0)
	{
		__kfree_skb(skb);
		return -1;
	}
#endif
	return 0;
}


int
pfq_netif_receive_skb(struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb)))
        {
		if (pfq_normalize_skb(skb) < 0)
                	return NET_RX_DROP;

		pfq_direct_receive(skb, skb->dev->ifindex, skb_get_rx_queue(skb), true);
		return NET_RX_SUCCESS;
	}

	return netif_rx(skb);
}


int 
pfq_netif_rx(struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb)))
        {
		if (pfq_normalize_skb(skb) < 0)
                	return NET_RX_DROP;
		
		pfq_direct_receive(skb, skb->dev->ifindex, skb_get_rx_queue(skb), true);
		return NET_RX_SUCCESS;
	}

	return netif_receive_skb(skb);
}


gro_result_t 
pfq_gro_receive(struct napi_struct *napi, struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb)))
        {
		if (pfq_normalize_skb(skb) < 0)
                	return GRO_DROP;

                pfq_direct_receive(skb, skb->dev->ifindex, skb_get_rx_queue(skb), true);
                return GRO_NORMAL;
        }

        return napi_gro_receive(napi,skb);
}



EXPORT_SYMBOL_GPL(pfq_netif_rx);
EXPORT_SYMBOL_GPL(pfq_netif_receive_skb);
EXPORT_SYMBOL_GPL(pfq_gro_receive);


module_init(pfq_init_module);
module_exit(pfq_exit_module);
