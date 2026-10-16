/* SPDX-License-Identifier: ISC
 * MT7981 IQ export: consume native DMA skbs before relay linearization/copying.
 * A bounded skb queue feeds an Ethernet worker. Each worker reads every
 * selected sample once into the final network skb.
 *
 * Output format is selected per run (optional 8th config token):
 *   iqd1  (default) - 32-byte IQD1 header + CI8 payload (legacy host tools).
 *   vrt8            - VITA 49 (VRT) Signal-Data, CI8 payload.
 *   vrt16           - VITA 49 (VRT) Signal-Data, int16 payload carrying the
 *                     full 14-bit capture resolution left-justified into 16
 *                     bits; a periodic IF-Context packet announces RF
 *                     frequency, sample rate and data format.
 * Optional ninth config token: native word stride in bytes (4 default or 8).
 * A native sample word packs signed 14-bit I in bits [29:16] and 14-bit Q in
 * bits [13:0]. CI8 keeps the top 8 bits of each; Ci16 left-justifies the 14.
 * IQD1 header (32 bytes, little endian): magic, run, sequence, offset(u16),
 * samples(u16), nominal rate, format(u16)=1(CI8), flags(u16), source, reserved.
 */
#include <linux/etherdevice.h>
#include <linux/ip.h>
#include <linux/inet.h>
#include <linux/if_arp.h>
#include <linux/udp.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <net/ip.h>
#include <net/net_namespace.h>

struct mt7915_iq_export {
	struct net_device *netdev;
	struct task_struct *task;
	struct sk_buff_head queue;
	wait_queue_head_t wait;
	__be32 src, dst;
	__be16 port;
	u8 mac[ETH_ALEN];
	u32 rate, run;
	u64 freq;
	u16 fmt, stride;
	u8 context_count;
	u32 sig_ctr;
	atomic64_t received, dropped, sent, tx_errors, tx_busy;
};
#define IQ_FMT_IQD1  0
#define IQ_FMT_VRT8  1
#define IQ_FMT_VRT16 2
static DEFINE_MUTEX(mt7915_iq_config_lock);

/* Build an Ethernet/IPv4/UDP skb with headers set; caller skb_put()s payload. */
static struct sk_buff *mt7915_iq_skb(struct mt7915_iq_export *x,
				     unsigned int payload, u16 ipid)
{
	const unsigned int size = ETH_HLEN + 20 + 8 + payload;
	struct ethhdr *eth;
	struct iphdr *ip;
	struct udphdr *udp;
	struct sk_buff *out = alloc_skb(LL_RESERVED_SPACE(x->netdev) + 6 + size, GFP_KERNEL);

	if (!out)
		return NULL;
	/* Six bytes align IP to four and VRT sample pairs to eight bytes.
	 * NET_IP_ALIGN is zero on arm64, so use explicit padding. */
	skb_reserve(out, LL_RESERVED_SPACE(x->netdev) + 6);
	eth = (void *)skb_put(out, ETH_HLEN);
	skb_reset_mac_header(out);
	ether_addr_copy(eth->h_dest, x->mac);
	ether_addr_copy(eth->h_source, x->netdev->dev_addr);
	eth->h_proto = htons(ETH_P_IP);
	skb_set_network_header(out, out->len);
	ip = (void *)skb_put(out, 20);
	*ip = (struct iphdr){ .version=4, .ihl=5, .ttl=64, .protocol=IPPROTO_UDP,
		.tot_len=htons(20+8+payload), .id=htons(ipid), .frag_off=htons(IP_DF),
		.saddr=x->src, .daddr=x->dst };
	ip_send_check(ip);
	skb_set_transport_header(out, out->len);
	udp = (void *)skb_put(out, 8);
	*udp = (struct udphdr){ .source=x->port, .dest=x->port,
		.len=htons(8+payload), .check=0 };
	return out;
}

static void mt7915_iq_xmit(struct mt7915_iq_export *x, struct sk_buff *out)
{
	int ret;
	u64 deadline = 0;
	out->dev = x->netdev;
	out->protocol = htons(ETH_P_IP);
	out->ip_summed = CHECKSUM_NONE;
	/* Match the reference SDR's direct submission path. Retry BUSY while we
	 * still own the skb; completed/error returns have consumed it already. */
	for (;;) {
		ret = __dev_direct_xmit(out, 0);
		if (dev_xmit_complete(ret)) break;
		atomic64_inc(&x->tx_busy);
		if (!deadline) deadline = ktime_get_ns() + 1000000;
		if (kthread_should_stop() || ktime_get_ns() >= deadline) {
			kfree_skb(out);
			atomic64_inc(&x->tx_errors);
			return;
		}
		cond_resched();
	}
	if (ret == NETDEV_TX_OK) atomic64_inc(&x->sent);
	else atomic64_inc(&x->tx_errors);
}

/* VITA 49 IF-Context: RF reference frequency, sample rate, data format.
 * Receivers block until they see this, then auto-detect the stream. */
static void mt7915_iq_send_context(struct mt7915_iq_export *x, int bits, u16 ipid)
{
	struct sk_buff *cx = mt7915_iq_skb(x, 36, ipid);
	u8 *q;
	if (!cx) { atomic64_inc(&x->tx_errors); return; }
	q = skb_put(cx, 36);
	put_unaligned_be32((4u << 28) | ((x->context_count++ & 15u) << 16) | 9u, q);
	put_unaligned_be32(1, q + 4);                        /* stream ID */
	put_unaligned_be32((1u<<27)|(1u<<21)|(1u<<15), q+8); /* RF freq | rate | format */
	put_unaligned_be64((s64)(x->freq << 20), q + 12);    /* Hz, 20-bit fraction */
	put_unaligned_be64((s64)((u64)x->rate << 20), q + 20);
	/* Complex signed samples: item width AND packing-field width. Both fields
	 * encode width minus one; omitting packing width falsely declares 1 bit. */
	put_unaligned_be32((1u << 29) | (((u32)bits - 1u) << 6) | ((u32)bits - 1u), q + 28);
	put_unaligned_be32(0, q + 32);
	mt7915_iq_xmit(x, cx);
}

/* Convert the selected RF path directly from skb fragments. Native buffers
 * use a verified per-run word stride: 4 for dense path0, 8 for alternating
 * paths. The RF profile selects this independently of the wire sample format.
 * Sequential mapping also works if a sample crosses a fragment boundary. */
static int mt7915_iq_convert(struct sk_buff *in, struct skb_seq_state *st,
                            unsigned offset, unsigned count, u8 *dst, int bits, unsigned stride)
{
    while (count) {
        const u8 *src;
        unsigned available = skb_seq_read(offset, &src, st);
        unsigned n = min(count, available / stride), i;
        if (!n) {
            u8 word[4];
            u32 v;
            if (skb_copy_bits(in, 44 + offset, word, 4)) return -EINVAL;
            v = get_unaligned_le32(word);
            if (bits == 16) { put_unaligned_be32((v << 2) & 0xfffcfffc, dst); dst += 4; }
            else { *dst++ = v >> 22; *dst++ = v >> 6; }
            offset += stride; count--; continue;
        }
        for (i = 0; i + 1 < n; i += 2, src += 2 * stride) {
            u32 a = get_unaligned_le32(src), b = get_unaligned_le32(src + stride);
            if (bits == 16) {
                u64 pair = ((u64)a << 32) | b;
                put_unaligned_be64((pair << 2) & 0xfffcfffcfffcfffcULL, dst);
                dst += 8;
            } else {
                put_unaligned_le32(((a >> 22) & 0xff) | ((a << 2) & 0xff00) |
                                  ((b >> 6) & 0xff0000) | ((b << 18) & 0xff000000), dst);
                dst += 4;
            }
        }
        if (i < n) {
            u32 v = get_unaligned_le32(src);
            if (bits == 16) { put_unaligned_be32((v << 2) & 0xfffcfffc, dst); dst += 4; }
            else { *dst++ = v >> 22; *dst++ = v >> 6; }
        }
        offset += n * stride; count -= n;
    }
    return 0;
}

static int mt7915_iq_worker(void *arg)
{
	struct mt7915_iq_export *x = arg;
	struct sk_buff *in;

	while (!kthread_should_stop()) {
		wait_event_interruptible(x->wait,
			kthread_should_stop() || !skb_queue_empty(&x->queue));
		if (kthread_should_stop())
			break;
		while ((in = skb_dequeue(&x->queue))) {
			u32 seq, source;
			u8 meta[8];
			struct skb_seq_state seqst;
			int half, convert_ret;
            unsigned samples = 8192 / x->stride;
			if (skb_copy_bits(in, 36, meta, sizeof(meta))) {
				atomic64_inc(&x->dropped);
				kfree_skb(in);
				continue;
			}
			seq = get_unaligned_le32(meta);
			source = get_unaligned_le32(meta + 4);
			if (x->fmt == IQ_FMT_IQD1) {
			for (half = 0; half < samples / 512; half++) {
				struct sk_buff *out;
				u8 *h, *data;
				const unsigned int payload = 32 + 1024;

				out = mt7915_iq_skb(x, payload, seq*4+half);
				if (!out) { atomic64_inc(&x->tx_errors); break; }
				h = skb_put_zero(out, 32);
				memcpy(h, "IQD1", 4);
				put_unaligned_le32(x->run, h+4);
				put_unaligned_le32(seq * (samples / 1024) + half / 2, h+8);
				put_unaligned_le16((half % 2)*512, h+12);
				put_unaligned_le16(512, h+14);
				put_unaligned_le32(x->rate, h+16);
				put_unaligned_le16(1, h+20);
				put_unaligned_le32(source, h+24);
				data = skb_put(out, 1024);
                skb_prepare_seq_read(in, 44, 44 + 8192, &seqst);
                convert_ret = mt7915_iq_convert(in, &seqst, half * 512 * x->stride, 512, data, 8, x->stride);
                skb_abort_seq_read(&seqst);
                if (convert_ret) {
                    kfree_skb(out); atomic64_inc(&x->dropped); break;
                }
				mt7915_iq_xmit(x, out);
			}
			} else {
				int bits = (x->fmt == IQ_FMT_VRT16) ? 16 : 8;
				int bps = (bits == 16) ? 4 : 2;
				/* MTU-safe chunks; sample count depends on the selected native lane. */
				int chunk = bits == 16 ? 360 : 720;
				int nchunks = DIV_ROUND_UP(samples, chunk);
				int c;
				for (c = 0; c < nchunks; c++) {
					struct sk_buff *out;
					u8 *p, *d;
					int nsamples = min(chunk, (int)samples - c * chunk);
					unsigned int payload = 8 + nsamples * bps;

					if ((x->sig_ctr++ % 128) == 0)
						mt7915_iq_send_context(x, bits, seq*8+c);
					out = mt7915_iq_skb(x, payload, seq*8+c+4);
					if (!out) { atomic64_inc(&x->tx_errors); break; }
					p = skb_put(out, 8);
					/* Preserve raw-source discontinuities in the VRT count. */
					put_unaligned_be32((1u<<28) | (((seq*nchunks+c)&15)<<16)
						| (2u + (u32)nsamples*bps/4), p); /* Signal-Data, w/ SID */
					put_unaligned_be32(1, p + 4);          /* stream ID */
					d = skb_put(out, nsamples * bps);
                    skb_prepare_seq_read(in, 44, 44 + 8192, &seqst);
                    convert_ret = mt7915_iq_convert(in, &seqst, c * chunk * x->stride, nsamples, d, bits, x->stride);
                    /* kmap_atomic mappings must end before xmit can reschedule. */
                    skb_abort_seq_read(&seqst);
                    if (convert_ret) {
                        kfree_skb(out); atomic64_inc(&x->dropped); break;
                    }
					mt7915_iq_xmit(x, out);
				}
			}
			kfree_skb(in);
			cond_resched();
			if (kthread_should_stop()) break;
		}
	}
	return 0;
}

/* Takes ownership only when configured and the packet matches our exporter. */
bool mt7915_iq_rx(struct mt7915_dev *dev, struct sk_buff *skb)
{
	struct mt7915_iq_export *x;
	bool taken = false;
	if (skb->len != 8240 || skb_headlen(skb) < 44 || skb->data[32] != 0xf1)
		return false;
	rcu_read_lock();
	x = rcu_dereference(dev->iq_export);
	if (x) {
		taken = true;
		atomic64_inc(&x->received);
		if (skb_queue_len(&x->queue) >= 8192) {
			atomic64_inc(&x->dropped);
			dev_kfree_skb_any(skb);
		} else {
			skb_queue_tail(&x->queue, skb);
			wake_up(&x->wait);
		}
	}
	rcu_read_unlock();
	return taken;
}

static void mt7915_iq_stop_locked(struct mt7915_dev *dev)
{
	struct mt7915_iq_export *x = rcu_dereference_protected(dev->iq_export,
		lockdep_is_held(&mt7915_iq_config_lock));
	if (!x) return;
	RCU_INIT_POINTER(dev->iq_export, NULL);
	synchronize_rcu();
	kthread_stop(x->task);
	skb_queue_purge(&x->queue);
	dev_put(x->netdev);
	kfree(x);
}
void mt7915_iq_stop(struct mt7915_dev *dev)
{
	mutex_lock(&mt7915_iq_config_lock);
	mt7915_iq_stop_locked(dev);
	mutex_unlock(&mt7915_iq_config_lock);
}
static ssize_t mt7915_iq_config_write(struct file *file, const char __user *user,
				    size_t count, loff_t *pos)
{
	struct mt7915_dev *dev=file->private_data;
	struct mt7915_iq_export *x;
	char text[160], iface[IFNAMSIZ], src[16], dst[16], mac[18], fmt[8]="";
	unsigned int port, rate, stride = 4;
	unsigned long long freq=0;
	int ret=-EINVAL, nf;
	if (!count || count >= sizeof(text)) return -EINVAL;
	if (copy_from_user(text,user,count)) return -EFAULT;
	text[count]=0;
	mutex_lock(&mt7915_iq_config_lock);
	if (sysfs_streq(text,"stop")) {
		mt7915_iq_stop_locked(dev); ret=count; goto out;
	}
	nf = sscanf(text,"%15s %15s %15s %17s %u %u %llu %7s %u",
		    iface,src,dst,mac,&port,&rate,&freq,fmt,&stride);
	if ((stride != 4 && stride != 8) || nf<6 || !port || port>65535 || (rate!=20000000 && rate!=40000000 && rate!=80000000)) goto out;
	if (rcu_access_pointer(dev->iq_export)) { ret=-EBUSY; goto out; }
	x=kzalloc(sizeof(*x),GFP_KERNEL);
	if (!x) { ret=-ENOMEM; goto out; }
	if (!in4_pton(src,-1,(u8 *)&x->src,-1,NULL) ||
	    !in4_pton(dst,-1,(u8 *)&x->dst,-1,NULL) ||
	    !mac_pton(mac,x->mac) || !is_valid_ether_addr(x->mac)) goto free;
	x->fmt = IQ_FMT_IQD1;
	if (nf>=8) {
		if (!strcmp(fmt,"vrt16")) x->fmt=IQ_FMT_VRT16;
		else if (!strcmp(fmt,"vrt8")) x->fmt=IQ_FMT_VRT8;
		else if (strcmp(fmt,"iqd1")) goto free;
	}
	x->freq = (nf>=7 && freq) ? freq : 2437000000ULL;
	x->netdev=dev_get_by_name(&init_net,iface);
	if (!x->netdev) { ret=-ENODEV; goto free; }
	if (!netif_running(x->netdev) || x->netdev->type!=ARPHRD_ETHER ||
	    x->netdev->mtu < (x->fmt==IQ_FMT_IQD1 ? 1084 : 1476)) goto put;
	x->stride=stride;
	x->port=htons(port); x->rate=rate; x->run=(u32)ktime_get_ns();
	skb_queue_head_init(&x->queue); init_waitqueue_head(&x->wait);
	x->task=kthread_create(mt7915_iq_worker,x,"iq-udp");
	if (IS_ERR(x->task)) { ret=PTR_ERR(x->task); goto put; }
	if (cpu_online(1)) kthread_bind(x->task,1);
	set_user_nice(x->task, -20);
	rcu_assign_pointer(dev->iq_export,x);
	wake_up_process(x->task);
	ret=count; goto out;
put:
	dev_put(x->netdev);
free:
	kfree(x);
out:
	mutex_unlock(&mt7915_iq_config_lock);
	return ret;
}
static ssize_t mt7915_iq_config_read(struct file *file,char __user *user,
				   size_t count,loff_t *pos)
{
	struct mt7915_dev *dev=file->private_data;
	struct mt7915_iq_export *x;
	char text[256]; int n;
	mutex_lock(&mt7915_iq_config_lock);
	x=rcu_dereference_protected(dev->iq_export,lockdep_is_held(&mt7915_iq_config_lock));
	if (!x) n=scnprintf(text,sizeof(text),"stopped\n");
	else n=scnprintf(text,sizeof(text),"run=%u fmt=%s freq=%llu rate=%u stride=%u received=%lld dropped=%lld sent=%lld tx_errors=%lld queued=%u rx_pid=%d tx_busy=%lld\n",
		x->run,x->fmt==IQ_FMT_VRT16?"vrt16":x->fmt==IQ_FMT_VRT8?"vrt8":"iqd1",
		x->freq,x->rate,x->stride,atomic64_read(&x->received),atomic64_read(&x->dropped),
		atomic64_read(&x->sent),atomic64_read(&x->tx_errors),skb_queue_len(&x->queue),
		dev->mt76.napi[MT_RXQ_MCU].thread ? task_pid_nr(dev->mt76.napi[MT_RXQ_MCU].thread) : 0,
		atomic64_read(&x->tx_busy));
	mutex_unlock(&mt7915_iq_config_lock);
	return simple_read_from_buffer(user,count,pos,text,n);
}
static const struct file_operations mt7915_iq_config_fops={
	.owner=THIS_MODULE,.open=simple_open,.read=mt7915_iq_config_read,
	.write=mt7915_iq_config_write,.llseek=default_llseek,
};
