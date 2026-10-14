/* SPDX-License-Identifier: ISC
 * Included by mt7915/debugfs.c. Only reads the known native capture window.
 * Retire native capture and clear ICAP_EN before taking a stable snapshot.
 */
static ssize_t mt7915_icap_window_read(struct file *file, char __user *user,
				     size_t count, loff_t *ppos)
{
	struct mt7915_dev *dev = file->private_data;
	void *buf;
	u64 start;
	ssize_t ret;

	if (*ppos < 0 || *ppos >= 65536)
		return 0;
	count = min_t(size_t, count, 65536 - *ppos);
	if ((*ppos | count) & 3)
		return -EINVAL;
	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	start = ktime_get_ns();
	mt7915_memcpy_fromio(dev, buf, 0x00400000 + *ppos, count);
	dev_info(dev->mt76.dev, "icap window: %zu bytes in %llu ns\n",
		 count, ktime_get_ns() - start);
	ret = copy_to_user(user, buf, count) ? -EFAULT : count;
	if (ret > 0)
		*ppos += ret;
	kfree(buf);
	return ret;
}

static const struct file_operations fops_icap_window = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mt7915_icap_window_read,
	.llseek = default_llseek,
};
