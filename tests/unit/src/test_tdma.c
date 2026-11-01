/*
 * Testes do superquadro TDMA (docs/03-protocolo-uwb.md, seção 5.2): T7.0 em
 * docs/08, seção 10.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include <rtls/tdma.h>

static const struct rtls_msg_beacon beacon = {
	.type = RTLS_MSG_BEACON,
	.superframe = 100,
	.n_slots = 8,
	.slot_us = 12000,
};

/* T7.0 */
ZTEST(tdma, test_slot_of_and_period)
{
	zassert_equal(rtls_tdma_slot_of(0x1001), 0);
	zassert_equal(rtls_tdma_slot_of(0x1008), 7);
	zassert_equal(rtls_tdma_slot_of(0x1000), -EINVAL);

	/* 4 ms + 8 x 12 ms = 100 ms */
	zassert_equal(rtls_tdma_period_us(8, 12000), 100000);
}

/* T7.0 */
ZTEST(tdma, test_slots_after_beacon)
{
	struct rtls_tdma_sync s = {0};
	int64_t start;

	zassert_equal(rtls_tdma_slot_start(&s, 0, &start), -EAGAIN, "sem beacon, sem slot");

	zassert_ok(rtls_tdma_on_beacon(&s, &beacon, 1000000));
	zassert_true(s.synced);

	zassert_ok(rtls_tdma_slot_start(&s, 0, &start));
	zassert_equal(start, 1004000);
	zassert_ok(rtls_tdma_slot_start(&s, 7, &start));
	zassert_equal(start, 1088000);
	zassert_equal(rtls_tdma_slot_start(&s, 8, &start), -ERANGE);
	zassert_equal(rtls_tdma_slot_start(&s, -1, &start), -ERANGE);

	zassert_equal(rtls_tdma_next_beacon(&s), 1100000);
}

/* T7.0: 3 beacons perdidos seguidos param a tag; um beacon novo volta */
ZTEST(tdma, test_missed_beacons)
{
	struct rtls_tdma_sync s = {0};
	int64_t start;

	zassert_ok(rtls_tdma_on_beacon(&s, &beacon, 0));

	rtls_tdma_on_missed(&s);
	zassert_true(s.synced);
	zassert_equal(s.superframe, 101);
	zassert_ok(rtls_tdma_slot_start(&s, 1, &start));
	zassert_equal(start, 100000 + 4000 + 12000, "slot pelo beacon previsto");

	rtls_tdma_on_missed(&s);
	zassert_true(s.synced);
	rtls_tdma_on_missed(&s);
	zassert_false(s.synced);
	zassert_equal(rtls_tdma_slot_start(&s, 1, &start), -EAGAIN);

	/* sem sincronismo, perder mais não avança nada */
	rtls_tdma_on_missed(&s);
	zassert_equal(s.missed, RTLS_TDMA_MAX_MISSED);

	zassert_ok(rtls_tdma_on_beacon(&s, &beacon, 5000000));
	zassert_true(s.synced);
	zassert_equal(s.missed, 0);
	zassert_ok(rtls_tdma_slot_start(&s, 0, &start));
	zassert_equal(start, 5004000);
}

/* T7.0 */
ZTEST(tdma, test_invalid_beacon)
{
	struct rtls_tdma_sync s = {0};
	struct rtls_msg_beacon bad = beacon;

	bad.n_slots = 0;
	zassert_equal(rtls_tdma_on_beacon(&s, &bad, 0), -EINVAL);
	bad = beacon;
	bad.slot_us = 0;
	zassert_equal(rtls_tdma_on_beacon(&s, &bad, 0), -EINVAL);
	zassert_false(s.synced);
}

ZTEST_SUITE(tdma, NULL, NULL, NULL, NULL, NULL);
