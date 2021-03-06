/*
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

#include "com_intel_daos_client_DaosFsClient.h"
#include <sys/stat.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <gurt/common.h>
#include <libgen.h>
#include <stdio.h>
#include <daos.h>
#include <daos_fs.h>
#include <daos_jni_common.h>
#include <fcntl.h>

static jclass daos_io_exception_class;

static jmethodID new_exception_msg;
static jmethodID new_exception_cause;
static jmethodID new_exception_msg_code_msg;
static jmethodID new_exception_msg_code_cause;

static int ERROR_PATH_LEN = 256;
static int ERROR_NOT_EXIST = 2;
static int ERROR_LOOKUP_MAX_RETRIES = 100;

/**
 * This function is called when JVM load native library through
 * System.loadLibrary or System.load methods.
 *
 * \param[in]	vm		denote a Java VM
 * \param[in]	reserved	reserved for future
 *
 * \return	JNI_VERSION expected by JVM on success return.
 * 		JNI_ERR or non-zeor rc code of daos_init() for any error. In
 * 		this case, JVM throws JNI error.
 */
jint
JNI_OnLoad(JavaVM *vm, void *reserved)
{
	JNIEnv *env;

	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION) != JNI_OK) {
		return JNI_ERR;
	}

	jclass local_class = (*env)->FindClass(env,
			"com/intel/daos/client/DaosIOException");

	daos_io_exception_class = (*env)->NewGlobalRef(env, local_class);
	jmethodID m1 = (*env)->GetMethodID(env, daos_io_exception_class,
			"<init>",
			"(Ljava/lang/String;)V");

	new_exception_msg = (*env)->NewGlobalRef(env, m1);
	if(new_exception_msg == NULL){
		printf("failed to get constructor msg\n");
		return JNI_ERR;
	}
	jmethodID m2 = (*env)->GetMethodID(env, daos_io_exception_class,
			"<init>",
			"(Ljava/lang/Throwable;)V");

	new_exception_cause = (*env)->NewGlobalRef(env, m2);
	if(new_exception_cause == NULL){
		printf("failed to get constructor cause\n");
		return JNI_ERR;
	}
	jmethodID m3 = (*env)->GetMethodID(env, daos_io_exception_class,
			"<init>",
			"(Ljava/lang/String;ILjava/lang/String;)V");

	new_exception_msg_code_msg = (*env)->NewGlobalRef(env, m3);
	if(new_exception_msg_code_msg == NULL){
		printf("failed to get constructor msg, code and daos msg\n");
		return JNI_ERR;
	}
	jmethodID m4 = (*env)->GetMethodID(env, daos_io_exception_class,
			"<init>",
			"(Ljava/lang/String;ILjava/lang/Throwable;)V");

	new_exception_msg_code_cause = (*env)->NewGlobalRef(env, m4);
	if(new_exception_msg_code_cause == NULL){
		printf("failed to get constructor msg, code and cause\n");
		return JNI_ERR;
	}
	int rc = daos_init();

	if (rc) {
		printf("daos_init() failed with rc = %d\n", rc);
		printf("error msg: %s\n", d_errstr(rc));
		return rc;
	}
	return JNI_VERSION;
}

/**
 * utility function to throw Java exception.
 *
 * \param[in]	env		JNI environment
 * \param[in]	msg		error message provided by caller
 * \param[in]	error_code	non-zero return code of DFS function or
 * 				customized error code
 * \param[in]	release_msg	is \a msg needed to be released, (0 or 1)
 * \param[in]	posix_error	is \a error_code posix error,
 * 				1 for true, 0 for false
 *
 * \return	return code of Throw function of \a env
 */
static int
throw_exception_base(JNIEnv *env, char *msg, int error_code,
			int release_msg, int posix_error)
{
	char *daos_msg;
	jstring jmsg = (*env)->NewStringUTF(env, strdup(msg));

	if (error_code > CUSTOM_ERROR_CODE_BASE) {
		const char* temp = posix_error ?
				strerror(error_code) : d_errstr(error_code);

		daos_msg = temp;
	} else {
		daos_msg = NULL;
	}
	jobject obj = (*env)->NewObject(env, daos_io_exception_class,
			new_exception_msg_code_msg, jmsg, error_code,
			daos_msg == NULL ?
				NULL : (*env)->NewStringUTF(env, daos_msg));

	if (release_msg) {
		free(msg);
	}
	return (*env)->Throw(env, obj);
}

/**
 * throw Java exception with dynamically constructed message for posix error.
 *
 * \param[in]	env		JNI environment
 * \param[in]	msg		error message provided by caller
 * \param[in]	error_code	non-zero return code of DFS function
 *
 * \return	return code of throw_exception_base
 */
static int
throw_exception(JNIEnv *env, char *msg, int error_code)
{
	return throw_exception_base(env, msg, error_code, 1, 1);
}

/**
 * throw Java exception with constant message for posix error.
 *
 * \param[in]	env		JNI environment
 * \param[in]	msg		error message provided by caller
 * \param[in]	error_code	non-zero return code of DFS function
 *
 * \return	return code of throw_exception_base
 */
static int
throw_exception_const_msg(JNIEnv *env, char *msg, int error_code)
{
	return throw_exception_base(env, msg, error_code, 0, 1);
}

/**
 * JNI method to open pool with given \a poolId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 * \param[in]	poolId		pool UUID
 * \param[in]	serverGroup	server group name
 * \param[in]	ranks		ranks separated by ':'
 *
 * \return	copied pool handle in long
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_daosOpenPool(JNIEnv *env,
		jclass clientClass, jstring poolId, jstring serverGroup,
		jstring ranks, jint flags)
{
	const char *pool_str = (*env)->GetStringUTFChars(env, poolId, 0);
	const char *server_group = (*env)->GetStringUTFChars(env, serverGroup,
								0);
	const char *svc_ranks = (*env)->GetStringUTFChars(env, ranks, 0);
	uuid_t pool_uuid;
	uuid_parse(pool_str, pool_uuid);
	d_rank_list_t *svcl = daos_rank_list_parse(svc_ranks, ":");
	jlong ret;

	if (svcl == NULL) {
		char *tmp = "Invalid pool service rank list (%s) when open " \
				"pool (%s)";
		char *msg = (char *)malloc(strlen(tmp) + strlen(svc_ranks) +
				strlen(pool_str));

		sprintf(msg, tmp, ranks, pool_str);
		throw_exception(env, msg, CUSTOM_ERR2);
		ret = -1;
	} else {
		daos_handle_t poh;
		int rc;
		rc = daos_pool_connect(pool_uuid, server_group, svcl,
			flags,
			&poh /* returned pool handle */,
			NULL /* returned pool info */,
			NULL /* event */);

		if (rc) {
			char *tmp = "Failed to connect to pool (%s)";
			char *msg = (char *)malloc(strlen(tmp) +
					strlen(pool_str));

			sprintf(msg, tmp, svc_ranks, pool_str);
			throw_exception_base(env, msg, rc, 1, 0);
			ret = -1;
		} else {
			memcpy(&ret, &poh, sizeof(poh));
		}
	}
	(*env)->ReleaseStringUTFChars(env, poolId, pool_str);
	if (serverGroup != NULL) {
		(*env)->ReleaseStringUTFChars(env, serverGroup, server_group);
	}
	if (ranks != NULL) {
		(*env)->ReleaseStringUTFChars(env, ranks, svc_ranks);
	}
	return ret;
}

/**
 * JNI method to close pool denoted by \a poolHandle.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 * \param[in]	poolHandle	pool handle
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_daosClosePool(JNIEnv *env,
		jclass clientClass, jlong poolHandle)
{
	daos_handle_t poh;

	memcpy(&poh, &poolHandle, sizeof(poh));
	int rc = daos_pool_disconnect(poh, NULL);

	if (rc) {
		printf("Failed to close pool rc: %d\n", rc);
		printf("error msg: %s\n", d_errstr(rc));
	}
}

/**
 * JNI method to open container with given \a contUuid.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 * \param[in]	poolHandle	pool handle
 * \param[in]	contUuid	container UUID
 * \param[in]	mode		container mode
 *
 * \return	copied container handle in long
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_daosOpenCont(JNIEnv *env,
		jclass clientClass, jlong poolHandle, jstring contUuid,
		jint mode)
{
	daos_handle_t poh;
	daos_cont_info_t co_info;
	const char *cont_str = (*env)->GetStringUTFChars(env, contUuid, NULL);
	uuid_t cont_uuid;
	uuid_parse(cont_str, cont_uuid);
	daos_handle_t coh;
	jlong ret = -1;

	memcpy(&poh, &poolHandle, sizeof(poh));
	int rc = daos_cont_open(poh, cont_uuid, mode, &coh, &co_info, NULL);

	if (rc) {
		char *tmp = "Failed to open container (id: %s)";
		char *msg = (char *)malloc(strlen(tmp) + strlen(cont_str));

		sprintf(msg, tmp, cont_str);
		throw_exception_base(env, msg, rc, 1, 0);
		ret = -1;
	} else {
		memcpy(&ret, &coh, sizeof(coh));
	}
	(*env)->ReleaseStringUTFChars(env, contUuid, cont_str);
	return ret;
}

/**
 * JNI method to close container denoted by \a contHandle.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 * \param[in]	contHandle	container handle
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_daosCloseContainer(JNIEnv *env,
		jclass clientClass, jlong contHandle)
{
	daos_handle_t coh;

	memcpy(&coh, &contHandle, sizeof(coh));
	int rc = daos_cont_close(coh, NULL);

	if(rc){
		printf("Failed to close container rc: %d\n", rc);
		printf("error msg: %s\n", d_errstr(rc));
	}
}

/**
 * JNI method to mount FS on given pool and container.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 * \param[in]	poolHandle	pool handle
 * \param[in]	contHandle	container handle
 * \param[in]	readOnly	is read only FS
 *
 * \return	address of dfs object
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsMountFs(JNIEnv *env,
		jclass clientClass, jlong poolHandle, jlong contHandle,
		jboolean readOnly)
{
	int flags = readOnly ? O_RDONLY : O_RDWR;
	dfs_t *dfsPtr;
	daos_handle_t poh;
	daos_handle_t coh;

	memcpy(&poh, &poolHandle, sizeof(poh));
	memcpy(&coh, &contHandle, sizeof(coh));
	int rc = dfs_mount(poh, coh, flags, &dfsPtr);

	if (rc) {
		char *msg = "Failed to mount fs ";

		throw_exception_const_msg(env, msg, rc);
		return -1;
	}
	return *(jlong*)&dfsPtr;
}

/**
 * JNI method to unmount FS denoted by \a dfsPtr.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 * \param[in]	dfsPtr		address of dfs object
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsUnmountFs(JNIEnv *env,
		jclass clientClass, jlong dfsPtr)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	int rc = dfs_umount(dfs);

	if (rc) {
		printf("Failed to unmount fs rc: %d\n", rc);
		printf("error msg: %s\n", strerror(rc));
	}
}

/**
 * JNI method to mount FS on root container of given pool denoted by
 * \a poolHandle.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 * \param[in]	poolHandle	pool handle
 *
 * \return	address of dfs object
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsMountFsOnRoot(JNIEnv *env,
		jclass clientClass, jlong poolHandle)
{
	dfs_t *dfsPtr;
	daos_handle_t poh;

	memcpy(&poh, &poolHandle, sizeof(poh));
	int rc = dfs_mount_root_cont(poh, &dfsPtr);

	if (rc) {
		char *msg = "Failed to mount fs on root container";

		throw_exception_const_msg(env, msg, rc);
		return -1;
	}
	return *(jlong*)&dfsPtr;
}

/**
 * JNI method to unmount FS denoted by \a dfsPtr from root container.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 * \param[in]	dfsPtr		address of dfs object
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsUnmountFsOnRoot(JNIEnv *env,
		jclass clientClass, jlong dfsPtr)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;

	int rc = dfs_umount_root_cont(dfs);

	if (rc) {
		printf("Failed to unmount fs on root container rc: %d\n", rc);
		printf("error msg: %s\n", strerror(rc));
	}
}

/**
 * JNI method to finalize DAOS.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	class of DaosFsClient
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_daosFinalize(JNIEnv *env,
		jclass clientClass)
{
	int rc = daos_fini();

	if (rc) {
		printf("Failed to finalize daos rc: %d\n", rc);
		printf("error msg: %s\n", d_errstr(rc));
	}
}

/**
 * JNI method to move file from \a srcPath to \a destPath.
 *
 * \param[in]	env		JNI environment
 * \param[in]	obj		object of DaosFsClient
 * \param[in]	dfsPtr		pointer to dfs object
 * \param[in]	srcPath		source path
 * \param[in]	destPath	destination path
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_move__JLjava_lang_String_2Ljava_lang_String_2(
		JNIEnv *env, jobject obj, jlong dfsPtr, jstring srcPath,
		jstring destPath)
{
	const char *src_path = (*env)->GetStringUTFChars(env, srcPath, NULL);
	const char *dest_path = (*env)->GetStringUTFChars(env, destPath, NULL);
	char *src_dir_path = NULL;
	char *src_base_path = NULL;
	char *dest_dir_path = NULL;
	char *dest_base_path = NULL;

	src_dir_path = strdup(src_path);
	src_base_path = strdup(src_path);
	dest_dir_path = strdup(dest_path);
	dest_base_path = strdup(dest_path);
	char *src_dir = dirname(src_dir_path);
	char *src_base = basename(src_base_path);
	char *dest_dir = dirname(dest_dir_path);
	char *dest_base = basename(dest_base_path);
	dfs_obj_t *src_dir_handle = NULL;
	dfs_obj_t *dest_dir_handle = NULL;
	mode_t tmp_mode;
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	int rc = dfs_lookup(dfs, src_dir, O_RDWR, &src_dir_handle, &tmp_mode,
			NULL);

	if (rc) {
		char *tmp = "Cannot open source directory (%s)";
		char *msg = (char *)malloc(strlen(tmp) + strlen(src_dir));

		sprintf(msg, tmp, src_dir);
		throw_exception(env, msg, rc);
		goto out;
	}
	if (strcmp(src_dir, dest_dir) == 0) {
		dest_dir_handle = src_dir_handle;
	} else {
		rc = dfs_lookup(dfs, dest_dir, O_RDWR, &dest_dir_handle,
				&tmp_mode, NULL);
		if (rc) {
			char *tmp = "Cannot open destination directory (%s)";
			char *msg = (char *)malloc(strlen(tmp) +
					strlen(dest_dir));

			sprintf(msg, tmp, dest_dir);
			throw_exception(env, msg, rc);
			goto out;
		}
	}
	rc = dfs_move(dfs, src_dir_handle, src_base, dest_dir_handle, dest_base,
			NULL);
	if (rc) {
		char *tmp = "Failed to move source path (%s) to destination " \
				"path (%s)";
		char *msg = (char *)malloc(strlen(tmp) + strlen(src_path) +
				strlen(dest_path));

		sprintf(msg, tmp, src_path, dest_path);
		throw_exception(env, msg, rc);
	}
out:
	if (src_dir_path) free(src_dir_path);
	if (src_base_path) free(src_base_path);
	if (dest_dir_path) free(dest_dir_path);
	if (dest_base_path) free(dest_base_path);
	if (src_dir_handle) dfs_release(src_dir_handle);
	if (src_dir_handle != dest_dir_handle) dfs_release(dest_dir_handle);
	(*env)->ReleaseStringUTFChars(env, srcPath, src_path);
	(*env)->ReleaseStringUTFChars(env, destPath, dest_path);
}

/**
 * move file from \a srcName under directory denoted by \a srcPrtObjId
 * to \a destName under directory denoted by \a destPrtObjId.
 * This method is more efficient than last move(srcPath, destPath) since we
 * don't need to open both source directory and destination directory.
 *
 * \param[in]	env		JNI environment
 * \param[in]	obj		object of DaosFsClient
 * \param[in]	dfsPtr		pointer to dfs object
 * \param[in]	srcPrtObjId	source parent object id
 * \param[in]	srcName		source name, just file name without any path
 * \param[in]	destPrtObjId	dest parent object id
 * \param[in]	destName	destination name, just file name without any
 * 				path
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_move__JJLjava_lang_String_2JLjava_lang_String_2(
		JNIEnv *env, jobject obj, jlong dfsPtr, jlong srcPrtObjId,
		jstring srcName, jlong destPrtObjId, jstring destName)
{
	char *src_base = (*env)->GetStringUTFChars(env, srcName, NULL);
	char *dest_base = (*env)->GetStringUTFChars(env, destName, NULL);
	dfs_obj_t *src_dir_handle = *(dfs_obj_t **)&srcPrtObjId;
	dfs_obj_t *dest_dir_handle = *(dfs_obj_t **)&destPrtObjId;
	mode_t tmp_mode;
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	int rc = dfs_move(dfs, src_dir_handle, src_base, dest_dir_handle,
			dest_base, NULL);

	if (rc) {
		char *tmp = "Failed to move source file (%s) under dir (%ld)" \
			" to destination file (%s) under dir (%ld)";
		char *msg = (char *)malloc(strlen(tmp) + strlen(src_base) +
			20 + strlen(dest_base) + 20);

		sprintf(msg, tmp, src_base, srcPrtObjId, dest_base,
				destPrtObjId);
		throw_exception(env, msg, rc);
	}
out:
	(*env)->ReleaseStringUTFChars(env, srcName, src_base);
	(*env)->ReleaseStringUTFChars(env, destName, dest_base);
}

/**
 * utility function to create directories. It could be called recursively.
 *
 * \param[in]		dfs		dfs object
 * \param[in]		path		path of directory
 * \param[in]		mode		mode of directory
 * \param[in]		recursive	recursively create ancestors
 * \param[in,out]	handle		handle of open DAOS object
 * \param[in,out]	msg		error message during creation
 *
 * \return	0 for success, non-zero for failure
 */
static int mkdirs(dfs_t *dfs, char *path, int mode, unsigned char recursive,
		dfs_obj_t **handle, char *msg)
{
	char *dirs = NULL;
	char *bases = NULL;
	char *dir = NULL;
	char *base = NULL;
	dfs_obj_t *parent_handle = NULL;
	mode_t parent_mode;
	int rc = dfs_lookup(dfs, path, O_RDWR, handle, &parent_mode, NULL);

	if (rc == -DER_NONEXIST || rc == -ENOENT || rc == ENOENT) {
		if (!recursive) {
			goto out;
		}
		// recursively create it
		dirs = strdup(path);
		dir = dirname(dirs);
		bases = strdup(path);
		base = basename(bases);
		rc = mkdirs(dfs, dir, mode, recursive, &parent_handle,
				msg);
		if (rc)	{
			if (msg[0] == '\0') {
				int dir_len = strlen(dir);
				int max_len = ERROR_PATH_LEN - 4;
				int l = dir_len > max_len ? max_len : dir_len;

				memcpy(msg, dir, l);
				if (l == max_len) {
					char *suffix = "...\0";

					memcpy(msg+l, suffix, 4);
				} else {
					msg[l] = '\0';
				}
			}
			goto out;
		}
		rc = dfs_mkdir(dfs, parent_handle, base, mode, 0);
		// code to mitigate DAOS concurrency issue
		// to be fixed by the conditional update feature in DAOS
		if (rc == ERROR_NOT_EXIST) {
			int count = 0;

			while (rc && (count<ERROR_LOOKUP_MAX_RETRIES)) {
				rc = dfs_lookup(dfs, path, O_RDWR, handle,
						&parent_mode, NULL);
				count++;
			}
		} else {
			rc = dfs_lookup(dfs, path, O_RDWR, handle, &parent_mode,
					NULL);
		}
	}
out:
	if (dirs) free(dirs);
	if (bases) free(bases);
	if (parent_handle) dfs_release(parent_handle);
	return rc;
}

/**
 * JNI method to create directory. If parent directory doesn't exist and
 * \a recursive is 0, a Java exception will be thrown.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \param[in]	dfsPtr		pointer to dfs object
 * \param[in]	path		path to directory to be created
 * \param[in]	mode		mode of directory
 * \param[in]	recursive	create directory recursively
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_mkdir(JNIEnv *env, jobject client,
		jlong dfsPtr, jstring path, jint mode, jboolean recursive)
{
	const char* path_str = (*env)->GetStringUTFChars(env, path, NULL);
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	char *dirs;
	char *bases;
	char *parent_dir;
	char *base;
	dfs_obj_t *parent_handle = NULL;
	mode_t parent_mode;
	char *parentError = NULL;

	dirs = strdup(path_str);
	parent_dir = dirname(dirs);
	bases = strdup(path_str);
	base = basename(bases);
	int rc = 0;

	if ((strlen(parent_dir) > 0) &&
			(strcmp(parent_dir, "/") != 0)) {
		parentError = (char*)malloc(ERROR_PATH_LEN);
		parentError[0] = '\0';
		rc = mkdirs(dfs, parent_dir, mode, recursive, &parent_handle,
				parentError);
	}
	if (rc) {
		char *dir_msg = parent_dir;
		char *tmp;

		if (recursive) {
			tmp = "Failed to create parent or ancestor " \
					"directories (%s)";
			if (parentError[0] != '\0') {
				dir_msg = parentError;
			}
		} else {
			tmp = "Parent directory doesn't exist (%s)";
		}
		char *msg = (char *)malloc(strlen(tmp) + strlen(dir_msg));

		sprintf(msg, tmp, dir_msg);
		throw_exception(env, msg, rc);
	} else {
		rc = dfs_mkdir(dfs, parent_handle, base, mode, 0);
		if (rc) {
			char *tmp = "Failed to create directory (%s) " \
					"under parent directory (%s)";
			char *msg = (char *)malloc(strlen(tmp) + strlen(base) +
					strlen(parent_dir));

			sprintf(msg, tmp, base, parent_dir);
			throw_exception(env, msg, rc);
		}
	}
	if (dirs) free(dirs);
	if (bases) free(bases);
	if (parentError) free(parentError);
	if (parent_handle) dfs_release(parent_handle);
	(*env)->ReleaseStringUTFChars(env, path, path_str);
}

/**
 * JNI method to create new file, \a name, under directory \a parentPath.
 * Java exception will be thrown if \a parentPath doesn't exist and
 * \a createParent is 0.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \param[in]	dfsPtr		pointer to dfs object
 * \param[in]	parentPath	path to directory under which new file is
 * 				created
 * \param[in]	name		new file name
 * \param[in]	mode		file mode
 * \param[in]	accessFlags	file access flags
 * \param[in]	objectType	object type of this new file
 * \param[in]	chunkSize	file chunk size
 * \param[in]	createParent	create parent directory recursively if it
 * 				doesn't exist
 *
 * \return	memory address of opened dfs object of new file
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_createNewFile(JNIEnv *env,
		jobject client, jlong dfsPtr, jstring parentPath, jstring name,
		jint mode, jint accessFlags, jstring objectType, jint chunkSize,
		jboolean createParent)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	const char *parent_path = (*env)->GetStringUTFChars(env, parentPath,
			NULL);
	const char *file_name = (*env)->GetStringUTFChars(env, name, NULL);
	const char *object_type = (*env)->GetStringUTFChars(env, objectType,
								NULL);
	int type_id = daos_oclass_name2id(object_type);
	char *parentError = NULL;
	dfs_obj_t *file = NULL;
	dfs_obj_t *parent = NULL;
	mode_t tmp_mode;

	int rc = dfs_lookup(dfs, parent_path, O_RDWR, &parent, &tmp_mode, NULL);

	if (rc) {
		if (createParent) {
			parentError = (char*)malloc(ERROR_PATH_LEN);
			parentError[0] = '\0';
			rc = mkdirs(dfs, parent_path, mode, 1, &parent,
					parentError);
			if (rc) {
				char *dir_msg = parent_path;

				if (parentError[0] != '\0') {
					dir_msg = parentError;
				}
				char *tmp = "Failed to create parent/ancestor "\
						"directories (%s)";
				char *msg = (char *)malloc(strlen(tmp) +
						strlen(dir_msg));

				sprintf(msg, tmp, dir_msg);
				throw_exception(env, msg, rc);
				goto out;
			}
		} else {
			char *tmp = "Failed to find parent directory (%s)";
			char *msg = (char *)malloc(strlen(tmp) +
					strlen(parent_path));

			sprintf(msg, tmp, parent_path);
			throw_exception(env, msg, rc);
			goto out;
		}
	}

	rc = dfs_open(dfs, parent, file_name, S_IFREG | mode,
			O_CREAT | accessFlags, type_id, chunkSize, NULL,
			&file);
	if (rc) {
		char *tmp = "Failed to create new file (%s) under " \
				"directory (%s)";
		char *msg = (char *)malloc(strlen(tmp) + strlen(file_name) +
				strlen(parent_path));

		sprintf(msg, tmp, file_name, parent_path);
		throw_exception(env, msg, rc);
	}
out:
	(*env)->ReleaseStringUTFChars(env, parentPath, parent_path);
	(*env)->ReleaseStringUTFChars(env, name, file_name);
	(*env)->ReleaseStringUTFChars(env, objectType, object_type);
	if (parentError) free(parentError);
	if (parent) dfs_release(parent);
	return *(jlong*)&file;
}

/**
 * JNI method to delete a file, \a name, from directory \a parentPath.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		pointer to dfs object
 * \param[in]	parentPath	directory from which file is deleted
 * \param[in]	name		file name
 * \param[in]	force		force delete if it's directory
 *
 * \return	0 if failed to delete, 1 if deleted successfully
 */
JNIEXPORT jboolean JNICALL
Java_com_intel_daos_client_DaosFsClient_delete(JNIEnv *env, jobject client,
		jlong dfsPtr, jstring parentPath, jstring name,
		jboolean force)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	const char *parent_path = (*env)->GetStringUTFChars(env, parentPath,
					NULL);
	const char *file_name = (*env)->GetStringUTFChars(env, name, NULL);
	dfs_obj_t *parent = NULL;
	mode_t tmp_mode;
	int rc;
	int ret;

	if ((strlen(parent_path) > 0) &&
			(strcmp(parent_path, "/") != 0)) {
		rc = dfs_lookup(dfs, parent_path, O_RDWR, &parent, &tmp_mode,
				NULL);
		if (rc) {
			printf("Failed to open parent dir, %s, when delete, " \
					"rc: %d, error msg: %s\n",
					parent_path, rc, strerror(rc));
			ret = 0;
			goto out;
		}
	}
	rc = dfs_remove(dfs, parent, file_name, force, NULL);
	if (rc) {
		printf("Failed to delete %s from %s, rc: %d, error msg: %s\n",
				file_name, parent_path, rc, strerror(rc));
		ret = 0;
		goto out;
	}
	ret = 1;
out:
	(*env)->ReleaseStringUTFChars(env, parentPath, parent_path);
	(*env)->ReleaseStringUTFChars(env, name, file_name);
	if (parent) dfs_release(parent);
	return ret;
}

/**
 * JNI method to open file, \a name, under directory denoted by open object,
 * \a parentObjId, which is pointer to DAOS fs object.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		pointer to dfs object
 * \param[in]	parentObjId	pointer to opened fs object
 * \param[in]	name		file name
 * \param[in]	flags		file flags
 * \param[in]	bufferAddress	buffer to hold stat attributes, not implemented
 * 				yet
 *
 * \return	memory address of opened fs object.
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsLookup__JJLjava_lang_String_2IJ(
		JNIEnv *env, jobject client, jlong dfsPtr, jlong parentObjId,
		jstring name, jint flags, jlong bufferAddress)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *parent = *(dfs_obj_t **)&parentObjId;
	dfs_obj_t *file;
	mode_t tmp_mode;
	const char *file_name = (*env)->GetStringUTFChars(env, name, NULL);
	int rc = dfs_lookup_rel(dfs, parent, file_name, flags, &file, &tmp_mode,
			NULL);

	if (rc) {
		char *tmp = "Failed to open file (%s) under parent with " \
				"flags (%d)";
		char *msg = (char *)malloc(strlen(tmp) + strlen(file_name) + 4);

		sprintf(msg, tmp, file_name, flags);
		throw_exception(env, msg, rc);
		file = NULL;
	}
	(*env)->ReleaseStringUTFChars(env, name, file_name);
	return *(jlong*)&file;
}

/**
 * JNI method to open file, \a name, under directory denoted by,
 * \a path.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		pointer to dfs object
 * \param[in]	path		path of file
 * \param[in]	flags		file flags
 * \param[in]	bufferAddress	buffer to hold stat attributes, not implemented
 * 				yet
 *
 * \return	memory address of opened fs object.
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsLookup__JLjava_lang_String_2IJ(
		JNIEnv *env, jobject client, jlong dfsPtr, jstring path,
		jint flags, jlong bufferAddress)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file;
	mode_t tmp_mode;
	const char *file_path = (*env)->GetStringUTFChars(env, path, NULL);
	int rc = dfs_lookup(dfs, file_path, flags, &file, &tmp_mode, NULL);

	if (rc) {
		char *tmp = "Failed to open file (%s) with flags (%d)";
		char *msg = (char *)malloc(strlen(tmp) + strlen(file_path) + 4);

		sprintf(msg, tmp, file_path, flags);
		throw_exception(env, msg, rc);
		file = NULL;
	}
	(*env)->ReleaseStringUTFChars(env, path, file_path);
	return *(jlong*)&file;
}

/**
 * JNI method to get size of file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		pointer to dfs object
 * \param[in]	objId		pointer to opened fs object
 *
 * \return	size of file
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsGetSize(JNIEnv *env, jobject client,
		jlong dfsPtr, jlong objId)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	daos_size_t size;
	int rc = dfs_get_size(dfs, file, &size);

	if (rc) {
		char *tmp = "Failed to get file size";

		throw_exception_const_msg(env, tmp, rc);
		return -1;
	}
	return size;
}

/**
 * JNI method to duplicae a file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		pointer to dfs object
 * \param[in]	objId		pointer to opened fs object
 * \param[in]	flags		flags of new file
 *
 * \return	memory address of new file object
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsDup(JNIEnv *env, jobject client,
		jlong dfsPtr, jlong objId, jint flags)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	dfs_obj_t *new_file;
	int rc = dfs_dup(dfs, file, flags, &new_file);

	if (rc) {
		char *tmp = "Failed to duplicate file";

		throw_exception_const_msg(env, tmp, rc);
		return -1;
	}
	return *(jlong*)&new_file;
}

/**
 * JNI method to release a file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	DaosFsClient class
 * \parem[in]	objId		pointer to fs object
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsRelease(JNIEnv *env,
		jclass clientClass, jlong objId)
{
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	int rc = dfs_release(file);

	if (rc) {
		char *tmp = "Failed to release file";

		throw_exception_const_msg(env, tmp, rc);
	}
}

/**
 * JNI method to read a file denoted by \a objId, into buffer denoted by
 * \a bufferAddress.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		pointer to dfs object
 * \param[in]	objId		pointer to opened fs object
 * \param[in]	bufferAddress	buffer address
 * \param[in]	fileOffset	file offset
 * \param[in]	len		requested length of bytes to be read
 * \param[in]	eventNo		event no for asynchronous read
 *
 * \return	actual read length
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsRead(JNIEnv *env, jobject client,
		jlong dfsPtr, jlong objId, jlong bufferAddress,
		jlong fileOffset, jlong len, jint eventNo)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	char *buf = (char*)bufferAddress;
	d_iov_t sg_iov = {0};
	d_sg_list_t sgl = {
		.sg_nr = 1,
		.sg_nr_out = 0,
		.sg_iovs = &sg_iov
	};

	d_iov_set(&sg_iov, buf, len);
	daos_size_t size = 0;
	int rc = dfs_read(dfs, file, &sgl, fileOffset, &size, NULL);

	if (rc) {
		char *tmp = "Failed to read %ld bytes from file starting " \
				"at %ld";
		char *msg = (char *)malloc(strlen(tmp) + 20 + 20);

		sprintf(msg, tmp, len, fileOffset);
		throw_exception(env, msg, rc);
		return 0;
	}
	return size;
}

/**
 * JNI method to write data from buffer denoted by \a bufferAddress to
 * a file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		pointer to dfs object
 * \param[in]	objId		pointer to opened fs object
 * \param[in]	bufferAddress	buffer address
 * \param[in]	fileOffset	file offset
 * \param[in]	len		requested length of bytes to write
 * \param[in]	eventNo		event no for asynchronous write
 *
 * \return	actual write length
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsWrite(JNIEnv *env, jobject client,
		jlong dfsPtr, jlong objId, jlong bufferAddress,
		jlong fileOffset, jlong len, jint eventNo)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	char *buf = (char*)bufferAddress;
	d_iov_t sg_iov = {0};
	d_sg_list_t sgl = {
		.sg_nr = 1,
		.sg_nr_out = 0,
		.sg_iovs = &sg_iov
	};

	d_iov_set(&sg_iov, buf, len);
	int rc = dfs_write(dfs, file, &sgl, fileOffset, NULL);

	if (rc) {
		char *tmp = "Failed to write %ld bytes to file starting at %ld";
		char *msg = (char *)malloc(strlen(tmp) + 20 + 20);

		sprintf(msg, tmp, len, fileOffset);
		throw_exception(env, msg, rc);
		return 0;
	}
	return len;
}

/**
 * JNI method to read children entries from directory denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		ointer to dfs object
 * \param[in]	objId		pointer to opened fs object
 * \param[in]	maxEntries	maximum entries to be read. not implemented yet
 *
 * \return	file name separated by ','
 */
JNIEXPORT jstring JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsReadDir(JNIEnv *env, jobject client,
		jlong dfsPtr, jlong objId, jint maxEntries)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *dir = *(dfs_obj_t **)&objId;
	daos_anchor_t anchor = {0};
	uint32_t nr = READ_DIR_BATCH_SIZE;
	struct dirent entries[nr];
	uint32_t size = READ_DIR_INITIAL_BUFFER_SIZE, acc = 0;
	char *buffer = malloc(size);

	if (!buffer) {
		char *tmp = "Failed to allocate %d bytes for reading " \
				"directory content";
		char *msg = (char *)malloc(strlen(tmp) + 4);

		sprintf(msg, tmp, size);
		throw_exception(env, msg, CUSTOM_ERR3);
		return NULL;
	}
	buffer[0] = '\0';
	int rc;
	int total = 0;
	int failed = 0;

	while (!daos_anchor_is_eof(&anchor)) {
		nr = READ_DIR_BATCH_SIZE;
		rc = dfs_readdir(dfs, dir, &anchor, &nr, entries);
		if (rc) {
			char *tmp = "Failed to read %d more entries from " \
					"directory after reading %d " \
					"entries.\n buffer length: %d";
			char *msg = (char *)malloc(strlen(tmp) + 4 + 4 + 4);

			sprintf(msg, tmp, READ_DIR_BATCH_SIZE, total, size);
			throw_exception(env, msg, rc);
			failed = 1;
			break;
		}
		if (!nr) continue;
		total += nr;
		int i;
		for(i=0; i<nr; i++){
			// exactly 1 for each file because ',' and \0
			acc += strlen(entries[i].d_name) + 1;
			if (acc >= size) {
				size += READ_DIR_INITIAL_BUFFER_SIZE;
				buffer = realloc(buffer, size);
				if (!buffer) {
					char *tmp = "Failed to re-allocate %d "\
					"bytes for reading directory content.";
					char *msg = (char *)malloc(strlen(tmp)
							+ 4);

					sprintf(msg, tmp, size);
					throw_exception(env, msg, CUSTOM_ERR3);
					failed = 1;
					break;
				}
			}
			if (buffer[0]) strcat(buffer, ",");
			strcat(buffer, entries[i].d_name);
		}
	}
	jstring result;

	if ((!failed) && buffer[0] != '\0' ) {
		result = (*env)->NewStringUTF(env, buffer);
	} else {
		result = NULL;
	}
	free(buffer);
	return result;
}

static void cpyfield(JNIEnv *env, char *buffer, int *value,
		int valueLen, int expLen)
{
	if (valueLen != expLen) {
		char *tmp = "value length (%d) not equal to expected (%d)";
		char *msg = (char *)malloc(strlen(tmp) + 4 + 4);

		sprintf(msg, tmp, valueLen, expLen);
		throw_exception(env, msg, CUSTOM_ERR4);
		return;
	}
	memcpy(buffer, value, valueLen);
}

/**
 * JNI method to get stat attributes into buffer denoted by \a bufferAddress
 *  from a file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		ointer to dfs object
 * \param[in]	objId		pointer to opened fs object
 * \param[in]	bufferAddress	pointer to opened fs object
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsOpenedObjStat(JNIEnv *env,
		jobject client, jlong dfsPtr, jlong objId,
		jlong bufferAddress)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	struct stat stat = {};
	int rc = dfs_ostat(dfs, file, &stat);

	if (rc) {
		char *tmp = "Failed to get StatAttribute of open object";

		throw_exception_const_msg(env, tmp, rc);
	} else {
		if (bufferAddress == -1L) {
			return;
		}
		char *buffer = (char *)bufferAddress;

		cpyfield(env, buffer, &objId, sizeof(objId), 8);
		cpyfield(env, buffer+8, &stat.st_mode,
				sizeof(stat.st_mode), 4);
		cpyfield(env, buffer+12, &stat.st_uid,
				sizeof(stat.st_uid), 4);
		cpyfield(env, buffer+16, &stat.st_gid,
				sizeof(stat.st_gid), 4);
		cpyfield(env, buffer+20, &stat.st_blocks,
				sizeof(stat.st_blocks), 8);
		cpyfield(env, buffer+28, &stat.st_blksize,
				sizeof(stat.st_blksize), 8);
		cpyfield(env, buffer+36, &stat.st_size,
				sizeof(stat.st_size), 8);
		cpyfield(env, buffer+44, &stat.st_atim,
				sizeof(stat.st_atim), 16);
		cpyfield(env, buffer+60, &stat.st_mtim,
				sizeof(stat.st_mtim), 16);
		cpyfield(env, buffer+76, &stat.st_ctim,
				sizeof(stat.st_ctim), 16);
		buffer[92] = S_ISDIR(stat.st_mode) ? '\0':'1';
	}
}

/**
 * JNI method to set extended attribute denoted by \a name
 * to file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		ointer to dfs object
 * \param[in]	objId		pointer to opened fs object
 * \param[in]	name		attribute name
 * \param[in]	value		attribute value
 * \param[in]	flags		attribute flags
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsSetExtAttr(JNIEnv *env,
		jobject client, jlong dfsPtr, jlong objId, jstring name,
		jstring value, jint flags)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	const char *attr_name = (*env)->GetStringUTFChars(env, name, NULL);
	const char *attr_value = (*env)->GetStringUTFChars(env, value, NULL);
	int rc = dfs_setxattr(dfs, file, attr_name, attr_value,
			(uint64_t)strlen(attr_value), flags);

	if (rc) {
		char *tmp = "Failed to set ext attribute name: %s, " \
				"value %s with flags %d.";
		char *msg = (char *)malloc(strlen(tmp) + strlen(attr_name)
				+ strlen(attr_value) + 4);

		sprintf(msg, tmp, attr_name, attr_value, flags);
		throw_exception(env, msg, rc);
	}
	(*env)->ReleaseStringUTFChars(env, name, attr_name);
	(*env)->ReleaseStringUTFChars(env, value, attr_value);
}

/**
 * JNI method to get extended attribute denoted by \a name
 * from file denoted by \a objId.
 *
 * \param[in]	env			JNI environment
 * \param[in]	client			DaosFsClient object
 * \parem[in]	dfsPtr			ointer to dfs object
 * \param[in]	objId			pointer to opened fs object
 * \param[in]	name			attribute name
 * \param[in]	expectedValenLen	expected value length
 *
 * \return	attribute value
 */
JNIEXPORT jstring JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsGetExtAttr(JNIEnv *env,
		jobject client, jlong dfsPtr, jlong objId, jstring name,
		jint expectedValueLen)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	const char *attr_name = (*env)->GetStringUTFChars(env, name, NULL);
	long value_len = expectedValueLen;
	char *value = (char *)malloc(value_len+1); // 1 for \0
	jstring ret = NULL;

	if (value == NULL) {
		char *tmp = "Failed to allocate %d bytes for reading " \
				"extended attribute value";
		char *msg = (char *)malloc(strlen(tmp) + 4);

		sprintf(msg, tmp, value_len);
		throw_exception(env, msg, CUSTOM_ERR3);
		goto out;
	}
	int rc = dfs_getxattr(dfs, file, attr_name, value, &value_len);

	if (rc) {
		char *tmp = "Failed to get ext attribute name: %s";
		char *msg = (char *)malloc(strlen(tmp) + strlen(attr_name));

		sprintf(msg, tmp, attr_name);
		throw_exception(env, msg, rc);
		goto out;
	}
	value[value_len] = '\0';
	ret = (*env)->NewStringUTF(env, value);

out:
	(*env)->ReleaseStringUTFChars(env, name, attr_name);
	if (value) free(value);
	return ret;
}

/**
 * JNI method to remove extended attribute denoted by \a name
 * from file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	client		DaosFsClient object
 * \parem[in]	dfsPtr		pointer to dfs object
 * \param[in]	objId		pointer to opened fs object
 * \param[in]	name		attribute name
 */
JNIEXPORT void JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsRemoveExtAttr(JNIEnv *env,
		jobject client, jlong dfsPtr, jlong objId, jstring name)
{
	dfs_t *dfs = *(dfs_t **)&dfsPtr;
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	const char *attr_name = (*env)->GetStringUTFChars(env, name, NULL);
	int rc = dfs_removexattr(dfs, file, attr_name);

	if (rc) {
		char *tmp = "Failed to remove ext attribute name: %s";
		char *msg = (char *)malloc(strlen(tmp) + strlen(attr_name));

		sprintf(msg, tmp, attr_name);
		throw_exception(env, msg, rc);
	}
	(*env)->ReleaseStringUTFChars(env, name, attr_name);
}

/**
 * JNI method to get chunk size of file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	DaosFsClient class
 * \param[in]	objId		pointer to opened fs object
 *
 * \return	chunk size
 */
JNIEXPORT jlong JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsGetChunkSize(JNIEnv *env,
		jclass clientClass, jlong objId)
{
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	daos_size_t size;
	int rc = dfs_get_chunk_size(file, &size);

	if (rc) {
		char *msg = "Failed to get chunk size of object. " \
				"It's a directory, not a file? ";

		throw_exception_const_msg(env, msg, rc);
	}
	return size;
}

/**
 * JNI method to get mode of file denoted by \a objId.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	DaosFsClient class
 * \param[in]	objId		pointer to opened fs object
 *
 * \return	file mode
 */
JNIEXPORT jint JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsGetMode(JNIEnv *env,
		jclass clientClass, jlong objId)
{
	dfs_obj_t *file = *(dfs_obj_t **)&objId;
	mode_t mode;
	int rc = dfs_get_mode(file, &mode);

	if (rc) {
		char *msg = "Failed to get mode object";

		throw_exception_const_msg(env, msg, rc);
	}
	return mode;
}

/**
 * JNI method to determine if file is directory by checking file \a mode.
 *
 * \param[in]	env		JNI environment
 * \param[in]	clientClass	DaosFsClient class
 * \param[in]	mode		file mode
 *
 * \return	0 for non-directory, 1 for directory
 */
JNIEXPORT jboolean JNICALL
Java_com_intel_daos_client_DaosFsClient_dfsIsDirectory(JNIEnv *env,
		jclass clientClass, jint mode)
{
	return S_ISDIR(mode) ? 1 : 0;
}

/**
 * This function is called when JVM unload native library.
 *
 * \param[in]	vm		Java vm
 * \param[in]	reserved	reserved for future
 */
void
JNI_OnUnload(JavaVM* vm, void *reserved)
{
	JNIEnv *env;

	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION) != JNI_OK) {
		return;
	}
	(*env)->DeleteGlobalRef(env, daos_io_exception_class);
	(*env)->DeleteGlobalRef(env, new_exception_msg);
	(*env)->DeleteGlobalRef(env, new_exception_cause);
	(*env)->DeleteGlobalRef(env, new_exception_msg_code_msg);
	(*env)->DeleteGlobalRef(env, new_exception_msg_code_cause);
	daos_fini();
}
