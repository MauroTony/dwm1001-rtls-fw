#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "tag_table.h"

struct tag_entry {
	bool used;
	int64_t last_ms;
	struct rtls_tag_record rec;
	struct rtls_round_tracker trk;
	struct tag_stats stats;
};

static struct tag_entry table[CONFIG_RTLS_GATEWAY_MAX_TAGS];

void tag_table_init(void)
{
	memset(table, 0, sizeof(table));
}

int tag_table_update(uint16_t tag_id, const struct rtls_msg_position *msg,
		     int16_t rx_power_cdbm, int64_t now_ms)
{
	struct tag_entry *entry = NULL;
	struct tag_entry *free_entry = NULL;
	int lost;

	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		if (table[i].used && table[i].rec.tag_id == tag_id) {
			entry = &table[i];
			break;
		}
		if (!table[i].used && free_entry == NULL) {
			free_entry = &table[i];
		}
	}

	if (entry == NULL) {
		if (free_entry == NULL) {
			return -ENOMEM;
		}
		/* entrada nova (ou que expirou): a sequência de round recomeça */
		entry = free_entry;
		memset(entry, 0, sizeof(*entry));
		entry->used = true;
		entry->stats.tag_id = tag_id;
	}

	lost = rtls_round_tracker_update(&entry->trk, msg->round, msg->t_ms);
	if (lost < 0) {
		return lost;
	}
	entry->stats.received++;
	entry->stats.lost += (uint32_t)lost;
	entry->stats.rx_power_sum_cdbm += rx_power_cdbm;

	entry->last_ms = now_ms;
	entry->rec.tag_id = tag_id;
	entry->rec.round = msg->round;
	entry->rec.n_ranges = msg->n_ranges;
	entry->rec.flags = msg->flags;
	entry->rec.rssi_dbm = (int8_t)(rx_power_cdbm / 100);
	entry->rec.age_ms = 0;
	for (int k = 0; k < 3; k++) {
		entry->rec.pos_mm[k] = msg->pos_mm[k];
		entry->rec.std_mm[k] = msg->std_mm[k];
	}

	return 0;
}

size_t tag_table_snapshot(struct rtls_tag_record *out, size_t max, int64_t now_ms)
{
	size_t n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		const int64_t age = now_ms - table[i].last_ms;

		if (!table[i].used) {
			continue;
		}
		if (age > CONFIG_RTLS_GATEWAY_TAG_TIMEOUT_MS) {
			table[i].used = false;
			continue;
		}
		if (n < max) {
			out[n] = table[i].rec;
			out[n].age_ms = (uint16_t)MIN(age, (int64_t)UINT16_MAX);
			n++;
		}
	}

	return n;
}

size_t tag_table_get_stats(struct tag_stats *out, size_t max, bool reset)
{
	size_t n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(table) && n < max; i++) {
		if (!table[i].used) {
			continue;
		}
		out[n++] = table[i].stats;
		if (reset) {
			table[i].stats.received = 0;
			table[i].stats.lost = 0;
			table[i].stats.rx_power_sum_cdbm = 0;
		}
	}

	return n;
}
