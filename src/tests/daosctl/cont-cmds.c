/**
 * (C) Copyright 2018-2019 Intel Corporation.
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
#include "common_utils.h"
#include <daos.h>
#include <daos_api.h>
#include <daos_mgmt.h>
#include <daos/common.h>

/**
 * This file contains the container related user commands.
 *
 * For each command there are 3 items of interest: a structure that
 * contains the arguments for the command; a callback function that
 * takes the arguments from argp and puts them in the structure; a
 * function that sends the arguments to the DAOS API and handles the
 * reply.  All commands share the same structure and callback function
 * at present.
 */

struct container_cmd_options {
	char          *server_group;
	char          *pool_uuid;
	char          *cont_uuid;
	char          *server_list;
	unsigned int  force;
	unsigned int  mode;
	unsigned int  uid;
	unsigned int  gid;
	unsigned int  csum_type;
};

enum {
	CSUM_ARG_VAL_TYPE		= 0x2713,
	CSUM_ARG_VAL_CHUNKSIZE		= 0x2714,
	CSUM_ARG_VAL_SERVERVERIFY	= 0x2715,
};

static unsigned int parse_csum_type(char *arg) {
	if (strcmp(arg, "crc16") == 0)
		return DAOS_PROP_CO_CSUM_CRC16;
	if (strcmp(arg, "crc32") == 0)
		return DAOS_PROP_CO_CSUM_CRC32;
	if (strcmp(arg, "crc64") == 0)
		return DAOS_PROP_CO_CSUM_CRC64;

	return DAOS_PROP_CO_CSUM_OFF;
}

/**
 * Callback function for container commands works with argp to put
 * all the arguments into a structure.
 */
static int
parse_cont_args_cb(int key, char *arg,
		   struct argp_state *state)
{
	struct container_cmd_options *options = state->input;

	switch (key) {
	case 's':
		options->server_group = arg;
		break;
	case 'i':
		options->pool_uuid = arg;
		break;
	case 'c':
		options->cont_uuid = arg;
		break;
	case 'f':
		options->force = 1;
		break;
	case 'l':
		options->server_list = arg;
		break;
	case CSUM_ARG_VAL_TYPE:
		options->csum_type = parse_csum_type(arg);
		break;
	}
	return 0;
}

/**
 * Process a create container command.
 */
int
cmd_create_container(int argc, const char **argv, void *ctx)
{
	int              rc = -ENXIO;
	uuid_t           pool_uuid;
	uuid_t           cont_uuid;
	daos_handle_t    poh;
	d_rank_list_t    pool_service_list = {NULL, 0};
	unsigned int     flag = DAOS_PC_EX;
	daos_pool_info_t info = {0};
	char		 pool_uuid_str[100];
	daos_prop_t	*props = NULL;

	struct argp_option options[] = {
		{"server-group", 's', "SERVER-GROUP", 0,
		 "ID of the server group that owns the pool"},
		{"servers",       'l',   "server rank-list", 0,
		 "pool service ranks, comma separated, no spaces e.g. -l 1,2"},
		{"p-uuid", 'i', "UUID", 0,
		 "ID of the pool that is to host the new container."},
		{"c-uuid", 'c', "UUID", 0,
		 "ID of the container if a specific one is desired."},
		{"csum-type", CSUM_ARG_VAL_TYPE, "TYPE", 0,
		 "Checksum type for the container (crc16|crc32|crc64"},
		{0}
	};
	struct argp argp = {options, parse_cont_args_cb};
	struct container_cmd_options cc_options = {"daos_server",
						   NULL, "12345678-1234-1234-1234-123456789012",
						   NULL,
						   0, 0, 0, 0};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &cc_options);

	/* uuid needs extra parsing */

	if (cc_options.pool_uuid == NULL) {
		D_PRINT("[RYON] %s:%d [%s()] > \n", __FILE__, __LINE__, __FUNCTION__);
		if (get_pool(pool_uuid_str)){
			cc_options.pool_uuid = pool_uuid_str;
		}
	}
	printf("Creating container on pool: %s\nServer Group: %s\n", cc_options.pool_uuid,
	       cc_options.server_group);
	if (!cc_options.pool_uuid ||
	    (uuid_parse(cc_options.pool_uuid, pool_uuid) < 0))
		return EINVAL;


	/* turn the list of pool service nodes into a rank list */
	rc = parse_rank_list(cc_options.server_list,
			     &pool_service_list);

	/* [todo-ryon]: ?? */
	if (pool_service_list.rl_nr == 0)
		pool_service_list.rl_nr = 1;

	if (rc < 0)
		/* TODO do a better job with failure return */
		return rc;

	rc = daos_pool_connect(pool_uuid, cc_options.server_group,
			       &pool_service_list,
			       flag, &poh, &info, NULL);
	if (rc) {
		printf("Pool connect fail, result: %d\n", rc);
		return rc;
	}

	/* create a UUID for the container if none supplied*/
	if (cc_options.cont_uuid == NULL) {
		uuid_generate(cont_uuid);
	} else {
		rc = uuid_parse(cc_options.cont_uuid, cont_uuid);
		if (rc != 0)
			goto done;
	}

	if (cc_options.csum_type != DAOS_PROP_CO_CSUM_OFF) {
		props = daos_prop_alloc(1);
		props->dpp_entries[0].dpe_type = DAOS_PROP_CO_CSUM;
		props->dpp_entries[0].dpe_val = cc_options.csum_type;
	}

	rc = daos_cont_create(poh, cont_uuid, props, NULL);

	if (props != NULL)
		daos_prop_free(props);

	if (rc) {
		printf("Container create fail, result: %d\n", rc);
	} else {
		char uuid_str[100];

		uuid_unparse(cont_uuid, uuid_str);
		printf("%s\n", uuid_str);
	}

done:
	daos_pool_disconnect(poh, NULL);
	return rc;
}

/**
 * Process a destroy container command.
 */
int
cmd_destroy_container(int argc, const char **argv, void *ctx)
{
	int              rc = -ENXIO;
	uuid_t           pool_uuid;
	char		 pool_uuid_str[100] = {0};
	uuid_t           cont_uuid;
	daos_handle_t    poh;
	d_rank_list_t    pool_service_list = {NULL, 0};
	unsigned int     flag = DAOS_PC_RW;
	daos_pool_info_t info = {0};

	struct argp_option options[] = {
		{"server-group",    's',  "SERVER-GROUP", 0,
		 "ID of the server group that owns the pool"},
		{"servers",         'l',   "server rank-list", 0,
		 "pool service ranks, comma separated, no spaces e.g. -l 1,2"},
		{"pool-uuid",       'i',  "UUID",         0,
		 "ID of the pool that hosts the container to be destroyed."},
		{"cont-uuid",       'c',  "UUID",         0,
		 "ID of the container to be destroyed."},
		{"force",           'f',  0,              OPTION_ARG_OPTIONAL,
		 "Force pool destruction regardless of current state."},
		{0}
	};
	struct argp argp = {options, parse_cont_args_cb};
	struct container_cmd_options cc_options = {"daos_server",
						   NULL, "12345678-1234-1234-1234-123456789012",
						   NULL,
						   0, 0, 0, 0};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &cc_options);

	/* uuids needs extra parsing */
	if (cc_options.pool_uuid == NULL) {
		if (get_pool(pool_uuid_str))
			cc_options.pool_uuid = pool_uuid_str;
	}
	rc = uuid_parse(cc_options.pool_uuid, pool_uuid);

	if (cc_options.cont_uuid == NULL) {
		printf("No Container UUID provided\n");
		return EINVAL;
	}
	rc = uuid_parse(cc_options.cont_uuid, cont_uuid);

	/* turn the list of pool service nodes into a rank list */
	rc = parse_rank_list(cc_options.server_list,
			     &pool_service_list);
	if (rc < 0)
		/* TODO do a better job with failure return */
		return rc;
	/* [todo-ryon]: ?? */
	if (pool_service_list.rl_nr == 0)
		pool_service_list.rl_nr = 1;

	rc = daos_pool_connect(pool_uuid, cc_options.server_group,
			       &pool_service_list,
			       flag, &poh, &info, NULL);

	if (rc) {
		printf("Pool connect fail, result: %d\n", rc);
		return rc;
	}

	/**
	 * For now ignore callers force preference because its not implemented
	 * and asserts.
	 */
	rc = daos_cont_destroy(poh, cont_uuid, 1, NULL);

	if (rc)
		printf("Container destroy fail, result: %d\n", rc);
	else
		printf("Container '%s' destroyed.\n", cc_options.cont_uuid);

	daos_pool_disconnect(poh, NULL);
	return rc;
}

/**
 * Process a container query command.
 */
int
cmd_query_container(int argc, const char **argv, void *ctx)
{
	int              rc = -ENXIO;
	uuid_t           pool_uuid;
	uuid_t           cont_uuid;
	daos_handle_t    poh;
	d_rank_list_t    pool_service_list = {NULL, 0};
	unsigned int     flag = DAOS_PC_RW;
	daos_pool_info_t pool_info = {0};
	daos_handle_t    coh;
	daos_cont_info_t cont_info;

	struct argp_option options[] = {
		{"server-group",    's',  "SERVER-GROUP", 0,
		 "ID of the server group that owns the pool"},
		{"servers",        'l',   "server rank-list", 0,
		 "pool service ranks, comma separated, no spaces e.g. -l 1,2"},
		{"pool-uuid",       'i',  "UUID",         0,
		 "ID of the pool that hosts the container to be queried."},
		{"cont-uuid",       'c',  "UUID",         0,
		 "ID of the container to be queried."},
		{0}
	};
	struct argp argp = {options, parse_cont_args_cb};
	struct container_cmd_options cc_options = {"daos_server",
						   NULL, NULL, NULL,
						   0, 0, 0, 0};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &cc_options);

	/* uuids needs extra parsing */
	rc = uuid_parse(cc_options.pool_uuid, pool_uuid);
	rc = uuid_parse(cc_options.cont_uuid, cont_uuid);

	/* turn the list of pool service nodes into a rank list */
	rc = parse_rank_list(cc_options.server_list,
			     &pool_service_list);
	if (rc < 0)
		/* TODO do a better job with failure return */
		return rc;

	rc = daos_pool_connect(pool_uuid, cc_options.server_group,
			       &pool_service_list, flag, &poh,
			       &pool_info, NULL);

	if (rc) {
		printf("Pool connect fail, result: %d\n", rc);
		return rc;
	}

	rc = daos_cont_open(poh, cont_uuid, DAOS_COO_RO, &coh,
			    &cont_info, NULL);

	if (rc) {
		printf("Container open fail, result: %d\n", rc);
		goto done2;
	}

	rc = daos_cont_query(coh, &cont_info, NULL, NULL);

	if (rc) {
		printf("Container query failed, result: %d\n", rc);
		goto done1;
	}

	char uuid_str[100];

	uuid_unparse(pool_uuid, uuid_str);
	printf("Pool UUID: %s\n", uuid_str);

	uuid_unparse(cont_uuid, uuid_str);
	printf("Container UUID: %s\n", uuid_str);

	printf("Number of snapshots: %i\n",
	       (int)cont_info.ci_nsnapshots);

	printf("Latest Persistent Snapshot: %i\n",
	       (int)cont_info.ci_lsnapshot);

 done1:
	daos_cont_close(coh, NULL);
 done2:
	daos_pool_disconnect(poh, NULL);
	return rc;
}
