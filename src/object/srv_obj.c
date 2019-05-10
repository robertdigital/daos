/**
 * (C) Copyright 2016-2019 Intel Corporation.
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
/**
 * object server operations
 *
 * This file contains the server API methods and the RPC handlers that are both
 * related to object.
 */
#define D_LOGFAC	DD_FAC(object)

#include <uuid/uuid.h>

#include <abt.h>
#include <daos/rpc.h>
#include <daos_srv/pool.h>
#include <daos_srv/rebuild.h>
#include <daos_srv/container.h>
#include <daos_srv/vos.h>
#include <daos_srv/bio.h>
#include <daos_srv/daos_server.h>
#include <daos_srv/dtx_srv.h>
#include "obj_rpc.h"
#include "obj_internal.h"

/**
 * After bulk finish, let's send reply, then release the resource.
 */
static int
ds_obj_rw_complete(crt_rpc_t *rpc, struct ds_cont_hdl *cont_hdl,
		   daos_handle_t ioh, int status, struct dtx_handle *dth)
{
	struct obj_tls		*tls = obj_tls_get();
	struct obj_rw_in	*orwi = crt_req_get(rpc);
	int			 rc;

	D_TIME_START(tls->ot_sp, OBJ_PF_UPDATE_END);

	if (!daos_handle_is_inval(ioh)) {
		uint32_t map_version = cont_hdl->sch_pool->spc_map_version;
		bool update = (opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_UPDATE ||
			       opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_TGT_UPDATE);

		rc = update ? vos_update_end(ioh, map_version, &orwi->orw_dkey,
					     status, dth) :
			      vos_fetch_end(ioh, status);

		if (rc != 0) {
			D_ERROR(DF_UOID "%s end failed: %d\n",
				DP_UOID(orwi->orw_oid),
				update ? "Update" : "Fetch", rc);
			if (status == 0)
				status = rc;
		}
	}

	if (cont_hdl != NULL && cont_hdl->sch_cont != NULL) {
		struct obj_rw_out	*orwo = crt_reply_get(rpc);

		rc = vos_oi_get_attr(cont_hdl->sch_cont->sc_hdl, orwi->orw_oid,
				     orwi->orw_epoch, dth, &orwo->orw_attr);
		if (rc) {
			D_ERROR(DF_UOID" can not get status: rc %d\n",
				DP_UOID(orwi->orw_oid), rc);
			if (status == 0)
				status = rc;
		}
	}

	D_TIME_END(tls->ot_sp, OBJ_PF_UPDATE_END);

	return status;
}

static void
ds_obj_rw_reply(crt_rpc_t *rpc, int status, uint32_t map_version,
		struct dtx_conflict_entry *dce)
{
	struct obj_tls		*tls = obj_tls_get();
	int			 rc;

	D_TIME_START(tls->ot_sp, OBJ_PF_UPDATE_REPLY);

	obj_reply_set_status(rpc, status);
	obj_reply_map_version_set(rpc, map_version);
	if (opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_UPDATE ||
	    opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_TGT_UPDATE)
		obj_reply_dtx_conflict_set(rpc, dce);

	D_DEBUG(DB_TRACE, "rpc %p opc %d send reply, ver %d, status %d.\n",
		rpc, opc_get(rpc->cr_opc), map_version, status);

	rc = crt_reply_send(rpc);
	if (rc != 0)
		D_ERROR("send reply failed: %d\n", rc);

	if (opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_FETCH) {
		struct obj_rw_out	*orwo = crt_reply_get(rpc);

		if (orwo->orw_sizes.ca_arrays != NULL) {
			D_FREE(orwo->orw_sizes.ca_arrays);
			orwo->orw_sizes.ca_count = 0;
		}

		if (orwo->orw_nrs.ca_arrays != NULL) {
			D_FREE(orwo->orw_nrs.ca_arrays);
			orwo->orw_nrs.ca_count = 0;
		}
	}

	D_TIME_END(tls->ot_sp, OBJ_PF_UPDATE_REPLY);
}

struct ds_bulk_async_args {
	int		bulks_inflight;
	ABT_eventual	eventual;
	int		result;
};

static int
bulk_complete_cb(const struct crt_bulk_cb_info *cb_info)
{
	struct ds_bulk_async_args	*arg;
	struct crt_bulk_desc		*bulk_desc;
	crt_rpc_t			*rpc;
	crt_bulk_t			local_bulk_hdl;

	if (cb_info->bci_rc != 0)
		D_ERROR("bulk transfer failed: %d\n", cb_info->bci_rc);

	bulk_desc = cb_info->bci_bulk_desc;
	local_bulk_hdl = bulk_desc->bd_local_hdl;
	rpc = bulk_desc->bd_rpc;
	arg = (struct ds_bulk_async_args *)cb_info->bci_arg;
	/**
	 * Note: only one thread will access arg.result, so
	 * it should be safe here.
	 **/
	if (arg->result == 0)
		arg->result = cb_info->bci_rc;

	D_ASSERT(arg->bulks_inflight > 0);
	arg->bulks_inflight--;
	if (arg->bulks_inflight == 0)
		ABT_eventual_set(arg->eventual, &arg->result,
				 sizeof(arg->result));

	crt_bulk_free(local_bulk_hdl);
	crt_req_decref(rpc);
	return cb_info->bci_rc;
}

/**
 * Simulate bulk transfer by memcpy, all data are actually dropped.
 */
static void
bulk_bypass(daos_sg_list_t *sgl, crt_bulk_op_t bulk_op)
{
	static const int  dummy_buf_len = 4096;
	static char	 *dummy_buf;
	int		  i;

	if (!dummy_buf) {
		D_ALLOC(dummy_buf, dummy_buf_len);
		if (!dummy_buf)
			return; /* ignore error */
	}

	for (i = 0; i < sgl->sg_nr_out; i++) {
		char	*buf;
		int	 total, nob;

		if (sgl->sg_iovs[i].iov_buf == NULL ||
		    sgl->sg_iovs[i].iov_len == 0)
			continue;

		buf   = sgl->sg_iovs[i].iov_buf;
		total = sgl->sg_iovs[i].iov_len;
		while (total != 0) {
			nob = min(dummy_buf_len, total);
			if (bulk_op == CRT_BULK_PUT)
				memcpy(dummy_buf, buf, nob);
			else
				memcpy(buf, dummy_buf, nob);

			total -= nob;
			buf   += nob;
		}
	}
}

static int
ds_bulk_transfer(crt_rpc_t *rpc, crt_bulk_op_t bulk_op, bool bulk_bind,
		 crt_bulk_t *remote_bulks, daos_handle_t ioh,
		 daos_sg_list_t **sgls, int sgl_nr)
{
	struct ds_bulk_async_args arg = { 0 };
	crt_bulk_opid_t		bulk_opid;
	crt_bulk_perm_t		bulk_perm;
	int			i, rc, *status, ret;

	bulk_perm = bulk_op == CRT_BULK_PUT ? CRT_BULK_RO : CRT_BULK_RW;
	rc = ABT_eventual_create(sizeof(*status), &arg.eventual);
	if (rc != 0)
		return dss_abterr2der(rc);

	D_DEBUG(DB_IO, "bulk_op:%d sgl_nr%d\n", bulk_op, sgl_nr);

	for (i = 0; i < sgl_nr; i++) {
		daos_sg_list_t		*sgl, tmp_sgl;
		struct crt_bulk_desc	 bulk_desc;
		crt_bulk_t		 local_bulk_hdl;
		daos_size_t		 offset = 0;
		unsigned int		 idx = 0;

		if (remote_bulks[i] == NULL)
			continue;

		if (sgls != NULL) {
			sgl = sgls[i];
		} else {
			struct bio_sglist *bsgl;

			D_ASSERT(!daos_handle_is_inval(ioh));
			bsgl = vos_iod_sgl_at(ioh, i);
			D_ASSERT(bsgl != NULL);

			sgl = &tmp_sgl;
			rc = bio_sgl_convert(bsgl, sgl);
			if (rc)
				break;
		}

		if (daos_io_bypass & IOBP_SRV_BULK) {
			/* this mode will bypass network bulk transfer and
			 * only copy data from/to dummy buffer. This is for
			 * performance evaluation on low bandwidth network.
			 */
			bulk_bypass(sgl, bulk_op);
			goto next;
		}

		/**
		 * Let's walk through the sgl to check if the iov is empty,
		 * which is usually gotten from punched/empty records (see
		 * akey_fetch()), and skip these empty iov during bulk
		 * transfer to avoid touching the input buffer.
		 */
		while (idx < sgl->sg_nr_out) {
			daos_sg_list_t	sgl_sent;
			daos_size_t	length = 0;
			unsigned int	start;

			/**
			 * Skip the punched/empty record, let's also skip the
			 * record in the input buffer instead of memset to 0.
			 */
			while (sgl->sg_iovs[idx].iov_buf == NULL &&
			       idx < sgl->sg_nr_out) {
				offset += sgl->sg_iovs[idx].iov_len;
				idx++;
			}

			if (idx == sgl->sg_nr_out)
				break;

			start = idx;
			sgl_sent.sg_iovs = &sgl->sg_iovs[start];
			/* Find the end of the non-empty record */
			while (sgl->sg_iovs[idx].iov_buf != NULL &&
			       idx < sgl->sg_nr_out) {
				length += sgl->sg_iovs[idx].iov_len;
				idx++;
			}

			sgl_sent.sg_nr = idx - start;
			sgl_sent.sg_nr_out = idx - start;

			rc = crt_bulk_create(rpc->cr_ctx,
					     daos2crt_sg(&sgl_sent),
					     bulk_perm, &local_bulk_hdl);
			if (rc != 0) {
				D_ERROR("crt_bulk_create %d error (%d).\n",
					i, rc);
				break;
			}

			crt_req_addref(rpc);

			bulk_desc.bd_rpc	= rpc;
			bulk_desc.bd_bulk_op	= bulk_op;
			bulk_desc.bd_remote_hdl	= remote_bulks[i];
			bulk_desc.bd_local_hdl	= local_bulk_hdl;
			bulk_desc.bd_len	= length;
			bulk_desc.bd_remote_off	= offset;
			bulk_desc.bd_local_off	= 0;

			arg.bulks_inflight++;
			if (bulk_bind)
				rc = crt_bulk_bind_transfer(&bulk_desc,
					bulk_complete_cb, &arg, &bulk_opid);
			else
				rc = crt_bulk_transfer(&bulk_desc,
					bulk_complete_cb, &arg, &bulk_opid);
			if (rc < 0) {
				D_ERROR("crt_bulk_transfer %d error (%d).\n",
					i, rc);
				arg.bulks_inflight--;
				crt_bulk_free(local_bulk_hdl);
				crt_req_decref(rpc);
				break;
			}
			offset += length;
		}
next:
		if (sgls == NULL)
			daos_sgl_fini(sgl, false);
		if (rc)
			break;
	}

	if (arg.bulks_inflight == 0)
		ABT_eventual_set(arg.eventual, &rc, sizeof(rc));

	ret = ABT_eventual_wait(arg.eventual, (void **)&status);
	if (rc == 0)
		rc = ret ? dss_abterr2der(ret) : *status;

	ABT_eventual_free(&arg.eventual);
	return rc;
}

static int
ds_sgls_prep(daos_sg_list_t *dst_sgls, daos_sg_list_t *sgls, int number)
{
	int i;
	int j;
	int rc = 0;

	for (i = 0; i < number; i++) {
		dst_sgls[i].sg_nr = sgls[i].sg_nr;
		D_ALLOC_ARRAY(dst_sgls[i].sg_iovs, sgls[i].sg_nr);
		if (dst_sgls[i].sg_iovs == NULL)
			D_GOTO(out, rc = -DER_NOMEM);

		for (j = 0; j < dst_sgls[i].sg_nr; j++) {
			dst_sgls[i].sg_iovs[j].iov_buf_len =
				sgls[i].sg_iovs[j].iov_buf_len;

			D_ALLOC(dst_sgls[i].sg_iovs[j].iov_buf,
				dst_sgls[i].sg_iovs[j].iov_buf_len);
			if (dst_sgls[i].sg_iovs[j].iov_buf == NULL)
				D_GOTO(out, rc = -DER_NOMEM);
		}
	}
out:
	return rc;
}

static int
ds_obj_update_sizes_in_reply(crt_rpc_t *rpc)
{
	struct obj_rw_in	*orw = crt_req_get(rpc);
	struct obj_rw_out	*orwo = crt_reply_get(rpc);
	daos_iod_t		*iods;
	uint64_t		*sizes;
	int			size_count;
	int			i;

	D_ASSERT(opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_FETCH);

	D_ASSERT(orwo != NULL);
	D_ASSERT(orw != NULL);

	iods = orw->orw_iods.ca_arrays;
	size_count = orw->orw_iods.ca_count;

	orwo->orw_sizes.ca_count = size_count;
	D_ALLOC_ARRAY(sizes, size_count);
	if (sizes == NULL)
		return -DER_NOMEM;

	for (i = 0; i < orw->orw_iods.ca_count; i++)
		sizes[i] = iods[i].iod_size;

	orwo->orw_sizes.ca_arrays = sizes;
	return 0;
}

/**
 * Pack nrs in sgls inside the reply, so the client can update
 * sgls before it returns to application. Note: this is only
 * needed for bulk transfer, for inline transfer, it will pack
 * the complete sgls inside the req/reply, see obj_shard_rw().
 */
static int
ds_obj_update_nrs_in_reply(crt_rpc_t *rpc, daos_handle_t ioh,
			   daos_sg_list_t *sgls)
{
	struct obj_rw_in	*orw = crt_req_get(rpc);
	struct obj_rw_out	*orwo = crt_reply_get(rpc);
	uint32_t		*nrs;
	uint32_t		nrs_count = orw->orw_nr;
	int			i;

	if (nrs_count == 0)
		return 0;

	/* return num_out for sgl */
	orwo->orw_nrs.ca_count = nrs_count;
	D_ALLOC(orwo->orw_nrs.ca_arrays,
		nrs_count * sizeof(uint32_t));

	if (orwo->orw_nrs.ca_arrays == NULL)
		return -DER_NOMEM;

	nrs = orwo->orw_nrs.ca_arrays;
	for (i = 0; i < nrs_count; i++) {
		struct bio_sglist	*bsgl;
		daos_sg_list_t		*sgl;

		if (sgls != NULL) {
			sgl = &sgls[i];
			D_ASSERT(sgl != NULL);
			nrs[i] = sgl->sg_nr_out;
		} else {
			bsgl = vos_iod_sgl_at(ioh, i);
			D_ASSERT(bsgl != NULL);
			nrs[i] = bsgl->bs_nr_out;
		}
	}

	return 0;
}

/**
 * Lookup and return the container handle, if it is a rebuild handle, which
 * will never associate a particular container, then the contaier structure
 * will be returned to \a contp.
 */
static int
ds_check_container(uuid_t cont_hdl_uuid, uuid_t cont_uuid, int opc,
		   struct ds_cont_hdl **hdlp, struct ds_cont_child **contp)
{
	struct ds_cont_hdl	*cont_hdl;
	int			 rc;

	cont_hdl = ds_cont_hdl_lookup(cont_hdl_uuid);
	if (cont_hdl == NULL) {
		D_DEBUG(DB_TRACE, "can not find "DF_UUID" hdl\n",
			 DP_UUID(cont_hdl_uuid));
		D_GOTO(failed, rc = -DER_NO_HDL);
	}

	if ((opc == DAOS_OBJ_RPC_UPDATE  || opc == DAOS_OBJ_RPC_PUNCH ||
	     opc == DAOS_OBJ_RPC_TGT_UPDATE || opc == DAOS_OBJ_RPC_TGT_PUNCH ||
	     opc == DAOS_OBJ_RPC_TGT_PUNCH_DKEYS ||
	     opc == DAOS_OBJ_RPC_TGT_PUNCH_AKEYS) &&
	    !(cont_hdl->sch_capas & DAOS_COO_RW)) {
		D_ERROR("cont "DF_UUID" hdl "DF_UUID" sch_capas "DF_U64", "
			"NO_PERM to update.\n", DP_UUID(cont_uuid),
			DP_UUID(cont_hdl_uuid), cont_hdl->sch_capas);
		D_GOTO(out, rc = -DER_NO_PERM);
	}

	if (cont_hdl->sch_cont != NULL) { /* a regular container */
		ds_cont_child_get(cont_hdl->sch_cont);
		*contp = cont_hdl->sch_cont;
		D_GOTO(out, rc = 0);
	}

	if (!is_rebuild_container(cont_hdl->sch_pool->spc_uuid,
				  cont_hdl_uuid)) {
		D_ERROR("Empty container "DF_UUID" (ref=%d) handle?\n",
			DP_UUID(cont_uuid), cont_hdl->sch_ref);
		D_GOTO(failed, rc = -DER_NO_HDL);
	}

	/* rebuild handle is a dummy and never attached by a real container */
	if (DAOS_FAIL_CHECK(DAOS_REBUILD_NO_HDL))
		D_GOTO(failed, rc = -DER_NO_HDL);

	if (DAOS_FAIL_CHECK(DAOS_REBUILD_STALE_POOL))
		D_GOTO(failed, rc = -DER_STALE);

	D_DEBUG(DB_TRACE, DF_UUID"/%p is rebuild cont hdl\n",
		DP_UUID(cont_hdl_uuid), cont_hdl);

	/* load or create VOS container on demand */
	rc = ds_cont_child_lookup(cont_hdl->sch_pool->spc_uuid, cont_uuid,
				  contp);
	if (rc)
		D_GOTO(failed, rc);
out:
	*hdlp = cont_hdl;
failed:
	if (cont_hdl != NULL && rc != 0)
		ds_cont_hdl_put(cont_hdl);

	return rc;
}

void
ds_obj_rw_echo_handler(crt_rpc_t *rpc)
{
	struct obj_rw_in	*orw = crt_req_get(rpc);
	struct obj_rw_out	*orwo = crt_reply_get(rpc);
	struct obj_tls		*tls;
	daos_iod_t		*iod;
	daos_sg_list_t		*p_sgl;
	crt_bulk_op_t		bulk_op;
	bool			bulk_bind;
	int			i;
	int			rc = 0;

	D_DEBUG(DB_TRACE, "opc %d "DF_UOID" dkey %d %s tgt/xs %d/%d eph "
		DF_U64".\n", opc_get(rpc->cr_opc), DP_UOID(orw->orw_oid),
		(int)orw->orw_dkey.iov_len, (char *)orw->orw_dkey.iov_buf,
		dss_get_module_info()->dmi_tgt_id,
		dss_get_module_info()->dmi_xs_id,
		orw->orw_epoch);

	if (opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_FETCH) {
		rc = ds_obj_update_sizes_in_reply(rpc);
		if (rc)
			D_GOTO(out, rc);
	}

	/* Inline fetch/update */
	if (orw->orw_bulks.ca_arrays == NULL && orw->orw_bulks.ca_count == 0) {
		if (opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_FETCH) {
			orwo->orw_sgls.ca_count = orw->orw_sgls.ca_count;
			orwo->orw_sgls.ca_arrays = orw->orw_sgls.ca_arrays;
		}
		D_GOTO(out, rc);
	}

	/* Only support 1 iod now */
	D_ASSERT(orw->orw_iods.ca_count == 1);
	iod = orw->orw_iods.ca_arrays;

	tls = obj_tls_get();
	p_sgl = &tls->ot_echo_sgl;

	/* Let's check if tls already have enough buffer */
	if (p_sgl->sg_nr < iod->iod_nr) {
		daos_sgl_fini(p_sgl, true);
		rc = daos_sgl_init(p_sgl, iod->iod_nr);
		if (rc)
			D_GOTO(out, rc);

		p_sgl->sg_nr_out = p_sgl->sg_nr;
	}

	for (i = 0; i < iod->iod_nr; i++) {
		daos_size_t size = iod->iod_size;

		if (size == DAOS_REC_ANY)
			size = sizeof(uint64_t);

		if (iod->iod_type == DAOS_IOD_ARRAY) {
			D_ASSERT(iod->iod_recxs);
			size *= iod->iod_recxs[i].rx_nr;
		}

		/* Check each vector */
		if (p_sgl->sg_iovs[i].iov_buf_len < size) {
			if (p_sgl->sg_iovs[i].iov_buf != NULL)
				D_FREE(p_sgl->sg_iovs[i].iov_buf);

			D_ALLOC(p_sgl->sg_iovs[i].iov_buf, size);
			/* obj_tls_fini() will free these buffer */
			if (p_sgl->sg_iovs[i].iov_buf == NULL)
				D_GOTO(out, rc = -DER_NOMEM);
			p_sgl->sg_iovs[i].iov_buf_len = size;
			p_sgl->sg_iovs[i].iov_len = size;
		}
	}

	orwo->orw_sgls.ca_count = 0;
	orwo->orw_sgls.ca_arrays = NULL;
	if (opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_FETCH) {
		rc = ds_obj_update_nrs_in_reply(rpc, DAOS_HDL_INVAL, p_sgl);
		if (rc != 0)
			D_GOTO(out, rc);
		bulk_op = CRT_BULK_PUT;
	} else {
		bulk_op = CRT_BULK_GET;
	}

	bulk_bind = orw->orw_flags & ORF_BULK_BIND;
	rc = ds_bulk_transfer(rpc, bulk_op, bulk_bind, orw->orw_bulks.ca_arrays,
			      DAOS_HDL_INVAL, &p_sgl, orw->orw_nr);

out:
	orwo->orw_ret = rc;
	orwo->orw_map_version = orw->orw_map_ver;
}

static int
obj_local_rw(crt_rpc_t *rpc, struct ds_cont_hdl *cont_hdl,
	     struct ds_cont_child *cont, struct dtx_handle *dth)
{
	struct obj_rw_in	*orw = crt_req_get(rpc);
	struct obj_rw_out	*orwo = crt_reply_get(rpc);
	uint32_t		tag = dss_get_module_info()->dmi_tgt_id;
	daos_handle_t		ioh = DAOS_HDL_INVAL;
	struct bio_desc		*biod;
	crt_bulk_op_t		bulk_op;
	bool			rma;
	bool			bulk_bind;
	int			rc, err;

	if (daos_oc_echo_type(daos_obj_id2class(orw->orw_oid.id_pub)) ||
	    (daos_io_bypass & IOBP_TARGET)) {
		ds_obj_rw_echo_handler(rpc);
		D_GOTO(out, rc = 0);
	}

	D_DEBUG(DB_TRACE, "opc %d "DF_UOID" dkey %d %s tag %d eph "DF_U64".\n",
		opc_get(rpc->cr_opc), DP_UOID(orw->orw_oid),
		(int)orw->orw_dkey.iov_len, (char *)orw->orw_dkey.iov_buf,
		tag, orw->orw_epoch);
	rma = (orw->orw_bulks.ca_arrays != NULL ||
	       orw->orw_bulks.ca_count != 0);

	/* Prepare IO descriptor */
	if (opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_UPDATE ||
	    opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_TGT_UPDATE) {
		bulk_op = CRT_BULK_GET;
		rc = vos_update_begin(cont->sc_hdl, orw->orw_oid,
				      orw->orw_epoch, &orw->orw_dkey,
				      orw->orw_nr, orw->orw_iods.ca_arrays,
				      &ioh, dth);
		if (rc) {
			D_ERROR(DF_UOID" Update begin failed: %d\n",
				DP_UOID(orw->orw_oid), rc);
			goto out;
		}
	} else {
		bool size_fetch = (!rma && orw->orw_sgls.ca_arrays == NULL);

		bulk_op = CRT_BULK_PUT;
		rc = vos_fetch_begin(cont->sc_hdl, orw->orw_oid, orw->orw_epoch,
				     &orw->orw_dkey, orw->orw_nr,
				     orw->orw_iods.ca_arrays, size_fetch, &ioh);
		if (rc) {
			D_ERROR(DF_UOID" Fetch begin failed: %d\n",
				DP_UOID(orw->orw_oid), rc);
			goto out;
		}

		rc = ds_obj_update_sizes_in_reply(rpc);
		if (rc != 0)
			goto out;

		if (rma) {
			orwo->orw_sgls.ca_count = 0;
			orwo->orw_sgls.ca_arrays = NULL;

			rc = ds_obj_update_nrs_in_reply(rpc, ioh, NULL);
			if (rc != 0)
				goto out;
		} else {
			orwo->orw_sgls.ca_count = orw->orw_sgls.ca_count;
			orwo->orw_sgls.ca_arrays = orw->orw_sgls.ca_arrays;
		}
	}

	biod = vos_ioh2desc(ioh);
	rc = bio_iod_prep(biod);
	if (rc) {
		D_ERROR(DF_UOID" bio_iod_prep failed: %d.\n",
			DP_UOID(orw->orw_oid), rc);
		goto out;
	}

	if (rma) {
		bulk_bind = orw->orw_flags & ORF_BULK_BIND;
		rc = ds_bulk_transfer(rpc, bulk_op, bulk_bind,
			orw->orw_bulks.ca_arrays, ioh, NULL, orw->orw_nr);
	} else if (orw->orw_sgls.ca_arrays != NULL) {
		rc = bio_iod_copy(biod, orw->orw_sgls.ca_arrays, orw->orw_nr);
	}

	if (rc == -DER_OVERFLOW) {
		rc = -DER_REC2BIG;
		D_ERROR(DF_UOID" ds_bulk_transfer/bio_iod_copy failed, rc %d",
			DP_UOID(orw->orw_oid), rc);
	}

	err = bio_iod_post(biod);
	rc = rc ? : err;
out:
	rc = ds_obj_rw_complete(rpc, cont_hdl, ioh, rc, dth);
	return rc;
}

static int
ds_obj_check_dtx_config(uint32_t flags, struct dtx_id *dti)
{
	if (flags & ORF_FROM_LEADER) {
		if (((flags & ORF_DTX_DISABLED) == 0) != srv_enable_dtx) {
			D_ERROR("Inconsistent DTX configuration among "
				"servers.\n");
			return -DER_PROTO;
		}
	} else {
		if (daos_is_zero_dti(dti) && srv_enable_dtx) {
			D_ERROR("Inconsistent DTX configuration between "
				"client and server.\n");
			return -DER_PROTO;
		}
	}

	return 0;
}

/* Various check before access VOS */
static int
ds_pre_check(daos_unit_oid_t oid, uint32_t rpc_map_ver, uuid_t hdl_uuid,
	     uuid_t co_uuid, uint32_t opc, uint32_t flags, struct dtx_id *dti,
	     struct ds_cont_hdl **hdlp, struct ds_cont_child **contp)
{
	uint32_t	map_ver;
	int		rc;

	*hdlp = NULL;
	*contp = NULL;
	rc = ds_check_container(hdl_uuid, co_uuid, opc, hdlp, contp);
	if (rc)
		return rc;

	D_ASSERT((*hdlp)->sch_pool != NULL);
	map_ver = (*hdlp)->sch_pool->spc_map_version;

	if (rpc_map_ver < map_ver) {
		D_DEBUG(DB_IO, "stale version req %d map_version %d\n",
			rpc_map_ver, map_ver);
		/* XXX punch as well? */
		if (opc == DAOS_OBJ_RPC_UPDATE ||
		    opc == DAOS_OBJ_RPC_TGT_UPDATE)
			D_GOTO(out_put, rc = -DER_STALE);
		/* It is harmless if fetch with old pool map version. */
	} else if (rpc_map_ver > map_ver) {
		/* XXX: Client (or leader replica) has newer pool map than
		 *	current replica. Two possibile cases:
		 *
		 *	1. The current replica was the old leader if with
		 *	   the old pool map version. According to current
		 *	   leader election algorithm, it is still the new
		 *	   leader with the new pool map version. Since no
		 *	   leader switch, the unmatched pool version will
		 *	   not affect DTX related availability check.
		 *
		 *	2. The current replica was NOT the old leader if
		 *	   with the old pool map version. But it becomes
		 *	   the new leader with the new pool map version.
		 *	   In the subsequent modificaiton, it may hit
		 *	   some 'prepared' DTX when make availability
		 *	   check, it will return -DER_INPROGRESS that
		 *	   will cause client to retry. It is possible
		 *	   that the pool map version event arrives at
		 *	   this server during the client retry. It is
		 *	   inefficient, but harmless.
		 */
	}

	/* Check DTX configuration. */
	if ((flags != 0 || dti != NULL) &&
	     !daos_oc_echo_type(daos_obj_id2class(oid.id_pub))) {
		rc = ds_obj_check_dtx_config(flags, dti);
		if (rc != 0)
			D_GOTO(out_put, rc);
	}

out_put:
	if (rc) {
		if (*contp != NULL) {
			ds_cont_child_put(*contp);
			*contp = NULL;
		}
		if (*hdlp != NULL) {
			ds_cont_hdl_put(*hdlp);
			*hdlp = NULL;
		}
	}

	return rc;
}

void
ds_obj_tgt_update_handler(crt_rpc_t *rpc)
{
	struct obj_rw_in		*orw = crt_req_get(rpc);
	struct obj_rw_out		*orwo = crt_reply_get(rpc);
	struct ds_cont_hdl		*cont_hdl = NULL;
	struct ds_cont_child		*cont = NULL;
	struct dtx_handle		*dth = NULL;
	struct dtx_conflict_entry	 conflict = { 0 };
	uint32_t			 map_ver = 0;
	int				 rc;

	D_ASSERT(orw != NULL);
	D_ASSERT(orwo != NULL);

	rc = ds_pre_check(orw->orw_oid, orw->orw_map_ver, orw->orw_co_hdl,
			  orw->orw_co_uuid, opc_get(rpc->cr_opc),
			  orw->orw_flags, &orw->orw_dti,
			  &cont_hdl, &cont);
	if (rc)
		goto out;

	D_ASSERT(cont_hdl->sch_pool != NULL);
	map_ver = cont_hdl->sch_pool->spc_map_version;

	D_DEBUG(DB_TRACE, "rpc %p opc %d "DF_UOID" dkey %d %s tag/xs %d/%d eph "
		DF_U64", pool ver %u/%u with "DF_DTI".\n",
		rpc, opc_get(rpc->cr_opc), DP_UOID(orw->orw_oid),
		(int)orw->orw_dkey.iov_len, (char *)orw->orw_dkey.iov_buf,
		dss_get_module_info()->dmi_tgt_id,
		dss_get_module_info()->dmi_xs_id, orw->orw_epoch,
		orw->orw_map_ver, map_ver, DP_DTI(&orw->orw_dti));

	/* Handle resend. */
	if (orw->orw_flags & ORF_RESEND) {
		rc = dtx_handle_resend(cont->sc_hdl, &orw->orw_oid,
				       &orw->orw_dti,
				       orw->orw_dkey_hash, false);
		if (rc == -DER_ALREADY)
			D_GOTO(out, rc = 0);
	}

	/**
	 * XXX we should use different api for local dtx, will fix in the
	 * following patch.
	 **/
	rc = dtx_begin(&orw->orw_dti, &orw->orw_oid, cont->sc_hdl,
		       orw->orw_epoch, orw->orw_dkey_hash,
		       &conflict, orw->orw_dti_cos.ca_arrays,
		       orw->orw_dti_cos.ca_count, orw->orw_map_ver,
		       DAOS_INTENT_UPDATE, false, &dth);
	if (rc != 0) {
		D_ERROR(DF_UOID": Failed to start DTX for update %d.\n",
			DP_UOID(orw->orw_oid), rc);
		D_GOTO(out, rc);
	}

	rc = obj_local_rw(rpc, cont_hdl, cont, dth);
	if (rc != 0) {
		D_ERROR(DF_UOID": rw_local (update) failed %d.\n",
			DP_UOID(orw->orw_oid), rc);
		D_GOTO(out, rc);
	}

	/**
	 * XXX layer violation, it should be moved to DTX module, will
	 * handle in following patches.
	 */
	if (orw->orw_dti_cos.ca_count > 0) {
		/* XXX: For non-leader replica, even if we fail to
		 *	make related modification for some reason,
		 *	we still need to commit the DTXs for CoS.
		 *	Because other replica may have already
		 *	committed them. For leader case, it is
		 *	not important even if we miss to commit
		 *	the CoS DTXs, because they are still in
		 *	CoS cache, and can be committed next time.
		 */
		rc = vos_dtx_commit(cont->sc_hdl,
				    orw->orw_dti_cos.ca_arrays,
				    orw->orw_dti_cos.ca_count);
		if (rc != 0)
			D_ERROR(DF_UUID": Fail to DTX CoS commit: %d\n",
				DP_UUID(cont->sc_uuid), rc);
	}

out:
	rc = dtx_end(dth, cont_hdl, cont, rc);
	ds_obj_rw_reply(rpc, rc, map_ver, &conflict);

	if (cont_hdl)
		ds_cont_hdl_put(cont_hdl);
	if (cont)
		ds_cont_child_put(cont);
}

static int
ds_obj_tgt_update(struct dtx_handle *dth, void *arg, int idx,
		  dtx_exec_shard_comp_cb_t comp_cb, void *comp_arg)
{
	struct ds_obj_exec_arg *exec_arg = arg;

	/* handle local operaion */
	if (idx == -1) {
		int rc = 0;

		/* No need re-exec local update */
		if (!(exec_arg->flags & ORF_RESEND)) {
			rc = obj_local_rw(exec_arg->rpc, exec_arg->cont_hdl,
					  exec_arg->cont, dth);
		}
		if (comp_cb != NULL)
			comp_cb(comp_arg, rc);

		return rc;
	}

	/* Handle the object remotely */
	return ds_obj_remote_update(dth, arg, idx, comp_cb, comp_arg);
}

void
ds_obj_rw_handler(crt_rpc_t *rpc)
{
	struct obj_rw_in		*orw = crt_req_get(rpc);
	struct obj_rw_out		*orwo = crt_reply_get(rpc);
	struct ds_cont_hdl		*cont_hdl = NULL;
	struct ds_cont_child		*cont = NULL;
	struct dtx_handle		*dth = NULL;
	struct dtx_conflict_entry	 conflict = { 0 };
	struct obj_tls			*tls = obj_tls_get();
	struct ds_obj_exec_arg		exec_arg = { 0 };
	uint32_t			 map_ver = 0;
	uint32_t			 flags = 0;
	bool				 resend = false;
	int				 rc;

	D_ASSERT(orw != NULL);
	D_ASSERT(orwo != NULL);

	D_TIME_START(tls->ot_sp, OBJ_PF_UPDATE);

	rc = ds_pre_check(orw->orw_oid, orw->orw_map_ver, orw->orw_co_hdl,
			  orw->orw_co_uuid, opc_get(rpc->cr_opc),
			  orw->orw_flags, &orw->orw_dti,
			  &cont_hdl, &cont);
	if (rc)
		goto out;

	D_ASSERT(cont_hdl->sch_pool != NULL);
	map_ver = cont_hdl->sch_pool->spc_map_version;

	D_DEBUG(DB_TRACE, "rpc %p opc %d "DF_UOID" dkey %d %s tag/xs %d/%d eph "
		DF_U64", pool ver %u/%u with "DF_DTI".\n",
		rpc, opc_get(rpc->cr_opc), DP_UOID(orw->orw_oid),
		(int)orw->orw_dkey.iov_len, (char *)orw->orw_dkey.iov_buf,
		dss_get_module_info()->dmi_tgt_id,
		dss_get_module_info()->dmi_xs_id, orw->orw_epoch,
		orw->orw_map_ver, map_ver, DP_DTI(&orw->orw_dti));
	if (map_ver < orw->orw_map_ver ||
	    cont_hdl->sch_pool->spc_pool->sp_map == NULL ||
	    DAOS_FAIL_CHECK(DAOS_FORCE_REFRESH_POOL_MAP)) {
		D_DEBUG(DB_IO, "stale server map_version %d req %d\n",
			map_ver, orw->orw_map_ver);
		rc = ds_pool_child_map_refresh_async(cont_hdl->sch_pool);
		if (rc)
			D_GOTO(out, rc);
		D_GOTO(out, rc = -DER_STALE);
	}

	/* Single replica update or fetch. */
	if (orw->orw_shard_tgts.ca_arrays == NULL) {
		/* No need resend check? XXX */
		D_TIME_START(tls->ot_sp, OBJ_PF_UPDATE_LOCAL);
		rc = obj_local_rw(rpc, cont_hdl, cont, dth);
		if (rc != 0) {
			D_ERROR(DF_UOID": rw_local (update) failed %d.\n",
				DP_UOID(orw->orw_oid), rc);
			D_GOTO(out, rc);
		}

		D_TIME_END(tls->ot_sp, OBJ_PF_UPDATE_LOCAL);
		D_GOTO(out, rc);
	}

	/* Handle resend. */
	if (orw->orw_flags & ORF_RESEND) {
		rc = dtx_handle_resend(cont->sc_hdl, &orw->orw_oid,
				       &orw->orw_dti,
				       orw->orw_dkey_hash, false);
		if (rc == -DER_ALREADY)
			D_GOTO(out, rc = 0);
		if (rc == 0) {
			resend = true;
			flags |= ORF_RESEND;
		} else if (rc == -DER_NONEXIST) {
			rc = 0;
		} else {
			D_GOTO(out, rc);
		}
	}

	/* FIXME: until distributed transaction. */
	if (orw->orw_epoch == DAOS_EPOCH_MAX) {
		orw->orw_epoch = daos_ts2epoch();
		D_DEBUG(DB_IO, "overwrite epoch "DF_U64"\n", orw->orw_epoch);
	}

	D_TIME_START(tls->ot_sp, OBJ_PF_UPDATE_PREP);
	if (!resend) {
		/* XXX: In fact, we do not need to start the DTX for
		 *	non-replicated object. But consider to handle
		 *	resend case, we need the DTX record. So start
		 *	the DTX unconditionally. It may be changed in
		 *	the future when we introduce more suitable
		 *	mechanism for handling resent modification.
		 *
		 *	For new leader, even though the local replica
		 *	has ever been modified before, but it doesn't
		 *	know whether other replicas have also done the
		 *	modification or not, so still need to dispatch
		 *	the RPC to other replicas.
		 */
		rc = dtx_begin(&orw->orw_dti, &orw->orw_oid, cont->sc_hdl,
			       orw->orw_epoch, orw->orw_dkey_hash,
			       &conflict, orw->orw_dti_cos.ca_arrays,
			       orw->orw_dti_cos.ca_count, orw->orw_map_ver,
			       DAOS_INTENT_UPDATE, true, &dth);
		if (rc != 0) {
			D_ERROR(DF_UOID": Failed to start DTX for update %d.\n",
				DP_UOID(orw->orw_oid), rc);
			D_GOTO(out, rc);
		}
	}
	D_TIME_END(tls->ot_sp, OBJ_PF_UPDATE_PREP);

	exec_arg.rpc = rpc;
	exec_arg.cont_hdl = cont_hdl;
	exec_arg.cont = cont;
again:
	exec_arg.flags = flags;
	/* Execute the operation on all targets */
	rc = dtx_exec_ops(orw->orw_shard_tgts.ca_arrays,
			  orw->orw_shard_tgts.ca_count, dth,
			  ds_obj_tgt_update, &exec_arg);
out:
	/* Stop the distribute transaction */
	rc = dtx_end(dth, cont_hdl, cont, rc);
	if (rc == -DER_AGAIN) {
		flags |= ORF_RESEND;
		D_GOTO(again, rc);
	}

	ds_obj_rw_reply(rpc, rc, map_ver, &conflict);
	D_TIME_END(tls->ot_sp, OBJ_PF_UPDATE);

	if (cont_hdl)
		ds_cont_hdl_put(cont_hdl);
	if (cont)
		ds_cont_child_put(cont);
}

static void
ds_eu_complete(crt_rpc_t *rpc, int status, int map_version)
{
	struct obj_key_enum_out *oeo;
	struct obj_key_enum_in *oei;
	int rc;

	obj_reply_set_status(rpc, status);
	obj_reply_map_version_set(rpc, map_version);
	rc = crt_reply_send(rpc);
	if (rc != 0)
		D_ERROR("send reply failed: %d\n", rc);

	oei = crt_req_get(rpc);
	D_ASSERT(oei != NULL);
	oeo = crt_reply_get(rpc);
	D_ASSERT(oeo != NULL);

	if (oeo->oeo_kds.ca_arrays != NULL)
		D_FREE(oeo->oeo_kds.ca_arrays);

	if (oeo->oeo_sgl.sg_iovs != NULL)
		daos_sgl_fini(&oeo->oeo_sgl, true);

	if (oeo->oeo_eprs.ca_arrays != NULL)
		D_FREE(oeo->oeo_eprs.ca_arrays);

	if (oeo->oeo_recxs.ca_arrays != NULL)
		D_FREE(oeo->oeo_recxs.ca_arrays);
}

static int
ds_iter_vos(crt_rpc_t *rpc, struct vos_iter_anchors *anchors,
	    struct dss_enum_arg *enum_arg, uint32_t *map_version)
{
	vos_iter_param_t	param = { 0 };
	struct obj_key_enum_in	*oei = crt_req_get(rpc);
	int			opc = opc_get(rpc->cr_opc);
	struct ds_cont_hdl	*cont_hdl;
	struct ds_cont_child	*cont;
	int			type;
	int			rc;
	bool			recursive = false;

	rc = ds_pre_check(oei->oei_oid, oei->oei_map_ver, oei->oei_co_hdl,
			  oei->oei_co_uuid, opc, 0, NULL, &cont_hdl, &cont);
	if (rc)
		D_GOTO(out, rc);

	D_ASSERT(cont_hdl->sch_pool != NULL);
	*map_version = cont_hdl->sch_pool->spc_map_version;

	/* prepare enumeration parameters */
	param.ip_hdl = cont->sc_hdl;
	param.ip_oid = oei->oei_oid;
	if (oei->oei_dkey.iov_len > 0)
		param.ip_dkey = oei->oei_dkey;
	if (oei->oei_akey.iov_len > 0)
		param.ip_akey = oei->oei_akey;
	param.ip_epr.epr_lo = oei->oei_epoch;
	param.ip_epr.epr_hi = oei->oei_epoch;
	param.ip_epc_expr = VOS_IT_EPC_LE;

	if (opc == DAOS_OBJ_RECX_RPC_ENUMERATE) {
		if (oei->oei_dkey.iov_len == 0 ||
		    oei->oei_akey.iov_len == 0)
			D_GOTO(out_cont_hdl, rc = -DER_PROTO);

		if (oei->oei_rec_type == DAOS_IOD_ARRAY) {
			type = VOS_ITER_RECX;
			/* To capture everything visible, we must search from
			 * 0 to our epoch
			 */
			param.ip_epr.epr_lo = 0;
		} else {
			type = VOS_ITER_SINGLE;
		}

		param.ip_epc_expr = VOS_IT_EPC_RE;
		/** Only show visible records and skip punches */
		param.ip_flags = VOS_IT_RECX_VISIBLE | VOS_IT_RECX_SKIP_HOLES;
		enum_arg->fill_recxs = true;
	} else if (opc == DAOS_OBJ_DKEY_RPC_ENUMERATE) {
		type = VOS_ITER_DKEY;
	} else if (opc == DAOS_OBJ_AKEY_RPC_ENUMERATE) {
		type = VOS_ITER_AKEY;
	} else {
		/* object iteration for rebuild */
		D_ASSERT(opc == DAOS_OBJ_RPC_ENUMERATE);
		type = VOS_ITER_DKEY;
		param.ip_epr.epr_lo = 0;
		param.ip_epc_expr = VOS_IT_EPC_RE;
		recursive = true;
		enum_arg->chk_key2big = true;
	}

	/*
	 * FIXME: enumeration RPC uses one anchor for both SV and EV,
	 * that won't be able to support recursive iteration in our
	 * current data model (one akey can have both SV tree and EV
	 * tree).
	 *
	 * Need to use separate anchors for SV and EV, or return a
	 * 'type' to indicate the anchor is on SV tree or EV tree.
	 */
	if (type == VOS_ITER_SINGLE)
		anchors->ia_sv = anchors->ia_ev;

	rc = dss_enum_pack(&param, type, recursive, anchors, enum_arg);

	if (type == VOS_ITER_SINGLE)
		anchors->ia_ev = anchors->ia_sv;

	D_DEBUG(DB_IO, ""DF_UOID" iterate type %d tag %d rc %d\n",
		DP_UOID(oei->oei_oid), type, dss_get_module_info()->dmi_tgt_id,
		rc);
out_cont_hdl:
	if (cont_hdl)
		ds_cont_hdl_put(cont_hdl);
	if (cont)
		ds_cont_child_put(cont);
out:
	return rc;
}

static int
obj_enum_reply_bulk(crt_rpc_t *rpc)
{
	daos_sg_list_t	*sgls[2] = { 0 };
	daos_sg_list_t  tmp_sgl;
	crt_bulk_t	bulks[2] = { 0 };
	struct obj_key_enum_in	*oei;
	struct obj_key_enum_out	*oeo;
	int		idx = 0;
	d_iov_t		tmp_iov;
	int		rc;

	oei = crt_req_get(rpc);
	oeo = crt_reply_get(rpc);
	if (oei->oei_kds_bulk) {
		tmp_iov.iov_buf = oeo->oeo_kds.ca_arrays;
		tmp_iov.iov_buf_len = oeo->oeo_kds.ca_count *
				      sizeof(daos_key_desc_t);
		tmp_iov.iov_len = oeo->oeo_kds.ca_count *
				      sizeof(daos_key_desc_t);
		tmp_sgl.sg_nr = 1;
		tmp_sgl.sg_nr_out = 1;
		tmp_sgl.sg_iovs = &tmp_iov;
		sgls[idx] = &tmp_sgl;
		bulks[idx] = oei->oei_kds_bulk;
		idx++;
		D_DEBUG(DB_IO, "reply kds bulk %zd\n", tmp_iov.iov_len);
	}

	if (oei->oei_bulk) {
		D_DEBUG(DB_IO, "reply bulk %zd nr_out %d\n",
			oeo->oeo_sgl.sg_iovs[0].iov_len,
			oeo->oeo_sgl.sg_nr_out);
		sgls[idx] = &oeo->oeo_sgl;
		bulks[idx] = oei->oei_bulk;
		idx++;
	}

	/* No need reply bulk */
	if (idx == 0)
		return 0;

	rc = ds_bulk_transfer(rpc, CRT_BULK_PUT, false, bulks, DAOS_HDL_INVAL,
			      sgls, idx);

	if (oei->oei_kds_bulk) {
		D_FREE(oeo->oeo_kds.ca_arrays);
		oeo->oeo_kds.ca_arrays = NULL;
		oeo->oeo_kds.ca_count = 0;
	}

	/* Free oeo_sgl here to avoid rpc reply the data inline */
	if (oei->oei_bulk)
		daos_sgl_fini(&oeo->oeo_sgl, true);

	return rc;
}

void
ds_obj_enum_handler(crt_rpc_t *rpc)
{
	struct dss_enum_arg	enum_arg = { 0 };
	struct vos_iter_anchors	anchors = { 0 };
	struct obj_key_enum_in	*oei;
	struct obj_key_enum_out	*oeo;
	int			opc = opc_get(rpc->cr_opc);
	unsigned int		map_version = 0;
	int			rc = 0;

	oei = crt_req_get(rpc);
	D_ASSERT(oei != NULL);
	oeo = crt_reply_get(rpc);
	D_ASSERT(oeo != NULL);
	/* prepare buffer for enumerate */

	anchors.ia_dkey = oei->oei_dkey_anchor;
	anchors.ia_akey = oei->oei_akey_anchor;
	anchors.ia_ev = oei->oei_anchor;

	/* TODO: Transfer the inline_thres from enumerate RPC */
	enum_arg.inline_thres = 32;

	if (opc == DAOS_OBJ_RECX_RPC_ENUMERATE ||
	    opc == DAOS_OBJ_RPC_ENUMERATE) {
		oeo->oeo_eprs.ca_count = 0;
		D_ALLOC(oeo->oeo_eprs.ca_arrays,
			oei->oei_nr * sizeof(daos_epoch_range_t));
		if (oeo->oeo_eprs.ca_arrays == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
		enum_arg.eprs = oeo->oeo_eprs.ca_arrays;
		enum_arg.eprs_cap = oei->oei_nr;
		enum_arg.eprs_len = 0;
	}

	if (opc == DAOS_OBJ_RECX_RPC_ENUMERATE) {
		oeo->oeo_recxs.ca_count = 0;
		D_ALLOC(oeo->oeo_recxs.ca_arrays,
			oei->oei_nr * sizeof(daos_recx_t));
		if (oeo->oeo_recxs.ca_arrays == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
		enum_arg.recxs = oeo->oeo_recxs.ca_arrays;
		enum_arg.recxs_cap = oei->oei_nr;
		enum_arg.recxs_len = 0;
	} else {
		rc = ds_sgls_prep(&oeo->oeo_sgl, &oei->oei_sgl, 1);
		if (rc != 0)
			D_GOTO(out, rc);
		enum_arg.sgl = &oeo->oeo_sgl;
		enum_arg.sgl_idx = 0;

		/* Prepare key desciptor buffer */
		oeo->oeo_kds.ca_count = 0;
		D_ALLOC(oeo->oeo_kds.ca_arrays,
			oei->oei_nr * sizeof(daos_key_desc_t));
		if (oeo->oeo_kds.ca_arrays == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
		enum_arg.kds = oeo->oeo_kds.ca_arrays;
		enum_arg.kds_cap = oei->oei_nr;
		enum_arg.kds_len = 0;
	}

	/* keep trying until the key_buffer is fully filled or
	 * reaching the end of the stream
	 */
	rc = ds_iter_vos(rpc, &anchors, &enum_arg, &map_version);
	if (rc == 1) {
		/* If the buffer is full, exit and
		 * reset failure.
		 */
		rc = 0;
	}

	if (rc)
		D_GOTO(out, rc);

	oeo->oeo_dkey_anchor = anchors.ia_dkey;
	oeo->oeo_akey_anchor = anchors.ia_akey;
	oeo->oeo_anchor = anchors.ia_ev;

	if (enum_arg.eprs)
		oeo->oeo_eprs.ca_count = enum_arg.eprs_len;

	if (opc == DAOS_OBJ_RECX_RPC_ENUMERATE) {
		oeo->oeo_recxs.ca_count = enum_arg.recxs_len;
		oeo->oeo_num = enum_arg.rnum;
		oeo->oeo_size = enum_arg.rsize;
	} else {
		D_ASSERT(enum_arg.eprs_len == 0 ||
			 enum_arg.eprs_len == enum_arg.kds_len);
		oeo->oeo_kds.ca_count = enum_arg.kds_len;
		oeo->oeo_num = enum_arg.kds_len;
		oeo->oeo_size = oeo->oeo_sgl.sg_iovs[0].iov_len;
	}

	rc = obj_enum_reply_bulk(rpc);
out:
	/* for KEY2BIG case, just reuse the oeo_size to reply the key len */
	if (rc == -DER_KEY2BIG)
		oeo->oeo_size = enum_arg.kds[0].kd_key_len;
	ds_eu_complete(rpc, rc, map_version);
}

static void
obj_punch_complete(crt_rpc_t *rpc, int status, uint32_t map_version,
		   struct dtx_conflict_entry *dce)
{
	int rc;

	obj_reply_set_status(rpc, status);
	obj_reply_map_version_set(rpc, map_version);
	obj_reply_dtx_conflict_set(rpc, dce);

	rc = crt_reply_send(rpc);
	if (rc != 0)
		D_ERROR("send reply failed: %d\n", rc);
}

static int
obj_local_punch(struct obj_punch_in *opi, crt_opcode_t opc,
		struct ds_cont_hdl *cont_hdl, struct ds_cont_child *cont,
		struct dtx_handle *dth)
{
	int	rc = 0;

	switch (opc) {
	default:
		D_ERROR("opc %#x not supported\n", opc);
		D_GOTO(out, rc = -DER_NOSYS);

	case DAOS_OBJ_RPC_PUNCH:
	case DAOS_OBJ_RPC_TGT_PUNCH:
		rc = vos_obj_punch(cont->sc_hdl, opi->opi_oid,
				   opi->opi_epoch, opi->opi_map_ver, 0,
				   NULL, 0, NULL, dth);
		break;
	case DAOS_OBJ_RPC_PUNCH_DKEYS:
	case DAOS_OBJ_RPC_PUNCH_AKEYS:
	case DAOS_OBJ_RPC_TGT_PUNCH_DKEYS:
	case DAOS_OBJ_RPC_TGT_PUNCH_AKEYS: {
		daos_key_t *dkey;

		D_ASSERTF(opi->opi_dkeys.ca_count == 1,
			  "NOT punch multiple (%llu) dkeys via one RPC\n",
			  (unsigned long long)opi->opi_dkeys.ca_count);

		dkey = &((daos_key_t *)opi->opi_dkeys.ca_arrays)[0];
		rc = vos_obj_punch(cont->sc_hdl, opi->opi_oid,
				   opi->opi_epoch, opi->opi_map_ver, 0, dkey,
				   opi->opi_akeys.ca_count,
				   opi->opi_akeys.ca_arrays, dth);
		break;
	}
	}

out:
	return rc;
}

/* Handle the punch requests on non-leader */
void
ds_obj_tgt_punch_handler(crt_rpc_t *rpc)
{
	struct ds_cont_hdl		*cont_hdl = NULL;
	struct ds_cont_child		*cont = NULL;
	struct dtx_handle		*dth = NULL;
	struct dtx_conflict_entry	 conflict = { 0 };
	struct obj_punch_in		*opi;
	uint32_t			 map_version = 0;
	int				 rc;

	opi = crt_req_get(rpc);
	D_ASSERT(opi != NULL);
	rc = ds_pre_check(opi->opi_oid, opi->opi_map_ver, opi->opi_co_hdl,
			  opi->opi_co_uuid, opc_get(rpc->cr_opc),
			  opi->opi_flags, &opi->opi_dti, &cont_hdl, &cont);
	if (rc)
		goto out;

	/* Handle resend. */
	if (opi->opi_flags & ORF_RESEND) {
		rc = dtx_handle_resend(cont->sc_hdl, &opi->opi_oid,
				       &opi->opi_dti, opi->opi_dkey_hash, true);
		if (rc == -DER_ALREADY)
			D_GOTO(out, rc = 0);
	}

	/* Start the local transaction */
	rc = dtx_begin(&opi->opi_dti, &opi->opi_oid, cont->sc_hdl,
		       opi->opi_epoch, opi->opi_dkey_hash,
		       &conflict, opi->opi_dti_cos.ca_arrays,
		       opi->opi_dti_cos.ca_count, opi->opi_map_ver,
		       DAOS_INTENT_PUNCH, false, &dth);
	if (rc != 0) {
		D_ERROR(DF_UOID": Failed to start DTX for punch %d.\n",
			DP_UOID(opi->opi_oid), rc);
		D_GOTO(out, rc);
	}

	/* local RPC handler */
	rc = obj_local_punch(opi, opc_get(rpc->cr_opc), cont_hdl, cont, dth);
	if (rc != 0) {
		D_ERROR(DF_UOID": obj_local_punch failed %d.\n",
			DP_UOID(opi->opi_oid), rc);
		D_GOTO(out, rc);
	}

	/**
	 * XXX layer violation, it should be moved to DTX module, will
	 * handle in following patches.
	 */
	if (opi->opi_dti_cos.ca_count > 0) {
		/* XXX: For non-leader replica, even if we fail to
		 *	make related modification for some reason,
		 *	we still need to commit the DTXs for CoS.
		 *	Because other replica may have already
		 *	committed them. For leader case, it is
		 *	not important even if we miss to commit
		 *	the CoS DTXs, because they are still in
		 *	CoS cache, and can be committed next time.
		 */
		rc = vos_dtx_commit(cont->sc_hdl,
				    opi->opi_dti_cos.ca_arrays,
				    opi->opi_dti_cos.ca_count);
		if (rc != 0) {
			D_ERROR(DF_UUID": Fail to DTX CoS commit: %d\n",
				DP_UUID(cont->sc_uuid), rc);
			D_GOTO(out, rc);
		}
	}
out:
	/* Stop the local transaction */
	rc = dtx_end(dth, cont_hdl, cont, rc);
	obj_punch_complete(rpc, rc, map_version, &conflict);
	if (cont_hdl)
		ds_cont_hdl_put(cont_hdl);
	if (cont)
		ds_cont_child_put(cont); /* -1 for rebuild container */
}

static int
ds_obj_tgt_punch(struct dtx_handle *dth, void *arg, int idx,
		 dtx_exec_shard_comp_cb_t comp_cb, void *comp_arg)
{
	struct ds_obj_exec_arg	*exec_arg = arg;

	/* handle local operaion */
	if (idx == -1) {
		crt_rpc_t		*rpc = exec_arg->rpc;
		struct obj_punch_in	*opi = crt_req_get(rpc);
		int			rc = 0;

		if (!(exec_arg->flags & ORF_RESEND)) {
			rc = obj_local_punch(opi, opc_get(rpc->cr_opc),
					     exec_arg->cont_hdl,
					     exec_arg->cont, dth);
		}
		if (comp_cb != NULL)
			comp_cb(comp_arg, rc);

		return rc;
	}

	/* Handle the object remotely */
	return ds_obj_remote_punch(dth, arg, idx, comp_cb, comp_arg);
}

/* Handle the punch requests on the leader */
void
ds_obj_punch_handler(crt_rpc_t *rpc)
{
	struct ds_cont_hdl		*cont_hdl = NULL;
	struct ds_cont_child		*cont = NULL;
	struct dtx_handle		*dth = NULL;
	struct dtx_conflict_entry	 conflict = { 0 };
	struct obj_punch_in		*opi;
	struct ds_obj_exec_arg		exec_arg = { 0 };
	uint32_t			 map_version = 0;
	uint32_t			 flags = 0;
	bool				 resend = false;
	int				 rc;

	opi = crt_req_get(rpc);
	D_ASSERT(opi != NULL);
	rc = ds_pre_check(opi->opi_oid, opi->opi_map_ver, opi->opi_co_hdl,
			  opi->opi_co_uuid, opc_get(rpc->cr_opc),
			  opi->opi_flags, &opi->opi_dti, &cont_hdl, &cont);
	if (rc)
		goto out;

	if (opi->opi_shard_tgts.ca_arrays == NULL) {
		/* local RPC handler */
		rc = obj_local_punch(opi, opc_get(rpc->cr_opc), cont_hdl, cont,
				     dth);
		if (rc != 0) {
			D_ERROR(DF_UOID": obj_local_punch failed %d.\n",
				DP_UOID(opi->opi_oid), rc);
			D_GOTO(out, rc);
		}

		D_GOTO(out, rc);
	}

	/* Handle resend. */
	if (opi->opi_flags & ORF_RESEND) {
		rc = dtx_handle_resend(cont->sc_hdl, &opi->opi_oid,
				       &opi->opi_dti, opi->opi_dkey_hash, true);
		if (rc == -DER_ALREADY)
			D_GOTO(out, rc = 0);
		if (rc == 0)
			resend = true;
		else if (rc == -DER_NONEXIST)
			rc = 0;
		else
			D_GOTO(out, rc);
	}

	/* FIXME: until distributed transaction. */
	if (opi->opi_epoch == DAOS_EPOCH_MAX) {
		opi->opi_epoch = daos_ts2epoch();
		D_DEBUG(DB_IO, "overwrite epoch "DF_U64"\n", opi->opi_epoch);
	}
	if (!resend) {
		/* XXX: In fact, we do not need to start the DTX for
		 *	non-replicated object. But consider to handle
		 *	resend case, we need the DTX record. So start
		 *	the DTX unconditionally. It may be changed in
		 *	the future when we introduce more suitable
		 *	mechanism for handling resent modification.
		 *
		 *	For new leader, even though the local replica
		 *	has ever been modified before, but it doesn't
		 *	know whether other replicas have also done the
		 *	modification or not, so still need to dispatch
		 *	the RPC to other replicas.
		 */

		rc = dtx_begin(&opi->opi_dti, &opi->opi_oid, cont->sc_hdl,
			       opi->opi_epoch, opi->opi_dkey_hash,
			       &conflict, opi->opi_dti_cos.ca_arrays,
			       opi->opi_dti_cos.ca_count, opi->opi_map_ver,
			       DAOS_INTENT_PUNCH, true, &dth);
		if (rc != 0) {
			D_ERROR(DF_UOID": Failed to start DTX for punch %d.\n",
				DP_UOID(opi->opi_oid), rc);
			D_GOTO(out, rc);
		}
	}

	exec_arg.rpc = rpc;
	exec_arg.cont_hdl = cont_hdl;
	exec_arg.cont = cont;
again:
	exec_arg.flags = flags;
	/* Execute the operation on all shards */
	rc = dtx_exec_ops(opi->opi_shard_tgts.ca_arrays,
			  opi->opi_shard_tgts.ca_count, dth, ds_obj_tgt_punch,
			  &exec_arg);
out:
	/* Stop the distribute transaction */
	rc = dtx_end(dth, cont_hdl, cont, rc);
	if (rc == -DER_AGAIN) {
		flags |= ORF_RESEND;
		D_GOTO(again, rc);
	}

	obj_punch_complete(rpc, rc, map_version, &conflict);
	if (cont_hdl)
		ds_cont_hdl_put(cont_hdl);
	if (cont)
		ds_cont_child_put(cont); /* -1 for rebuild container */
}

void
ds_obj_query_key_handler(crt_rpc_t *rpc)
{
	struct obj_query_key_in		*okqi;
	struct obj_query_key_out	*okqo;
	struct ds_cont_hdl		*cont_hdl = NULL;
	struct ds_cont_child		*cont = NULL;
	daos_key_t			*dkey;
	daos_key_t			*akey;
	uint32_t			map_version = 0;
	int				rc;

	okqi = crt_req_get(rpc);
	D_ASSERT(okqi != NULL);
	okqo = crt_reply_get(rpc);
	D_ASSERT(okqo != NULL);

	D_DEBUG(DB_IO, "ds_obj_query_key_handler: flags = %d\n",
		okqi->okqi_flags);

	rc = ds_check_container(okqi->okqi_co_hdl, okqi->okqi_co_uuid,
				opc_get(rpc->cr_opc), &cont_hdl, &cont);
	if (rc)
		D_GOTO(out, rc);

	D_ASSERT(cont_hdl->sch_pool != NULL);
	map_version = cont_hdl->sch_pool->spc_map_version;

	dkey = &okqi->okqi_dkey;
	akey = &okqi->okqi_akey;
	d_iov_set(&okqo->okqo_akey, NULL, 0);
	d_iov_set(&okqo->okqo_dkey, NULL, 0);
	if (okqi->okqi_flags & DAOS_GET_DKEY)
		dkey = &okqo->okqo_dkey;
	if (okqi->okqi_flags & DAOS_GET_AKEY)
		akey = &okqo->okqo_akey;

	rc = vos_obj_query_key(cont->sc_hdl, okqi->okqi_oid, okqi->okqi_flags,
			       okqi->okqi_epoch, dkey, akey, &okqo->okqo_recx);
out:
	if (cont_hdl)
		ds_cont_hdl_put(cont_hdl);
	if (cont)
		ds_cont_child_put(cont); /* -1 for rebuild container */

	obj_reply_set_status(rpc, rc);
	obj_reply_map_version_set(rpc, map_version);

	rc = crt_reply_send(rpc);
	if (rc != 0)
		D_ERROR("send reply failed: %d\n", rc);
}

/**
 * Choose abt pools for object RPC. Because those update RPC might create pool
 * map refresh ULT, let's put it to share pool. For other RPC, it can be put to
 * the private pool. XXX we might just instruct the server create task for
 * inline I/O.
 */
ABT_pool
ds_obj_abt_pool_choose_cb(crt_rpc_t *rpc, ABT_pool *pools)
{
	ABT_pool	pool;

	switch (opc_get(rpc->cr_opc)) {
	case DAOS_OBJ_RPC_ENUMERATE:
	case DAOS_OBJ_RPC_PUNCH:
	case DAOS_OBJ_RPC_PUNCH_DKEYS:
	case DAOS_OBJ_RPC_PUNCH_AKEYS:
	case DAOS_OBJ_RPC_UPDATE:
	case DAOS_OBJ_RPC_TGT_UPDATE:
	case DAOS_OBJ_RPC_FETCH:
		pool = pools[DSS_POOL_SHARE];
		break;
	default:
		pool = pools[DSS_POOL_PRIV];
		break;
	};

	return pool;
}