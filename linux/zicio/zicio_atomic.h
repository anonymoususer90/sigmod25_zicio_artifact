#ifndef ZICIO_ATOMIC_H
#define ZICIO_ATOMIC_H

#include <linux/atomic.h>
#include <uapi/linux/zicio.h>

#define STATUS_BITS_SIZE 2
#define STATUS_BITS_SHIFT 0
#define STATUS_BITS_MASK \
	(((1ULL << STATUS_BITS_SIZE) - 1) << STATUS_BITS_SHIFT)
#define BYTES_BITS_SHIFT (2)
#define BYTES_BITS_MASK (0xFFFFFFFc)


/*
 * zicio_read_status
 */
static inline int
zicio_read_status(zicio_switch_board *msb, int idx) 
{
	int ret;

	ret = atomic_read((atomic_t *) &msb->entries[idx].val) & STATUS_BITS_MASK;
	ret = ret >> STATUS_BITS_SHIFT;

	return ret;
}

/*
 * zicio_set_status
 */
static inline void
zicio_set_status(zicio_switch_board *msb, int idx, int new_val) 
{
	int value;

	value = atomic_read((atomic_t *) &msb->entries[idx].val);

	value &= ~STATUS_BITS_MASK;
	value |= (new_val << STATUS_BITS_SHIFT);

	atomic_set((atomic_t *) &msb->entries[idx].val, value);
}

/*
 * zicio_cmpxchg_status
 */
static inline bool
zicio_cmpxchg_status(zicio_switch_board *msb, int idx, int new_val,
		int old_status)
{
	int value, expected_val, old_val;

	value = atomic_read((atomic_t *) &msb->entries[idx].val);
	value &= ~STATUS_BITS_MASK;
	expected_val = value;

	value |= (new_val << STATUS_BITS_SHIFT);
	expected_val |= (old_status << STATUS_BITS_SHIFT);

	old_val = atomic_cmpxchg((atomic_t *) &msb->entries[idx].val, expected_val,
			value);

	return (old_val == expected_val);
}

/*
 * zicio_set_bytes
 */
static inline void
zicio_set_bytes(zicio_switch_board *msb, int idx, int bytes)
{
	int value;

	value = atomic_read((atomic_t *) &msb->entries[idx].val);

	value &= ~BYTES_BITS_MASK;
	value |= (bytes << BYTES_BITS_SHIFT);

	atomic_set((atomic_t *) &msb->entries[idx].val, value);
}

/*
 * zicio_cas_status
 *
 * Return 0, success
 * Return -1, fail
 */
static inline int
zicio_cas_status(zicio_switch_board *msb, int idx, 
  int expected, int new_val) 
{
	int tmp;
	int expected_value;
	int new_value;

	expected_value = atomic_read((atomic_t *) &msb->entries[idx].val);
	new_value = expected_value;

	expected_value &= ~STATUS_BITS_MASK;
	expected_value |= (expected << STATUS_BITS_SHIFT);

	new_value &= ~STATUS_BITS_MASK;
	new_value |= (new_val << STATUS_BITS_SHIFT);

	tmp = atomic_cmpxchg((atomic_t *) &msb->entries[idx].val, 
					expected_value, new_value);

	if (tmp == expected_value)
		return 0;
	else 
		return -1;
}
#endif /* ZICIO_ATOMIC_H */
