/* fsops_uvol.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008-2011 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <spawn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "apk_context.h"
#include "apk_fs.h"

static int uvol_run(struct apk_ctx *ac, char *action, const char *volname, char *arg1, char *arg2)
{
	char buf[APK_EXIT_STATUS_MAX_SIZE];
	struct apk_out *out = &ac->out;
	pid_t pid;
	int r, status;
	char *argv[] = { (char*)apk_ctx_get_uvol(ac), action, (char*) volname, arg1, arg2, 0 };
	posix_spawn_file_actions_t act;

	posix_spawn_file_actions_init(&act);
	posix_spawn_file_actions_addclose(&act, STDIN_FILENO);
	r = posix_spawn(&pid, apk_ctx_get_uvol(ac), &act, 0, argv, environ);
	posix_spawn_file_actions_destroy(&act);
	if (r != 0) {
		apk_err(out, "%s: uvol run exec error: %s", volname, apk_error_str(r));
		return r;
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);

	if (apk_exit_status_str(status, buf, sizeof buf)) {
		apk_err(out, "%s: uvol run %s", volname, buf);
		return -APKE_UVOL_ERROR;
	}
	return 0;
}

static int uvol_extract(struct apk_ctx *ac, const char *volname, char *arg1, off_t sz,
	struct apk_istream *is, apk_progress_cb cb, void *cb_ctx)
{
	char buf[APK_EXIT_STATUS_MAX_SIZE];
	struct apk_out *out = &ac->out;
	struct apk_ostream *os;
	pid_t pid;
	int r, status, pipefds[2];
	char *argv[] = { (char*)apk_ctx_get_uvol(ac), "write", (char*) volname, arg1, 0 };
	posix_spawn_file_actions_t act;

	if (pipe2(pipefds, O_CLOEXEC) != 0) return -errno;

	posix_spawn_file_actions_init(&act);
	posix_spawn_file_actions_adddup2(&act, pipefds[0], STDIN_FILENO);
	r = posix_spawn(&pid, apk_ctx_get_uvol(ac), &act, 0, argv, environ);
	posix_spawn_file_actions_destroy(&act);
	if (r != 0) {
		apk_err(out, "%s: uvol exec error: %s", volname, apk_error_str(r));
		return r;
	}
	close(pipefds[0]);
	os = apk_ostream_to_fd(pipefds[1]);
	apk_stream_copy(is, os, sz, cb, cb_ctx, 0);
	r = apk_ostream_close(os);
	if (r != 0) {
		if (r >= 0) r = -APKE_UVOL_ERROR;
		apk_err(out, "%s: uvol write error: %s", volname, apk_error_str(r));
		return r;
	}

	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);

	if (apk_exit_status_str(status, buf, sizeof buf)) {
		apk_err(out, "%s: uvol extract %s", volname, buf);
		return -APKE_UVOL_ERROR;
	}
	return 0;
}

static int uvol_dir_create(struct apk_fsdir *d, mode_t mode, uid_t uid, gid_t gid)
{
	return 0;
}

static int uvol_dir_delete(struct apk_fsdir *d)
{
	return 0;
}

static int uvol_dir_check(struct apk_fsdir *d, mode_t mode, uid_t uid, gid_t gid)
{
	return 0;
}

static int uvol_dir_update_perms(struct apk_fsdir *d, mode_t mode, uid_t uid, gid_t gid)
{
	return 0;
}

static int uvol_file_extract(struct apk_ctx *ac, const struct apk_file_info *fi, struct apk_istream *is,
	apk_progress_cb cb, void *cb_ctx, unsigned int extract_flags, apk_blob_t pkgctx)
{
	char size[64];
	const char *uvol_name;
	int r;

	if (IS_ERR(ac->uvol)) return PTR_ERR(ac->uvol);

	uvol_name = strrchr(fi->name, '/');
	uvol_name = uvol_name ? uvol_name + 1 : fi->name;

	r = apk_fmt(size, sizeof size, "%ju", (intmax_t) fi->size);
	if (r < 0) return r;

	r = uvol_run(ac, "create", uvol_name, size, "ro");
	if (r != 0) return r;

	r = uvol_extract(ac, uvol_name, size, fi->size, is, cb, cb_ctx);
	if (r == 0 && !pkgctx.ptr)
		r = uvol_run(ac, "up", uvol_name, 0, 0);

	if (r != 0) uvol_run(ac, "remove", uvol_name, 0, 0);

	return r;
}

static int uvol_file_control(struct apk_fsdir *d, apk_blob_t filename, int ctrl)
{
	struct apk_ctx *ac = d->ac;
	struct apk_pathbuilder pb;
	const char *uvol_name;
	int r;

	if (IS_ERR(ac->uvol)) return PTR_ERR(ac->uvol);

	apk_pathbuilder_setb(&pb, filename);
	uvol_name = apk_pathbuilder_cstr(&pb);

	switch (ctrl) {
	case APK_FS_CTRL_COMMIT:
		return uvol_run(ac, "up", uvol_name, 0, 0);
	case APK_FS_CTRL_APKNEW:
	case APK_FS_CTRL_CANCEL:
	case APK_FS_CTRL_DELETE:
		r = uvol_run(ac, "down", uvol_name, 0, 0);
		if (r)
			return r;
		return uvol_run(ac, "remove", uvol_name, 0, 0);
	case APK_FS_CTRL_DELETE_APKNEW:
		return 0;
	default:
		return -APKE_UVOL_ERROR;
	}
}

static int uvol_file_info(struct apk_fsdir *d, apk_blob_t filename, unsigned int flags, struct apk_file_info *fi)
{
	return -APKE_UVOL_ERROR;
}

const struct apk_fsdir_ops fsdir_ops_uvol = {
	.priority = APK_FS_PRIO_UVOL,
	.dir_create = uvol_dir_create,
	.dir_delete = uvol_dir_delete,
	.dir_check = uvol_dir_check,
	.dir_update_perms = uvol_dir_update_perms,
	.file_extract = uvol_file_extract,
	.file_control = uvol_file_control,
	.file_info = uvol_file_info,
};
