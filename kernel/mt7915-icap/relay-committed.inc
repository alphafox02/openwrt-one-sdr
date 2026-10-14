/* SPDX-License-Identifier: ISC
 * relay_reserve publishes offset before the producer copies bytes. The generic
 * relay reader can therefore expose a partially written current subbuffer.
 * Snapshot only flushed subbuffers under the writer lock, then copy without
 * that lock. Unconsumed subbuffers cannot be reused by this non-overwrite relay.
 * Use one reader; don't reset the relay concurrently with acquisition.
 */
static DEFINE_SPINLOCK(mt7915_fwlog_lock);
static int mt7915_fwlog_open(struct inode *inode, struct file *file)
{
	return relay_file_operations.open(inode, file);
}
static int mt7915_fwlog_release(struct inode *inode, struct file *file)
{
	return relay_file_operations.release(inode, file);
}
static ssize_t mt7915_fwlog_read(struct file *file, char __user *user,
			       size_t count, loff_t *pos)
{
	struct rchan_buf *buf = file->private_data;
	size_t produced, consumed, available = 0, i;
	unsigned long flags;

	spin_lock_irqsave(&mt7915_fwlog_lock, flags);
	produced = buf->subbufs_produced;
	consumed = buf->subbufs_consumed;
	if (produced - consumed > buf->chan->n_subbufs) {
		spin_unlock_irqrestore(&mt7915_fwlog_lock, flags);
		return -EOVERFLOW;
	}
	for (i = consumed; i != produced && available < count; i++)
		available += buf->chan->subbuf_size -
			buf->padding[i % buf->chan->n_subbufs];
	if (available)
		available -= buf->bytes_consumed;
	spin_unlock_irqrestore(&mt7915_fwlog_lock, flags);
	if (!available)
		return -EAGAIN;
	return relay_file_operations.read(file, user, min(count, available), pos);
}
static const struct file_operations mt7915_fwlog_fops = {
	.owner = THIS_MODULE,
	.open = mt7915_fwlog_open,
	.release = mt7915_fwlog_release,
	.read = mt7915_fwlog_read,
	.llseek = no_llseek,
};
