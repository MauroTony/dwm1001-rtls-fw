#include <errno.h>
#include <stddef.h>

#include <rtls/tdma.h>

int rtls_tdma_slot_of(uint16_t tag_id)
{
	return (tag_id >= RTLS_TDMA_FIRST_TAG_ID) ? (int)(tag_id - RTLS_TDMA_FIRST_TAG_ID)
						  : -EINVAL;
}

uint32_t rtls_tdma_period_us(uint8_t n_slots, uint16_t slot_us)
{
	return RTLS_TDMA_BEACON_SEG_US + (uint32_t)n_slots * slot_us;
}

int rtls_tdma_on_beacon(struct rtls_tdma_sync *s, const struct rtls_msg_beacon *b, int64_t rx_us)
{
	if (b->n_slots == 0 || b->slot_us == 0) {
		return -EINVAL;
	}

	s->synced = true;
	s->missed = 0;
	s->n_slots = b->n_slots;
	s->slot_us = b->slot_us;
	s->superframe = b->superframe;
	s->beacon_us = rx_us;
	return 0;
}

void rtls_tdma_on_missed(struct rtls_tdma_sync *s)
{
	if (!s->synced) {
		return;
	}

	s->beacon_us = rtls_tdma_next_beacon(s);
	s->superframe++;
	if (++s->missed >= RTLS_TDMA_MAX_MISSED) {
		s->synced = false;
	}
}

int rtls_tdma_slot_start(const struct rtls_tdma_sync *s, int slot, int64_t *start_us)
{
	if (!s->synced) {
		return -EAGAIN;
	}
	if (slot < 0 || slot >= s->n_slots) {
		return -ERANGE;
	}

	*start_us = s->beacon_us + RTLS_TDMA_BEACON_SEG_US + (int64_t)slot * s->slot_us;
	return 0;
}

int64_t rtls_tdma_next_beacon(const struct rtls_tdma_sync *s)
{
	if (s->n_slots == 0) {
		return s->beacon_us;
	}

	return s->beacon_us + rtls_tdma_period_us(s->n_slots, s->slot_us);
}
