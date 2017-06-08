/* Copyright (C) 2016-2017 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>
#include <string.h>
#include <math.h>

#include <crt_internal.h>

#define CRT_SELF_TEST_AUTO_BULK_THRESH		(1 << 20)
#define CRT_SELF_TEST_GROUP_NAME		("crt_self_test")

struct st_size_params {
	uint32_t send_size;
	uint32_t reply_size;
	union {
		struct {
			enum crt_st_msg_type send_type: 2;
			enum crt_st_msg_type reply_type: 2;
		};
		uint32_t flags;
	};
};

struct st_endpoint {
	uint32_t rank;
	uint32_t tag;
};

struct st_master_endpt {
	crt_endpoint_t endpt;
	struct crt_st_status_req_reply reply;
	int32_t test_failed;
	int32_t test_completed;
};

static const char * const crt_st_msg_type_str[] = { "EMPTY",
						    "IOV",
						    "BULK_PUT",
						    "BULK_GET" };

/* User input maximum values */
#define SELF_TEST_MAX_REPETITIONS (0x40000000)
#define SELF_TEST_MAX_INFLIGHT (0x40000000)
#define SELF_TEST_MAX_LIST_STR_LEN (1 << 16)
#define SELF_TEST_MAX_NUM_ENDPOINTS (UINT32_MAX)

/* Global shutdown flag, used to terminate the progress thread */
static int g_shutdown_flag;


static void *progress_fn(void *arg)
{
	int		 ret;
	crt_context_t	*crt_ctx = NULL;

	crt_ctx = (crt_context_t *)arg;
	C_ASSERT(crt_ctx != NULL);
	C_ASSERT(*crt_ctx != NULL);

	while (!g_shutdown_flag) {
		ret = crt_progress(*crt_ctx, 1, NULL, NULL);
		if (ret != 0 && ret != -CER_TIMEDOUT) {
			C_ERROR("crt_progress failed; ret = %d\n", ret);
			break;
		}
	};

	pthread_exit(NULL);
}

static int self_test_init(char *dest_name, crt_context_t *crt_ctx,
			  crt_group_t **srv_grp, pthread_t *tid)
{
	crt_rank_t	myrank;
	int		ret;

	ret = crt_init(CRT_SELF_TEST_GROUP_NAME, 0);
	if (ret != 0) {
		C_ERROR("crt_init failed; ret = %d\n", ret);
		return ret;
	}

	ret = crt_context_create(NULL, crt_ctx);
	if (ret != 0) {
		C_ERROR("crt_context_create failed; ret = %d\n", ret);
		return ret;
	}

	ret = crt_group_attach(dest_name, srv_grp);
	if (ret != 0) {
		C_ERROR("crt_group_attach failed; ret = %d\n", ret);
		return ret;
	}
	C_ASSERTF(*srv_grp != NULL,
		  "crt_group_attach succeeded but returned group is NULL\n");

	ret = crt_group_rank(NULL, &myrank);
	if (ret != 0) {
		C_ERROR("crt_group_rank failed; ret = %d\n", ret);
		return ret;
	}

	g_shutdown_flag = 0;

	ret = pthread_create(tid, NULL, progress_fn, crt_ctx);
	if (ret != 0) {
		C_ERROR("failed to create progress thread: %s\n",
			strerror(errno));
		return -CER_MISC;
	}

	return 0;
}

static int st_compare_endpts(const void *a_in, const void *b_in)
{
	struct st_endpoint *a = (struct st_endpoint *)a_in;
	struct st_endpoint *b = (struct st_endpoint *)b_in;

	if (a->rank != b->rank)
		return a->rank > b->rank;
	return a->tag > b->tag;
}

static int st_compare_latencies_by_vals(const void *a_in, const void *b_in)
{
	struct st_latency *a = (struct st_latency *)a_in;
	struct st_latency *b = (struct st_latency *)b_in;

	if (a->val != b->val)
		return a->val > b->val;
	return a->cci_rc > b->cci_rc;
}

static int st_compare_latencies_by_ranks(const void *a_in, const void *b_in)
{
	struct st_latency *a = (struct st_latency *)a_in;
	struct st_latency *b = (struct st_latency *)b_in;

	if (a->rank != b->rank)
		return a->rank > b->rank;
	if (a->tag != b->tag)
		return a->tag > b->tag;
	if (a->val != b->val)
		return a->val > b->val;
	return a->cci_rc > b->cci_rc;
}

static int start_test_cb(const struct crt_cb_info *cb_info)
{
	/* Result returned to main thread */
	int32_t *return_status = (int32_t *)cb_info->cci_arg;

	/* Status retrieved from the RPC result payload */
	int32_t *reply_status;

	/* Check the status of the RPC transport itself */
	if (cb_info->cci_rc != 0) {
		*return_status = cb_info->cci_rc;
		return 0;
	}

	/* Get the status from the payload */
	reply_status = (int32_t *)crt_reply_get(cb_info->cci_rpc);
	C_ASSERT(reply_status != NULL);

	/* Return whatever result we got to the main thread */
	*return_status = *reply_status;

	return 0;
}

static int status_req_cb(const struct crt_cb_info *cb_info)
{
	/* Result returned to main thread */
	struct crt_st_status_req_reply   *return_status =
		(struct crt_st_status_req_reply *)cb_info->cci_arg;

	/* Status retrieved from the RPC result payload */
	struct crt_st_status_req_reply   *reply_status;

	/* Check the status of the RPC transport itself */
	if (cb_info->cci_rc != 0) {
		return_status->status = cb_info->cci_rc;
		return 0;
	}

	/* Get the status from the payload */
	reply_status = (struct crt_st_status_req_reply *)
		       crt_reply_get(cb_info->cci_rpc);
	C_ASSERT(reply_status != NULL);

	/*
	 * Return whatever result we got to the main thread
	 *
	 * Write these in specific order so we can avoid locking
	 * TODO: This assumes int32 writes are atomic
	 *   (they are on x86 if 4-byte aligned)
	 */
	return_status->test_duration_ns = reply_status->test_duration_ns;
	return_status->num_remaining = reply_status->num_remaining;
	return_status->status = reply_status->status;

	return 0;
}

/*
 * Iterates over a list of failing latency measurements and prints out the
 * count of each type of failure, along with the error string and code
 *
 * The input latencies must be sorted by cci_rc to group all same cci_rc values
 * together into contiguous blocks (-1 -1 -1, -2 -2 -2, etc.)
 */
static void print_fail_counts(struct st_latency *latencies,
			      uint32_t num_latencies,
			      const char *prefix)
{
	uint32_t last_err_idx = 0;
	uint32_t local_rep = 0;

	/* Function called but no errors to print */
	if (latencies[0].cci_rc == 0)
		return;

	while (1) {
		/*
		 * Detect when the error code has changed and print the count
		 * of the last error code block
		 */
		if (local_rep >= num_latencies ||
		    latencies[local_rep].cci_rc == 0 ||
		    latencies[last_err_idx].cci_rc !=
		    latencies[local_rep].cci_rc) {
			printf("%s%u: -%s (%d)\n", prefix,
			       local_rep - last_err_idx,
			       crt_errstr(-latencies[last_err_idx].cci_rc),
			       latencies[last_err_idx].cci_rc);
			last_err_idx = local_rep;
		}

		/* Abort upon reaching the end of the list or a non-failure */
		if (local_rep >= num_latencies ||
		    latencies[local_rep].cci_rc == 0)
			break;

		local_rep++;
	}

}

static void print_results(struct st_latency *latencies,
			  struct crt_st_start_params *test_params,
			  int64_t test_duration_ns, int output_megabits)
{
	uint32_t	 local_rep;
	uint32_t	 num_failed = 0;
	uint32_t	 num_passed = 0;
	double		 latency_std_dev;
	int64_t		 latency_avg;
	double		 throughput;
	double		 bandwidth;

	/* Check for bugs */
	C_ASSERT(latencies != NULL);
	C_ASSERT(test_params != NULL);
	C_ASSERT(test_params->rep_count != 0);
	C_ASSERT(test_duration_ns > 0);

	/* Compute the throughput in RPCs/sec */
	throughput = test_params->rep_count /
		(test_duration_ns / 1000000000.0F);
	/* Compute bandwidth in bytes */
	bandwidth = throughput * (test_params->send_size +
				  test_params->reply_size);

	/* Print the results for this size */
	if (output_megabits)
		printf("\tRPC Bandwidth (Mbits/sec): %.2f\n",
		       bandwidth * 8.0F / 1000000.0F);
	else
		printf("\tRPC Bandwidth (MB/sec): %.2f\n",
		       bandwidth / (1024.0F * 1024.0F));
	printf("\tRPC Throughput (RPCs/sec): %.0f\n", throughput);


	/* Figure out how many repetitions were errors */
	num_failed = 0;
	for (local_rep = 0; local_rep < test_params->rep_count; local_rep++)
		if (latencies[local_rep].cci_rc < 0) {
			num_failed++;

			/* Since this RPC failed, overwrite its latency
			 * with -1 so it will sort before any passing
			 * RPCs. This segments the latencies into two
			 * sections - from [0:num_failed] will be -1,
			 * and from [num_failed:] will be succesful RPC
			 * latencies
			 */
			latencies[local_rep].val = -1;
		}

	/*
	 * Compute number successful and exit early if none worked to
	 * guard against overflow and divide by zero later
	 */
	num_passed = test_params->rep_count - num_failed;
	if (num_passed == 0) {
		printf("\tAll RPCs for this message size failed\n");
		return;
	}

	/*
	 * Sort the latencies by: (in descending order of precedence)
	 * - val
	 * - cci_rc
	 *
	 * Note that errors have a val = -1, so they get grouped together
	 */
	qsort(latencies, test_params->rep_count,
	      sizeof(latencies[0]), st_compare_latencies_by_vals);

	/* Compute average and standard deviation of all results */
	latency_avg = 0;
	for (local_rep = num_failed; local_rep < test_params->rep_count;
	     local_rep++)
		latency_avg += latencies[local_rep].val;
	latency_avg /= test_params->rep_count;

	latency_std_dev = 0;
	for (local_rep = num_failed; local_rep < test_params->rep_count;
	     local_rep++)
		latency_std_dev +=
			pow(latencies[local_rep].val - latency_avg,
			    2);
	latency_std_dev /= num_passed;
	latency_std_dev = sqrt(latency_std_dev);

	/* Print latency summary results */
	printf("\tRPC Latencies (us):\n"
	       "\t\tMin    : %ld\n"
	       "\t\t25th  %%: %ld\n"
	       "\t\tMedian : %ld\n"
	       "\t\t75th  %%: %ld\n"
	       "\t\tMax    : %ld\n"
	       "\t\tAverage: %ld\n"
	       "\t\tStd Dev: %.2f\n",
	       latencies[num_failed].val / 1000,
	       latencies[num_failed + num_passed / 4].val / 1000,
	       latencies[num_failed + num_passed / 2].val / 1000,
	       latencies[num_failed + num_passed*3/4].val / 1000,
	       latencies[test_params->rep_count - 1].val / 1000,
	       latency_avg / 1000, latency_std_dev / 1000);

	/* Print error summary results */
	printf("\tRPC Failures: %u\n", num_failed);
	/* print_fail_counts(&latencies[0], num_failed, "\t\t"); */

	printf("\n");

	/*
	 * Sort the latencies by: (in descending order of precedence)
	 * - rank
	 * - tag
	 * - val
	 * - cci_rc
	 *
	 * Note that errors have a val = -1, so they get grouped together
	 */
	qsort(latencies, test_params->rep_count,
	      sizeof(latencies[0]), st_compare_latencies_by_ranks);

	printf("\tEndpoint results (rank:tag - Median Latency (us)):\n");

	/* Iterate over each rank / tag pair */
	local_rep = 0;
	do {
		uint32_t rank = latencies[local_rep].rank;
		uint32_t tag = latencies[local_rep].tag;
		uint32_t start_idx = local_rep;
		uint32_t last_idx;
		uint32_t median_idx;

		/* Compute start, last, and num_failed for this rank/tag */
		num_failed = 0;
		while (1) {
			if (latencies[local_rep].rank != rank ||
			    latencies[local_rep].tag != tag)
				break;

			if (latencies[local_rep].cci_rc < 0)
				num_failed++;

			local_rep++;
			if (local_rep >= test_params->rep_count)
				break;
		}
		last_idx = local_rep - 1;
		C_ASSERT(start_idx + num_failed <= last_idx);

		/* Compute median index */
		median_idx = start_idx + num_failed +
			(last_idx - start_idx - num_failed) / 2;
		C_ASSERT(median_idx <= last_idx);
		C_ASSERT(median_idx >= start_idx + num_failed);

		printf("\t\t%u:%u - ", rank, tag);

		/* At least some messages to this endpoint succeeded */
		if (start_idx + num_failed < last_idx)
			printf("%ld", latencies[median_idx].val / 1000);

		printf("\n");
		if (num_failed > 0)
			printf("\t\t\tFailures: %u\n", num_failed);
		print_fail_counts(&latencies[start_idx], num_failed, "\t\t\t");

	} while (local_rep < test_params->rep_count);

	printf("\n");

}

static int test_msg_size(crt_context_t crt_ctx,
			 struct st_master_endpt *ms_endpts,
			 uint32_t num_ms_endpts,
			 struct crt_st_start_params *test_params,
			 struct st_latency **latencies,
			 crt_bulk_t *latencies_bulk_hdl, int output_megabits)
{

	int				 ret;
	int				 done;
	uint32_t			 failed_count;
	uint32_t			 complete_count;
	crt_rpc_t			*new_rpc;
	struct crt_st_start_params	*start_args;
	uint32_t			 m_idx;

	/*
	 * Launch self-test 1:many sessions on each master endpoint
	 * as simultaneously as possible (don't wait for acknowledgement)
	 */
	for (m_idx = 0; m_idx < num_ms_endpts; m_idx++) {
		crt_endpoint_t endpt = ms_endpts[m_idx].endpt;

		/* Create and send a new RPC starting the test */
		ret = crt_req_create(crt_ctx, endpt, CRT_OPC_SELF_TEST_START,
				     &new_rpc);
		if (ret != 0) {
			C_ERROR("Creating start RPC failed to endpoint"
				" %u:%u; ret = %d\n", endpt.ep_rank,
				endpt.ep_tag, ret);
			ms_endpts[m_idx].test_failed = 1;
			ms_endpts[m_idx].test_completed = 1;
			continue;
		}

		start_args = (struct crt_st_start_params *)
			crt_req_get(new_rpc);
		C_ASSERTF(start_args != NULL,
			  "crt_req_get returned NULL\n");
		memcpy(start_args, test_params, sizeof(*test_params));

		/* Set the launch status to a known impossible value */
		ms_endpts[m_idx].reply.status = INT32_MAX;

		ret = crt_req_send(new_rpc, start_test_cb,
				   &ms_endpts[m_idx].reply.status);
		if (ret != 0) {
			C_ERROR("Failed to send start RPC to endpoint %u:%u; "
				"ret = %d\n", endpt.ep_rank, endpt.ep_tag, ret);
			ms_endpts[m_idx].test_failed = 1;
			ms_endpts[m_idx].test_completed = 1;
			continue;
		}
	}

	/*
	 * Wait for each node to report whether or not the test launched
	 * successfully
	 */
	do {
		/* Flag indicating all test launches have returned a status */
		done = 1;

		/* Wait a bit for tests to finish launching */
		sched_yield();

		for (m_idx = 0; m_idx < num_ms_endpts; m_idx++)
			if (ms_endpts[m_idx].reply.status == INT32_MAX) {
				/* No response yet... */
				done = 0;
				break;
			}
	} while (done != 1);

	/* Print a warning for any 1:many sessions that failed to launch */
	failed_count = 0;
	for (m_idx = 0; m_idx < num_ms_endpts; m_idx++)
		if (ms_endpts[m_idx].reply.status != 0) {
			C_ERROR("Failed to launch self-test 1:many session on"
				" %u:%u; ret = %d\n",
				ms_endpts[m_idx].endpt.ep_rank,
				ms_endpts[m_idx].endpt.ep_tag,
				ms_endpts[m_idx].reply.status);
			ms_endpts[m_idx].test_failed = 1;
			ms_endpts[m_idx].test_completed = 1;
			failed_count++;
		} else if (ms_endpts[m_idx].test_failed != 0) {
			ms_endpts[m_idx].test_failed = 1;
			ms_endpts[m_idx].test_completed = 1;
			failed_count++;
		} else {
			ms_endpts[m_idx].test_failed = 0;
			ms_endpts[m_idx].test_completed = 0;

		}


	/* Check to make sure that at least one 1:many session was started */
	if (failed_count >= num_ms_endpts) {
		C_ERROR("Failed to launch any 1:many test sessions\n");
		return ms_endpts[0].reply.status;
	}

	/*
	 * Poll the master nodes until all tests complete
	 *   (either successfully or by returning an error)
	 */
	do {
		/* Wait a small amount of time for tests to progress */
		sleep(1);

		/* Send status requests to every non-finished node */
		for (m_idx = 0; m_idx < num_ms_endpts; m_idx++) {
			/* Skip endpoints that have finished */
			if (ms_endpts[m_idx].test_completed != 0)
				continue;

			/* Set result status to impossible guard value */
			ms_endpts[m_idx].reply.status = INT32_MAX;

			/* Create a new RPC to check the status */
			ret = crt_req_create(crt_ctx, ms_endpts[m_idx].endpt,
					     CRT_OPC_SELF_TEST_STATUS_REQ,
					     &new_rpc);
			if (ret != 0) {
				C_ERROR("Creating status request RPC to"
					" endpoint %u:%u; ret = %d\n",
					ms_endpts[m_idx].endpt.ep_rank,
					ms_endpts[m_idx].endpt.ep_tag,
					ret);
				ms_endpts[m_idx].test_failed = 1;
				ms_endpts[m_idx].test_completed = 1;
				continue;
			}

			/*
			 * Sent data is the bulk handle where results should
			 * be written
			 */
			*((crt_bulk_t *)crt_req_get(new_rpc)) =
				latencies_bulk_hdl[m_idx];

			/* Send the status request */
			ret = crt_req_send(new_rpc, status_req_cb,
					   &ms_endpts[m_idx].reply);
			if (ret != 0) {
				C_ERROR("Failed to send status RPC to endpoint"
					" %u:%u; ret = %d\n",
					ms_endpts[m_idx].endpt.ep_rank,
					ms_endpts[m_idx].endpt.ep_tag, ret);
				ms_endpts[m_idx].test_failed = 1;
				ms_endpts[m_idx].test_completed = 1;
				continue;
			}
		}

		/* Wait for all status request results to come back */
		do {
			/* Flag indicating all status requests have returned */
			done = 1;

			/* Wait a bit for status requests to be handled */
			sched_yield();

			for (m_idx = 0; m_idx < num_ms_endpts; m_idx++)
				if (ms_endpts[m_idx].reply.status ==
				    INT32_MAX &&
				    ms_endpts[m_idx].test_completed == 0) {
					/* No response yet... */
					done = 0;
					break;
				}
		} while (done != 1);

		complete_count = 0;
		for (m_idx = 0; m_idx < num_ms_endpts; m_idx++) {
			/* Skip endpoints that have already finished */
			if (ms_endpts[m_idx].test_completed != 0) {
				complete_count++;
				continue;
			}

			switch (ms_endpts[m_idx].reply.status) {
			case CRT_ST_STATUS_TEST_IN_PROGRESS:
				C_DEBUG("Test still processing on %u:%u -"
					" # RPCs remaining: %u\n",
					ms_endpts[m_idx].endpt.ep_rank,
					ms_endpts[m_idx].endpt.ep_tag,
					ms_endpts[m_idx].reply.num_remaining);
				break;
			case CRT_ST_STATUS_TEST_COMPLETE:
				ms_endpts[m_idx].test_completed = 1;
				break;
			default:
				C_ERROR("Detected test failure on %u:%u -"
					" ret = %d\n",
					ms_endpts[m_idx].endpt.ep_rank,
					ms_endpts[m_idx].endpt.ep_tag,
					ms_endpts[m_idx].reply.status);
				ms_endpts[m_idx].test_failed = 1;
				ms_endpts[m_idx].test_completed = 1;
				complete_count++;
			}
		}
	} while (complete_count < num_ms_endpts);

	/*
	 * TODO:
	 * In the future, probably want to return the latencies here
	 * before they are processed for display to the user.
	 */

	/* Print the results for this size */
	printf("##################################################\n");
	printf("Results for message size (%d-%s %d-%s)"
	       " (max_inflight_rpcs = %d):\n\n",
	       test_params->send_size,
	       crt_st_msg_type_str[test_params->send_type],
	       test_params->reply_size,
	       crt_st_msg_type_str[test_params->reply_type],
	       test_params->max_inflight);

	for (m_idx = 0; m_idx < num_ms_endpts; m_idx++) {
		int print_count;

		/* Skip endpoints that failed */
		if (ms_endpts[m_idx].test_failed != 0)
			continue;

		/* Print a header for this endpoint and store number of chars */
		printf("Master Endpoint %u:%u%n\n",
		       ms_endpts[m_idx].endpt.ep_rank,
		       ms_endpts[m_idx].endpt.ep_tag,
		       &print_count);

		/* Print a nice line under the header of the right length */
		for (; print_count > 0; print_count--)
			printf("-");
		printf("\n");

		print_results(latencies[m_idx], test_params,
			      ms_endpts[m_idx].reply.test_duration_ns,
			      output_megabits);
	}

	return 0;
}

static int run_self_test(struct st_size_params all_params[],
			 int num_msg_sizes, int rep_count, int max_inflight,
			 char *dest_name, struct st_endpoint *ms_endpts_in,
			 uint32_t num_ms_endpts_in,
			 struct st_endpoint *endpts, uint32_t num_endpts,
			 int output_megabits, int16_t buf_alignment)
{
	crt_context_t		  crt_ctx;
	crt_group_t		 *srv_grp;
	pthread_t		  tid;

	int			  size_idx;
	uint32_t		  m_idx;

	int			  ret;
	int			  cleanup_ret;

	struct st_master_endpt	 *ms_endpts;
	uint32_t		  num_ms_endpts;

	struct st_latency	**latencies = NULL;
	crt_iov_t		 *latencies_iov = NULL;
	crt_sg_list_t		 *latencies_sg_list = NULL;
	crt_bulk_t		 *latencies_bulk_hdl = CRT_BULK_NULL;

	crt_endpoint_t		  self_endpt;

	/* Sanity checks that would indicate bugs */
	C_ASSERT(endpts != NULL && num_endpts > 0);
	C_ASSERT((ms_endpts_in == NULL && num_ms_endpts_in == 0) ||
		 (ms_endpts_in != NULL && num_ms_endpts_in > 0));

	/* Initialize CART */
	ret = self_test_init(dest_name, &crt_ctx, &srv_grp, &tid);
	if (ret != 0) {
		C_ERROR("self_test_init failed; ret = %d\n", ret);
		C_GOTO(cleanup_nothread, ret);
	}

	/* Get the group/rank/tag for this application (self_endpt) */
	ret = crt_group_rank(NULL, &self_endpt.ep_rank);
	if (ret != 0) {
		C_ERROR("crt_group_rank failed; ret = %d\n", ret);
		C_GOTO(cleanup, ret);
	}
	self_endpt.ep_grp = crt_group_lookup(CRT_SELF_TEST_GROUP_NAME);
	if (self_endpt.ep_grp == NULL) {
		C_ERROR("crt_group_lookup failed for group %s\n",
			CRT_SELF_TEST_GROUP_NAME);
		C_GOTO(cleanup, ret = -CER_NONEXIST);
	}
	self_endpt.ep_tag = 0;

	/*
	 * Allocate a new list of unique master endpoints, each with a
	 * crt_endpoint_t and additional metadata
	 */
	if (ms_endpts_in == NULL) {
		/*
		 * If no master endpoints were specified, allocate just one and
		 * set it to self_endpt
		 */
		num_ms_endpts = 1;
		C_ALLOC_PTR(ms_endpts);
		if (ms_endpts == NULL) {
			C_ERROR("Allocating ms_endpts failed\n");
			C_GOTO(cleanup, ret = -CER_NOMEM);
		}
		ms_endpts[0].endpt.ep_rank = self_endpt.ep_rank;
		ms_endpts[0].endpt.ep_tag = self_endpt.ep_tag;
		/*
		 * TODO: The commented line below is what should be used, but
		 * this requires additional changes elsewhere. See CART-187
		 */
		/* ms_endpts[0].endpt.ep_grp = self_endpt.ep_grp; */
		ms_endpts[0].endpt.ep_grp = srv_grp;
	} else {
		/*
		 * If master endpoints were specified, initially allocate enough
		 * space to hold all of them, but only unique master endpoints
		 * to the new list
		 */
		C_ALLOC(ms_endpts, num_ms_endpts_in * sizeof(*ms_endpts));
		if (ms_endpts == NULL) {
			C_ERROR("Allocating ms_endpts failed\n");
			C_GOTO(cleanup, ret = -CER_NOMEM);
		}

		/*
		 * Sort the supplied endpoints to make it faster to identify
		 * duplicates
		 */
		qsort(ms_endpts_in, num_ms_endpts_in,
		      sizeof(ms_endpts_in[0]), st_compare_endpts);

		/* Add the first element to the new list */
		ms_endpts[0].endpt.ep_rank = ms_endpts_in[0].rank;
		ms_endpts[0].endpt.ep_tag = ms_endpts_in[0].tag;
		/*
		 * TODO: This isn't right - it should be self_endpt.ep_grp.
		 * However, this requires changes elsewhere - this is tracked
		 * by CART-187.
		 *
		 * As implemented here, rank 0 tag 0 in the client group will
		 * be used as the master endpoint by default
		 */
		ms_endpts[0].endpt.ep_grp = srv_grp;
		num_ms_endpts = 1;

		/*
		 * Add unique elements to the new list
		 */
		for (m_idx = 1; m_idx < num_ms_endpts_in; m_idx++)
			if ((ms_endpts_in[m_idx].rank !=
			     ms_endpts[num_ms_endpts - 1].endpt.ep_rank) ||
			    (ms_endpts_in[m_idx].tag !=
			     ms_endpts[num_ms_endpts - 1].endpt.ep_tag)) {
				ms_endpts[num_ms_endpts].endpt.ep_rank =
					ms_endpts_in[m_idx].rank;
				ms_endpts[num_ms_endpts].endpt.ep_tag =
					ms_endpts_in[m_idx].tag;
				ms_endpts[num_ms_endpts].endpt.ep_grp =
					srv_grp;
				num_ms_endpts++;
			}

		/*
		 * If the counts don't match up, some were duplicates - resize
		 * the resulting smaller array which contains only unique
		 * entries
		 */
		if (num_ms_endpts != num_ms_endpts_in) {
			struct st_master_endpt *realloc_ptr =
				C_REALLOC(ms_endpts, num_ms_endpts *
						       sizeof(*ms_endpts));
			if (realloc_ptr == NULL) {
				C_ERROR("Failed to shrink ms_endpts array\n");
				C_GOTO(cleanup, ret = -CER_NOMEM);
			}
			ms_endpts = realloc_ptr;
		}
	}

	/* Allocate latency lists for each 1:many session */
	C_ALLOC(latencies, num_ms_endpts * sizeof(*latencies));
	if (latencies == NULL) {
		C_ERROR("Failed to allocate latency pointers\n");
		C_GOTO(cleanup, ret = -CER_NOMEM);
	}
	C_ALLOC(latencies_iov, num_ms_endpts * sizeof(*latencies_iov));
	if (latencies_iov == NULL) {
		C_ERROR("Failed to allocate latency pointers\n");
		C_GOTO(cleanup, ret = -CER_NOMEM);
	}
	C_ALLOC(latencies_sg_list,
		num_ms_endpts * sizeof(*latencies_sg_list));
	if (latencies_sg_list == NULL) {
		C_ERROR("Failed to allocate latency pointers\n");
		C_GOTO(cleanup, ret = -CER_NOMEM);
	}
	C_ALLOC(latencies_bulk_hdl,
		num_ms_endpts * sizeof(*latencies_bulk_hdl));
	if (latencies_bulk_hdl == NULL) {
		C_ERROR("Failed to allocate latency pointers\n");
		C_GOTO(cleanup, ret = -CER_NOMEM);
	}

	/*
	 * For each 1:many session, allocate an array for latency results.
	 * Map that array to an IOV, and create a bulk handle that will be used
	 * to transfer latency results back into that buffer
	 */
	for (m_idx = 0; m_idx < num_ms_endpts; m_idx++) {
		C_ALLOC(latencies[m_idx], rep_count * sizeof(**latencies));
		if (latencies[m_idx] == NULL) {
			C_ERROR("Failed to allocate latency data storage\n");
			C_GOTO(cleanup, ret = -CER_NOMEM);
		}
		crt_iov_set(&latencies_iov[m_idx], latencies[m_idx],
			    rep_count * sizeof(**latencies));
		latencies_sg_list[m_idx].sg_iovs =
			&latencies_iov[m_idx];
		latencies_sg_list[m_idx].sg_nr.num = 1;

		ret = crt_bulk_create(crt_ctx, &latencies_sg_list[m_idx],
				      CRT_BULK_RW,
				      &latencies_bulk_hdl[m_idx]);
		if (ret != 0) {
			C_ERROR("Failed to allocate latencies bulk handle;"
				" ret = %d\n", ret);
			C_GOTO(cleanup, ret);
		}
		C_ASSERT(latencies_bulk_hdl != CRT_BULK_NULL);
	}

	for (size_idx = 0; size_idx < num_msg_sizes; size_idx++) {
		struct crt_st_start_params	 test_params = { {0} };

		/* Set test parameters to send to the test node */
		crt_iov_set(&test_params.endpts, endpts,
			    num_endpts * sizeof(*endpts));
		test_params.rep_count = rep_count;
		test_params.max_inflight = max_inflight;
		test_params.send_size = all_params[size_idx].send_size;
		test_params.reply_size = all_params[size_idx].reply_size;
		test_params.send_type = all_params[size_idx].send_type;
		test_params.reply_type = all_params[size_idx].reply_type;
		test_params.buf_alignment = buf_alignment;

		ret = test_msg_size(crt_ctx, ms_endpts, num_ms_endpts,
				    &test_params, latencies, latencies_bulk_hdl,
				    output_megabits);
		if (ret != 0) {
			C_ERROR("Testing message size (%d-%s %d-%s) failed;"
				" ret = %d\n",
				test_params.send_size,
				crt_st_msg_type_str[test_params.send_type],
				test_params.reply_size,
				crt_st_msg_type_str[test_params.reply_type],
				ret);
			C_GOTO(cleanup, ret);
		}
	}

cleanup:
	/* Tell the progress thread to abort and exit */
	g_shutdown_flag = 1;

	ret = pthread_join(tid, NULL);
	if (ret)
		C_ERROR("Could not join progress thread");

cleanup_nothread:
	if (latencies_bulk_hdl != NULL) {
		for (m_idx = 0; m_idx < num_ms_endpts; m_idx++)
			if (latencies_bulk_hdl[m_idx] != CRT_BULK_NULL)
				crt_bulk_free(latencies_bulk_hdl[m_idx]);
		C_FREE(latencies_bulk_hdl,
		       num_ms_endpts * sizeof(*latencies_bulk_hdl));
	}
	if (latencies_sg_list != NULL)
		C_FREE(latencies_sg_list,
		       num_ms_endpts * sizeof(*latencies_sg_list));
	if (latencies_iov != NULL)
		C_FREE(latencies_iov, num_ms_endpts * sizeof(*latencies_iov));
	if (latencies != NULL) {
		for (m_idx = 0; m_idx < num_ms_endpts; m_idx++)
			if (latencies[m_idx] != NULL)
				C_FREE(latencies[m_idx],
				       rep_count * sizeof(*latencies));
		C_FREE(latencies, num_ms_endpts * sizeof(*latencies));
	}

	if (srv_grp != NULL) {
		cleanup_ret = crt_group_detach(srv_grp);
		if (cleanup_ret != 0)
			C_ERROR("crt_group_detach failed; ret = %d\n",
				cleanup_ret);
		/* Make sure first error is returned, if applicable */
		ret = ((ret == 0) ? cleanup_ret : ret);
	}

	cleanup_ret = crt_context_destroy(crt_ctx, 0);
	if (cleanup_ret != 0)
		C_ERROR("crt_context_destroy failed; ret = %d\n", cleanup_ret);
	/* Make sure first error is returned, if applicable */
	ret = ((ret == 0) ? cleanup_ret : ret);

	cleanup_ret = crt_finalize();
	if (cleanup_ret != 0)
		C_ERROR("crt_finalize failed; ret = %d\n", cleanup_ret);
	/* Make sure first error is returned, if applicable */
	ret = ((ret == 0) ? cleanup_ret : ret);
	return ret;
}

static void print_usage(const char *prog_name, const char *msg_sizes_str,
			int rep_count,
			int max_inflight)
{
	/* TODO --randomize-endpoints */
	/* TODO --verbose */
	printf("Usage: %s --group-name <name> --endpoint <ranks:tags> [optional arguments]\n"
	       "\n"
	       "Required Arguments\n"
	       "  --group-name <group_name>\n"
	       "      Short version: -g\n"
	       "      The name of the process set to test against\n"
	       "\n"
	       "  --endpoint <ranks:tags>\n"
	       "      Short version: -e\n"
	       "      Describes an endpoint (or range of endpoints) to connect to\n"
	       "        Note: Can be specified multiple times\n"
	       "\n"
	       "      ranks and tags are comma-separated lists to connect to\n"
	       "        Supports both ranges and lists - for example, \"1-5,3,8\"\n"
	       "\n"
	       "      Example: --endpoint 1-3,2:0-1\n"
	       "        This would create these endpoints:\n"
	       "          1:0\n"
	       "          1:1\n"
	       "          2:0\n"
	       "          2:1\n"
	       "          3:0\n"
	       "          3:1\n"
	       "          2:0\n"
	       "          2:1\n"
	       "\n"
	       "        By default, self-test will send test messages to these\n"
	       "        endpoints in the order listed above. See --randomize-endpoints\n"
	       "        for more information\n"
	       "\n"
	       "Optional Arguments\n"
	       "  --message-sizes <(a b),(c d),...>\n"
	       "      Short version: -s\n"
	       "      List of size tuples (in bytes) to use for the self test.\n"
	       "\n"
	       "      Note that the ( ) are not strictly necessary\n"
	       "      Providing a single size (a) is interpreted as an alias for (a a)\n"
	       "\n"
	       "      For each tuple, the first value is the sent size, and the second value is the reply size\n"
	       "      Valid sizes are [0-%d]\n"
	       "      Performance results will be reported individually for each tuple.\n"
	       "\n"
	       "      Each size integer can be prepended with a single character to specify\n"
	       "      the underlying transport mechanism. Available types are:\n"
	       "        'e' - Empty (no payload)\n"
	       "        'i' - I/O vector (IOV)\n"
	       "        'b' - Bulk transfer\n"
	       "      For example, (b1000) would transfer 1000 bytes via bulk in both directions\n"
	       "      Similarly, (i100 b1000) would use IOV to send and bulk to reply\n"
	       "      Only reasonable combinations are permitted (i.e. e1000 is not allowed)\n"
	       "      If no type specifier is specified, one will be chosen automatically. The simple\n"
	       "        heuristic is that bulk will be used if a specified size is >= %u\n"
	       "      BULK_GET will be used on the service side to 'send' data from client\n"
	       "        to service, and BULK_PUT will be used on the service side to 'reply'\n"
	       "        (assuming bulk transfers specified)\n"
	       "\n"
	       "      Note that different messages are sent internally via different structures.\n"
	       "      These are enumerated as follows, with x,y > 0:\n"
	       "        (0  0)  - Empty payload sent in both directions\n"
	       "        (ix 0)  - 8-byte session_id + x-byte iov sent, empty reply\n"
	       "        (0  iy) - 8-byte session_id sent, y-byte iov reply\n"
	       "        (ix iy) - 8-byte session_id + x-byte iov sent, y-byte iov reply\n"
	       "        (0  by) - 8-byte session_id + 8-byte bulk handle sent\n"
	       "                  y-byte BULK_PUT, empty reply\n"
	       "        (bx 0)  - 8-byte session_id + 8-byte bulk_handle sent\n"
	       "                  x-byte BULK_GET, empty reply\n"
	       "        (ix by) - 8-byte session_id + x-byte iov + 8-byte bulk_handle sent\n"
	       "                  y-byte BULK_PUT, empty reply\n"
	       "        (bx iy) - 8-byte session_id + 8-byte bulk_handle sent\n"
	       "                  x-byte BULK_GET, y-byte iov reply\n"
	       "        (bx by) - 8-byte session_id + 8-byte bulk_handle sent\n"
	       "                  x-byte BULK_GET, y-byte BULK_PUT, empty reply\n"
	       "\n"
	       "      Note also that any message size other than (0 0) will use test sessions.\n"
	       "        A self-test session will be negotiated with the service before sending\n"
	       "        any traffic, and the session will be closed after testing this size completes.\n"
	       "        The time to create and tear down these sessions is NOT measured.\n"
	       "\n"
	       "      Default: \"%s\"\n"
	       "\n"
	       "  --master-endpoint <ranks:tags>\n"
	       "      Short version: -m\n"
	       "      Describes an endpoint (or range of endpoints) that will each run a\n"
	       "        1:many self-test against the list of endpoints given via the\n"
	       "        --endpoint argument.\n"
	       "\n"
	       "      Specifying multiple --master-endpoint ranks/tags sets up a many:many\n"
	       "        self-test - the first 'many' is the list of master endpoints, each\n"
	       "        which executes a separate concurrent test against the second\n"
	       "        'many' (the list of test endpoints)\n"
	       "\n"
	       "      The argument syntax for this option is identical to that for\n"
	       "        --endpoint. Also, like --endpoint, --master-endpoint can be\n"
	       "        specified multiple times\n"
	       "\n"
	       "      Unlike --endpoint, the list of master endpoints is sorted and\n"
	       "        any duplicate entries are removed automatically. This is because\n"
	       "        each instance of self-test can only manage one 1:many test at\n"
	       "        a time\n"
	       "\n"
	       "      If not specified, the default value is to use this command-line\n"
	       "        application itself to run a 1:many test against the test endpoints\n"
	       "\n"
	       "      This client application sends all of the self-test parameters to\n"
	       "        this master node and instructs it to run a self-test session against\n"
	       "        the other endpoints specified by the --endpoint argument\n"
	       "\n"
	       "      This allows self-test to be run between any arbitrary CART-enabled\n"
	       "        applications without having to make them self-test aware. These\n"
	       "        other applications can be busy doing something else entirely and\n"
	       "        self-test will have no impact on that workload beyond consuming\n"
	       "        additional network and compute resources\n"
	       "\n"
	       "  --repetitions-per-size <N>\n"
	       "      Short version: -r\n"
	       "      Number of samples per message size. RPCs for each particular size\n"
	       "      will be repeated this many times.\n"
	       "      Default: %d\n"
	       "\n"
	       "  --max-inflight-rpcs <N>\n"
	       "      Short version: -i\n"
	       "      Maximum number of RPCs allowed to be executing concurrently.\n"
	       "\n"
	       "      Note that at the beginning of each test run, a buffer of size send_size\n"
	       "        is allocated for each inflight RPC (total max_inflight * send_size).\n"
	       "        This could be a lot of memory. Also, if the reply uses bulk, the\n"
	       "        size increases to (max_inflight * max(send_size, reply_size))\n"
	       "\n"
	       "      Default: %d\n"
	       "\n"
	       "  --align <alignment>\n"
	       "      Short version: -a\n"
	       "\n"
	       "      Forces all test buffers to be aligned (or misaligned) as specified.\n"
	       "\n"
	       "      The argument specifies what the least-significant byte of all test buffer\n"
	       "        addresses should be forced to be. For example, if --align 0 is specified,\n"
	       "        all test buffer addresses will end in 0x00 (thus aligned to 256 bytes).\n"
	       "        To force misalignment, use something like --align 3. For 64-bit (8-byte)\n"
	       "        alignment, use something like --align 8 or --align 24 (0x08 and 0x18)\n"
	       "\n"
	       "      Alignment should be specified as a decimal value in the range [%d:%d]\n"
	       "\n"
	       "      If specified, buffers will be allocated with an extra 256 bytes of\n"
	       "        alignment padding and the buffer to transfer will start at the point which\n"
	       "        the least - significant byte of the address matches the requested alignment.\n"
	       "\n"
	       "      Default is no alignment - whatever is returned by the allocator is used\n"
	       "\n"
	       "  --Mbits\n"
	       "      Short version: -b\n"
	       "      By default, self-test outputs performance results in MB (#Bytes/1024^2)\n"
	       "      Specifying --Mbits switches the output to megabits (#bits/1000000)\n",
	       prog_name, UINT32_MAX,
	       CRT_SELF_TEST_AUTO_BULK_THRESH, msg_sizes_str, rep_count,
	       max_inflight, CRT_ST_BUF_ALIGN_MIN, CRT_ST_BUF_ALIGN_MIN);
}

#define ST_ENDPT_RANK_IDX 0
#define ST_ENDPT_TAG_IDX 1
static int st_validate_range_str(const char *str)
{
	const char *start = str;

	while (*str != '\0') {
		if ((*str < '0' || *str > '9')
		    && (*str != '-') && (*str != ',')) {
			return -CER_INVAL;
		}

		str++;

		/* Make sure the range string isn't ridiculously large */
		if (str - start > SELF_TEST_MAX_LIST_STR_LEN)
			return -CER_INVAL;
	}
	return 0;
}

static void st_parse_range_str(char *const str, char *const validated_str,
			       uint32_t *num_elements)
{
	char *pch;
	char *pch_sub;
	char *saveptr_comma = NULL;
	char *saveptr_hyphen = NULL;
	char *validated_cur_ptr = validated_str;

	/* Split into tokens based on commas */
	pch = strtok_r(str, ",", &saveptr_comma);
	while (pch != NULL) {
		/* Number of valid hyphen-delimited values encountered so far */
		int		hyphen_count = 0;
		/* Start/stop values */
		uint32_t	val[2] = {0, 0};
		/* Flag indicating if values were filled, 0=no 1=yes */
		int		val_valid[2] = {0, 0};

		/* Number of characters left to write to before overflowing */
		int		num_avail = SELF_TEST_MAX_LIST_STR_LEN -
					(validated_cur_ptr - validated_str);
		int		num_written = 0;

		/*
		 * Split again on hyphens, using only the first two non-empty
		 * values
		 */
		pch_sub = strtok_r(pch, "-", &saveptr_hyphen);
		while (pch_sub != NULL && hyphen_count < 2) {
			if (*pch_sub != '\0') {
				/*
				 * Seems like we have a valid number.
				 * If anything goes wrong, skip over this
				 * comma-separated range/value.
				 */
				if (sscanf(pch_sub, "%u", &val[hyphen_count])
				    != 1) {
					val_valid[0] = 0;
					val_valid[1] = 0;
					break;
				}

				val_valid[hyphen_count] = 1;

				hyphen_count++;
			}

			pch_sub = strtok_r(NULL, "-", &saveptr_hyphen);
		}

		if (val_valid[0] == 1 && val_valid[1] == 1) {
			/* This was a valid range */
			uint32_t min = val[0] < val[1] ? val[0] : val[1];
			uint32_t max = val[0] > val[1] ? val[0] : val[1];

			*num_elements += max - min + 1;
			num_written = snprintf(validated_cur_ptr, num_avail,
					       "%u-%u,", min, max);
		} else if (val_valid[0] == 1) {
			/* Only one valid value */
			*num_elements += 1;
			num_written = snprintf(validated_cur_ptr, num_avail,
					       "%u,", val[0]);
		}

		/*
		 * It should not be possible to provide input that gets larger
		 * after sanition
		 */
		C_ASSERT(num_written <= num_avail);

		validated_cur_ptr += num_written;

		pch = strtok_r(NULL, ",", &saveptr_comma);
	}

	/* Trim off the trailing , */
	if (validated_cur_ptr > validated_str)
		*(validated_cur_ptr - 1) = '\0';
}

int parse_endpoint_string(char *const optarg, struct st_endpoint **const endpts,
			  uint32_t *const num_endpts)
{
	char			*token_ptrs[2] = {NULL, NULL};
	int			 separator_count = 0;
	int			 ret = 0;
	char			*pch = NULL;
	char			*rank_valid_str = NULL;
	uint32_t		 num_ranks = 0;
	char			*tag_valid_str = NULL;
	uint32_t		 num_tags = 0;
	void			*realloced_mem;
	struct st_endpoint	*next_endpoint;

	/*
	 * strtok replaces separators with \0 characters
	 * Use this to divide up the input argument into three strings
	 *
	 * Use the first three ; delimited strings - ignore the rest
	 */
	pch = strtok(optarg, ":");
	while (pch != NULL && separator_count < 2) {
		token_ptrs[separator_count] = pch;

		separator_count++;

		pch = strtok(NULL, ":");
	}

	/* Validate the input strings */
	if (token_ptrs[ST_ENDPT_RANK_IDX] == NULL
	    || token_ptrs[ST_ENDPT_TAG_IDX] == NULL
	    || *token_ptrs[ST_ENDPT_RANK_IDX] == '\0'
	    || *token_ptrs[ST_ENDPT_TAG_IDX] == '\0') {
		printf("endpoint must contain non-empty rank:tag\n");
		return -CER_INVAL;
	}
	/* Both group and tag can only contain [0-9\-,] */
	if (st_validate_range_str(token_ptrs[ST_ENDPT_RANK_IDX]) != 0) {
		printf("endpoint rank contains invalid characters\n");
		return -CER_INVAL;
	}
	if (st_validate_range_str(token_ptrs[ST_ENDPT_TAG_IDX]) != 0) {
		printf("endpoint tag contains invalid characters\n");
		return -CER_INVAL;
	}

	/*
	 * Now that strings have been sanity checked, allocate some space for a
	 * fully-validated copy of the rank and tag list. This works as follows:
	 * - The input string is tokenized and parsed
	 * - Each value (or range) is checked for validity
	 * - If valid, that range is written to the _valid_ string and the
	 *   number of elements in that range is added to the counter
	 *
	 * After both ranks and tags have been validated, the endpoint array can
	 * be resized to accomodate the new entries and the validated string
	 * can be re-parsed (without error checking) to add elements to the
	 * array.
	 */
	C_ALLOC(rank_valid_str, SELF_TEST_MAX_LIST_STR_LEN);
	if (rank_valid_str == NULL)
		C_GOTO(cleanup, ret = -CER_NOMEM);

	C_ALLOC(tag_valid_str, SELF_TEST_MAX_LIST_STR_LEN);
	if (tag_valid_str == NULL)
		C_GOTO(cleanup, ret = -CER_NOMEM);

	st_parse_range_str(token_ptrs[ST_ENDPT_RANK_IDX], rank_valid_str,
			   &num_ranks);

	st_parse_range_str(token_ptrs[ST_ENDPT_TAG_IDX], tag_valid_str,
			   &num_tags);

	/* Validate num_ranks and num_tags */
	if ((((uint64_t)num_ranks * (uint64_t)num_tags)
	     > (uint64_t)SELF_TEST_MAX_NUM_ENDPOINTS) ||
	    (((uint64_t)*num_endpts + (uint64_t)num_ranks * (uint64_t)num_tags)
	     > (uint64_t)SELF_TEST_MAX_NUM_ENDPOINTS)) {
		C_ERROR("Too many endpoints - current=%u, "
			"additional requested=%lu, max=%u\n",
			*num_endpts,
			(uint64_t)num_ranks * (uint64_t)num_tags,
			SELF_TEST_MAX_NUM_ENDPOINTS);
		C_GOTO(cleanup, ret = -CER_INVAL);
	}

	printf("Adding endpoints:\n");
	printf("  ranks: %s (# ranks = %u)\n", rank_valid_str, num_ranks);
	printf("  tags: %s (# tags = %u)\n", tag_valid_str, num_tags);

	/* Reallocate/expand the endpoints array */
	*num_endpts += num_ranks * num_tags;
	realloced_mem = C_REALLOC(*endpts,
				  sizeof(struct st_endpoint) * (*num_endpts));
	if (realloced_mem == NULL)
		C_GOTO(cleanup, ret = -CER_NOMEM);
	*endpts = (struct st_endpoint *)realloced_mem;

	/* Populate the newly expanded values in the endpoints array */
	next_endpoint = *endpts + *num_endpts - (num_ranks * num_tags);

	/*
	 * This block uses simpler tokenization logic (not strtok) because it
	 * has already been pre-validated
	 */

	/* Iterate over all rank tokens */
	pch = rank_valid_str;
	while (*pch != '\0') {
		uint32_t	rank;
		uint32_t	rank_max;
		int		num_scanned;

		num_scanned = sscanf(pch, "%u-%u", &rank, &rank_max);
		C_ASSERT(num_scanned == 1 || num_scanned == 2);
		if (num_scanned == 1)
			rank_max = rank;

		/* For this rank token, iterate over its range */
		do {
			uint32_t	 tag;
			uint32_t	 tag_max;
			char		*pch_tag;

			pch_tag = tag_valid_str;

			/*
			 * For this particular rank, iterate over all tag tokens
			 */
			while (*pch_tag != '\0') {
				num_scanned = sscanf(pch_tag, "%u-%u", &tag,
						     &tag_max);
				C_ASSERT(num_scanned == 1 || num_scanned == 2);
				if (num_scanned == 1)
					tag_max = tag;

				/*
				 * For this rank and chosen tag token, iterate
				 * over the range of tags in this tag token
				 */
				do {
					next_endpoint->rank = rank;
					next_endpoint->tag = tag;
					next_endpoint++;
					tag++;
				} while (tag <= tag_max);

				/* Advance the pointer just past the next , */
				do {
					pch_tag++;
				} while (*pch_tag != '\0' && *pch_tag != ',');
				if (*pch_tag == ',')
					pch_tag++;
			}

			rank++;
		} while (rank <= rank_max);

		/* Advance the pointer just past the next , */
		do {
			pch++;
		} while (*pch != '\0' && *pch != ',');
		if (*pch == ',')
			pch++;
	}

	/* Make sure all the allocated space got filled with real endpoints */
	C_ASSERT(next_endpoint == *endpts + *num_endpts);

	ret = 0;

cleanup:
	if (rank_valid_str != NULL)
		C_FREE(rank_valid_str, SELF_TEST_MAX_LIST_STR_LEN);
	if (tag_valid_str != NULL)
		C_FREE(tag_valid_str, SELF_TEST_MAX_LIST_STR_LEN);

	return ret;

}

/**
 * Parse a message size tuple from the user. The input format for this is
 * described in the usage text - basically one or two unsigned integer sizes,
 * each optionally prefixed by a character that specifies what underlying IO
 * type should be used to transfer a payload of that size (empty, iov, bulk).
 *
 * \return	0 on successfully filling *test_params, nonzero otherwise
 */
int parse_message_sizes_string(const char *pch,
			       struct st_size_params *test_params)
{
	/*
	 * Note whether a type is specified or not. If no type is
	 * specified by the user, it will be automatically selected
	 */
	int send_type_specified = 0;
	int reply_type_specified = 0;

	/*
	 * A simple map between identifier ('e') and type (...EMPTY)
	 *
	 * Note that BULK_PUT (for send) or BULK_GET (for reply) are
	 * not yet implemented. For this reason, only 'b' for bulk is
	 * accepted as a type here, which will automatically choose
	 * PUT or GET depending on the direction.
	 *
	 * If send/PUT or reply/GET are ever implemented, the map can be
	 * easily changed to support this.
	 */
	const struct {
		char identifier;
		enum crt_st_msg_type type;
	} type_map[] = { {'e', CRT_SELF_TEST_MSG_TYPE_EMPTY},
			 {'i', CRT_SELF_TEST_MSG_TYPE_IOV},
			 {'b', CRT_SELF_TEST_MSG_TYPE_BULK_GET} };

	/* Number of types recognized */
	const int num_types = ARRAY_SIZE(type_map);

	int ret;

	/*
	 * Advance pch to the next numerical character in the token
	 * If along the way pch happens to be one of the type
	 * characters, note down that type and continue hunting for a
	 * number. In this way, only the last type specifier before the
	 * number is stored.
	 */
	while (*pch != '\0' && (*pch < '0' || *pch > '9')) {
		int i;

		for (i = 0; i < num_types; i++)
			if (*pch == type_map[i].identifier) {
				send_type_specified = 1;
				test_params->send_type = type_map[i].type;
			}
		pch++;
	}
	if (*pch == '\0')
		return -1;

	/* Read the first size */
	ret = sscanf(pch, "%u", &test_params->send_size);
	if (ret != 1)
		return -1;

	/* Advance pch to the next non-numeric character */
	while (*pch != '\0' && *pch >= '0' && *pch <= '9')
		pch++;

	/*
	 * Advance pch to the next numerical character in the token
	 * If along the way pch happens to be one of the type
	 * characters, note down that type and continue hunting for a
	 * number. In this way, only the last type specifier before the
	 * number is stored.
	 */
	while (*pch != '\0' && (*pch < '0' || *pch > '9')) {
		int i;

		for (i = 0; i < num_types; i++)
			if (*pch == type_map[i].identifier) {
				reply_type_specified = 1;
				test_params->reply_type = type_map[i].type;
			}
		pch++;
	}
	if (*pch != '\0') {
		/* Read the second size */
		ret = sscanf(pch, "%u", &test_params->reply_size);
		if (ret != 1)
			return -1;
	} else {
		/* Only one numerical value - that's perfectly valid */
		test_params->reply_size = test_params->send_size;
		test_params->reply_type = test_params->send_type;
		reply_type_specified = send_type_specified;
	}

	/* If we got here, the send_size and reply_size are valid */

	/***** Automatically assign types if they were not specified *****/
	if (send_type_specified == 0) {
		if (test_params->send_size == 0)
			test_params->send_type = CRT_SELF_TEST_MSG_TYPE_EMPTY;
		else if (test_params->send_size <
			  CRT_SELF_TEST_AUTO_BULK_THRESH)
			test_params->send_type = CRT_SELF_TEST_MSG_TYPE_IOV;
		else
			test_params->send_type =
				CRT_SELF_TEST_MSG_TYPE_BULK_GET;
	}
	if (reply_type_specified == 0) {
		if (test_params->reply_size == 0)
			test_params->reply_type = CRT_SELF_TEST_MSG_TYPE_EMPTY;
		else if (test_params->reply_size <
			  CRT_SELF_TEST_AUTO_BULK_THRESH)
			test_params->reply_type = CRT_SELF_TEST_MSG_TYPE_IOV;
		else
			test_params->reply_type =
				CRT_SELF_TEST_MSG_TYPE_BULK_PUT;
	}

	/***** Silently / automatically correct invalid types *****/
	/* Empty messages always have empty type */
	if (test_params->send_size == 0)
		test_params->send_type = CRT_SELF_TEST_MSG_TYPE_EMPTY;
	if (test_params->reply_size == 0)
		test_params->reply_type = CRT_SELF_TEST_MSG_TYPE_EMPTY;

	/* All other empty requests with nonzero payload convert to iov */
	if (test_params->send_size != 0 &&
	    test_params->send_type == CRT_SELF_TEST_MSG_TYPE_EMPTY)
		test_params->send_type = CRT_SELF_TEST_MSG_TYPE_IOV;
	if (test_params->reply_size != 0 &&
	    test_params->reply_type == CRT_SELF_TEST_MSG_TYPE_EMPTY)
		test_params->reply_type = CRT_SELF_TEST_MSG_TYPE_IOV;

	/* Bulk requests convert to the type allowed by send/reply */
	if (test_params->send_type == CRT_SELF_TEST_MSG_TYPE_BULK_PUT)
		test_params->send_type = CRT_SELF_TEST_MSG_TYPE_BULK_GET;
	if (test_params->reply_type == CRT_SELF_TEST_MSG_TYPE_BULK_GET)
		test_params->reply_type = CRT_SELF_TEST_MSG_TYPE_BULK_PUT;

	return 0;
}

int main(int argc, char *argv[])
{
	/* Default parameters */
	char				 default_msg_sizes_str[] =
		 "b200000,b200000 0,0 b200000,b200000 i1000,i1000 b200000,"
		 "i1000,i1000 0,0 i1000,0";
	const int			 default_rep_count = 10000;
	const int			 default_max_inflight = 1000;

	char				*dest_name = NULL;
	const char			 tuple_tokens[] = "(),";
	char				*msg_sizes_str = default_msg_sizes_str;
	int				 rep_count = default_rep_count;
	int				 max_inflight = default_max_inflight;
	struct st_size_params		*all_params = NULL;
	char				*sizes_ptr = NULL;
	char				*pch = NULL;
	int				 num_msg_sizes;
	int				 num_tokens;
	int				 c;
	int				 j;
	int				 ret = 0;
	struct st_endpoint		*endpts = NULL;
	struct st_endpoint		*ms_endpts = NULL;
	uint32_t			 num_endpts = 0;
	uint32_t			 num_ms_endpts = 0;
	int				 output_megabits = 0;
	int16_t				 buf_alignment =
		CRT_ST_BUF_ALIGN_DEFAULT;

	/********************* Parse user arguments *********************/
	while (1) {
		static struct option long_options[] = {
			{"group-name", required_argument, 0, 'g'},
			{"master-endpoint", required_argument, 0, 'm'},
			{"endpoint", required_argument, 0, 'e'},
			{"message-sizes", required_argument, 0, 's'},
			{"repetitions-per-size", required_argument, 0, 'r'},
			{"max-inflight-rpcs", required_argument, 0, 'i'},
			{"align", required_argument, 0, 'a'},
			{"Mbits", no_argument, 0, 'b'},
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "g:m:e:s:r:i:a:b",
				long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'g':
			dest_name = optarg;
			break;
		case 'm':
			parse_endpoint_string(optarg, &ms_endpts,
					      &num_ms_endpts);
			break;
		case 'e':
			parse_endpoint_string(optarg, &endpts, &num_endpts);
			break;
		case 's':
			msg_sizes_str = optarg;
			break;
		case 'r':
			ret = sscanf(optarg, "%d", &rep_count);
			if (ret != 1) {
				rep_count = default_rep_count;
				printf("Warning: Invalid repetitions-per-size\n"
				       "  Using default value %d instead\n",
				       rep_count);
			}
			break;
		case 'i':
			ret = sscanf(optarg, "%d", &max_inflight);
			if (ret != 1) {
				max_inflight = default_max_inflight;
				printf("Warning: Invalid max-inflight-rpcs\n"
				       "  Using default value %d instead\n",
				       max_inflight);
			}
			break;
		case 'a':
			ret = sscanf(optarg, "%" SCNd16, &buf_alignment);
			if (ret != 1 || buf_alignment < CRT_ST_BUF_ALIGN_MIN ||
			    buf_alignment > CRT_ST_BUF_ALIGN_MAX) {
				printf("Warning: Invalid align value %d;"
				       " Expected value in range [%d:%d]\n",
				       buf_alignment, CRT_ST_BUF_ALIGN_MIN,
				       CRT_ST_BUF_ALIGN_MAX);
				buf_alignment = CRT_ST_BUF_ALIGN_DEFAULT;
			}
			break;
		case 'b':
			output_megabits = 1;
			break;
		case '?':
		default:
			print_usage(argv[0], default_msg_sizes_str,
				    default_rep_count,
				    default_max_inflight);
			C_GOTO(cleanup, ret = -CER_INVAL);
		}
	}

	/******************** Parse message sizes argument ********************/

	/*
	 * Count the number of tuple tokens (',') in the user-specified string
	 * This gives an upper limit on the number of arguments the user passed
	 */
	num_tokens = 0;
	sizes_ptr = msg_sizes_str;
	while (1) {
		const char *token_ptr = tuple_tokens;

		/* Break upon reaching the end of the argument */
		if (*sizes_ptr == '\0')
			break;

		/*
		 * For each valid token, check if this character is that token
		 */
		while (1) {
			if (*token_ptr == '\0')
				break;
			if (*token_ptr == *sizes_ptr)
				num_tokens++;
			token_ptr++;
		}

		sizes_ptr++;
	}

	/* Allocate a large enough buffer to hold the message sizes list */
	C_ALLOC(all_params, (num_tokens + 1) * sizeof(all_params[0]));
	if (all_params == NULL)
		C_GOTO(cleanup, ret = -CER_NOMEM);

	/* Iterate over the user's message sizes and parse / validate them */
	num_msg_sizes = 0;
	pch = strtok(msg_sizes_str, tuple_tokens);
	while (pch != NULL) {
		C_ASSERTF(num_msg_sizes <= num_tokens, "Token counting err\n");

		ret = parse_message_sizes_string(pch,
						 &all_params[num_msg_sizes]);
		if (ret == 0)
			num_msg_sizes++;
		else
			printf("Warning: Invalid message sizes tuple\n"
			       "  Expected values in range [0:%u], got '%s'\n",
			       UINT32_MAX,
			       pch);

		pch = strtok(NULL, tuple_tokens);
	}

	if (num_msg_sizes <= 0) {
		printf("No valid message sizes given\n");
		C_GOTO(cleanup, ret = -CER_INVAL);
	}

	/* Shrink the buffer if some of the user's tokens weren't kept */
	if (num_msg_sizes < num_tokens + 1) {
		void *realloced_mem;

		/* This should always succeed since the buffer is shrinking.. */
		realloced_mem = C_REALLOC(all_params,
					  num_msg_sizes
					  * sizeof(all_params[0]));
		if (realloced_mem == NULL)
			C_GOTO(cleanup, ret = -CER_NOMEM);
		all_params = (struct st_size_params *)realloced_mem;
	}

	/******************** Validate arguments ********************/
	if (dest_name == NULL || crt_validate_grpid(dest_name) != 0) {
		printf("--group-name argument not specified or is invalid\n");
		C_GOTO(cleanup, ret = -CER_INVAL);
	}
	if (ms_endpts == NULL)
		printf("Warning: No --master-endpoint specified; using this"
		       " command line application as the master endpoint\n");
	if (endpts == NULL || num_endpts == 0) {
		printf("No endpoints specified\n");
		C_GOTO(cleanup, ret = -CER_INVAL);
	}
	if ((rep_count <= 0) || (rep_count > SELF_TEST_MAX_REPETITIONS)) {
		printf("Invalid --repetitions-per-size argument\n"
		       "  Expected value in range (0:%d], got %d\n",
		       SELF_TEST_MAX_REPETITIONS, rep_count);
		C_GOTO(cleanup, ret = -CER_INVAL);
	}
	if ((max_inflight <= 0) || (max_inflight > SELF_TEST_MAX_INFLIGHT)) {
		printf("Invalid --max-inflight-rpcs argument\n"
		       "  Expected value in range (0:%d], got %d\n",
		       SELF_TEST_MAX_INFLIGHT, max_inflight);
		C_GOTO(cleanup, ret = -CER_INVAL);
	}

	/*
	 * No reason to have max_inflight bigger than the total number of RPCs
	 * each session
	 */
	max_inflight = max_inflight > rep_count ? rep_count : max_inflight;

	/********************* Print out parameters *********************/
	printf("Self Test Parameters:\n"
	       "  Group name to test against: %s\n"
	       "  # endpoints:                %u\n"
	       "  Message sizes:              [", dest_name, num_endpts);
	for (j = 0; j < num_msg_sizes; j++) {
		if (j > 0)
			printf(", ");
		printf("(%d-%s %d-%s)", all_params[j].send_size,
		       crt_st_msg_type_str[all_params[j].send_type],
		       all_params[j].reply_size,
		       crt_st_msg_type_str[all_params[j].reply_type]);
	}
	printf("]\n");
	if (buf_alignment == CRT_ST_BUF_ALIGN_DEFAULT)
		printf("  Buffer addresses end with:  <Default>\n");
	else
		printf("  Buffer addresses end with:  %d\n", buf_alignment);
	printf("  Repetitions per size:       %d\n"
	       "  Max inflight RPCs:          %d\n\n",
	       rep_count, max_inflight);

	/********************* Run the self test *********************/
	ret = run_self_test(all_params, num_msg_sizes, rep_count,
			    max_inflight, dest_name, ms_endpts,
			    num_ms_endpts, endpts, num_endpts,
			    output_megabits, buf_alignment);

	/********************* Clean up *********************/
cleanup:
	if (all_params != NULL)
		C_FREE(all_params,
		       num_msg_sizes * sizeof(all_params[0]));

	return ret;
}