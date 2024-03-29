
/* Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* MMC block test */

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/debugfs.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/delay.h>
#include <linux/test-iosched.h>
#include "queue.h"

#define MODULE_NAME "mmc_block_test"
#define TEST_MAX_SECTOR_RANGE		(600*1024*1024) /* 600 MB */
#define TEST_MAX_BIOS_PER_REQ		120
#define CMD23_PACKED_BIT	(1 << 30)
#define LARGE_PRIME_1	1103515367
#define LARGE_PRIME_2	35757
#define PACKED_HDR_VER_MASK 0x000000FF
#define PACKED_HDR_RW_MASK 0x0000FF00
#define PACKED_HDR_NUM_REQS_MASK 0x00FF0000
#define PACKED_HDR_BITS_16_TO_29_SET 0x3FFF0000

#define test_pr_debug(fmt, args...) pr_debug("%s: "fmt"\n", MODULE_NAME, args)
#define test_pr_info(fmt, args...) pr_info("%s: "fmt"\n", MODULE_NAME, args)
#define test_pr_err(fmt, args...) pr_err("%s: "fmt"\n", MODULE_NAME, args)

enum is_random {
	NON_RANDOM_TEST,
	RANDOM_TEST,
};

enum mmc_block_test_testcases {
	/* Start of send write packing test group */
	SEND_WRITE_PACKING_MIN_TESTCASE,
	TEST_STOP_DUE_TO_READ = SEND_WRITE_PACKING_MIN_TESTCASE,
	TEST_STOP_DUE_TO_READ_AFTER_MAX_REQS,
	TEST_STOP_DUE_TO_FLUSH,
	TEST_STOP_DUE_TO_FLUSH_AFTER_MAX_REQS,
	TEST_STOP_DUE_TO_EMPTY_QUEUE,
	TEST_STOP_DUE_TO_MAX_REQ_NUM,
	TEST_STOP_DUE_TO_THRESHOLD,
	SEND_WRITE_PACKING_MAX_TESTCASE = TEST_STOP_DUE_TO_THRESHOLD,

	/* Start of err check test group */
	ERR_CHECK_MIN_TESTCASE,
	TEST_RET_ABORT = ERR_CHECK_MIN_TESTCASE,
	TEST_RET_PARTIAL_FOLLOWED_BY_SUCCESS,
	TEST_RET_PARTIAL_FOLLOWED_BY_ABORT,
	TEST_RET_PARTIAL_MULTIPLE_UNTIL_SUCCESS,
	TEST_RET_PARTIAL_MAX_FAIL_IDX,
	TEST_RET_RETRY,
	TEST_RET_CMD_ERR,
	TEST_RET_DATA_ERR,
	ERR_CHECK_MAX_TESTCASE = TEST_RET_DATA_ERR,

	/* Start of send invalid test group */
	INVALID_CMD_MIN_TESTCASE,
	TEST_HDR_INVALID_VERSION = INVALID_CMD_MIN_TESTCASE,
	TEST_HDR_WRONG_WRITE_CODE,
	TEST_HDR_INVALID_RW_CODE,
	TEST_HDR_DIFFERENT_ADDRESSES,
	TEST_HDR_REQ_NUM_SMALLER_THAN_ACTUAL,
	TEST_HDR_REQ_NUM_LARGER_THAN_ACTUAL,
	TEST_HDR_CMD23_PACKED_BIT_SET,
	TEST_CMD23_MAX_PACKED_WRITES,
	TEST_CMD23_ZERO_PACKED_WRITES,
	TEST_CMD23_PACKED_BIT_UNSET,
	TEST_CMD23_REL_WR_BIT_SET,
	TEST_CMD23_BITS_16TO29_SET,
	TEST_CMD23_HDR_BLK_NOT_IN_COUNT,
	INVALID_CMD_MAX_TESTCASE = TEST_CMD23_HDR_BLK_NOT_IN_COUNT,
};

enum mmc_block_test_group {
	TEST_NO_GROUP,
	TEST_GENERAL_GROUP,
	TEST_SEND_WRITE_PACKING_GROUP,
	TEST_ERR_CHECK_GROUP,
	TEST_SEND_INVALID_GROUP,
};

struct mmc_block_test_debug {
	struct dentry *send_write_packing_test;
	struct dentry *err_check_test;
	struct dentry *send_invalid_packed_test;
	struct dentry *random_test_seed;
};

struct mmc_block_test_data {
	/* The number of write requests that the test will issue */
	int num_requests;
	/* The expected write packing statistics for the current test */
	struct mmc_wr_pack_stats exp_packed_stats;
	/*
	 * A user-defined seed for random choices of number of bios written in
	 * a request, and of number of requests issued in a test
	 * This field is randomly updated after each use
	 */
	unsigned int random_test_seed;
	/* A retry counter used in err_check tests */
	int err_check_counter;
	/* Can be one of the values of enum test_group */
	enum mmc_block_test_group test_group;
	/*
	 * Indicates if the current testcase is running with random values of
	 * num_requests and num_bios (in each request)
	 */
	int is_random;
	/* Data structure for debugfs dentrys */
	struct mmc_block_test_debug debug;
	/*
	 * Data structure containing individual test information, including
	 * self-defined specific data
	 */
	struct test_info test_info;
	/* mmc block device test */
	struct blk_dev_test_type bdt;
};

static struct mmc_block_test_data *mbtd;

/*
 * A callback assigned to the packed_test_fn field.
 * Called from block layer in mmc_blk_packed_hdr_wrq_prep.
 * Here we alter the packed header or CMD23 in order to send an invalid
 * packed command to the card.
 */
static void test_invalid_packed_cmd(struct request_queue *q,
				    struct mmc_queue_req *mqrq)
{
	struct mmc_queue *mq = q->queuedata;
	u32 *packed_cmd_hdr = mqrq->packed_cmd_hdr;
	struct request *req = mqrq->req;
	struct request *second_rq;
	struct test_request *test_rq;
	struct mmc_blk_request *brq = &mqrq->brq;
	int num_requests;
	int max_packed_reqs;

	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return;
	}

	test_rq = (struct test_request *)req->elevator_private[0];
	if (!test_rq) {
		test_pr_err("%s: NULL test_rq", __func__);
		return;
	}
	max_packed_reqs = mq->card->ext_csd.max_packed_writes;

	switch (mbtd->test_info.testcase) {
	case TEST_HDR_INVALID_VERSION:
		test_pr_info("%s: set invalid header version", __func__);
		/* Put 0 in header version field (1 byte, offset 0 in header) */
		packed_cmd_hdr[0] = packed_cmd_hdr[0] & ~PACKED_HDR_VER_MASK;
		break;
	case TEST_HDR_WRONG_WRITE_CODE:
		test_pr_info("%s: wrong write code", __func__);
		/* Set R/W field with R value (1 byte, offset 1 in header) */
		packed_cmd_hdr[0] = packed_cmd_hdr[0] & ~PACKED_HDR_RW_MASK;
		packed_cmd_hdr[0] = packed_cmd_hdr[0] | 0x00000100;
		break;
	case TEST_HDR_INVALID_RW_CODE:
		test_pr_info("%s: invalid r/w code", __func__);
		/* Set R/W field with invalid value */
		packed_cmd_hdr[0] = packed_cmd_hdr[0] & ~PACKED_HDR_RW_MASK;
		packed_cmd_hdr[0] = packed_cmd_hdr[0] | 0x00000400;
		break;
	case TEST_HDR_DIFFERENT_ADDRESSES:
		test_pr_info("%s: different addresses", __func__);
		second_rq = list_entry(req->queuelist.next, struct request,
				queuelist);
		test_pr_info("%s: test_rq->sector=%ld, second_rq->sector=%ld",
			      __func__, (long)req->__sector,
			     (long)second_rq->__sector);
		/*
		 * Put start sector of second write request in the first write
		 * request's cmd25 argument in the packed header
		 */
		packed_cmd_hdr[3] = second_rq->__sector;
		break;
	case TEST_HDR_REQ_NUM_SMALLER_THAN_ACTUAL:
		test_pr_info("%s: request num smaller than actual" , __func__);
		num_requests = (packed_cmd_hdr[0] & PACKED_HDR_NUM_REQS_MASK)
									>> 16;
		/* num of entries is decremented by 1 */
		num_requests = (num_requests - 1) << 16;
		/*
		 * Set number of requests field in packed write header to be
		 * smaller than the actual number (1 byte, offset 2 in header)
		 */
		packed_cmd_hdr[0] = (packed_cmd_hdr[0] &
				     ~PACKED_HDR_NUM_REQS_MASK) + num_requests;
		break;
	case TEST_HDR_REQ_NUM_LARGER_THAN_ACTUAL:
		test_pr_info("%s: request num larger than actual" , __func__);
		num_requests = (packed_cmd_hdr[0] & PACKED_HDR_NUM_REQS_MASK)
									>> 16;
		/* num of entries is incremented by 1 */
		num_requests = (num_requests + 1) << 16;
		/*
		 * Set number of requests field in packed write header to be
		 * larger than the actual number (1 byte, offset 2 in header).
		 */
		packed_cmd_hdr[0] = (packed_cmd_hdr[0] &
				     ~PACKED_HDR_NUM_REQS_MASK) + num_requests;
		break;
	case TEST_HDR_CMD23_PACKED_BIT_SET:
		test_pr_info("%s: header CMD23 packed bit set" , __func__);
		/*
		 * Set packed bit (bit 30) in cmd23 argument of first and second
		 * write requests in packed write header.
		 * These are located at bytes 2 and 4 in packed write header
		 */
		packed_cmd_hdr[2] = packed_cmd_hdr[2] | CMD23_PACKED_BIT;
		packed_cmd_hdr[4] = packed_cmd_hdr[4] | CMD23_PACKED_BIT;
		break;
	case TEST_CMD23_MAX_PACKED_WRITES:
		test_pr_info("%s: CMD23 request num > max_packed_reqs",
			      __func__);
		/*
		 * Set the individual packed cmd23 request num to
		 * max_packed_reqs + 1
		 */
		brq->sbc.arg = MMC_CMD23_ARG_PACKED | (max_packed_reqs + 1);
		break;
	case TEST_CMD23_ZERO_PACKED_WRITES:
		test_pr_info("%s: CMD23 request num = 0", __func__);
		/* Set the individual packed cmd23 request num to zero */
		brq->sbc.arg = MMC_CMD23_ARG_PACKED;
		break;
	case TEST_CMD23_PACKED_BIT_UNSET:
		test_pr_info("%s: CMD23 packed bit unset", __func__);
		/*
		 * Set the individual packed cmd23 packed bit to 0,
		 *  although there is a packed write request
		 */
		brq->sbc.arg &= ~CMD23_PACKED_BIT;
		break;
	case TEST_CMD23_REL_WR_BIT_SET:
		test_pr_info("%s: CMD23 REL WR bit set", __func__);
		/* Set the individual packed cmd23 reliable write bit */
		brq->sbc.arg = MMC_CMD23_ARG_PACKED | MMC_CMD23_ARG_REL_WR;
		break;
	case TEST_CMD23_BITS_16TO29_SET:
		test_pr_info("%s: CMD23 bits [16-29] set", __func__);
		brq->sbc.arg = MMC_CMD23_ARG_PACKED |
			PACKED_HDR_BITS_16_TO_29_SET;
		break;
	case TEST_CMD23_HDR_BLK_NOT_IN_COUNT:
		test_pr_info("%s: CMD23 hdr not in block count", __func__);
		brq->sbc.arg = MMC_CMD23_ARG_PACKED |
		((rq_data_dir(req) == READ) ? 0 : mqrq->packed_blocks);
		break;
	default:
		test_pr_err("%s: unexpected testcase %d",
			__func__, mbtd->test_info.testcase);
		break;
	}
}

/*
 * A callback assigned to the err_check_fn field of the mmc_request by the
 * MMC/card/block layer.
 * Called upon request completion by the MMC/core layer.
 * Here we emulate an error return value from the card.
 */
static int test_err_check(struct mmc_card *card, struct mmc_async_req *areq)
{
	struct mmc_queue_req *mq_rq = container_of(areq, struct mmc_queue_req,
			mmc_active);
	struct request_queue *req_q = test_iosched_get_req_queue();
	struct mmc_queue *mq;
	int max_packed_reqs;
	int ret = 0;

	if (req_q)
		mq = req_q->queuedata;
	else {
		test_pr_err("%s: NULL request_queue", __func__);
		return 0;
	}

	if (!mq) {
		test_pr_err("%s: %s: NULL mq", __func__,
			mmc_hostname(card->host));
		return 0;
	}

	max_packed_reqs = mq->card->ext_csd.max_packed_writes;

	if (!mq_rq) {
		test_pr_err("%s: %s: NULL mq_rq", __func__,
			mmc_hostname(card->host));
		return 0;
	}

	switch (mbtd->test_info.testcase) {
	case TEST_RET_ABORT:
		test_pr_info("%s: return abort", __func__);
		ret = MMC_BLK_ABORT;
		break;
	case TEST_RET_PARTIAL_FOLLOWED_BY_SUCCESS:
		test_pr_info("%s: return partial followed by success",
			      __func__);
		/*
		 * Since in this testcase num_requests is always >= 2,
		 * we can be sure that packed_fail_idx is always >= 1
		 */
		mq_rq->packed_fail_idx = (mbtd->num_requests / 2);
		test_pr_info("%s: packed_fail_idx = %d"
			, __func__, mq_rq->packed_fail_idx);
		mq->err_check_fn = NULL;
		ret = MMC_BLK_PARTIAL;
		break;
	case TEST_RET_PARTIAL_FOLLOWED_BY_ABORT:
		if (!mbtd->err_check_counter) {
			test_pr_info("%s: return partial followed by abort",
				      __func__);
			mbtd->err_check_counter++;
			/*
			 * Since in this testcase num_requests is always >= 3,
			 * we have that packed_fail_idx is always >= 1
			 */
			mq_rq->packed_fail_idx = (mbtd->num_requests / 2);
			test_pr_info("%s: packed_fail_idx = %d"
				, __func__, mq_rq->packed_fail_idx);
			ret = MMC_BLK_PARTIAL;
			break;
		}
		mbtd->err_check_counter = 0;
		mq->err_check_fn = NULL;
		ret = MMC_BLK_ABORT;
		break;
	case TEST_RET_PARTIAL_MULTIPLE_UNTIL_SUCCESS:
		test_pr_info("%s: return partial multiple until success",
			     __func__);
		if (++mbtd->err_check_counter >= (mbtd->num_requests)) {
			mq->err_check_fn = NULL;
			mbtd->err_check_counter = 0;
			ret = MMC_BLK_PARTIAL;
			break;
		}
		mq_rq->packed_fail_idx = 1;
		ret = MMC_BLK_PARTIAL;
		break;
	case TEST_RET_PARTIAL_MAX_FAIL_IDX:
		test_pr_info("%s: return partial max fail_idx", __func__);
		mq_rq->packed_fail_idx = max_packed_reqs - 1;
		mq->err_check_fn = NULL;
		ret = MMC_BLK_PARTIAL;
		break;
	case TEST_RET_RETRY:
		test_pr_info("%s: return retry", __func__);
		ret = MMC_BLK_RETRY;
		break;
	case TEST_RET_CMD_ERR:
		test_pr_info("%s: return cmd err", __func__);
		ret = MMC_BLK_CMD_ERR;
		break;
	case TEST_RET_DATA_ERR:
		test_pr_info("%s: return data err", __func__);
		ret = MMC_BLK_DATA_ERR;
		break;
	default:
		test_pr_err("%s: unexpected testcase %d",
			__func__, mbtd->test_info.testcase);
	}

	return ret;
}

/*
 * This is a specific implementation for the get_test_case_str_fn function
 * pointer in the test_info data structure. Given a valid test_data instance,
 * the function returns a string resembling the test name, based on the testcase
 */
static char *get_test_case_str(struct test_data *td)
{
	if (!td) {
		test_pr_err("%s: NULL td", __func__);
		return NULL;
	}

	switch (td->test_info.testcase) {
	case TEST_STOP_DUE_TO_FLUSH:
		return "Test stop due to flush";
	case TEST_STOP_DUE_TO_FLUSH_AFTER_MAX_REQS:
		return "Test stop due to flush after max-1 reqs";
	case TEST_STOP_DUE_TO_READ:
		return "Test stop due to read";
	case TEST_STOP_DUE_TO_READ_AFTER_MAX_REQS:
		return "Test stop due to read after max-1 reqs";
	case TEST_STOP_DUE_TO_EMPTY_QUEUE:
		return "Test stop due to empty queue";
	case TEST_STOP_DUE_TO_MAX_REQ_NUM:
		return "Test stop due to max req num";
	case TEST_STOP_DUE_TO_THRESHOLD:
		return "Test stop due to exceeding threshold";
	case TEST_RET_ABORT:
		return "Test err_check return abort";
	case TEST_RET_PARTIAL_FOLLOWED_BY_SUCCESS:
		return "Test err_check return partial followed by success";
	case TEST_RET_PARTIAL_FOLLOWED_BY_ABORT:
		return "Test err_check return partial followed by abort";
	case TEST_RET_PARTIAL_MULTIPLE_UNTIL_SUCCESS:
		return "Test err_check return partial multiple until success";
	case TEST_RET_PARTIAL_MAX_FAIL_IDX:
		return "Test err_check return partial max fail index";
	case TEST_RET_RETRY:
		return "Test err_check return retry";
	case TEST_RET_CMD_ERR:
		return "Test err_check return cmd error";
	case TEST_RET_DATA_ERR:
		return "Test err_check return data error";
	case TEST_HDR_INVALID_VERSION:
		return "Test invalid - wrong header version";
	case TEST_HDR_WRONG_WRITE_CODE:
		return "Test invalid - wrong write code";
	case TEST_HDR_INVALID_RW_CODE:
		return "Test invalid - wrong R/W code";
	case TEST_HDR_DIFFERENT_ADDRESSES:
		return "Test invalid - header different addresses";
	case TEST_HDR_REQ_NUM_SMALLER_THAN_ACTUAL:
		return "Test invalid - header req num smaller than actual";
	case TEST_HDR_REQ_NUM_LARGER_THAN_ACTUAL:
		return "Test invalid - header req num larger than actual";
	case TEST_HDR_CMD23_PACKED_BIT_SET:
		return "Test invalid - header cmd23 packed bit set";
	case TEST_CMD23_MAX_PACKED_WRITES:
		return "Test invalid - cmd23 max packed writes";
	case TEST_CMD23_ZERO_PACKED_WRITES:
		return "Test invalid - cmd23 zero packed writes";
	case TEST_CMD23_PACKED_BIT_UNSET:
		return "Test invalid - cmd23 packed bit unset";
	case TEST_CMD23_REL_WR_BIT_SET:
		return "Test invalid - cmd23 rel wr bit set";
	case TEST_CMD23_BITS_16TO29_SET:
		return "Test invalid - cmd23 bits [16-29] set";
	case TEST_CMD23_HDR_BLK_NOT_IN_COUNT:
		return "Test invalid - cmd23 header block not in count";
	default:
		 return "Unknown testcase";
	}

	return NULL;
}

/*
 * Compare individual testcase's statistics to the expected statistics:
 * Compare stop reason and number of packing events
 */
static int check_wr_packing_statistics(struct test_data *td)
{
	struct mmc_wr_pack_stats *mmc_packed_stats;
	struct mmc_queue *mq = td->req_q->queuedata;
	int max_packed_reqs = mq->card->ext_csd.max_packed_writes;
	int i;
	struct mmc_card *card = mq->card;
	struct mmc_wr_pack_stats expected_stats;
	int *stop_reason;
	int ret = 0;

	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return -EINVAL;
	}

	expected_stats = mbtd->exp_packed_stats;

	mmc_packed_stats = mmc_blk_get_packed_statistics(card);
	if (!mmc_packed_stats) {
		test_pr_err("%s: NULL mmc_packed_stats", __func__);
		return -EINVAL;
	}

	if (!mmc_packed_stats->packing_events) {
		test_pr_err("%s: NULL packing_events", __func__);
		return -EINVAL;
	}

	spin_lock(&mmc_packed_stats->lock);

	if (!mmc_packed_stats->enabled) {
		test_pr_err("%s write packing statistics are not enabled",
			     __func__);
		ret = -EINVAL;
		goto exit_err;
	}

	stop_reason = mmc_packed_stats->pack_stop_reason;

	for (i = 1 ; i <= max_packed_reqs ; ++i) {
		if (mmc_packed_stats->packing_events[i] !=
		    expected_stats.packing_events[i]) {
			test_pr_err(
			"%s: Wrong pack stats in index %d, got %d, expected %d",
			__func__, i, mmc_packed_stats->packing_events[i],
			       expected_stats.packing_events[i]);
			if (td->fs_wr_reqs_during_test)
				goto cancel_round;
			ret = -EINVAL;
			goto exit_err;
		}
	}

	if (mmc_packed_stats->pack_stop_reason[EXCEEDS_SEGMENTS] !=
	    expected_stats.pack_stop_reason[EXCEEDS_SEGMENTS]) {
		test_pr_err(
		"%s: Wrong pack stop reason EXCEEDS_SEGMENTS %d, expected %d",
			__func__, stop_reason[EXCEEDS_SEGMENTS],
		       expected_stats.pack_stop_reason[EXCEEDS_SEGMENTS]);
		if (td->fs_wr_reqs_during_test)
			goto cancel_round;
		ret = -EINVAL;
		goto exit_err;
	}

	if (mmc_packed_stats->pack_stop_reason[EXCEEDS_SECTORS] !=
	    expected_stats.pack_stop_reason[EXCEEDS_SECTORS]) {
		test_pr_err(
		"%s: Wrong pack stop reason EXCEEDS_SECTORS %d, expected %d",
			__func__, stop_reason[EXCEEDS_SECTORS],
		       expected_stats.pack_stop_reason[EXCEEDS_SECTORS]);
		if (td->fs_wr_reqs_during_test)
			goto cancel_round;
		ret = -EINVAL;
		goto exit_err;
	}

	if (mmc_packed_stats->pack_stop_reason[WRONG_DATA_DIR] !=
	    expected_stats.pack_stop_reason[WRONG_DATA_DIR]) {
		test_pr_err(
		"%s: Wrong pack stop reason WRONG_DATA_DIR %d, expected %d",
		       __func__, stop_reason[WRONG_DATA_DIR],
		       expected_stats.pack_stop_reason[WRONG_DATA_DIR]);
		if (td->fs_wr_reqs_during_test)
			goto cancel_round;
		ret = -EINVAL;
		goto exit_err;
	}

	if (mmc_packed_stats->pack_stop_reason[FLUSH_OR_DISCARD] !=
	    expected_stats.pack_stop_reason[FLUSH_OR_DISCARD]) {
		test_pr_err(
		"%s: Wrong pack stop reason FLUSH_OR_DISCARD %d, expected %d",
		       __func__, stop_reason[FLUSH_OR_DISCARD],
		       expected_stats.pack_stop_reason[FLUSH_OR_DISCARD]);
		if (td->fs_wr_reqs_during_test)
			goto cancel_round;
		ret = -EINVAL;
		goto exit_err;
	}

	if (mmc_packed_stats->pack_stop_reason[EMPTY_QUEUE] !=
	    expected_stats.pack_stop_reason[EMPTY_QUEUE]) {
		test_pr_err(
		"%s: Wrong pack stop reason EMPTY_QUEUE %d, expected %d",
		       __func__, stop_reason[EMPTY_QUEUE],
		       expected_stats.pack_stop_reason[EMPTY_QUEUE]);
		if (td->fs_wr_reqs_during_test)
			goto cancel_round;
		ret = -EINVAL;
		goto exit_err;
	}

	if (mmc_packed_stats->pack_stop_reason[REL_WRITE] !=
	    expected_stats.pack_stop_reason[REL_WRITE]) {
		test_pr_err(
			"%s: Wrong pack stop reason REL_WRITE %d, expected %d",
		       __func__, stop_reason[REL_WRITE],
		       expected_stats.pack_stop_reason[REL_WRITE]);
		if (td->fs_wr_reqs_during_test)
			goto cancel_round;
		ret = -EINVAL;
		goto exit_err;
	}

exit_err:
	spin_unlock(&mmc_packed_stats->lock);
	if (ret && mmc_packed_stats->enabled)
		print_mmc_packing_stats(card);
	return ret;
cancel_round:
	spin_unlock(&mmc_packed_stats->lock);
	test_iosched_set_ignore_round(true);
	return 0;
}

/*
 * Pseudo-randomly choose a seed based on the last seed, and update it in
 * seed_number. then return seed_number (mod max_val), or min_val.
 */
static unsigned int pseudo_random_seed(unsigned int *seed_number,
				       unsigned int min_val,
				       unsigned int max_val)
{
	int ret = 0;

	if (!seed_number)
		return 0;

	*seed_number = ((unsigned int)(((unsigned long)*seed_number *
				(unsigned long)LARGE_PRIME_1) + LARGE_PRIME_2));
	ret = (unsigned int)((*seed_number) % max_val);

	return (ret > min_val ? ret : min_val);
}

/*
 * Given a pseudo-random seed, find a pseudo-random num_of_bios.
 * Make sure that num_of_bios is not larger than TEST_MAX_SECTOR_RANGE
 */
static void pseudo_rnd_num_of_bios(unsigned int *num_bios_seed,
				   unsigned int *num_of_bios)
{
	do {
		*num_of_bios = pseudo_random_seed(num_bios_seed, 1,
						  TEST_MAX_BIOS_PER_REQ);
		if (!(*num_of_bios))
			*num_of_bios = 1;
	} while ((*num_of_bios) * BIO_U32_SIZE * 4 > TEST_MAX_SECTOR_RANGE);
}

/* Add a single read request to the given td's request queue */
static int prepare_request_add_read(struct test_data *td)
{
	int ret;
	int start_sec;

	if (td)
		start_sec = td->start_sector;
	else {
		test_pr_err("%s: NULL td", __func__);
		return 0;
	}

	test_pr_info("%s: Adding a read request, first req_id=%d", __func__,
		     td->wr_rd_next_req_id);

	ret = test_iosched_add_wr_rd_test_req(0, READ, start_sec, 2,
					      TEST_PATTERN_5A, NULL);
	if (ret) {
		test_pr_err("%s: failed to add a read request", __func__);
		return ret;
	}

	return 0;
}

/* Add a single flush request to the given td's request queue */
static int prepare_request_add_flush(struct test_data *td)
{
	int ret;

	if (!td) {
		test_pr_err("%s: NULL td", __func__);
		return 0;
	}

	test_pr_info("%s: Adding a flush request, first req_id=%d", __func__,
		     td->unique_next_req_id);
	ret = test_iosched_add_unique_test_req(0, REQ_UNIQUE_FLUSH,
				  0, 0, NULL);
	if (ret) {
		test_pr_err("%s: failed to add a flush request", __func__);
		return ret;
	}

	return ret;
}

/*
 * Add num_requets amount of write requests to the given td's request queue.
 * If random test mode is chosen we pseudo-randomly choose the number of bios
 * for each write request, otherwise add between 1 to 5 bio per request.
 */
static int prepare_request_add_write_reqs(struct test_data *td,
					  int num_requests, int is_err_expected,
					  int is_random)
{
	int i;
	unsigned int start_sec;
	int num_bios;
	int ret = 0;
	unsigned int *bio_seed = &mbtd->random_test_seed;

	if (td)
		start_sec = td->start_sector;
	else {
		test_pr_err("%s: NULL td", __func__);
		return ret;
	}

	test_pr_info("%s: Adding %d write requests, first req_id=%d", __func__,
		     num_requests, td->wr_rd_next_req_id);

	for (i = 1 ; i <= num_requests ; i++) {
		start_sec = td->start_sector + 4096 * td->num_of_write_bios;
		if (is_random)
			pseudo_rnd_num_of_bios(bio_seed, &num_bios);
		else
			/*
			 * For the non-random case, give num_bios a value
			 * between 1 and 5, to keep a small number of BIOs
			 */
			num_bios = (i%5)+1;

		ret = test_iosched_add_wr_rd_test_req(is_err_expected, WRITE,
				start_sec, num_bios, TEST_PATTERN_5A, NULL);

		if (ret) {
			test_pr_err("%s: failed to add a write request",
				    __func__);
			return ret;
		}
	}
	return 0;
}

/*
 * Prepare the write, read and flush requests for a generic packed commands
 * testcase
 */
static int prepare_packed_requests(struct test_data *td, int is_err_expected,
				   int num_requests, int is_random)
{
	int ret = 0;
	struct mmc_queue *mq;
	int max_packed_reqs;
	struct request_queue *req_q;

	if (!td) {
		pr_err("%s: NULL td", __func__);
		return -EINVAL;
	}

	req_q = td->req_q;

	if (!req_q) {
		pr_err("%s: NULL request queue", __func__);
		return -EINVAL;
	}

	mq = req_q->queuedata;
	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return -EINVAL;
	}

	max_packed_reqs = mq->card->ext_csd.max_packed_writes;

	if (mbtd->random_test_seed <= 0) {
		mbtd->random_test_seed =
			(unsigned int)(get_jiffies_64() & 0xFFFF);
		test_pr_info("%s: got seed from jiffies %d",
			     __func__, mbtd->random_test_seed);
	}

	mmc_blk_init_packed_statistics(mq->card);

	ret = prepare_request_add_write_reqs(td, num_requests, is_err_expected,
					     is_random);
	if (ret)
		return ret;

	/* Avoid memory corruption in upcoming stats set */
	if (td->test_info.testcase == TEST_STOP_DUE_TO_THRESHOLD)
		num_requests--;

	memset((void *)mbtd->exp_packed_stats.pack_stop_reason, 0,
		sizeof(mbtd->exp_packed_stats.pack_stop_reason));
	memset(mbtd->exp_packed_stats.packing_events, 0,
		(max_packed_reqs + 1) * sizeof(u32));
	if (num_requests <= max_packed_reqs)
		mbtd->exp_packed_stats.packing_events[num_requests] = 1;

	switch (td->test_info.testcase) {
	case TEST_STOP_DUE_TO_FLUSH:
	case TEST_STOP_DUE_TO_FLUSH_AFTER_MAX_REQS:
		ret = prepare_request_add_flush(td);
		if (ret)
			return ret;

		mbtd->exp_packed_stats.pack_stop_reason[FLUSH_OR_DISCARD] = 1;
		break;
	case TEST_STOP_DUE_TO_READ:
	case TEST_STOP_DUE_TO_READ_AFTER_MAX_REQS:
		ret = prepare_request_add_read(td);
		if (ret)
			return ret;

		mbtd->exp_packed_stats.pack_stop_reason[WRONG_DATA_DIR] = 1;
		break;
	case TEST_STOP_DUE_TO_THRESHOLD:
		mbtd->exp_packed_stats.packing_events[num_requests] = 1;
		mbtd->exp_packed_stats.packing_events[1] = 1;
		mbtd->exp_packed_stats.pack_stop_reason[THRESHOLD] = 1;
		mbtd->exp_packed_stats.pack_stop_reason[EMPTY_QUEUE] = 1;
		break;
	case TEST_STOP_DUE_TO_MAX_REQ_NUM:
	case TEST_RET_PARTIAL_MAX_FAIL_IDX:
		mbtd->exp_packed_stats.pack_stop_reason[THRESHOLD] = 1;
		break;
	default:
		mbtd->exp_packed_stats.pack_stop_reason[EMPTY_QUEUE] = 1;
	}
	mbtd->num_requests = num_requests;

	return 0;
}

/*
 * Prepare requests for the TEST_RET_PARTIAL_FOLLOWED_BY_ABORT testcase.
 * In this testcase we have mixed error expectations from different
 * write requests, hence the special prepare function.
 */
static int prepare_partial_followed_by_abort(struct test_data *td,
					      int num_requests)
{
	int i, start_address;
	int is_err_expected = 0;
	int ret = 0;
	struct mmc_queue *mq = test_iosched_get_req_queue()->queuedata;
	int max_packed_reqs;

	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return -EINVAL;
	}

	max_packed_reqs = mq->card->ext_csd.max_packed_writes;

	mmc_blk_init_packed_statistics(mq->card);

	for (i = 1 ; i <= num_requests ; i++) {
		if (i > (num_requests / 2))
			is_err_expected = 1;

		start_address = td->start_sector + 4096*td->num_of_write_bios;
		ret = test_iosched_add_wr_rd_test_req(is_err_expected, WRITE,
				start_address, (i%5)+1, TEST_PATTERN_5A, NULL);
		if (ret) {
			test_pr_err("%s: failed to add a write request",
				    __func__);
			return ret;
		}
	}

	memset((void *)&mbtd->exp_packed_stats.pack_stop_reason, 0,
		sizeof(mbtd->exp_packed_stats.pack_stop_reason));
	memset(mbtd->exp_packed_stats.packing_events, 0,
		(max_packed_reqs + 1) * sizeof(u32));
	mbtd->exp_packed_stats.packing_events[num_requests] = 1;
	mbtd->exp_packed_stats.pack_stop_reason[EMPTY_QUEUE] = 1;

	mbtd->num_requests = num_requests;

	return ret;
}

/*
 * Get number of write requests for current testcase. If random test mode was
 * chosen, pseudo-randomly choose the number of requests, otherwise set to
 * two less than the packing threshold.
 */
static int get_num_requests(struct test_data *td)
{
	int *seed = &mbtd->random_test_seed;
	struct request_queue *req_q;
	struct mmc_queue *mq;
	int max_num_requests;
	int num_requests;
	int min_num_requests = 2;
	int is_random = mbtd->is_random;

	req_q = test_iosched_get_req_queue();
	if (req_q)
		mq = req_q->queuedata;
	else {
		test_pr_err("%s: NULL request queue", __func__);
		return 0;
	}

	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return -EINVAL;
	}

	max_num_requests = mq->card->ext_csd.max_packed_writes;
	num_requests = max_num_requests - 2;

	if (is_random) {
		if (td->test_info.testcase ==
		    TEST_RET_PARTIAL_FOLLOWED_BY_ABORT)
			min_num_requests = 3;

		num_requests = pseudo_random_seed(seed, min_num_requests,
						  max_num_requests - 1);
	}

	return num_requests;
}

/*
 * An implementation for the prepare_test_fn pointer in the test_info
 * data structure. According to the testcase we add the right number of requests
 * and decide if an error is expected or not.
 */
static int prepare_test(struct test_data *td)
{
	struct mmc_queue *mq = test_iosched_get_req_queue()->queuedata;
	int max_num_requests;
	int num_requests = 0;
	int ret = 0;
	int is_random = mbtd->is_random;

	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return -EINVAL;
	}

	max_num_requests = mq->card->ext_csd.max_packed_writes;

	if (is_random && mbtd->random_test_seed == 0) {
		mbtd->random_test_seed =
			(unsigned int)(get_jiffies_64() & 0xFFFF);
		test_pr_info("%s: got seed from jiffies %d",
			__func__, mbtd->random_test_seed);
	}

	num_requests = get_num_requests(td);

	if (mbtd->test_group == TEST_SEND_INVALID_GROUP)
		mq->packed_test_fn =
				test_invalid_packed_cmd;

	if (mbtd->test_group == TEST_ERR_CHECK_GROUP)
		mq->err_check_fn = test_err_check;

	switch (td->test_info.testcase) {
	case TEST_STOP_DUE_TO_FLUSH:
	case TEST_STOP_DUE_TO_READ:
	case TEST_RET_PARTIAL_FOLLOWED_BY_SUCCESS:
	case TEST_RET_PARTIAL_MULTIPLE_UNTIL_SUCCESS:
	case TEST_STOP_DUE_TO_EMPTY_QUEUE:
	case TEST_CMD23_PACKED_BIT_UNSET:
		ret = prepare_packed_requests(td, 0, num_requests, is_random);
		break;
	case TEST_STOP_DUE_TO_FLUSH_AFTER_MAX_REQS:
	case TEST_STOP_DUE_TO_READ_AFTER_MAX_REQS:
		ret = prepare_packed_requests(td, 0, max_num_requests - 1,
					      is_random);
		break;
	case TEST_RET_PARTIAL_FOLLOWED_BY_ABORT:
		ret = prepare_partial_followed_by_abort(td, num_requests);
		break;
	case TEST_STOP_DUE_TO_MAX_REQ_NUM:
	case TEST_RET_PARTIAL_MAX_FAIL_IDX:
		ret = prepare_packed_requests(td, 0, max_num_requests,
					      is_random);
		break;
	case TEST_STOP_DUE_TO_THRESHOLD:
		ret = prepare_packed_requests(td, 0, max_num_requests + 1,
					      is_random);
		break;
	case TEST_RET_ABORT:
	case TEST_RET_RETRY:
	case TEST_RET_CMD_ERR:
	case TEST_RET_DATA_ERR:
	case TEST_HDR_INVALID_VERSION:
	case TEST_HDR_WRONG_WRITE_CODE:
	case TEST_HDR_INVALID_RW_CODE:
	case TEST_HDR_DIFFERENT_ADDRESSES:
	case TEST_HDR_REQ_NUM_SMALLER_THAN_ACTUAL:
	case TEST_HDR_REQ_NUM_LARGER_THAN_ACTUAL:
	case TEST_CMD23_MAX_PACKED_WRITES:
	case TEST_CMD23_ZERO_PACKED_WRITES:
	case TEST_CMD23_REL_WR_BIT_SET:
	case TEST_CMD23_BITS_16TO29_SET:
	case TEST_CMD23_HDR_BLK_NOT_IN_COUNT:
	case TEST_HDR_CMD23_PACKED_BIT_SET:
		ret = prepare_packed_requests(td, 1, num_requests, is_random);
		break;
	default:
		test_pr_info("%s: Invalid test case...", __func__);
		return -EINVAL;
	}

	return ret;
}

/*
 * An implementation for the post_test_fn in the test_info data structure.
 * In our case we just reset the function pointers in the mmc_queue in order for
 * the FS to be able to dispatch it's requests correctly after the test is
 * finished.
 */
static int post_test(struct test_data *td)
{
	struct mmc_queue *mq;

	if (!td)
		return -EINVAL;

	mq = td->req_q->queuedata;

	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return -EINVAL;
	}

	mq->packed_test_fn = NULL;
	mq->err_check_fn = NULL;

	return 0;
}

/*
 * This function checks, based on the current test's test_group, that the
 * packed commands capability and control are set right. In addition, we check
 * if the card supports the packed command feature.
 */
static int validate_packed_commands_settings(void)
{
	struct request_queue *req_q;
	struct mmc_queue *mq;
	int max_num_requests;
	struct mmc_host *host;

	req_q = test_iosched_get_req_queue();
	if (!req_q) {
		test_pr_err("%s: test_iosched_get_req_queue failed", __func__);
		test_iosched_set_test_result(TEST_FAILED);
		return -EINVAL;
	}

	mq = req_q->queuedata;
	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return -EINVAL;
	}

	max_num_requests = mq->card->ext_csd.max_packed_writes;
	host = mq->card->host;

	if (!(host->caps2 && MMC_CAP2_PACKED_WR)) {
		test_pr_err("%s: Packed Write capability disabled, exit test",
			    __func__);
		test_iosched_set_test_result(TEST_NOT_SUPPORTED);
		return -EINVAL;
	}

	if (max_num_requests == 0) {
		test_pr_err(
		"%s: no write packing support, ext_csd.max_packed_writes=%d",
		__func__, mq->card->ext_csd.max_packed_writes);
		test_iosched_set_test_result(TEST_NOT_SUPPORTED);
		return -EINVAL;
	}

	test_pr_info("%s: max number of packed requests supported is %d ",
		     __func__, max_num_requests);

	switch (mbtd->test_group) {
	case TEST_SEND_WRITE_PACKING_GROUP:
	case TEST_ERR_CHECK_GROUP:
	case TEST_SEND_INVALID_GROUP:
		/* disable the packing control */
		host->caps2 &= ~MMC_CAP2_PACKED_WR_CONTROL;
		break;
	default:
		break;
	}

	return 0;
}

static bool message_repeat;
static int test_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	message_repeat = 1;
	return 0;
}

/* send_packing TEST */
static ssize_t send_write_packing_test_write(struct file *file,
				const char __user *buf,
				size_t count,
				loff_t *ppos)
{
	int ret = 0;
	int i = 0;
	int number = -1;
	int j = 0;

	test_pr_info("%s: -- send_write_packing TEST --", __func__);

	sscanf(buf, "%d", &number);

	if (number <= 0)
		number = 1;


	mbtd->test_group = TEST_SEND_WRITE_PACKING_GROUP;

	if (validate_packed_commands_settings())
		return count;

	if (mbtd->random_test_seed > 0)
		test_pr_info("%s: Test seed: %d", __func__,
			      mbtd->random_test_seed);

	memset(&mbtd->test_info, 0, sizeof(struct test_info));

	mbtd->test_info.data = mbtd;
	mbtd->test_info.prepare_test_fn = prepare_test;
	mbtd->test_info.check_test_result_fn = check_wr_packing_statistics;
	mbtd->test_info.get_test_case_str_fn = get_test_case_str;
	mbtd->test_info.post_test_fn = post_test;

	for (i = 0 ; i < number ; ++i) {
		test_pr_info("%s: Cycle # %d / %d", __func__, i+1, number);
		test_pr_info("%s: ====================", __func__);

		for (j = SEND_WRITE_PACKING_MIN_TESTCASE ;
		      j <= SEND_WRITE_PACKING_MAX_TESTCASE ; j++) {

			mbtd->test_info.testcase = j;
			mbtd->is_random = RANDOM_TEST;
			ret = test_iosched_start_test(&mbtd->test_info);
			if (ret)
				break;
			/* Allow FS requests to be dispatched */
			msleep(1000);
			mbtd->test_info.testcase = j;
			mbtd->is_random = NON_RANDOM_TEST;
			ret = test_iosched_start_test(&mbtd->test_info);
			if (ret)
				break;
			/* Allow FS requests to be dispatched */
			msleep(1000);
		}
	}

	test_pr_info("%s: Completed all the test cases.", __func__);

	return count;
}

static ssize_t send_write_packing_test_read(struct file *file,
			       char __user *buffer,
			       size_t count,
			       loff_t *offset)
{
	memset((void *)buffer, 0, count);

	snprintf(buffer, count,
		 "\nsend_write_packing_test\n"
		 "=========\n"
		 "Description:\n"
		 "This test checks the following scenarios\n"
		 "- Pack due to FLUSH message\n"
		 "- Pack due to FLUSH after threshold writes\n"
		 "- Pack due to READ message\n"
		 "- Pack due to READ after threshold writes\n"
		 "- Pack due to empty queue\n"
		 "- Pack due to threshold writes\n"
		 "- Pack due to one over threshold writes\n");

	if (message_repeat == 1) {
		message_repeat = 0;
		return strnlen(buffer, count);
	} else {
		return 0;
	}
}

const struct file_operations send_write_packing_test_ops = {
	.open = test_open,
	.write = send_write_packing_test_write,
	.read = send_write_packing_test_read,
};

/* err_check TEST */
static ssize_t err_check_test_write(struct file *file,
				const char __user *buf,
				size_t count,
				loff_t *ppos)
{
	int ret = 0;
	int i = 0;
	int number = -1;
	int j = 0;

	test_pr_info("%s: -- err_check TEST --", __func__);

	sscanf(buf, "%d", &number);

	if (number <= 0)
		number = 1;

	mbtd->test_group = TEST_ERR_CHECK_GROUP;

	if (validate_packed_commands_settings())
		return count;

	if (mbtd->random_test_seed > 0)
		test_pr_info("%s: Test seed: %d", __func__,
			      mbtd->random_test_seed);

	memset(&mbtd->test_info, 0, sizeof(struct test_info));

	mbtd->test_info.data = mbtd;
	mbtd->test_info.prepare_test_fn = prepare_test;
	mbtd->test_info.check_test_result_fn = check_wr_packing_statistics;
	mbtd->test_info.get_test_case_str_fn = get_test_case_str;
	mbtd->test_info.post_test_fn = post_test;

	for (i = 0 ; i < number ; ++i) {
		test_pr_info("%s: Cycle # %d / %d", __func__, i+1, number);
		test_pr_info("%s: ====================", __func__);

		for (j = ERR_CHECK_MIN_TESTCASE;
					j <= ERR_CHECK_MAX_TESTCASE ; j++) {
			mbtd->test_info.testcase = j;
			mbtd->is_random = RANDOM_TEST;
			ret = test_iosched_start_test(&mbtd->test_info);
			if (ret)
				break;
			/* Allow FS requests to be dispatched */
			msleep(1000);
			mbtd->test_info.testcase = j;
			mbtd->is_random = NON_RANDOM_TEST;
			ret = test_iosched_start_test(&mbtd->test_info);
			if (ret)
				break;
			/* Allow FS requests to be dispatched */
			msleep(1000);
		}
	}

	test_pr_info("%s: Completed all the test cases.", __func__);

	return count;
}

static ssize_t err_check_test_read(struct file *file,
			       char __user *buffer,
			       size_t count,
			       loff_t *offset)
{
	memset((void *)buffer, 0, count);

	snprintf(buffer, count,
		 "\nerr_check_TEST\n"
		 "=========\n"
		 "Description:\n"
		 "This test checks the following scenarios\n"
		 "- Return ABORT\n"
		 "- Return PARTIAL followed by success\n"
		 "- Return PARTIAL followed by abort\n"
		 "- Return PARTIAL multiple times until success\n"
		 "- Return PARTIAL with fail index = threshold\n"
		 "- Return RETRY\n"
		 "- Return CMD_ERR\n"
		 "- Return DATA_ERR\n");

	if (message_repeat == 1) {
		message_repeat = 0;
		return strnlen(buffer, count);
	} else {
		return 0;
	}
}

const struct file_operations err_check_test_ops = {
	.open = test_open,
	.write = err_check_test_write,
	.read = err_check_test_read,
};

/* send_invalid_packed TEST */
static ssize_t send_invalid_packed_test_write(struct file *file,
				const char __user *buf,
				size_t count,
				loff_t *ppos)
{
	int ret = 0;
	int i = 0;
	int number = -1;
	int j = 0;
	int num_of_failures = 0;

	test_pr_info("%s: -- send_invalid_packed TEST --", __func__);

	sscanf(buf, "%d", &number);

	if (number <= 0)
		number = 1;

	mbtd->test_group = TEST_SEND_INVALID_GROUP;

	if (validate_packed_commands_settings())
		return count;

	if (mbtd->random_test_seed > 0)
		test_pr_info("%s: Test seed: %d", __func__,
			      mbtd->random_test_seed);

	memset(&mbtd->test_info, 0, sizeof(struct test_info));

	mbtd->test_info.data = mbtd;
	mbtd->test_info.prepare_test_fn = prepare_test;
	mbtd->test_info.check_test_result_fn = check_wr_packing_statistics;
	mbtd->test_info.get_test_case_str_fn = get_test_case_str;
	mbtd->test_info.post_test_fn = post_test;

	for (i = 0 ; i < number ; ++i) {
		test_pr_info("%s: Cycle # %d / %d", __func__, i+1, number);
		test_pr_info("%s: ====================", __func__);

		for (j = INVALID_CMD_MIN_TESTCASE;
				j <= INVALID_CMD_MAX_TESTCASE ; j++) {

			mbtd->test_info.testcase = j;
			mbtd->is_random = RANDOM_TEST;
			ret = test_iosched_start_test(&mbtd->test_info);
			if (ret)
				num_of_failures++;
			/* Allow FS requests to be dispatched */
			msleep(1000);

			mbtd->test_info.testcase = j;
			mbtd->is_random = NON_RANDOM_TEST;
			ret = test_iosched_start_test(&mbtd->test_info);
			if (ret)
				num_of_failures++;
			/* Allow FS requests to be dispatched */
			msleep(1000);
		}
	}

	test_pr_info("%s: Completed all the test cases.", __func__);

	if (num_of_failures > 0) {
		test_iosched_set_test_result(TEST_FAILED);
		test_pr_err(
			"There were %d failures during the test, TEST FAILED",
			num_of_failures);
	}
	return count;
}

static ssize_t send_invalid_packed_test_read(struct file *file,
			       char __user *buffer,
			       size_t count,
			       loff_t *offset)
{
	memset((void *)buffer, 0, count);

	snprintf(buffer, count,
		 "\nsend_invalid_packed_TEST\n"
		 "=========\n"
		 "Description:\n"
		 "This test checks the following scenarios\n"
		 "- Send an invalid header version\n"
		 "- Send the wrong write code\n"
		 "- Send an invalid R/W code\n"
		 "- Send wrong start address in header\n"
		 "- Send header with block_count smaller than actual\n"
		 "- Send header with block_count larger than actual\n"
		 "- Send header CMD23 packed bit set\n"
		 "- Send CMD23 with block count over threshold\n"
		 "- Send CMD23 with block_count equals zero\n"
		 "- Send CMD23 packed bit unset\n"
		 "- Send CMD23 reliable write bit set\n"
		 "- Send CMD23 bits [16-29] set\n"
		 "- Send CMD23 header block not in block_count\n");

	if (message_repeat == 1) {
		message_repeat = 0;
		return strnlen(buffer, count);
	} else {
		return 0;
	}
}

const struct file_operations send_invalid_packed_test_ops = {
	.open = test_open,
	.write = send_invalid_packed_test_write,
	.read = send_invalid_packed_test_read,
};

static void mmc_block_test_debugfs_cleanup(void)
{
	debugfs_remove(mbtd->debug.random_test_seed);
	debugfs_remove(mbtd->debug.send_write_packing_test);
	debugfs_remove(mbtd->debug.err_check_test);
	debugfs_remove(mbtd->debug.send_invalid_packed_test);
}

static int mmc_block_test_debugfs_init(void)
{
	struct dentry *utils_root, *tests_root;

	utils_root = test_iosched_get_debugfs_utils_root();
	tests_root = test_iosched_get_debugfs_tests_root();

	if (!utils_root || !tests_root)
		return -EINVAL;

	mbtd->debug.random_test_seed = debugfs_create_u32(
					"random_test_seed",
					S_IRUGO | S_IWUGO,
					utils_root,
					&mbtd->random_test_seed);

	if (!mbtd->debug.random_test_seed)
		goto err_nomem;

	mbtd->debug.send_write_packing_test =
		debugfs_create_file("send_write_packing_test",
				    S_IRUGO | S_IWUGO,
				    tests_root,
				    NULL,
				    &send_write_packing_test_ops);

	if (!mbtd->debug.send_write_packing_test)
		goto err_nomem;

	mbtd->debug.err_check_test =
		debugfs_create_file("err_check_test",
				    S_IRUGO | S_IWUGO,
				    tests_root,
				    NULL,
				    &err_check_test_ops);

	if (!mbtd->debug.err_check_test)
		goto err_nomem;

	mbtd->debug.send_invalid_packed_test =
		debugfs_create_file("send_invalid_packed_test",
				    S_IRUGO | S_IWUGO,
				    tests_root,
				    NULL,
				    &send_invalid_packed_test_ops);

	if (!mbtd->debug.send_invalid_packed_test)
		goto err_nomem;

	return 0;

err_nomem:
	mmc_block_test_debugfs_cleanup();
	return -ENOMEM;
}

static void mmc_block_test_probe(void)
{
	struct request_queue *q = test_iosched_get_req_queue();
	struct mmc_queue *mq;
	int max_packed_reqs;

	if (!q) {
		test_pr_err("%s: NULL request queue", __func__);
		return;
	}

	mq = q->queuedata;
	if (!mq) {
		test_pr_err("%s: NULL mq", __func__);
		return;
	}

	max_packed_reqs = mq->card->ext_csd.max_packed_writes;
	mbtd->exp_packed_stats.packing_events =
			kzalloc((max_packed_reqs + 1) *
				sizeof(*mbtd->exp_packed_stats.packing_events),
				GFP_KERNEL);

	mmc_block_test_debugfs_init();
}

static void mmc_block_test_remove(void)
{
	mmc_block_test_debugfs_cleanup();
}

static int __init mmc_block_test_init(void)
{
	mbtd = kzalloc(sizeof(struct mmc_block_test_data), GFP_KERNEL);
	if (!mbtd) {
		test_pr_err("%s: failed to allocate mmc_block_test_data",
			    __func__);
		return -ENODEV;
	}

	mbtd->bdt.init_fn = mmc_block_test_probe;
	mbtd->bdt.exit_fn = mmc_block_test_remove;
	INIT_LIST_HEAD(&mbtd->bdt.list);
	test_iosched_register(&mbtd->bdt);

	return 0;
}

static void __exit mmc_block_test_exit(void)
{
	test_iosched_unregister(&mbtd->bdt);
	kfree(mbtd);
}

module_init(mmc_block_test_init);
module_exit(mmc_block_test_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MMC block test");
