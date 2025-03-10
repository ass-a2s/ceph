/*
 * Copyright(c) 2012-2018 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "cleaning.h"
#include "alru.h"
#include "../metadata/metadata.h"
#include "../utils/utils_cleaner.h"
#include "../utils/utils_part.h"
#include "../utils/utils_allocator.h"
#include "../concurrency/ocf_cache_concurrency.h"
#include "../ocf_def_priv.h"
#include "cleaning_priv.h"

#define is_alru_head(x) (x == collision_table_entries)
#define is_alru_tail(x) (x == collision_table_entries)

#define OCF_CLEANING_DEBUG 0

#if 1 == OCF_CLEANING_DEBUG

#define OCF_DEBUG_PREFIX "[Clean] %s():%d "

#define OCF_DEBUG_LOG(cache, format, ...) \
	ocf_cache_log_prefix(cache, log_info, OCF_DEBUG_PREFIX, \
			format"\n", __func__, __LINE__, ##__VA_ARGS__)

#define OCF_DEBUG_TRACE(cache) OCF_DEBUG_LOG(cache, "")

#define OCF_DEBUG_MSG(cache, msg) OCF_DEBUG_LOG(cache, "- %s", msg)

#define OCF_DEBUG_PARAM(cache, format, ...) OCF_DEBUG_LOG(cache, "- "format, \
			##__VA_ARGS__)

#else
#define OCF_DEBUG_PREFIX
#define OCF_DEBUG_LOG(cache, format, ...)
#define OCF_DEBUG_TRACE(cache)
#define OCF_DEBUG_MSG(cache, msg)
#define OCF_DEBUG_PARAM(cache, format, ...)
#endif

struct flush_merge_struct {
	ocf_cache_line_t cache_line;
	ocf_core_id_t core_id;
	uint64_t core_sector;
};

struct alru_flush_ctx {
	struct ocf_cleaner_attribs attribs;
	bool flush_perfomed;
	uint32_t clines_no;
	ocf_cache_t cache;
	ocf_cleaner_end_t cmpl;
	struct flush_data *flush_data;
	size_t flush_data_limit;
};

/* -- Start of ALRU functions -- */


/* Sets the given collision_index as the new _head_ of the ALRU list. */
static inline void update_alru_head(struct ocf_cache *cache,
		int partition_id, unsigned int collision_index)
{
	struct ocf_user_part *part = &cache->user_parts[partition_id];

	part->runtime->cleaning.policy.alru.lru_head = collision_index;
}

/* Sets the given collision_index as the new _tail_ of the ALRU list. */
static inline void update_alru_tail(struct ocf_cache *cache,
		int partition_id, unsigned int collision_index)
{
	struct ocf_user_part *part = &cache->user_parts[partition_id];

	part->runtime->cleaning.policy.alru.lru_tail = collision_index;
}

/* Sets the given collision_index as the new _head_ and _tail_
 * of the ALRU list.
 */
static inline void update_alru_head_tail(struct ocf_cache *cache,
		int partition_id, unsigned int collision_index)
{
	update_alru_head(cache, partition_id, collision_index);
	update_alru_tail(cache, partition_id, collision_index);
}


/* Adds the given collision_index to the _head_ of the ALRU list */
static void add_alru_head(struct ocf_cache *cache, int partition_id,
		unsigned int collision_index)
{
	unsigned int curr_head_index;
	unsigned int collision_table_entries = cache->device->collision_table_entries;
	struct ocf_user_part *part = &cache->user_parts[partition_id];
	struct cleaning_policy_meta policy;

	ENV_BUG_ON(!(collision_index < collision_table_entries));

	ENV_BUG_ON(env_atomic_read(
			&part->runtime->cleaning.policy.alru.size) < 0);

	ENV_WARN_ON(!metadata_test_dirty(cache, collision_index));
	ENV_WARN_ON(!metadata_test_valid_any(cache, collision_index));

	/* First node to be added/ */
	if (env_atomic_read(&part->runtime->cleaning.policy.alru.size) == 0) {
		update_alru_head_tail(cache, partition_id, collision_index);

		ocf_metadata_get_cleaning_policy(cache, collision_index,
				&policy);
		policy.meta.alru.lru_next = collision_table_entries;
		policy.meta.alru.lru_prev = collision_table_entries;
		policy.meta.alru.timestamp = env_ticks_to_secs(
			env_get_tick_count());
		ocf_metadata_set_cleaning_policy(cache, collision_index,
						&policy);
	} else {
		/* Not the first node to be added. */

		curr_head_index = part->runtime->cleaning.policy.alru.lru_head;

		ENV_BUG_ON(!(curr_head_index < collision_table_entries));

		ocf_metadata_get_cleaning_policy(cache, collision_index,
				&policy);
		policy.meta.alru.lru_next = curr_head_index;
		policy.meta.alru.lru_prev = collision_table_entries;
		policy.meta.alru.timestamp = env_ticks_to_secs(
			env_get_tick_count());
		ocf_metadata_set_cleaning_policy(cache, collision_index,
				&policy);

		ocf_metadata_get_cleaning_policy(cache, curr_head_index,
				&policy);
		policy.meta.alru.lru_prev = collision_index;
		ocf_metadata_set_cleaning_policy(cache, curr_head_index,
				&policy);

		update_alru_head(cache, partition_id, collision_index);
	}

	env_atomic_inc(&part->runtime->cleaning.policy.alru.size);
}

/* Deletes the node with the given collision_index from the ALRU list */
static void remove_alru_list(struct ocf_cache *cache, int partition_id,
		unsigned int collision_index)
{
	uint32_t prev_lru_node, next_lru_node;
	uint32_t collision_table_entries = cache->device->collision_table_entries;
	struct ocf_user_part *part = &cache->user_parts[partition_id];
	struct alru_cleaning_policy *cleaning_policy =
			&part->runtime->cleaning.policy.alru;
	struct cleaning_policy_meta policy;

	ENV_BUG_ON(!(collision_index < collision_table_entries));

	if (env_atomic_read(&part->runtime->cleaning.policy.alru.size) == 0) {
		ocf_cache_log(cache, log_err, "ERROR: Attempt to remove item "
				"from empty ALRU Cleaning Policy queue!\n");
		ENV_BUG();
	}

	ocf_metadata_get_cleaning_policy(cache, collision_index, &policy);

	/* Set prev and next (even if non existent) */
	next_lru_node = policy.meta.alru.lru_next;
	prev_lru_node = policy.meta.alru.lru_prev;

	/* Check if entry is not part of the ALRU list */
	if ((next_lru_node == collision_table_entries) &&
			(prev_lru_node == collision_table_entries) &&
			(cleaning_policy->lru_head != collision_index) &&
			(cleaning_policy->lru_tail != collision_index)) {
		return;
	}

	/* Case 0: If we are head AND tail, there is only one node. So unlink
	 * node and set that there is no node left in the list.
	 */
	if (cleaning_policy->lru_head == collision_index &&
			cleaning_policy->lru_tail == collision_index) {
		policy.meta.alru.lru_next = collision_table_entries;
		policy.meta.alru.lru_prev = collision_table_entries;

		ocf_metadata_set_cleaning_policy(cache, collision_index,
				&policy);

		update_alru_head_tail(cache, partition_id,
				collision_table_entries);
	}

	/* Case 1: else if this collision_index is ALRU head, but not tail,
	 * update head and return
	 */
	else if ((cleaning_policy->lru_tail != collision_index) &&
			(cleaning_policy->lru_head == collision_index)) {
		struct cleaning_policy_meta next_policy;

		ENV_BUG_ON(!(next_lru_node < collision_table_entries));

		ocf_metadata_get_cleaning_policy(cache, next_lru_node,
				&next_policy);

		update_alru_head(cache, partition_id, next_lru_node);

		policy.meta.alru.lru_next = collision_table_entries;
		next_policy.meta.alru.lru_prev = collision_table_entries;

		ocf_metadata_set_cleaning_policy(cache, collision_index,
				&policy);
		ocf_metadata_set_cleaning_policy(cache, next_lru_node,
						&next_policy);
	}

	/* Case 2: else if this collision_index is ALRU tail, but not head,
	 * update tail and return
	 */
	else if ((cleaning_policy->lru_head != collision_index) &&
			(cleaning_policy->lru_tail == collision_index)) {
		struct cleaning_policy_meta prev_policy;

		ENV_BUG_ON(!(prev_lru_node < collision_table_entries));

		ocf_metadata_get_cleaning_policy(cache, prev_lru_node,
				&prev_policy);

		update_alru_tail(cache, partition_id, prev_lru_node);

		policy.meta.alru.lru_prev = collision_table_entries;
		prev_policy.meta.alru.lru_next = collision_table_entries;

		ocf_metadata_set_cleaning_policy(cache, collision_index,
				&policy);
		ocf_metadata_set_cleaning_policy(cache, prev_lru_node,
						&prev_policy);
	}

	/* Case 3: else this collision_index is a middle node. There is no
	 * change to the head and the tail pointers.
	 */
	else {
		struct cleaning_policy_meta next_policy;
		struct cleaning_policy_meta prev_policy;

		ENV_BUG_ON(!(next_lru_node < collision_table_entries));
		ENV_BUG_ON(!(prev_lru_node < collision_table_entries));

		ocf_metadata_get_cleaning_policy(cache, prev_lru_node,
				&prev_policy);
		ocf_metadata_get_cleaning_policy(cache, next_lru_node,
				&next_policy);

		/* Update prev and next nodes */
		prev_policy.meta.alru.lru_next = policy.meta.alru.lru_next;
		next_policy.meta.alru.lru_prev = policy.meta.alru.lru_prev;

		/* Update the given node */
		policy.meta.alru.lru_next = collision_table_entries;
		policy.meta.alru.lru_prev = collision_table_entries;

		ocf_metadata_set_cleaning_policy(cache, collision_index,
				&policy);
		ocf_metadata_set_cleaning_policy(cache, prev_lru_node,
				&prev_policy);
		ocf_metadata_set_cleaning_policy(cache, next_lru_node,
				&next_policy);
	}

	env_atomic_dec(&part->runtime->cleaning.policy.alru.size);
}

static bool is_on_alru_list(struct ocf_cache *cache, int partition_id,
		unsigned int collision_index)
{
	uint32_t prev_lru_node, next_lru_node;
	uint32_t collision_table_entries = cache->device->collision_table_entries;
	struct ocf_user_part *part = &cache->user_parts[partition_id];
	struct alru_cleaning_policy *cleaning_policy =
			&part->runtime->cleaning.policy.alru;
	struct cleaning_policy_meta policy;

	ENV_BUG_ON(!(collision_index < collision_table_entries));

	ocf_metadata_get_cleaning_policy(cache, collision_index, &policy);

	next_lru_node = policy.meta.alru.lru_next;
	prev_lru_node = policy.meta.alru.lru_prev;

	return cleaning_policy->lru_tail == collision_index ||
			cleaning_policy->lru_head == collision_index ||
			next_lru_node != collision_table_entries ||
			prev_lru_node != collision_table_entries;
}


/* -- End of ALRU functions -- */

void cleaning_policy_alru_init_cache_block(struct ocf_cache *cache,
		uint32_t cache_line)
{
	struct cleaning_policy_meta policy;

	ocf_metadata_get_cleaning_policy(cache, cache_line, &policy);

	policy.meta.alru.timestamp = 0;
	policy.meta.alru.lru_prev = cache->device->collision_table_entries;
	policy.meta.alru.lru_next = cache->device->collision_table_entries;

	ocf_metadata_set_cleaning_policy(cache, cache_line, &policy);
}

void cleaning_policy_alru_purge_cache_block(struct ocf_cache *cache,
		uint32_t cache_line)
{
	ocf_part_id_t part_id = ocf_metadata_get_partition_id(cache,
			cache_line);

	remove_alru_list(cache, part_id, cache_line);
}

static void __cleaning_policy_alru_purge_cache_block_any(
		struct ocf_cache *cache, uint32_t cache_line)
{
	ocf_part_id_t part_id = ocf_metadata_get_partition_id(cache,
			cache_line);

	if (is_on_alru_list(cache, part_id, cache_line))
		remove_alru_list(cache, part_id, cache_line);
}

int cleaning_policy_alru_purge_range(struct ocf_cache *cache, int core_id,
		uint64_t start_byte, uint64_t end_byte) {
	struct ocf_user_part *part;
	ocf_part_id_t part_id;
	int ret = 0;

	for_each_part(cache, part, part_id) {
		if (env_atomic_read(&part->runtime->cleaning.
				policy.alru.size) == 0)
			continue;

		ret |= ocf_metadata_actor(cache, part_id,
				core_id, start_byte, end_byte,
				__cleaning_policy_alru_purge_cache_block_any);
	}

	return ret;
}

void cleaning_policy_alru_set_hot_cache_line(struct ocf_cache *cache,
		uint32_t cache_line)
{
	ocf_part_id_t part_id = ocf_metadata_get_partition_id(cache,
			cache_line);
	struct ocf_user_part *part = &cache->user_parts[part_id];

	uint32_t prev_lru_node, next_lru_node;
	uint32_t collision_table_entries = cache->device->collision_table_entries;
	struct cleaning_policy_meta policy;

	ENV_WARN_ON(!metadata_test_dirty(cache, cache_line));
	ENV_WARN_ON(!metadata_test_valid_any(cache, cache_line));

	ocf_metadata_get_cleaning_policy(cache, cache_line, &policy);
	next_lru_node = policy.meta.alru.lru_next;
	prev_lru_node = policy.meta.alru.lru_prev;

	if ((next_lru_node != collision_table_entries) ||
			(prev_lru_node != collision_table_entries) ||
			((part->runtime->cleaning.policy.
				alru.lru_head == cache_line) &&
			(part->runtime->cleaning.policy.
				alru.lru_tail == cache_line)))
		remove_alru_list(cache, part_id, cache_line);

	add_alru_head(cache, part_id, cache_line);
}

static void _alru_rebuild(struct ocf_cache *cache)
{
	struct ocf_user_part *part;
	ocf_part_id_t part_id;
	ocf_core_id_t core_id;
	ocf_cache_line_t cline;
	uint32_t step = 0;

	for_each_part(cache, part, part_id) {
		/* ALRU initialization */
		env_atomic_set(&part->runtime->cleaning.policy.alru.size, 0);
		part->runtime->cleaning.policy.alru.lru_head =
				cache->device->collision_table_entries;
		part->runtime->cleaning.policy.alru.lru_tail =
				cache->device->collision_table_entries;
		cache->device->runtime_meta->cleaning_thread_access = 0;
	}

	for (cline = 0; cline < cache->device->collision_table_entries; cline++) {
		ocf_metadata_get_core_and_part_id(cache, cline, &core_id,
				NULL);

		OCF_COND_RESCHED_DEFAULT(step);

		if (core_id == OCF_CORE_MAX)
			continue;

		cleaning_policy_alru_init_cache_block(cache, cline);

		if (!metadata_test_dirty(cache, cline))
			continue;

		cleaning_policy_alru_set_hot_cache_line(cache, cline);
	}
}

static int cleaning_policy_alru_initialize_part(struct ocf_cache *cache,
		struct ocf_user_part *part, int init_metadata)
{
	if (init_metadata) {
		/* ALRU initialization */
		env_atomic_set(&part->runtime->cleaning.policy.alru.size, 0);
		part->runtime->cleaning.policy.alru.lru_head =
				cache->device->collision_table_entries;
		part->runtime->cleaning.policy.alru.lru_tail =
				cache->device->collision_table_entries;
	}

	cache->device->runtime_meta->cleaning_thread_access = 0;

	return 0;
}

void cleaning_policy_alru_setup(struct ocf_cache *cache)
{
	struct alru_cleaning_policy_config *config;

	config = (void *)&cache->conf_meta->cleaning[ocf_cleaning_alru].data;

	config->thread_wakeup_time = OCF_ALRU_DEFAULT_WAKE_UP;
	config->stale_buffer_time = OCF_ALRU_DEFAULT_STALENESS_TIME;
	config->flush_max_buffers = OCF_ALRU_DEFAULT_FLUSH_MAX_BUFFERS;
	config->activity_threshold = OCF_ALRU_DEFAULT_ACTIVITY_THRESHOLD;
}

int cleaning_policy_alru_initialize(ocf_cache_t cache, int init_metadata)
{
	struct ocf_user_part *part;
	ocf_part_id_t part_id;
	struct alru_flush_ctx *fctx;

	fctx = env_vzalloc(sizeof(*fctx));
	if (!fctx) {
		ocf_cache_log(cache, log_err, "alru ctx allocation error\n");
		return -OCF_ERR_NO_MEM;
	}

	cache->cleaner.cleaning_policy_context = fctx;

	for_each_part(cache, part, part_id) {
		cleaning_policy_alru_initialize_part(cache,
				part, init_metadata);
	}

	if (init_metadata)
		_alru_rebuild(cache);

	ocf_kick_cleaner(cache);

	return 0;
}

void cleaning_policy_alru_deinitialize(struct ocf_cache *cache)
{
	env_vfree(cache->cleaner.cleaning_policy_context);
	cache->cleaner.cleaning_policy_context = NULL;
}

int cleaning_policy_alru_set_cleaning_param(ocf_cache_t cache,
		uint32_t param_id, uint32_t param_value)
{
	struct alru_cleaning_policy_config *config;

	config = (void *)&cache->conf_meta->cleaning[ocf_cleaning_alru].data;

	switch (param_id) {
	case ocf_alru_wake_up_time:
		OCF_CLEANING_CHECK_PARAM(cache, param_value,
				OCF_ALRU_MIN_WAKE_UP,
				OCF_ALRU_MAX_WAKE_UP,
				"thread_wakeup_time");
		config->thread_wakeup_time = param_value;
		ocf_cache_log(cache, log_info, "Write-back flush thread "
			"wake-up time: %d\n", config->thread_wakeup_time);
		ocf_kick_cleaner(cache);
		break;
	case ocf_alru_stale_buffer_time:
		OCF_CLEANING_CHECK_PARAM(cache, param_value,
				OCF_ALRU_MIN_STALENESS_TIME,
				OCF_ALRU_MAX_STALENESS_TIME,
				"stale_buffer_time");
		config->stale_buffer_time = param_value;
		ocf_cache_log(cache, log_info, "Write-back flush thread "
			"staleness time: %d\n", config->stale_buffer_time);
		break;
	case ocf_alru_flush_max_buffers:
		OCF_CLEANING_CHECK_PARAM(cache, param_value,
				OCF_ALRU_MIN_FLUSH_MAX_BUFFERS,
				OCF_ALRU_MAX_FLUSH_MAX_BUFFERS,
				"flush_max_buffers");
		config->flush_max_buffers = param_value;
		ocf_cache_log(cache, log_info, "Write-back flush thread max "
				"buffers flushed per iteration: %d\n",
				config->flush_max_buffers);
		break;
	case ocf_alru_activity_threshold:
		OCF_CLEANING_CHECK_PARAM(cache, param_value,
				OCF_ALRU_MIN_ACTIVITY_THRESHOLD,
				OCF_ALRU_MAX_ACTIVITY_THRESHOLD,
				"activity_threshold");
		config->activity_threshold = param_value;
		ocf_cache_log(cache, log_info, "Write-back flush thread "
				"activity time threshold: %d\n",
				config->activity_threshold);
		break;
	default:
		return -OCF_ERR_INVAL;
	}

	return 0;
}

int cleaning_policy_alru_get_cleaning_param(ocf_cache_t cache,
		uint32_t param_id, uint32_t *param_value)
{
	struct alru_cleaning_policy_config *config;

	config = (void *)&cache->conf_meta->cleaning[ocf_cleaning_alru].data;

	switch (param_id) {
	case ocf_alru_wake_up_time:
		*param_value = config->thread_wakeup_time;
		break;
	case ocf_alru_stale_buffer_time:
		*param_value = config->stale_buffer_time;
		break;
	case ocf_alru_flush_max_buffers:
		*param_value = config->flush_max_buffers;
		break;
	case ocf_alru_activity_threshold:
		*param_value = config->activity_threshold;
		break;
	default:
		return -OCF_ERR_INVAL;
	}

	return 0;
}

static inline uint32_t compute_timestamp(
		const struct alru_cleaning_policy_config *config)
{
	unsigned long time;

	time = env_get_tick_count();
	time -= env_secs_to_ticks(config->stale_buffer_time);
	time = env_ticks_to_secs(time);

	return (uint32_t) time;
}

static int check_for_io_activity(struct ocf_cache *cache,
		struct alru_cleaning_policy_config *config)
{
	unsigned int now, last;

	now = env_ticks_to_msecs(env_get_tick_count());
	last = env_atomic_read(&cache->last_access_ms);

	if ((now - last) < config->activity_threshold)
		return 1;
	return 0;
}

static int cmp_ocf_user_parts(const void *p1, const void *p2) {
	const struct ocf_user_part *t1 = *(const struct ocf_user_part**)p1;
	const struct ocf_user_part *t2 = *(const struct ocf_user_part**)p2;

	if (t1->config->priority > t2->config->priority)
		return 1;
	else if (t1->config->priority < t2->config->priority)
		return -1;

	return 0;
}

static void swp_ocf_user_part(void *part1, void *part2, int size) {
	void *tmp = *(void **)part1;

	*(void **)part1 = *(void **) part2;
	*(void **)part2 = tmp;
}

static void get_parts_sorted(struct ocf_user_part **parts,
		struct ocf_cache *cache) {
	int i;

	for (i = 0; i < OCF_IO_CLASS_MAX; i++)
		parts[i] = &cache->user_parts[i];

	env_sort(parts, OCF_IO_CLASS_MAX, sizeof(struct ocf_user_part*),
			cmp_ocf_user_parts, swp_ocf_user_part);
}

static bool clean_later(ocf_cache_t cache, uint32_t *delta)
{
	struct alru_cleaning_policy_config *config;

	config = (void *)&cache->conf_meta->cleaning[ocf_cleaning_alru].data;

	*delta = env_ticks_to_secs(env_get_tick_count()) -
			cache->device->runtime_meta->cleaning_thread_access;
	if (*delta <= config->thread_wakeup_time)
		return true;

	return false;
}

static bool is_cleanup_possible(ocf_cache_t cache)
{
	struct alru_cleaning_policy_config *config;
	uint32_t delta;

	config = (void *)&cache->conf_meta->cleaning[ocf_cleaning_alru].data;

	if (check_for_io_activity(cache, config)) {
		OCF_DEBUG_PARAM(cache, "IO activity detected");
		return false;
	}

	if (clean_later(cache, &delta)) {
		OCF_DEBUG_PARAM(cache,
			"Cleaning policy configured to clean later "
			"delta=%u wake_up=%u", delta,
			config->thread_wakeup_time);
		return false;
	}

	//Cleaning policy configured to not clean anything
	if (config->flush_max_buffers == 0)
		return false;

	return true;
}

static void get_block_to_flush(struct flush_data* dst,
		ocf_cache_line_t cache_line, struct ocf_cache* cache)
{
	ocf_core_id_t core_id;
	uint64_t core_line;

	ocf_metadata_get_core_info(cache, cache_line,
			&core_id, &core_line);

	dst->cache_line = cache_line;
	dst->core_id = core_id;
	dst->core_line = core_line;
}

static bool more_blocks_to_flush(struct ocf_cache *cache,
		ocf_cache_line_t cache_line, uint32_t last_access)
{
	struct cleaning_policy_meta policy;

	if (cache_line >= cache->device->collision_table_entries)
		return false;

	ocf_metadata_get_cleaning_policy(cache, cache_line, &policy);

	if (policy.meta.alru.timestamp >= last_access)
		return false;

	return true;
}

static bool block_is_busy(struct ocf_cache *cache,
		ocf_cache_line_t cache_line)
{
	ocf_core_id_t core_id;
	uint64_t core_line;

	ocf_metadata_get_core_info(cache, cache_line,
			&core_id, &core_line);

	if (!cache->core[core_id].opened)
		return true;

	if (ocf_cache_line_is_used(cache, cache_line))
		return true;

	return false;
}

static int get_data_to_flush(struct alru_flush_ctx *fctx)
{
	ocf_cache_t cache = fctx->cache;
	struct alru_cleaning_policy_config *config;
	struct cleaning_policy_meta policy;
	ocf_cache_line_t cache_line;
	struct ocf_user_part *parts[OCF_IO_CLASS_MAX];
	uint32_t last_access;
	int to_flush = 0;
	int part_id = OCF_IO_CLASS_ID_MAX;

	config = (void *)&cache->conf_meta->cleaning[ocf_cleaning_alru].data;

	get_parts_sorted(parts, cache);

	while (part_id >= OCF_IO_CLASS_ID_MIN) {
		cache_line =
			parts[part_id]->runtime->cleaning.policy.alru.lru_tail;

		last_access = compute_timestamp(config);

		OCF_DEBUG_PARAM(cache, "Last access=%u, timestamp=%u rel=%d",
				last_access, policy.meta.alru.timestamp,
				policy.meta.alru.timestamp < last_access);

		while (more_blocks_to_flush(cache, cache_line, last_access)) {
			if (to_flush >= fctx->clines_no)
				goto end;

			if (!block_is_busy(cache, cache_line)) {
				get_block_to_flush(&fctx->flush_data[to_flush], cache_line,
						cache);
				to_flush++;
			}

			ocf_metadata_get_cleaning_policy(cache, cache_line, &policy);
			cache_line = policy.meta.alru.lru_prev;
		}
		part_id--;
	}

end:
	OCF_DEBUG_PARAM(cache, "Collected items_to_clean=%u", to_flush);

	return to_flush;
}

static void alru_clean_complete(void *priv, int err)
{
	struct alru_cleaning_policy_config *config;
	struct alru_flush_ctx *fctx = priv;
	ocf_cache_t cache = fctx->cache;
	int interval;

	OCF_REALLOC_DEINIT(&fctx->flush_data, &fctx->flush_data_limit);

	config = (void *)&cache->conf_meta->cleaning[ocf_cleaning_alru].data;

	interval = fctx->flush_perfomed ? 0 : config->thread_wakeup_time * 1000;

	fctx->cmpl(&fctx->cache->cleaner, interval);
}

static void alru_clean(struct alru_flush_ctx *fctx)
{
	ocf_cache_t cache = fctx->cache;
	int to_clean;

	if (!is_cleanup_possible(cache)) {
		alru_clean_complete(fctx, 0);
		return;
	}

	if (OCF_METADATA_LOCK_WR_TRY()) {
		alru_clean_complete(fctx, 0);
		return;
	}

	OCF_REALLOC(&fctx->flush_data, sizeof(fctx->flush_data[0]),
			fctx->clines_no, &fctx->flush_data_limit);
	if (!fctx->flush_data) {
		ocf_cache_log(cache, log_warn, "No memory to allocate flush "
				"data for ALRU cleaning policy");
		goto end;
	}

	to_clean = get_data_to_flush(fctx);
	if (to_clean > 0) {
		fctx->flush_perfomed = true;
		ocf_cleaner_do_flush_data_async(cache, fctx->flush_data, to_clean,
				&fctx->attribs);
		OCF_METADATA_UNLOCK_WR();
		return;
	}

	/* Update timestamp only if there are no items to be cleaned */
	cache->device->runtime_meta->cleaning_thread_access =
		env_ticks_to_secs(env_get_tick_count());

end:
	OCF_METADATA_UNLOCK_WR();
	alru_clean_complete(fctx, 0);
}

void cleaning_alru_perform_cleaning(ocf_cache_t cache, ocf_cleaner_end_t cmpl)
{
	struct alru_flush_ctx *fctx = cache->cleaner.cleaning_policy_context;
	struct alru_cleaning_policy_config *config;

	config = (void *)&cache->conf_meta->cleaning[ocf_cleaning_alru].data;

	OCF_REALLOC_INIT(&fctx->flush_data, &fctx->flush_data_limit);

	fctx->attribs.cmpl_context = fctx;
	fctx->attribs.cmpl_fn = alru_clean_complete;
	fctx->attribs.cache_line_lock = true;
	fctx->attribs.do_sort = true;
	fctx->attribs.io_queue = cache->cleaner.io_queue;

	fctx->clines_no = config->flush_max_buffers;
	fctx->cache = cache;
	fctx->cmpl = cmpl;
	fctx->flush_perfomed = false;

	alru_clean(fctx);
}
