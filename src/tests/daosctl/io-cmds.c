/**
 * (C) Copyright 2018-2020 Intel Corporation.
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

/* generic */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mpi.h>
#include <stdint.h>
#include <inttypes.h>
#include <argp.h>
#include <uuid/uuid.h>

/* daos specific */
#include <daos.h>
#include <daos_api.h>
#include <daos_types.h>
#include <tests_lib.h>
#include <daos_mgmt.h>
#include <daos/common.h>
#include <daos/checksum.h>

#include "common_utils.h"

/**
 * This file contains commands that do some basic I/O operations.
 *
 * For each command there are 3 items of interest: a structure that
 * contains the arguments for the command; a callback function that
 * takes the arguments from argp and puts them in the structure; a
 * function that sends the arguments to the DAOS API and handles the
 * reply.  All commands share the same structure and callback function
 * at present.
 */

static int dts_obj_class = OC_S1;

struct io_cmd_options {
	char		*server_group;
	char		*pool_uuid;
	char		*cont_uuid;
	char		*server_list;
	uint64_t	 size;
	daos_obj_id_t	*oid;
	char		*pattern;

	daos_recx_t	 recx;
	char		*string;
	int		 obj_class;
	bool		 fault;
	int		 rank_to_fault; /** -1 = all ranks */
	int		 type;
	char		*akey;
	char		*dkey;
};

#define UPDATE_CSUM_SIZE	32
#define IOREQ_IOD_NR	5
#define IOREQ_SG_NR	5
#define IOREQ_SG_IOD_NR	5

#define TEST_PATTERN_SIZE 64

static const unsigned char PATTERN_0[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0};

static const unsigned char PATTERN_1[] = {
	0, 1, 2, 3, 4, 5, 6, 7,
	8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23,
	24, 25, 26, 27, 28, 29, 30, 31,
	32, 33, 34, 35, 36, 37, 38, 39,
	40, 41, 42, 43, 44, 45, 46, 47,
	48, 49, 50, 51, 52, 53, 54, 55,
	56, 57, 58, 59, 60, 61, 62, 63};

struct ioreq {
	daos_handle_t		oh;
	daos_event_t		ev;
	daos_key_t		dkey;
	daos_key_t		akey;
	d_iov_t			val_iov[IOREQ_SG_IOD_NR][IOREQ_SG_NR];
	d_sg_list_t		sgl[IOREQ_SG_IOD_NR];
	daos_recx_t		rex[IOREQ_SG_IOD_NR][IOREQ_IOD_NR];
	daos_epoch_range_t	erange[IOREQ_SG_IOD_NR][IOREQ_IOD_NR];
	daos_iod_t		iod[IOREQ_SG_IOD_NR];
	daos_iod_type_t		iod_type;
	uint64_t		fail_loc;
};

struct container_info {
	uuid_t         pool_uuid;
	char           *server_group;
	d_rank_list_t  pool_service_list;
	daos_handle_t  poh;
	uuid_t         cont_uuid;
	daos_handle_t  coh;
};

static int parseValueType(char *arg)
{
	if (strcmp(arg, "array") == 0)
		return DAOS_IOD_ARRAY;
	return DAOS_IOD_SINGLE;
}

static int
parseObjType(char *arg)
{
	if (strcmp(arg, "OC_SX") == 0)
		return OC_SX;
	if (strcmp(arg, "OC_RP_2G1") == 0) {
		printf("******Replica Enabled\n");
		return OC_RP_2GX;
	}


	return OC_SX;
}

int
ioreq_init(struct ioreq *req, daos_handle_t coh, daos_obj_id_t oid,
	   daos_iod_type_t iod_type)
{
	int rc;
	int i;

	memset(req, 0, sizeof(*req));

	req->iod_type = iod_type;

	/* init sgl */
	for (i = 0; i < IOREQ_SG_IOD_NR; i++) {
		req->sgl[i].sg_nr = IOREQ_SG_NR;
		req->sgl[i].sg_iovs = req->val_iov[i];
	}

	/* init record extent */
	for (i = 0; i < IOREQ_SG_IOD_NR; i++) {
		int j;

		for (j = 0; j < IOREQ_IOD_NR; j++) {
			req->rex[i][j].rx_nr = 1;
			req->rex[i][j].rx_idx = 0;

			/** epoch range: required by the wire format */
			req->erange[i][j].epr_lo = 0;
			req->erange[i][j].epr_hi = DAOS_EPOCH_MAX;
		}

		/* I/O descriptor */
		req->iod[i].iod_recxs = req->rex[i];
		req->iod[i].iod_nr = IOREQ_IOD_NR;
		req->iod[i].iod_type = iod_type;
	}
	D_DEBUG(DF_MISC, "open oid="DF_OID"\n", DP_OID(oid));

	/** open the object */
	rc = daos_obj_open(coh, oid, 0, &req->oh, NULL);
	return rc;
}

static void
ioreq_dkey_set(struct ioreq *req, const char *dkey)
{
	d_iov_set(&req->dkey, (void *)dkey, strlen(dkey));
}

static void
ioreq_io_akey_set(struct ioreq *req, const char **akey, int nr)
{
	int i;

	if (nr < 1 || nr > IOREQ_SG_IOD_NR) {
		printf("Invalid request, nr is %i\n", nr);
		exit(-EINVAL);
	}
	/** akey */
	for (i = 0; i < nr; i++)
		d_iov_set(&req->iod[i].iod_name, (void *)akey[i],
			     strlen(akey[i]));
}

static int
open_container(struct container_info *oc_info)
{
	int              rc;
	unsigned int     flag = DAOS_PC_EX;
	daos_pool_info_t pinfo = {0};
	daos_cont_info_t cinfo;

	rc = daos_pool_connect(oc_info->pool_uuid, oc_info->server_group,
			       &oc_info->pool_service_list, flag,
			       &oc_info->poh, &pinfo, NULL);
	if (rc) {
		printf("Pool connect fail, result: %d\n", rc);
		exit(-EINVAL);
	}

	rc = daos_cont_open(oc_info->poh, oc_info->cont_uuid, DAOS_COO_RW,
			    &oc_info->coh, &cinfo, NULL);
	if (rc) {
		printf("daos_cont_open failed, rc: %d\n", rc);
		daos_pool_disconnect(oc_info->poh, NULL);
		exit(-EINVAL);
	}
	return 0;
}

void
ioreq_fini(struct ioreq *req)
{
	int rc;

	rc = daos_obj_close(req->oh, NULL);
	if (rc != 0)
		printf("problem closing object %i\n", rc);
	daos_fail_loc_set(0);

}

/* no wait for async insert, for sync insert it still will block */
static int
insert_internal_nowait(daos_key_t *dkey, int nr, d_sg_list_t *sgls,
		       daos_iod_t *iods, daos_handle_t th, struct ioreq *req)
{
	int rc;

	/** execute update operation */
	rc = daos_obj_update(req->oh, th, 0, dkey, nr, iods, sgls, NULL);

	return rc;
}

static void
lookup_internal(daos_key_t *dkey, int nr, d_sg_list_t *sgls,
		daos_iod_t *iods, daos_handle_t th, struct ioreq *req,
		bool empty)
{
	int rc;


	/** execute fetch operation */
	rc = daos_obj_fetch(req->oh, th, 0, dkey, nr, iods, sgls, NULL, NULL);
	if (rc != 0) {
		printf("object fetch failed with %i\n", rc);
		exit(1);
	}

	/* Only single iov for each sgls during the test */
	if (!empty && req->ev.ev_error == 0)
		if (sgls->sg_nr_out != 1) {
			printf("something went wrong, I don't know what\n");
			exit(1);
		}
}

static void
ioreq_sgl_simple_set(struct ioreq *req, void **value,
		     daos_size_t *size, int nr)
{
	d_sg_list_t *sgl = req->sgl;
	int i;

	if (nr < 1 || nr > IOREQ_SG_IOD_NR) {
		printf("Invalid request, nr is %i\n", nr);
		exit(-EINVAL);
	}
	for (i = 0; i < nr; i++) {
		sgl[i].sg_nr = 1;
		sgl[i].sg_nr_out = 1;
		d_iov_set(&sgl[i].sg_iovs[0], value[i], size[i]);
	}
}

static void
ioreq_iod_simple_set(struct ioreq *req, daos_size_t *size, bool lookup,
		     uint64_t *idx, int nr)
{
	daos_iod_t *iod = req->iod;
	int i;

	if (nr < 1 || nr > IOREQ_SG_IOD_NR) {
		printf("Invalid request, nr is %i\n", nr);
		exit(-EINVAL);
	}

	for (i = 0; i < nr; i++) {
		/* record extent */
		iod[i].iod_type = req->iod_type;
		iod[i].iod_size = size[i];
		if (req->iod_type == DAOS_IOD_ARRAY) {
			iod[i].iod_recxs[0].rx_idx = idx[i] + i * 10485760;
			iod[i].iod_recxs[0].rx_nr = 1;
		}
		iod[i].iod_nr = 1;
	}
}

static void
insert_single(const char *dkey, const char *akey, uint64_t idx,
	      void *value, daos_size_t size, daos_handle_t th,
	      struct ioreq *req)
{
	int nr = 1;

	/* dkey */
	ioreq_dkey_set(req, dkey);

	/* akey */
	ioreq_io_akey_set(req, &akey, nr);

	/* set sgl */
	if (value != NULL)
		ioreq_sgl_simple_set(req, &value, &size, nr);

	/* set iod */
	ioreq_iod_simple_set(req, &size, false, &idx, nr);

	int rc = insert_internal_nowait(&req->dkey, nr,
					value == NULL ? NULL : req->sgl,
					req->iod, th, req);

	if (rc != 0)
		printf("object update failed \n");
}

void
lookup_single(const char *dkey, const char *akey, uint64_t idx,
	      void *val, daos_size_t size, daos_handle_t th,
	      struct ioreq *req)
{
	/*daos_size_t read_size = DAOS_REC_ANY;*/
	daos_size_t read_size = 128;

	fflush(stdout);
	/* dkey */
	ioreq_dkey_set(req, dkey);
	/* akey */
	ioreq_io_akey_set(req, &akey, 1);
	/* set sgl */
	ioreq_sgl_simple_set(req, &val, &size, 1);

	/* set iod */
	ioreq_iod_simple_set(req, &read_size, true, &idx, 1);
	lookup_internal(&req->dkey, 1, req->sgl, req->iod, th, req,
			false);
}

static int
open_container_from_args(struct io_cmd_options *io_options,
	struct container_info *cinfo)
{
	int rc;
	char		*pool_uuid_str = NULL;

	if (io_options->pool_uuid == NULL) {
		D_ALLOC(pool_uuid_str, 100);
		if (get_pool(pool_uuid_str))
			io_options->pool_uuid = pool_uuid_str;
	}


	cinfo->server_group = io_options->server_group;
	cinfo->pool_service_list = (d_rank_list_t){NULL, 0};

	/* uuid needs extra parsing */
	if (io_options->pool_uuid == NULL)
		return -EINVAL;
	rc = uuid_parse(io_options->pool_uuid, cinfo->pool_uuid);
	D_FREE(pool_uuid_str);


	if (io_options->cont_uuid == NULL)
		return -EINVAL;
	rc = uuid_parse(io_options->cont_uuid, cinfo->cont_uuid);
	if (rc < 0) {
		D_PRINT("uuid_parse failed with %i\n", rc);
		return rc;
	}
	/* turn the list of pool service nodes into a rank list */
	rc = parse_rank_list(io_options->server_list,
			     &cinfo->pool_service_list);
	if (rc < 0) {
		D_PRINT("Rank list parameter parsing failed with %i\n", rc);
		return rc;
	}
	if (cinfo->pool_service_list.rl_nr == 0)
		cinfo->pool_service_list.rl_nr = 1;

	rc = open_container(cinfo);
	return rc;
}



static struct io_cmd_options io_options_default = {
	"daos_server",
	NULL, "12345678-1234-1234-1234-123456789012",
	NULL,
	0, NULL, "all_zeros",
	.type = DAOS_IOD_SINGLE,
	.akey = "unspecified akey",
	.dkey = "unspecified dkey",
	.obj_class = OC_SX,

};

static void
create_oid(daos_obj_id_t *oid, int obj_class) {
	(*oid).lo	= 1;
	(*oid).hi	= 0;
	daos_obj_generate_id(oid, 0, obj_class, 0);

}

enum arg_options {
	OPT_OBJECT_TYPE	= 0x1234,
	OPT_AKEY	= 0x1235,
	OPT_DKEY	= 0x1236,
	OPT_VALUE_TYPE	= 0x1237,
};

/**
 * Callback function for io commands works with argp to put
 * all the arguments into a structure.
 */
static int
parse_cont_args_cb(int key, char *arg,
		   struct argp_state *state)
{
	struct io_cmd_options *options = state->input;

	switch (key) {
		case 'c':
			options->cont_uuid = arg;
			break;
		case 'i':
			options->pool_uuid = arg;
			break;
		case 'l':
			options->server_list = arg;
			break;
		case 'o':
			parse_oid(arg, options->oid);
			break;
		case 'p':
			options->pattern = arg;
			break;
		case 's':
			options->server_group = arg;
			break;
		case 'z':
			parse_size(arg, &(options->size));
			break;
		case 'v':
			options->string = arg;
			break;
		case 'd':
			options->recx.rx_idx = atoi(arg);
			break;
		case 'h':
			options->recx.rx_nr = atoi(arg);
			break;
		case 'x':
			options->fault = true;
			options->rank_to_fault = atoi(arg);
			break;
		case OPT_OBJECT_TYPE:
			options->obj_class = daos_oclass_name2id(arg);
//			options->obj_class = parseObjType(arg);
			break;
		case OPT_AKEY:
			options->akey = arg;
			break;
		case OPT_DKEY:
			options->dkey = arg;
			break;
		case OPT_VALUE_TYPE:
			options->type = parseValueType(arg);
			break;
	}
	return 0;
}

void print_pkey(daos_obj_id_t oid, char *dkey, char *akey)
{
	char obj_class_name[100];
	daos_oclass_id2name(daos_obj_id2class(oid), obj_class_name);
	printf("object %lu (ver: %hhu, feat: %hu, class: %s), dkey: '%s', akey: '%s'\n",
	       oid.lo,
	       daos_obj_id2ver(oid),
	       daos_obj_id2feat(oid),
	       obj_class_name,
	       dkey, akey);
}

/**
 * Process a write command.
 */
int
cmd_write_string(int argc, const char **argv, void *ctx)
{
	int		 rc = -ENXIO;
	daos_obj_id_t	 oid;
	daos_key_t	 dkey;
	char		*string_cpy = NULL;

	struct argp_option options[] = {
		{"server-group", 's', "SERVER-GROUP", 0,
			"ID of the server group that owns the pool"},
		{"servers",       'l',   "server rank-list", 0,
			"Pool service ranks, comma separated, no spaces e.g. -l 1,2"},
		{"p-uuid", 'i', "UUID", 0,
			"ID of the pool where data is to be written."},
		{"c-uuid", 'c', "UUID", 0,
			"ID of the container where data is to be written."},
		{"string",           'v',    "string value",             0,
			"String to write to an extent"},
		{"index",       'd',   "index",           0,
			"Starting index of the extent to write the string "},
		{"fault", 'x', "rank", 0, "Corrupt data"},
		{"value-type", 't', "single(default)|array", 0,
			"Store the array as a single value or as an array of single bytes"},
		/* [todo-ryon]: more object classes */
		{"object-type", OPT_OBJECT_TYPE, "OC_SX(default)|OC_RP_2G1", 0,
			"Object class to create for the value."},
		{"akey", OPT_AKEY, "akey", 0,
			"akey to store the value under"},
		{"dkey", OPT_DKEY, "dkey", 0,
			"dkey to store the value under"},
		{0}
	};
	struct argp argp = {options, parse_cont_args_cb};

	struct io_cmd_options io_options = io_options_default;
	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &io_options);

	struct container_info cinfo;

	rc = open_container_from_args(&io_options, &cinfo);
	if (rc != 0) {
		printf("Container Open Failed: %d\n", rc);
		goto out;
	}

	size_t str_len = strlen(io_options.string) + 1;
	io_options.recx.rx_nr = str_len;

	daos_iod_t iod;

	if (io_options.type == DAOS_IOD_SINGLE) {
		iod.iod_size = str_len;
	} else {
		iod.iod_size = 1;
		iod.iod_recxs = &io_options.recx;
	}
	iod.iod_type = io_options.type;
	iod.iod_nr = 1;

	iod.iod_name.iov_buf = (void *)io_options.akey;
	iod.iod_name.iov_len = iod.iod_name.iov_buf_len = strlen(io_options.akey);
	dkey.iov_len = dkey.iov_buf_len = strlen(io_options.dkey);
	dkey.iov_buf = (void *)io_options.dkey;

	/** copy string in case there's any corruption on update */
	D_ALLOC(string_cpy, str_len);
	strcpy(string_cpy, io_options.string);

	d_sg_list_t sgl;
	daos_sgl_init(&sgl, 1);
	sgl.sg_iovs->iov_buf = string_cpy;
	sgl.sg_iovs->iov_len = sgl.sg_iovs->iov_buf_len = str_len;

	create_oid(&oid, io_options.obj_class);
	daos_handle_t oh;
	rc = daos_obj_open(cinfo.coh, oid, 0, &oh, NULL);
	if (rc != 0) {
		printf("Object Open Error: %d\n", rc);
		goto out;
	}

	if (io_options.fault) {
		/** where to fault */
//		daos_fail_loc_set(DAOS_OBJ_SPECIAL_SHARD);
//		daos_fail_value_set(1);
		printf("Faulting network to rank: %d\n", io_options.rank_to_fault);
		daos_mgmt_set_params(io_options.server_group,
				     io_options.rank_to_fault,
				     DMG_KEY_FAIL_LOC,
				     DAOS_CHECKSUM_FAULT_NETWORK | DAOS_FAIL_ALWAYS,
				     0, NULL);
	}

	rc = daos_obj_update(oh, DAOS_TX_NONE, 0, &dkey, 1, &iod, &sgl, NULL);

	if (rc != 0) {
		printf("Update Error: %d\n", rc);
		goto out;
	}

	printf("'%s' written to: ", io_options.string);
	print_pkey(oid, io_options.dkey, io_options.akey);


out:
	daos_mgmt_set_params(io_options.server_group, -1, DMG_KEY_FAIL_LOC,
			     0,
			     0, NULL);

	daos_obj_close(oh, NULL);
	daos_cont_close(cinfo.coh, NULL);
	D_FREE(string_cpy);

	if (cinfo.poh.cookie != 0)
		daos_pool_disconnect(cinfo.poh, NULL);
	return rc;
}

/**
 * Process a read command.
 */
int
cmd_read_string(int argc, const char **argv, void *ctx)
{
	int              rc = -ENXIO;
	daos_obj_id_t    oid;
//	const char	 dkey_str[] = "dkey";
	daos_key_t	 dkey;
//	const char	 akey_str[] = "akey";

	struct container_info cinfo;

	struct argp_option options[] = {
		{"server-group", 's', "SERVER-GROUP", 0,
			"ID of the server group that owns the pool"},
		{"servers",       'l',   "server rank-list", 0,
			"Pool service ranks, comma separated, no spaces e.g. -l 1,2"},
		{"p-uuid", 'i', "UUID", 0,
			"ID of the pool where data is to be written."},
		{"c-uuid", 'c', "UUID", 0,
			"ID of the container where data is to be written."},
		{"index",       'd',   "index",           0,
			"Starting index of the extent to write the string "},
		{"length",       'h',   "length",           0,
			"Number of characters to read"},
		{"fault", 'x', NULL, 0,
			"Corrupt data"},
		{"type", 't', "single(default)|array", 0,
			"Store the array as a single value or as an array of single bytes"},
		{"object-type", OPT_OBJECT_TYPE, "OC_SX(default)|OC_RP_2G1", 0,
			"Object class to create for the value."},
		{"akey", OPT_AKEY, "akey", 0,
			"akey to store the value under"},
		{"dkey", OPT_DKEY, "dkey", 0,
			"dkey to store the value under"},


		{0}
	};
	struct argp argp = {options, parse_cont_args_cb};

	struct io_cmd_options io_options = io_options_default;

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &io_options);

	rc = open_container_from_args(&io_options, &cinfo);

	if (rc != 0) {
		printf("Container Open Failed: %d\n", rc);
		goto out;
	}

	create_oid(&oid, io_options.obj_class);

	/** Read */

	daos_handle_t oh;
	rc = daos_obj_open(cinfo.coh, oid, 0, &oh, NULL);
	if (rc != 0) {
		printf("Object Open Error: %d\n", rc);
		goto out;
	}

	char *buf = NULL;
#define BUF_SIZE 1024
	D_ALLOC(buf, BUF_SIZE);

	d_sg_list_t sgl;
	daos_sgl_init(&sgl, 1);

	sgl.sg_iovs->iov_buf = buf;
	sgl.sg_iovs->iov_len = sgl.sg_iovs->iov_buf_len = BUF_SIZE;

	daos_iod_t iod;
	iod.iod_nr = 1;

	if (io_options.type == DAOS_IOD_SINGLE) {
		iod.iod_size = BUF_SIZE;
	} else {
		iod.iod_size = 1;
		iod.iod_recxs = &io_options.recx;
	}
	iod.iod_type = io_options.type;

	/** Setup Keys */
	iod.iod_name.iov_buf = (void *)io_options.akey;
	iod.iod_name.iov_len = iod.iod_name.iov_buf_len = strlen(io_options.akey);
	dkey.iov_len = dkey.iov_buf_len = strlen(io_options.dkey);
	dkey.iov_buf = (void *)io_options.dkey;


	if (io_options.fault)
		daos_fail_loc_set(DAOS_CHECKSUM_FETCH_FAIL);

	rc = daos_obj_fetch(oh, DAOS_TX_NONE, 0, &dkey, 1, &iod, &sgl, NULL,
		NULL);
	if (rc != 0) {
		printf("Fetch Failed: %d\n", rc);
		goto out;
	}

	printf("Read value from object: %" PRIu64 "-%" PRIu64 ", dkey: '%s', akey: '%s'\n'%s'\n",
	       oid.hi, oid.lo, io_options.dkey, io_options.akey, buf);

out:
	daos_obj_close(oh, NULL);
	/** done with the container */
	daos_cont_close(cinfo.coh, NULL);

	if (cinfo.poh.cookie != 0)
		daos_pool_disconnect(cinfo.poh, NULL);
	return rc;
}

/**
 * Process a write command.
 */
int
cmd_write_pattern(int argc, const char **argv, void *ctx)
{
	int              rc = -ENXIO;
	daos_obj_id_t    oid;
	struct ioreq	 req;
	const char	 dkey[] = "test_update dkey";
	const char	 akey[] = "test_update akey";

	const unsigned char *rec = PATTERN_1;

	struct container_info cinfo;

	struct argp_option options[] = {
		{"server-group", 's', "SERVER-GROUP", 0,
		 "ID of the server group that owns the pool"},
		{"servers",       'l',   "server rank-list", 0,
		 "Pool service ranks, comma separated, no spaces e.g. -l 1,2"},
		{"p-uuid", 'i', "UUID", 0,
		 "ID of the pool where data is to be written."},
		{"c-uuid", 'c', "UUID", 0,
		 "ID of the container where data is to be written."},
		{"size",           'z',    "size",             0,
		 "How much to write in bytes or with k/m/g (e.g. 10g)"},
		{"pattern",       'p',   "pattern",           0,
		 "Data pattern to be written, one of: [0, 1]"},
		{0}
	};
	struct argp argp = {options, parse_cont_args_cb};

	struct io_cmd_options io_options = {"daos_server",
					    NULL, NULL, NULL,
					    0, NULL, "all_zeros"};

	cinfo.server_group = io_options.server_group;
	cinfo.pool_service_list = (d_rank_list_t){NULL, 0};

	cinfo.server_group = io_options.server_group;
	cinfo.pool_service_list = (d_rank_list_t){NULL, 0};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &io_options);

	/* uuid needs extra parsing */
	if (io_options.pool_uuid == NULL)
		return -EINVAL;
	rc = uuid_parse(io_options.pool_uuid, cinfo.pool_uuid);
	if (io_options.cont_uuid == NULL)
		return -EINVAL;
	rc = uuid_parse(io_options.cont_uuid, cinfo.cont_uuid);

	/* turn the list of pool service nodes into a rank list */
	rc = parse_rank_list(io_options.server_list,
			     &cinfo.pool_service_list);
	if (rc < 0) {
		D_PRINT("Rank list parameter parsing failed with %i\n", rc);
		return rc;
	}

	rc = open_container(&cinfo);

	oid = dts_oid_gen(dts_obj_class, 0, 0);

	if (!strncmp("all_zeros", io_options.pattern, 3))
		rec = PATTERN_0;
	else if (!strncmp("sequential", io_options.pattern, 3))
		rec = PATTERN_1;

	ioreq_init(&req, cinfo.coh, oid, DAOS_IOD_SINGLE);

	/** Insert */
	insert_single(dkey, akey, 0, (void *)rec, 64, DAOS_TX_NONE, &req);

	/** done with the container */
	daos_cont_close(cinfo.coh, NULL);
	printf("%" PRIu64 "-%" PRIu64 "\n", oid.hi, oid.lo);

	if (cinfo.poh.cookie != 0)
		daos_pool_disconnect(cinfo.poh, NULL);
	return rc;
}

/**
 * Read data written with the write-pattern command and verify
 * that its correct.
 */
int
cmd_verify_pattern(int argc, const char **argv, void *ctx)
{
	char buf[128];
	int rc = 0;
	struct container_info cinfo;
	struct ioreq	 req;
	const char	 dkey[] = "test_update dkey";
	const char	 akey[] = "test_update akey";

	struct argp_option options[] = {
		{"server-group", 's', "SERVER-GROUP", 0,
		 "ID of the server group that owns the pool"},
		{"servers",       'l',   "server rank-list", 0,
		 "pool service ranks, comma separated, no spaces e.g. -l 1,2"},
		{"p-uuid", 'i', "UUID", 0,
		 "ID of the pool that hosts the container to be read from."},
		{"c-uuid", 'c', "UUID", 0,
		 "ID of the container."},
		{"oid", 'o', "OID", 0, "ID of the object."},
		{"size",           'z',    "size",             0,
		 "how much to read in bytes or with k/m/g (e.g. 10g)"},
		{"pattern",       'p',   "pattern",           0,
		 "which of the available data patterns to verify"},
		{0}
	};
	daos_obj_id_t	 oid;
	struct argp argp = {options, parse_cont_args_cb};
	struct io_cmd_options io_options = {"daos_server",
					    NULL, NULL, NULL,
					    0, &oid, "all_zeros"};

	cinfo.server_group = io_options.server_group;
	cinfo.pool_service_list = (d_rank_list_t){NULL, 0};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &io_options);

	/* uuid needs extra parsing */
	if (io_options.pool_uuid == NULL)
		return -EINVAL;
	rc = uuid_parse(io_options.pool_uuid, cinfo.pool_uuid);
	if (io_options.cont_uuid == NULL)
		return -EINVAL;
	rc = uuid_parse(io_options.cont_uuid, cinfo.cont_uuid);

	/* turn the list of pool service nodes into a rank list */
	rc = parse_rank_list(io_options.server_list,
			     &cinfo.pool_service_list);
	if (rc < 0) {
		D_PRINT("Rank list parameter parsing failed with %i\n", rc);
		return rc;
	}

	rc = open_container(&cinfo);

	printf("%" PRIu64 "-%" PRIu64 "\n", oid.hi, oid.lo);
	ioreq_init(&req, cinfo.coh, oid, DAOS_IOD_SINGLE);

	memset(buf, 0, sizeof(buf));
	lookup_single(dkey, akey, 0, buf, sizeof(buf), DAOS_TX_NONE, &req);

	/** Verify data consistency */
	printf("size = %lu\n", req.iod[0].iod_size);
	if (req.iod[0].iod_size != TEST_PATTERN_SIZE) {
		printf("sizes don't match\n");
		exit(1);
	}

	for (int i = 0; i < TEST_PATTERN_SIZE; i++) {
		if (buf[i] != PATTERN_1[i]) {
			printf("Data mismatch at position %i value %i",
			       i, buf[i]);
			break;
		}
	}

	ioreq_fini(&req);
	if (cinfo.poh.cookie != 0)
		daos_pool_disconnect(cinfo.poh, NULL);

	return rc;
}
int
cmd_list_obj_class(int argc, const char **argv, void *ctx)
{
	int rc = 0;

	int str_len = 1024;
	char str[str_len];
	rc = daos_oclass_names_list(str_len, str);
	if (rc > str_len) {
		printf("Need to increase str_len ...\n");
	}

	printf("%s\n", str);

	return rc;
}
