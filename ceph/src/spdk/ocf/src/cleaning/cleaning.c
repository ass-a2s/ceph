/*
 * Copyright(c) 2012-2018 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "cleaning.h"
#include "alru.h"
#include "nop.h"
#include "acp.h"
#include "../ocf_priv.h"
#include "../ocf_cache_priv.h"
#include "../ocf_ctx_priv.h"
#include "../mngt/ocf_mngt_common.h"
#include "../metadata/metadata.h"
#include "../ocf_queue_priv.h"

struct cleaning_policy_ops cleaning_policy_ops[ocf_cleaning_max] = {
	[ocf_cleaning_nop] = {
		.name = "nop",
		.perform_cleaning = cleaning_nop_perform_cleaning,
	},
	[ocf_cleaning_alru] = {
		.setup = cleaning_policy_alru_setup,
		.init_cache_block = cleaning_policy_alru_init_cache_block,
		.purge_cache_block = cleaning_policy_alru_purge_cache_block,
		.purge_range = cleaning_policy_alru_purge_range,
		.set_hot_cache_line = cleaning_policy_alru_set_hot_cache_line,
		.initialize = cleaning_policy_alru_initialize,
		.deinitialize = cleaning_policy_alru_deinitialize,
		.set_cleaning_param = cleaning_policy_alru_set_cleaning_param,
		.get_cleaning_param = cleaning_policy_alru_get_cleaning_param,
		.perform_cleaning = cleaning_alru_perform_cleaning,
		.name = "alru",
	},
	[ocf_cleaning_acp] = {
		.setup = cleaning_policy_acp_setup,
		.init_cache_block = cleaning_policy_acp_init_cache_block,
		.purge_cache_block = cleaning_policy_acp_purge_block,
		.purge_range = cleaning_policy_acp_purge_range,
		.set_hot_cache_line = cleaning_policy_acp_set_hot_cache_line,
		.initialize = cleaning_policy_acp_initialize,
		.deinitialize = cleaning_policy_acp_deinitialize,
		.set_cleaning_param = cleaning_policy_acp_set_cleaning_param,
		.get_cleaning_param = cleaning_policy_acp_get_cleaning_param,
		.add_core = cleaning_policy_acp_add_core,
		.remove_core = cleaning_policy_acp_remove_core,
		.perform_cleaning = cleaning_policy_acp_perform_cleaning,
		.name = "acp",
	},
};

int ocf_start_cleaner(ocf_cache_t cache)
{
	return ctx_cleaner_init(cache->owner, &cache->cleaner);
}

void ocf_stop_cleaner(ocf_cache_t cache)
{
	ctx_cleaner_stop(cache->owner, &cache->cleaner);
}

void ocf_kick_cleaner(ocf_cache_t cache)
{
	ctx_cleaner_kick(cache->owner, &cache->cleaner);
}

void ocf_cleaner_set_cmpl(ocf_cleaner_t cleaner, ocf_cleaner_end_t fn)
{
	cleaner->end = fn;
}

void ocf_cleaner_set_priv(ocf_cleaner_t c, void *priv)
{
	OCF_CHECK_NULL(c);
	c->priv = priv;
}

void *ocf_cleaner_get_priv(ocf_cleaner_t c)
{
	OCF_CHECK_NULL(c);
	return c->priv;
}

ocf_cache_t ocf_cleaner_get_cache(ocf_cleaner_t c)
{
	OCF_CHECK_NULL(c);
	return container_of(c, struct ocf_cache, cleaner);
}

static int _ocf_cleaner_run_check_dirty_inactive(ocf_cache_t cache)
{
	int i;

	if (!env_bit_test(ocf_cache_state_incomplete, &cache->cache_state))
		return 0;

	for (i = 0; i < OCF_CORE_MAX; ++i) {
		if (!env_bit_test(i, cache->conf_meta->valid_core_bitmap))
			continue;

		if (cache->core[i].opened && env_atomic_read(&(cache->
				core_runtime_meta[i].dirty_clines))) {
			return 0;
		}
	}

	return 1;
}

static void ocf_cleaner_run_complete(ocf_cleaner_t cleaner, uint32_t interval)
{
	ocf_cache_t cache = ocf_cleaner_get_cache(cleaner);

	env_rwsem_up_write(&cache->lock);
	cleaner->end(cleaner, interval);

	ocf_queue_put(cleaner->io_queue);
}

void ocf_cleaner_run(ocf_cleaner_t cleaner, ocf_queue_t queue)
{
	ocf_cache_t cache;
	ocf_cleaning_t clean_type;

	OCF_CHECK_NULL(cleaner);
	OCF_CHECK_NULL(queue);

	cache = ocf_cleaner_get_cache(cleaner);

	/* Do not involve cleaning when cache is not running
	 * (error, etc.).
	 */
	if (!env_bit_test(ocf_cache_state_running, &cache->cache_state) ||
			ocf_mngt_is_cache_locked(cache)) {
		cleaner->end(cleaner, SLEEP_TIME_MS);
		return;
	}

	/* Sleep in case there is management operation in progress. */
	if (env_rwsem_down_write_trylock(&cache->lock)) {
		cleaner->end(cleaner, SLEEP_TIME_MS);
		return;
	}

	if (_ocf_cleaner_run_check_dirty_inactive(cache)) {
		env_rwsem_up_write(&cache->lock);
		cleaner->end(cleaner, SLEEP_TIME_MS);
		return;
	}

	clean_type = cache->conf_meta->cleaning_policy_type;

	ENV_BUG_ON(clean_type >= ocf_cleaning_max);

	ocf_queue_get(queue);
	cleaner->io_queue = queue;

	cleaning_policy_ops[clean_type].perform_cleaning(cache,
			ocf_cleaner_run_complete);
}
