/**
 * (C) Copyright 2017-2019 Intel Corporation.
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
 * ds_pool: Pool IV cache
 */
#define D_LOGFAC	DD_FAC(pool)

#include <daos_srv/pool.h>
#include <daos/pool_map.h>
#include <daos_srv/iv.h>
#include <daos_prop.h>
#include "srv_internal.h"

uint32_t
pool_iv_map_ent_size(int nr)
{
	return pool_buf_size(nr) +
	       sizeof(struct pool_iv_entry) - sizeof(struct pool_buf);
}

static uint32_t
pool_iv_prop_ent_size(int nr_aces, int nr_ranks)
{
	uint32_t acl_size;
	uint32_t svc_size;

	/* Calculate pool_iv_buf size */
	acl_size = roundup(offsetof(struct daos_acl, dal_ace[nr_aces]), 8);
	svc_size = roundup(nr_ranks * sizeof(d_rank_t), 8);

	return sizeof(struct pool_iv_entry) + acl_size + svc_size;
}

static int
pool_iv_value_alloc_internal(struct ds_iv_key *key, d_sg_list_t *sgl)
{
	struct pool_iv_key *pool_key = (struct pool_iv_key *)key->key_buf;
	uint32_t	buf_size = pool_key->pik_entry_size;
	int		rc;

	D_ASSERT(buf_size != 0);
	rc = daos_sgl_init(sgl, 1);
	if (rc)
		return rc;

	D_ALLOC(sgl->sg_iovs[0].iov_buf, buf_size);
	if (sgl->sg_iovs[0].iov_buf == NULL)
		D_GOTO(free, rc = -DER_NOMEM);

	sgl->sg_iovs[0].iov_buf_len = buf_size;
free:
	if (rc)
		daos_sgl_fini(sgl, true);

	return rc;
}

/* FIXME: Need better handling here, for example retry if the size is not
 * enough.
 */
#define PROP_SVC_LIST_MAX_TMP	16

static void
pool_iv_prop_l2g(daos_prop_t *prop, struct pool_iv_prop *iv_prop)
{
	struct daos_prop_entry	*prop_entry;
	struct daos_acl		*acl;
	d_rank_list_t		*svc_list;
	unsigned int		offset = 0;
	int			i;

	D_ASSERT(prop->dpp_nr == DAOS_PROP_PO_NUM);
	for (i = 0; i < DAOS_PROP_PO_NUM; i++) {
		prop_entry = &prop->dpp_entries[i];
		switch (prop_entry->dpe_type) {
		case DAOS_PROP_PO_LABEL:
			D_ASSERT(strlen(prop_entry->dpe_str) <=
				 DAOS_PROP_LABEL_MAX_LEN);
			strcpy(iv_prop->pip_label, prop_entry->dpe_str);
			break;
		case DAOS_PROP_PO_OWNER:
			D_ASSERT(strlen(prop_entry->dpe_str) <=
				 DAOS_ACL_MAX_PRINCIPAL_LEN);
			strcpy(iv_prop->pip_owner, prop_entry->dpe_str);
			break;
		case DAOS_PROP_PO_OWNER_GROUP:
			D_ASSERT(strlen(prop_entry->dpe_str) <=
				 DAOS_ACL_MAX_PRINCIPAL_LEN);
			strcpy(iv_prop->pip_owner_grp, prop_entry->dpe_str);
			break;
		case DAOS_PROP_PO_SPACE_RB:
			iv_prop->pip_space_rb = prop_entry->dpe_val;
			break;
		case DAOS_PROP_PO_SELF_HEAL:
			iv_prop->pip_self_heal = prop_entry->dpe_val;
			break;
		case DAOS_PROP_PO_RECLAIM:
			iv_prop->pip_reclaim = prop_entry->dpe_val;
			break;
		case DAOS_PROP_PO_ACL:
			acl = prop_entry->dpe_val_ptr;
			if (acl != NULL) {
				ssize_t acl_size = daos_acl_get_size(acl);

				iv_prop->pip_acl_offset = offset;
				iv_prop->pip_acl =
						(void *)(iv_prop->pip_iv_buf +
						roundup(offset, 8));
				memcpy(iv_prop->pip_acl, acl, acl_size);
				offset += roundup(acl_size, 8);
			}
			break;
		case DAOS_PROP_PO_SVC_LIST:
			svc_list = prop_entry->dpe_val_ptr;
			if (svc_list) {
				D_ASSERT(svc_list->rl_nr <
					 PROP_SVC_LIST_MAX_TMP);
				iv_prop->pip_svc_list.rl_nr = svc_list->rl_nr;
				iv_prop->pip_svc_list.rl_ranks =
						(void *)(iv_prop->pip_iv_buf +
						roundup(offset, 8));
				iv_prop->pip_svc_list_offset = offset;
				d_rank_list_copy(&iv_prop->pip_svc_list,
						 svc_list);
				offset += roundup(
					svc_list->rl_nr * sizeof(d_rank_t), 8);
			}
			break;
		default:
			D_ASSERTF(0, "bad dpe_type %d\n", prop_entry->dpe_type);
			break;
		}
	}
}

static int
pool_iv_prop_g2l(struct pool_iv_prop *iv_prop, daos_prop_t *prop)
{
	struct daos_prop_entry	*prop_entry;
	struct daos_acl		*acl;
	void			*label_alloc = NULL;
	void			*owner_alloc = NULL;
	void			*owner_grp_alloc = NULL;
	void			*acl_alloc = NULL;
	d_rank_list_t		*svc_list = NULL;
	d_rank_list_t		*dst_list;
	int			i;
	int			rc = 0;

	D_ASSERT(prop->dpp_nr == DAOS_PROP_PO_NUM);
	for (i = 0; i < DAOS_PROP_PO_NUM; i++) {
		prop_entry = &prop->dpp_entries[i];
		prop_entry->dpe_type = DAOS_PROP_PO_MIN + i + 1;
		switch (prop_entry->dpe_type) {
		case DAOS_PROP_PO_LABEL:
			D_ASSERT(strlen(iv_prop->pip_label) <=
				 DAOS_PROP_LABEL_MAX_LEN);
			D_STRNDUP(prop_entry->dpe_str, iv_prop->pip_label,
				  DAOS_PROP_LABEL_MAX_LEN);
			if (prop_entry->dpe_str)
				label_alloc = prop_entry->dpe_str;
			else
				D_GOTO(out, rc = -DER_NOMEM);
			break;
		case DAOS_PROP_PO_OWNER:
			D_ASSERT(strlen(iv_prop->pip_owner) <=
				 DAOS_ACL_MAX_PRINCIPAL_LEN);
			D_STRNDUP(prop_entry->dpe_str, iv_prop->pip_owner,
				  DAOS_ACL_MAX_PRINCIPAL_LEN);
			if (prop_entry->dpe_str)
				owner_alloc = prop_entry->dpe_str;
			else
				D_GOTO(out, rc = -DER_NOMEM);
			break;
		case DAOS_PROP_PO_OWNER_GROUP:
			D_ASSERT(strlen(iv_prop->pip_owner_grp) <=
				 DAOS_ACL_MAX_PRINCIPAL_LEN);
			D_STRNDUP(prop_entry->dpe_str, iv_prop->pip_owner_grp,
				  DAOS_ACL_MAX_PRINCIPAL_LEN);
			if (prop_entry->dpe_str)
				owner_grp_alloc = prop_entry->dpe_str;
			else
				D_GOTO(out, rc = -DER_NOMEM);
			break;
		case DAOS_PROP_PO_SPACE_RB:
			prop_entry->dpe_val = iv_prop->pip_space_rb;
			break;
		case DAOS_PROP_PO_SELF_HEAL:
			prop_entry->dpe_val = iv_prop->pip_self_heal;
			break;
		case DAOS_PROP_PO_RECLAIM:
			prop_entry->dpe_val = iv_prop->pip_reclaim;
			break;
		case DAOS_PROP_PO_ACL:
			iv_prop->pip_acl =
				(void *)(iv_prop->pip_iv_buf +
				 roundup(iv_prop->pip_acl_offset, 8));
			acl = iv_prop->pip_acl;
			if (acl->dal_len > 0) {
				D_ASSERT(daos_acl_validate(acl) == 0);
				acl_alloc = daos_acl_dup(acl);
				if (acl_alloc != NULL)
					prop_entry->dpe_val_ptr = acl_alloc;
				else
					D_GOTO(out, rc = -DER_NOMEM);
			} else {
				prop_entry->dpe_val_ptr = NULL;
			}
			break;
		case DAOS_PROP_PO_SVC_LIST:
			iv_prop->pip_svc_list.rl_ranks =
				(void *)(iv_prop->pip_iv_buf +
				 roundup(iv_prop->pip_svc_list_offset, 8));
			svc_list = &iv_prop->pip_svc_list;
			if (svc_list->rl_nr > 0) {
				rc = d_rank_list_dup(&dst_list, svc_list);
				if (rc)
					D_GOTO(out, rc);
				prop_entry->dpe_val_ptr = dst_list;
			}
			break;
		default:
			D_ASSERTF(0, "bad dpe_type %d\n", prop_entry->dpe_type);
			break;
		}
	}

out:
	if (rc) {
		if (acl_alloc)
			daos_acl_free(acl_alloc);
		if (label_alloc)
			D_FREE(label_alloc);
		if (owner_alloc)
			D_FREE(owner_alloc);
		if (owner_grp_alloc)
			D_FREE(owner_grp_alloc);
		if (svc_list)
			d_rank_list_free(dst_list);
	}
	return rc;
}

static int
pool_iv_ent_init(struct ds_iv_key *iv_key, void *data,
		 struct ds_iv_entry *entry)
{
	int	rc;

	rc = pool_iv_value_alloc_internal(iv_key, &entry->iv_value);
	if (rc)
		return rc;

	memcpy(&entry->iv_key, iv_key, sizeof(*iv_key));

	return rc;
}

static int
pool_iv_ent_get(struct ds_iv_entry *entry, void **priv)
{
	return 0;
}

static int
pool_iv_ent_put(struct ds_iv_entry *entry, void **priv)
{
	return 0;
}

static int
pool_iv_ent_destroy(d_sg_list_t *sgl)
{
	daos_sgl_fini(sgl, true);
	return 0;
}

static int
pool_iv_ent_copy(int class_id, d_sg_list_t *dst, d_sg_list_t *src)
{
	struct pool_iv_entry *src_iv = src->sg_iovs[0].iov_buf;
	struct pool_iv_entry *dst_iv = dst->sg_iovs[0].iov_buf;

	if (dst_iv == src_iv)
		return 0;

	D_ASSERT(src_iv != NULL);
	D_ASSERT(dst_iv != NULL);

	dst_iv->piv_master_rank = src_iv->piv_master_rank;
	uuid_copy(dst_iv->piv_pool_uuid, src_iv->piv_pool_uuid);

	if (class_id == IV_POOL_MAP) {
		dst_iv->piv_pool_map_ver = src_iv->piv_pool_map_ver;
		if (src_iv->piv_map.piv_pool_buf.pb_nr > 0) {
			int src_len = pool_buf_size(
				src_iv->piv_map.piv_pool_buf.pb_nr);
			int dst_len = dst->sg_iovs[0].iov_buf_len -
				      sizeof(*dst_iv) + sizeof(struct pool_buf);

			/* copy pool buf */
			if (dst_len < src_len) {
				D_ERROR("dst %d\n src %d\n", dst_len, src_len);
				return -DER_REC2BIG;
			}

			memcpy(&dst_iv->piv_map.piv_pool_buf,
			       &src_iv->piv_map.piv_pool_buf, src_len);
		}
		dst->sg_iovs[0].iov_len = src->sg_iovs[0].iov_len;
		D_DEBUG(DB_TRACE, "pool "DF_UUID" map ver %d\n",
			DP_UUID(dst_iv->piv_pool_uuid),
			dst_iv->piv_pool_map_ver);
	} else if (class_id == IV_POOL_PROP) {
		daos_prop_t	*prop_fetch;
		int rc;

		prop_fetch = daos_prop_alloc(DAOS_PROP_PO_NUM);
		if (prop_fetch == NULL)
			return -DER_NOMEM;

		rc = pool_iv_prop_g2l(&src_iv->piv_prop, prop_fetch);
		if (rc) {
			daos_prop_free(prop_fetch);
			D_ERROR("prop g2l failed: rc %d\n", rc);
			return rc;
		}

		pool_iv_prop_l2g(prop_fetch, &dst_iv->piv_prop);
		daos_prop_free(prop_fetch);
	} else {
		D_ERROR("bad class id %d\n", class_id);
		return -DER_INVAL;
	}

	return 0;
}

static int
pool_iv_ent_fetch(struct ds_iv_entry *entry, struct ds_iv_key *key,
		  d_sg_list_t *dst, d_sg_list_t *src, void **priv)
{
	return pool_iv_ent_copy(entry->iv_class->iv_class_id, dst, src);
}

static int
pool_iv_ent_update(struct ds_iv_entry *entry, struct ds_iv_key *key,
		   d_sg_list_t *src, void **priv)
{
	struct pool_iv_entry	*src_iv = src->sg_iovs[0].iov_buf;
	struct ds_pool		*pool;
	d_rank_t		rank;
	int			rc;

	pool = ds_pool_lookup(src_iv->piv_pool_uuid);
	if (pool == NULL)
		return -DER_NONEXIST;

	rc = crt_group_rank(pool->sp_group, &rank);
	if (rc) {
		ds_pool_put(pool);
		return rc;
	}

	if (rank != src_iv->piv_master_rank) {
		ds_pool_put(pool);
		return -DER_IVCB_FORWARD;
	}

	D_DEBUG(DB_TRACE, "rank %d master rank %d\n", rank,
		src_iv->piv_master_rank);

	/* Update pool map version or pool map */
	if (entry->iv_class->iv_class_id == IV_POOL_MAP)
		rc = ds_pool_tgt_map_update(pool,
			src_iv->piv_map.piv_pool_buf.pb_nr > 0 ?
			&src_iv->piv_map.piv_pool_buf : NULL,
			src_iv->piv_pool_map_ver);
	else if (entry->iv_class->iv_class_id == IV_POOL_PROP)
		rc = ds_pool_tgt_prop_update(pool, &src_iv->piv_prop);

	ds_pool_put(pool);

	return pool_iv_ent_copy(entry->iv_class->iv_class_id,
				&entry->iv_value, src);
}

static int
pool_iv_ent_refresh(struct ds_iv_entry *entry, struct ds_iv_key *key,
		    d_sg_list_t *src, int ref_rc, void **priv)
{
	struct pool_iv_entry	*dst_iv = entry->iv_value.sg_iovs[0].iov_buf;
	struct pool_iv_entry	*src_iv;
	struct ds_pool		*pool;
	int			rc;

	if (src == NULL) /* invalidate */
		return 0;

	src_iv = src->sg_iovs[0].iov_buf;
	D_ASSERT(dst_iv != NULL);
	rc = pool_iv_ent_copy(entry->iv_class->iv_class_id,
			      &entry->iv_value, src);
	if (rc)
		return rc;

	D_ASSERTF(src_iv != NULL, "%d\n", entry->iv_class->iv_class_id);
	/* Update pool map version or pool map */
	pool = ds_pool_lookup(src_iv->piv_pool_uuid);
	if (pool == NULL) {
		D_WARN("No pool "DF_UUID"\n", DP_UUID(src_iv->piv_pool_uuid));
		return 0;
	}

	if (entry->iv_class->iv_class_id == IV_POOL_PROP)
		rc = ds_pool_tgt_prop_update(pool, &src_iv->piv_prop);
	else
		rc = ds_pool_tgt_map_update(pool,
				src_iv->piv_map.piv_pool_buf.pb_nr > 0 ?
				&src_iv->piv_map.piv_pool_buf : NULL,
				src_iv->piv_pool_map_ver);
	ds_pool_put(pool);

	return rc;
}

static int
pool_iv_value_alloc(struct ds_iv_entry *entry, d_sg_list_t *sgl)
{
	return pool_iv_value_alloc_internal(&entry->iv_key, sgl);
}

static int
pool_iv_pre_sync(struct ds_iv_entry *entry, struct ds_iv_key *key,
		 d_sg_list_t *value)
{
	struct pool_iv_entry	*v = value->sg_iovs[0].iov_buf;
	struct ds_pool		*pool;
	struct pool_buf		*map_buf = NULL;
	int			 rc;

	/* This function is only for IV_POOL_MAP. */
	if (entry->iv_class->iv_class_id != IV_POOL_MAP)
		return 0;

	pool = ds_pool_lookup(v->piv_pool_uuid);
	if (pool == NULL) {
		D_DEBUG(DB_TRACE, DF_UUID": pool not found\n",
			DP_UUID(v->piv_pool_uuid));
		/* Return 0 to keep forwarding this sync request. */
		return 0;
	}

	if (v->piv_map.piv_pool_buf.pb_nr > 0)
		map_buf = &v->piv_map.piv_pool_buf;

	rc = ds_pool_tgt_map_update(pool, map_buf, v->piv_pool_map_ver);

	ds_pool_put(pool);
	return rc;
}

struct ds_iv_class_ops pool_iv_ops = {
	.ivc_ent_init		= pool_iv_ent_init,
	.ivc_ent_get		= pool_iv_ent_get,
	.ivc_ent_put		= pool_iv_ent_put,
	.ivc_ent_destroy	= pool_iv_ent_destroy,
	.ivc_ent_fetch		= pool_iv_ent_fetch,
	.ivc_ent_update		= pool_iv_ent_update,
	.ivc_ent_refresh	= pool_iv_ent_refresh,
	.ivc_value_alloc	= pool_iv_value_alloc,
	.ivc_pre_sync		= pool_iv_pre_sync
};

int
pool_iv_map_fetch(void *ns, struct pool_iv_entry *pool_iv)
{
	d_sg_list_t		sgl = { 0 };
	d_iov_t			iov = { 0 };
	uint32_t		pool_iv_len = 0;
	struct ds_iv_key	key;
	struct pool_iv_key	*pool_key;
	int			rc;

	/* pool_iv == NULL, it means only refreshing local IV cache entry,
	 * i.e. no need fetch the IV value for the caller.
	 */
	if (pool_iv != NULL) {
		pool_iv_len = pool_iv_map_ent_size(
				pool_iv->piv_map.piv_pool_buf.pb_nr);
		d_iov_set(&iov, pool_iv, pool_iv_len);
		sgl.sg_nr = 1;
		sgl.sg_nr_out = 0;
		sgl.sg_iovs = &iov;
	}

	memset(&key, 0, sizeof(key));
	key.class_id = IV_POOL_MAP;
	pool_key = (struct pool_iv_key *)key.key_buf;
	pool_key->pik_entry_size = pool_iv_len;
	rc = ds_iv_fetch(ns, &key, pool_iv == NULL ? NULL : &sgl,
			 false /* retry */);
	if (rc)
		D_ERROR("iv fetch failed "DF_RC"\n", DP_RC(rc));

	return rc;
}

static int
pool_iv_update(void *ns, int class_id, struct pool_iv_entry *pool_iv,
	       uint32_t pool_iv_len, unsigned int shortcut,
	       unsigned int sync_mode)
{
	d_sg_list_t		sgl;
	d_iov_t			iov;
	struct ds_iv_key	key;
	struct pool_iv_key	*pool_key;
	int			rc;

	iov.iov_buf = pool_iv;
	iov.iov_len = pool_iv_len;
	iov.iov_buf_len = pool_iv_len;
	sgl.sg_nr = 1;
	sgl.sg_nr_out = 0;
	sgl.sg_iovs = &iov;

	memset(&key, 0, sizeof(key));
	key.class_id = class_id;
	pool_key = (struct pool_iv_key *)key.key_buf;
	pool_key->pik_entry_size = pool_iv_len;
	rc = ds_iv_update(ns, &key, &sgl, shortcut, sync_mode, 0,
			  true /* retry */);
	if (rc)
		D_ERROR("iv update failed "DF_RC"\n", DP_RC(rc));

	return rc;
}

int
ds_pool_iv_map_update(struct ds_pool *pool, struct pool_buf *buf,
		      uint32_t map_ver)
{
	struct pool_iv_entry	*iv_entry;
	uint32_t		 size;
	int			 rc;

	D_DEBUG(DB_TRACE, DF_UUID": map_ver=%u\n", DP_UUID(pool->sp_uuid),
		map_ver);

	size = pool_iv_map_ent_size(buf->pb_nr);
	D_ALLOC(iv_entry, size);
	if (iv_entry == NULL)
		return -DER_NOMEM;

	crt_group_rank(pool->sp_group, &iv_entry->piv_master_rank);
	uuid_copy(iv_entry->piv_pool_uuid, pool->sp_uuid);
	iv_entry->piv_pool_map_ver = map_ver;
	memcpy(&iv_entry->piv_map.piv_pool_buf, buf, pool_buf_size(buf->pb_nr));

	/* FIXME: Let's update the pool map synchronously for the moment,
	 * since there is no easy way to free the iv_entry buffer. Needs
	 * to revisit here once pool/cart_group/IV is upgraded.
	 */
	rc = pool_iv_update(pool->sp_iv_ns, IV_POOL_MAP, iv_entry, size,
			    CRT_IV_SHORTCUT_NONE, CRT_IV_SYNC_EAGER);
	if (rc != 0)
		D_DEBUG(DB_TRACE, DF_UUID": map_ver=%u: %d\n",
			DP_UUID(pool->sp_uuid), map_ver, rc);

	D_FREE(iv_entry);
	return rc;
}

static int
pool_iv_map_invalidate(void *ns, unsigned int shortcut, unsigned int sync_mode)
{
	struct ds_iv_key	key = { 0 };
	int			rc;

	key.class_id = IV_POOL_MAP;
	rc = ds_iv_invalidate(ns, &key, shortcut, sync_mode, 0,
			      false /* retry */);
	if (rc)
		D_ERROR("iv invalidate failed "DF_RC"\n", DP_RC(rc));

	return rc;
}

/* ULT to refresh pool map version */
void
ds_pool_map_refresh_ult(void *arg)
{
	struct pool_map_refresh_ult_arg	*iv_arg = arg;
	struct ds_pool			*pool;
	d_rank_t			 rank;
	int				 rc = 0;

	/* Pool IV fetch should only be done in xstream 0 */
	D_ASSERT(dss_get_module_info()->dmi_xs_id == 0);
	pool = ds_pool_lookup(iv_arg->iua_pool_uuid);
	if (pool == NULL) {
		rc = -DER_NONEXIST;
		goto out;
	}

	rc = crt_group_rank(pool->sp_group, &rank);
	if (rc)
		goto out;

	if (rank == pool->sp_iv_ns->iv_master_rank) {
		D_WARN("try to refresh pool map on pool leader\n");
		goto out;
	}

	/* If there are already refresh going on, let's wait
	 * until the refresh is done.
	 */
	ABT_mutex_lock(pool->sp_iv_refresh_lock);
	if (pool->sp_map_version >= iv_arg->iua_pool_version &&
	    pool->sp_map != NULL &&
	    !DAOS_FAIL_CHECK(DAOS_FORCE_REFRESH_POOL_MAP)) {
		D_DEBUG(DB_TRACE, "current pool version %u >= %u\n",
			pool_map_get_version(pool->sp_map),
			iv_arg->iua_pool_version);
		goto unlock;
	}

	/* Invalidate the local pool IV cache, then call pool_iv_map_fetch to
	 * refresh local pool IV cache, which will update the local pool map
	 * see pool_iv_ent_refresh()
	 */
	rc = pool_iv_map_invalidate(pool->sp_iv_ns, CRT_IV_SHORTCUT_NONE,
				    CRT_IV_SYNC_NONE);
	if (rc)
		goto unlock;

	rc = pool_iv_map_fetch(pool->sp_iv_ns, NULL);

unlock:
	ABT_mutex_unlock(pool->sp_iv_refresh_lock);
out:
	if (pool != NULL)
		ds_pool_put(pool);
	if (iv_arg->iua_eventual)
		ABT_eventual_set(iv_arg->iua_eventual, (void *)&rc, sizeof(rc));
	D_FREE_PTR(iv_arg);
}

int
ds_pool_iv_prop_update(struct ds_pool *pool, daos_prop_t *prop)
{
	struct pool_iv_entry	*iv_entry;
	struct daos_prop_entry	*prop_entry;
	d_rank_list_t		*svc_list;
	uint32_t		 size;
	int			 rc;

	/* Only happens on xstream 0 */
	D_ASSERT(dss_get_module_info()->dmi_xs_id == 0);

	/* Serialize the prop */
	prop_entry = daos_prop_entry_get(prop, DAOS_PROP_PO_SVC_LIST);
	svc_list = prop_entry->dpe_val_ptr;

	size = pool_iv_prop_ent_size(DAOS_ACL_MAX_ACE_LEN, svc_list->rl_nr);
	D_ALLOC(iv_entry, size);
	if (iv_entry == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	crt_group_rank(pool->sp_group, &iv_entry->piv_master_rank);
	uuid_copy(iv_entry->piv_pool_uuid, pool->sp_uuid);
	pool_iv_prop_l2g(prop, &iv_entry->piv_prop);

	rc = pool_iv_update(pool->sp_iv_ns, IV_POOL_PROP, iv_entry, size,
			    CRT_IV_SHORTCUT_NONE, CRT_IV_SYNC_EAGER);
	if (rc)
		D_ERROR("pool_iv_update failed "DF_RC"\n", DP_RC(rc));
	D_FREE(iv_entry);

out:
	return rc;
}

int
ds_pool_iv_prop_fetch(struct ds_pool *pool, daos_prop_t *prop)
{
	daos_prop_t		*prop_fetch = NULL;
	struct pool_iv_entry	*iv_entry;
	d_sg_list_t		 sgl = { 0 };
	d_iov_t			 iov = { 0 };
	struct ds_iv_key	 key;
	uint32_t		 size;
	int			 rc;

	if (prop == NULL)
		return -DER_INVAL;

	size = pool_iv_prop_ent_size(DAOS_ACL_MAX_ACE_LEN,
				     PROP_SVC_LIST_MAX_TMP);
	D_ALLOC(iv_entry, size);
	if (iv_entry == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	prop_fetch = daos_prop_alloc(DAOS_PROP_PO_NUM);
	if (prop_fetch == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	d_iov_set(&iov, iv_entry, size);
	sgl.sg_nr = 1;
	sgl.sg_nr_out = 0;
	sgl.sg_iovs = &iov;

	memset(&key, 0, sizeof(key));
	key.class_id = IV_POOL_PROP;
	rc = ds_iv_fetch(pool->sp_iv_ns, &key, &sgl, false /* retry */);
	if (rc) {
		D_ERROR("iv fetch failed "DF_RC"\n", DP_RC(rc));
		D_GOTO(out, rc);
	}

	rc = pool_iv_prop_g2l(&iv_entry->piv_prop, prop_fetch);
	if (rc) {
		D_ERROR("pool_iv_prop_g2l failed "DF_RC"\n", DP_RC(rc));
		D_GOTO(out, rc);
	}

	rc = daos_prop_copy(prop, prop_fetch);
	if (rc) {
		D_ERROR("daos_prop_copy failed "DF_RC"\n", DP_RC(rc));
		D_GOTO(out, rc);
	}

out:
	D_FREE(iv_entry);
	daos_prop_free(prop_fetch);
	return rc;
}

int
ds_pool_iv_init(void)
{
	int	rc;

	rc = ds_iv_class_register(IV_POOL_MAP, &iv_cache_ops, &pool_iv_ops);
	if (rc)
		return rc;

	rc = ds_iv_class_register(IV_POOL_PROP, &iv_cache_ops, &pool_iv_ops);
	if (rc) {
		ds_iv_class_unregister(IV_POOL_MAP);
		return rc;
	}

	return rc;
}

int
ds_pool_iv_fini(void)
{
	ds_iv_class_unregister(IV_POOL_MAP);
	ds_iv_class_unregister(IV_POOL_PROP);

	return 0;
}
