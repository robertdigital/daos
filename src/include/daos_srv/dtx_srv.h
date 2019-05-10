/**
 * (C) Copyright 2019 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

#ifndef __DAOS_DTX_SRV_H__
#define __DAOS_DTX_SRV_H__

#include <daos/mem.h>
#include <daos/dtx.h>
#include <daos/placement.h>
#include <daos_srv/vos_types.h>
#include <daos_srv/pool.h>
#include <daos_srv/container.h>

struct dtx_entry {
	/** The identifier of the DTX */
	struct dtx_id		dte_xid;
	/** The identifier of the modified object (shard). */
	daos_unit_oid_t		dte_oid;
};

enum dtx_cos_list_types {
	DCLT_UPDATE		= (1 << 0),
	DCLT_PUNCH		= (1 << 1),
};

/**
 * DAOS two-phase commit transaction handle in DRAM.
 */
struct dtx_handle {
	union {
		struct {
			/** The identifier of the DTX */
			struct dtx_id		dth_xid;
			/** The identifier of the shard to be modified. */
			daos_unit_oid_t		dth_oid;
		};
		struct dtx_entry		dth_dte;
	};
	/** The container handle */
	daos_handle_t			 dth_coh;
	/** The epoch# for the DTX. */
	daos_epoch_t			 dth_epoch;
	/** The {obj/dkey/akey}-tree records that are created
	 * by other DTXs, but not ready for commit yet.
	 */
	d_list_t			 dth_shares;
	/* The time when the DTX is handled on the server. */
	uint64_t			 dth_handled_time;
	/* The hash of the dkey to be modified if applicable */
	uint64_t			 dth_dkey_hash;
	/** Pool map version. */
	uint32_t			 dth_ver;
	/** The intent of related modification. */
	uint32_t			 dth_intent;
	uint32_t			 dth_sync:1, /* commit synchronously. */
					 dth_leader:1, /* leader replica. */
					 dth_non_rep:1, /* non-replicated. */
					 /* dti_cos has been committed. */
					 dth_dti_cos_done:1;
	/* The count the DTXs in the dth_dti_cos array. */
	uint32_t			 dth_dti_cos_count;
	/* The array of the DTXs for Commit on Share (conflcit). */
	struct dtx_id			*dth_dti_cos;
	/* The identifier of the DTX that conflict with current one. */
	struct dtx_conflict_entry	*dth_conflict;
	/* The data attached to the dth for dispatch */
	void				*dth_exec_arg;
	/** The address of the DTX entry in SCM. */
	umem_off_t			 dth_ent;
	/** The address (offset) of the (new) object to be modified. */
	umem_off_t			 dth_obj;
};

struct dtx_stat {
	uint64_t	dtx_committable_count;
	uint64_t	dtx_oldest_committable_time;
	uint64_t	dtx_committed_count;
	uint64_t	dtx_oldest_committed_time;
};

/**
 * DAOS two-phase commit transaction status.
 */
enum dtx_status {
	/**  Initialized, but local modification has not completed. */
	DTX_ST_INIT		= 1,
	/** Local participant has done the modification. */
	DTX_ST_PREPARED		= 2,
	/** The DTX has been committed. */
	DTX_ST_COMMITTED	= 3,
};

typedef void (*dtx_exec_shard_comp_cb_t)(void *cb_arg, int rc);
typedef int (*dtx_exec_shard_func_t)(struct dtx_handle *dth, void *arg, int idx,
				     dtx_exec_shard_comp_cb_t comp_cb,
				     void *cb_arg);
struct dtx_exec_arg {
	dtx_exec_shard_func_t	exec_func;
	void			*exec_func_arg;
	struct dtx_handle	*dth;
	ABT_future		future;
	uint32_t		shard_cnt;
	int			exec_result;
};

struct dtx_exec_shard_arg {
	struct daos_shard_tgt		*exec_shard_tgt;
	struct dtx_exec_arg		*exec_arg;
	struct dtx_conflict_entry	 exec_dce;
	int				 exec_shard_rc;
};

int dtx_resync(daos_handle_t po_hdl, uuid_t po_uuid, uuid_t co_uuid,
	       uint32_t ver, bool block);

int dtx_begin(struct dtx_id *dti, daos_unit_oid_t *oid, daos_handle_t coh,
	      daos_epoch_t epoch, uint64_t dkey_hash,
	      struct dtx_conflict_entry *conflict, struct dtx_id *dti_cos,
	      int dti_cos_count, uint32_t pm_ver, uint32_t intent, bool leader,
	      struct dtx_handle **dth);

int dtx_end(struct dtx_handle *dth, struct ds_cont_hdl *cont_hdl,
	    struct ds_cont_child *cont, int result);

int dtx_conflict(daos_handle_t coh, struct dtx_handle *dth, uuid_t po_uuid,
		 uuid_t co_uuid, struct dtx_conflict_entry *dces, int count,
		 uint32_t version);
int dtx_exec_ops(struct daos_shard_tgt *shard_tgts, int tgts_cnt,
		 struct dtx_handle *dth, dtx_exec_shard_func_t exec_func,
		 void *func_arg);

int dtx_batched_commit_register(struct ds_cont_hdl *hdl);

void dtx_batched_commit_deregister(struct ds_cont_hdl *hdl);

int dtx_handle_resend(daos_handle_t coh, daos_unit_oid_t *oid,
		      struct dtx_id *dti, uint64_t dkey_hash, bool punch);

/* XXX: The higher 48 bits of HLC is the wall clock, the lower bits are for
 *	logic clock that will be hidden when divided by NSEC_PER_SEC.
 */
static inline uint64_t
dtx_hlc_age2sec(uint64_t hlc)
{
	return (crt_hlc_get() - hlc) / NSEC_PER_SEC;
}

static inline bool
dtx_is_null(umem_off_t umoff)
{
	return umoff == UMOFF_NULL;
}

#endif /* __DAOS_DTX_SRV_H__ */