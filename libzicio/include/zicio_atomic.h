#ifndef ZICIO_ATOMIC_H
#define ZICIO_ATOMIC_H

#include <stdbool.h>
#include <stdatomic.h>
#include <linux/zicio.h>

#define STATUS_BITS_SIZE 2
#define STATUS_BITS_SHIFT 0
#define STATUS_BITS_MASK \
	(((1ULL << STATUS_BITS_SIZE) - 1) << STATUS_BITS_SHIFT)
#define BYTES_BITS_SHIFT (2)
#define BYTES_BITS_MASK (0xFFFFFFFc)

/*
 * zicio_read_status
 */
static int
zicio_read_status(zicio_switch_board *msb, int idx) 
{
	int ret;

	ret = atomic_load((int *) &msb->entries[idx].val);
	ret &= STATUS_BITS_MASK;
	ret = ret >> STATUS_BITS_SHIFT;

	return ret;
}

/*
 * zicio_read_bytes
 */
static int
zicio_read_bytes(zicio_switch_board *msb, int idx)
{
	int ret;

	ret = atomic_load((int *) &msb->entries[idx].val);
	ret &= BYTES_BITS_MASK;
	ret = ((unsigned) ret) >> BYTES_BITS_SHIFT;

	return ret;
}

/*
 * zicio_set_status
 */
static void 
zicio_set_status(zicio_switch_board *msb, int idx, int new_val) 
{
	int value;

	value = atomic_load((int *) &msb->entries[idx].val);

	value &= ~STATUS_BITS_MASK;
	value |= (new_val << STATUS_BITS_SHIFT);

	atomic_store((int *) &msb->entries[idx].val, value);
}

/*
 * zicio_cas_status
 *
 * Return 0, success
 * Return -1, fail
 */
static int 
zicio_cas_status(zicio_switch_board *msb, int idx, 
  int expected, int new_val) 
{
	int expected_value;
	int new_value;

	expected_value = atomic_load((int *) &msb->entries[idx].val);
	new_value = expected_value;

	expected_value &= ~STATUS_BITS_MASK;
	expected_value |= (expected << STATUS_BITS_SHIFT);

	new_value &= ~STATUS_BITS_MASK;
	new_value |= (new_val << STATUS_BITS_SHIFT);

	if(atomic_compare_exchange_weak((int *) &msb->entries[idx].val, 
					   &expected_value, new_value) == true)
		return 0;
	else 
		return -1;
}

#endif /* ZICIO_ATOMIC_H */
