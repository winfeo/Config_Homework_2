/* io.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008-2011 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <endian.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>

#include "apk_defines.h"
#include "apk_io.h"
#include "apk_crypto.h"
#include "apk_xattr.h"

#if defined(__GLIBC__) || defined(__UCLIBC__)
#define HAVE_FGETPWENT_R
#define HAVE_FGETGRENT_R
#endif

size_t apk_io_bufsize = 128*1024;


static inline int atfd_error(int atfd)
{
	return atfd < -1 && atfd != AT_FDCWD;
}

int apk_make_dirs(int root_fd, const char *dirname, mode_t dirmode, mode_t parentmode)
{
	char parentdir[PATH_MAX], *slash;

	if (faccessat(root_fd, dirname, F_OK, 0) == 0) return 0;
	if (mkdirat(root_fd, dirname, dirmode) == 0) return 0;
	if (errno != ENOENT || !parentmode) return -1;

	slash = strrchr(dirname, '/');
	if (!slash || slash == dirname || slash-dirname+1 >= sizeof parentdir) return -1;
	strlcpy(parentdir, dirname, slash-dirname+1);
	if (apk_make_dirs(root_fd, parentdir, parentmode, parentmode) < 0) return -1;
	return mkdirat(root_fd, dirname, dirmode);
}

ssize_t apk_write_fully(int fd, const void *ptr, size_t size)
{
	ssize_t i = 0, r;

	while (i < size) {
		r = write(fd, ptr + i, size - i);
		if (r <= 0) {
			if (r == 0) return i;
			return -errno;
		}
		i += r;
	}

	return i;
}

static void apk_file_meta_from_fd(int fd, struct apk_file_meta *meta)
{
	struct stat st;

	if (fstat(fd, &st) == 0) {
		meta->mtime = st.st_mtime;
		meta->atime = st.st_atime;
	} else {
		memset(meta, 0, sizeof(*meta));
	}
}

apk_blob_t apk_istream_mmap(struct apk_istream *is)
{
	if (is->flags & APK_ISTREAM_SINGLE_READ)
		return APK_BLOB_PTR_LEN((char*)is->buf, is->buf_size);
	return APK_BLOB_NULL;
}

ssize_t apk_istream_read_max(struct apk_istream *is, void *ptr, size_t size)
{
	ssize_t left = size, r = 0;

	if (is->err < 0) return is->err;

	while (left) {
		if (is->ptr != is->end) {
			r = min(left, is->end - is->ptr);
			if (ptr) {
				memcpy(ptr, is->ptr, r);
				ptr += r;
			}
			is->ptr += r;
			left -= r;
			continue;
		}
		if (is->err) break;

		if (ptr && left > is->buf_size/4) {
			r = is->ops->read(is, ptr, left);
			if (r <= 0) break;
			left -= r;
			ptr += r;
			continue;
		}

		r = is->ops->read(is, is->buf, is->buf_size);
		if (r <= 0) break;

		is->ptr = is->buf;
		is->end = is->buf + r;
	}

	if (r < 0) return apk_istream_error(is, r);
	if (left == size) return apk_istream_error(is, (size && !is->err) ? 1 : 0);
	return size - left;
}

int apk_istream_read(struct apk_istream *is, void *ptr, size_t size)
{
	ssize_t r = apk_istream_read_max(is, ptr, size);
	return r == size ? 0 : apk_istream_error(is, -APKE_EOF);
}

static int __apk_istream_fill(struct apk_istream *is)
{
	ssize_t sz;

	if (is->err) return is->err;

	if (is->ptr != is->buf) {
		sz = is->end - is->ptr;
		memmove(is->buf, is->ptr, sz);
		is->ptr = is->buf;
		is->end = is->buf + sz;
	} else if (is->end-is->ptr == is->buf_size)
		return -ENOBUFS;

	sz = is->ops->read(is, is->end, is->buf + is->buf_size - is->end);
	if (sz <= 0) return apk_istream_error(is, sz ?: 1);
	is->end += sz;
	return 0;
}

void *apk_istream_peek(struct apk_istream *is, size_t len)
{
	int r;

	if (is->err < 0) return ERR_PTR(is->err);

	do {
		if (is->end - is->ptr >= len) {
			void *ptr = is->ptr;
			return ptr;
		}
		r = __apk_istream_fill(is);
	} while (r == 0);

	return ERR_PTR(r > 0 ? -APKE_EOF : r);
}

void *apk_istream_get(struct apk_istream *is, size_t len)
{
	void *p = apk_istream_peek(is, len);
	if (!IS_ERR(p)) is->ptr += len;
	else apk_istream_error(is, PTR_ERR(p));
	return p;
}

int apk_istream_get_max(struct apk_istream *is, size_t max, apk_blob_t *data)
{
	if (is->ptr == is->end) __apk_istream_fill(is);
	if (is->ptr != is->end) {
		*data = APK_BLOB_PTR_LEN((char*)is->ptr, min((size_t)(is->end - is->ptr), max));
		is->ptr += data->len;
		return 0;
	}
	*data = APK_BLOB_NULL;
	return is->err < 0 ? is->err : -APKE_EOF;
}

int apk_istream_get_delim(struct apk_istream *is, apk_blob_t token, apk_blob_t *data)
{
	apk_blob_t ret = APK_BLOB_NULL, left = APK_BLOB_NULL;
	int r = 0;

	do {
		if (apk_blob_split(APK_BLOB_PTR_LEN((char*)is->ptr, is->end - is->ptr), token, &ret, &left))
			break;
		r = __apk_istream_fill(is);
	} while (r == 0);

	/* Last segment before end-of-file. Return also zero length non-null
	 * blob if eof comes immediately after the delimiter. */
	if (is->ptr && r > 0)
		ret = APK_BLOB_PTR_LEN((char*)is->ptr, is->end - is->ptr);

	if (!APK_BLOB_IS_NULL(ret)) {
		is->ptr = (uint8_t*)left.ptr;
		is->end = (uint8_t*)left.ptr + left.len;
		*data = ret;
		return 0;
	}
	if (r < 0) apk_istream_error(is, r);
	*data = APK_BLOB_NULL;
	return r < 0 ? r : -APKE_EOF;
}

static void blob_get_meta(struct apk_istream *is, struct apk_file_meta *meta)
{
	*meta = (struct apk_file_meta) { };
}

static ssize_t blob_read(struct apk_istream *is, void *ptr, size_t size)
{
	return 0;
}

static int blob_close(struct apk_istream *is)
{
	return is->err < 0 ? is->err : 0;
}

static const struct apk_istream_ops blob_istream_ops = {
	.get_meta = blob_get_meta,
	.read = blob_read,
	.close = blob_close,
};

struct apk_istream *apk_istream_from_blob(struct apk_istream *is, apk_blob_t blob)
{
	*is = (struct apk_istream) {
		.ops = &blob_istream_ops,
		.buf = (uint8_t*) blob.ptr,
		.buf_size = blob.len,
		.ptr = (uint8_t*) blob.ptr,
		.end = (uint8_t*) blob.ptr + blob.len,
		.flags = APK_ISTREAM_SINGLE_READ,
		.err = 1,
	};
	return is;
}

static void segment_get_meta(struct apk_istream *is, struct apk_file_meta *meta)
{
	struct apk_segment_istream *sis = container_of(is, struct apk_segment_istream, is);
	*meta = (struct apk_file_meta) {
		.atime = sis->mtime,
		.mtime = sis->mtime,
	};
}

static ssize_t segment_read(struct apk_istream *is, void *ptr, size_t size)
{
	struct apk_segment_istream *sis = container_of(is, struct apk_segment_istream, is);
	ssize_t r;

	if (size > sis->bytes_left) size = sis->bytes_left;
	if (size == 0) return 0;

	r = sis->pis->ops->read(sis->pis, ptr, size);
	if (r <= 0) {
		/* If inner stream returned zero (end-of-stream), we
		 * are getting short read, because tar header indicated
		 * more was to be expected. */
		if (r == 0) r = -ECONNABORTED;
	} else {
		sis->bytes_left -= r;
	}
	return r;
}

static int segment_close(struct apk_istream *is)
{
	int r = is->err;
	struct apk_segment_istream *sis = container_of(is, struct apk_segment_istream, is);

	if (sis->bytes_left) {
		apk_istream_read(sis->pis, NULL, sis->bytes_left);
		sis->bytes_left = 0;
	}
	return r < 0 ? r : 0;
}

static const struct apk_istream_ops segment_istream_ops = {
	.get_meta = segment_get_meta,
	.read = segment_read,
	.close = segment_close,
};

struct apk_istream *apk_istream_segment(struct apk_segment_istream *sis, struct apk_istream *is, size_t len, time_t mtime)
{
	*sis = (struct apk_segment_istream) {
		.is.ops = &segment_istream_ops,
		.is.buf = is->buf,
		.is.buf_size = is->buf_size,
		.is.ptr = is->ptr,
		.is.end = is->end,
		.pis = is,
		.bytes_left = len,
		.mtime = mtime,
	};
	if (sis->is.end - sis->is.ptr > len) {
		sis->is.end = sis->is.ptr + len;
		is->ptr += len;
	} else {
		is->ptr = is->end = 0;
	}
	sis->bytes_left -= sis->is.end - sis->is.ptr;
	return &sis->is;
}

static void digest_get_meta(struct apk_istream *is, struct apk_file_meta *meta)
{
	struct apk_digest_istream *dis = container_of(is, struct apk_digest_istream, is);
	return apk_istream_get_meta(dis->pis, meta);
}

static ssize_t digest_read(struct apk_istream *is, void *ptr, size_t size)
{
	struct apk_digest_istream *dis = container_of(is, struct apk_digest_istream, is);
	ssize_t r;

	r = dis->pis->ops->read(dis->pis, ptr, size);
	if (r > 0) {
		apk_digest_ctx_update(&dis->dctx, ptr, r);
		dis->size_left -= r;
	}
	return r;
}

static int digest_close(struct apk_istream *is)
{
	struct apk_digest_istream *dis = container_of(is, struct apk_digest_istream, is);

	if (dis->digest && dis->size_left == 0) {
		struct apk_digest res;
		apk_digest_ctx_final(&dis->dctx, &res);
		if (apk_digest_cmp(&res, dis->digest) != 0)
			apk_istream_error(is, -APKE_FILE_INTEGRITY);
		dis->digest = 0;
	}
	apk_digest_ctx_free(&dis->dctx);

	return is->err < 0 ? is->err : 0;
}

static const struct apk_istream_ops digest_istream_ops = {
	.get_meta = digest_get_meta,
	.read = digest_read,
	.close = digest_close,
};

struct apk_istream *apk_istream_verify(struct apk_digest_istream *dis, struct apk_istream *is, off_t size, struct apk_digest *d)
{
	*dis = (struct apk_digest_istream) {
		.is.ops = &digest_istream_ops,
		.is.buf = is->buf,
		.is.buf_size = is->buf_size,
		.is.ptr = is->ptr,
		.is.end = is->end,
		.pis = is,
		.digest = d,
		.size_left = size,
	};
	apk_digest_ctx_init(&dis->dctx, d->alg);
	if (dis->is.ptr != dis->is.end) {
		apk_digest_ctx_update(&dis->dctx, dis->is.ptr, dis->is.end - dis->is.ptr);
		dis->size_left -= dis->is.end - dis->is.ptr;
	}
	return &dis->is;
}

struct apk_tee_istream {
	struct apk_istream is;
	struct apk_istream *inner_is;
	struct apk_ostream *to;
	int flags;
	size_t size;
	apk_progress_cb cb;
	void *cb_ctx;
};

static void tee_get_meta(struct apk_istream *is, struct apk_file_meta *meta)
{
	struct apk_tee_istream *tee = container_of(is, struct apk_tee_istream, is);
	apk_istream_get_meta(tee->inner_is, meta);
}

static int __tee_write(struct apk_tee_istream *tee, void *ptr, size_t size)
{
	int r = apk_ostream_write(tee->to, ptr, size);
	if (r < 0) return r;
	tee->size += size;
	if (tee->cb) tee->cb(tee->cb_ctx, tee->size);
	return size;
}

static ssize_t tee_read(struct apk_istream *is, void *ptr, size_t size)
{
	struct apk_tee_istream *tee = container_of(is, struct apk_tee_istream, is);
	ssize_t r;

	r = tee->inner_is->ops->read(tee->inner_is, ptr, size);
	if (r <= 0) return r;

	return __tee_write(tee, ptr, r);
}

static int tee_close(struct apk_istream *is)
{
	struct apk_tee_istream *tee = container_of(is, struct apk_tee_istream, is);
	int r;

	if (tee->flags & APK_ISTREAM_TEE_COPY_META)
		apk_ostream_copy_meta(tee->to, tee->inner_is);

	r = apk_istream_close_error(tee->inner_is, tee->is.err);
	if (r < 0) apk_ostream_cancel(tee->to, r);
	r = apk_ostream_close(tee->to);
	free(tee);
	return r;
}

static const struct apk_istream_ops tee_istream_ops = {
	.get_meta = tee_get_meta,
	.read = tee_read,
	.close = tee_close,
};

struct apk_istream *apk_istream_tee(struct apk_istream *from, struct apk_ostream *to, int flags, apk_progress_cb cb, void *cb_ctx)
{
	struct apk_tee_istream *tee;
	int r;

	if (IS_ERR(from)) {
		r = PTR_ERR(from);
		goto err;
	}
	if (IS_ERR(to)) {
		r = PTR_ERR(to);
		goto err;
	}

	tee = malloc(sizeof *tee);
	if (!tee) {
		r = -ENOMEM;
		goto err;
	}

	*tee = (struct apk_tee_istream) {
		.is.ops = &tee_istream_ops,
		.is.buf = from->buf,
		.is.buf_size = from->buf_size,
		.is.ptr = from->ptr,
		.is.end = from->end,
		.inner_is = from,
		.to = to,
		.flags = flags,
		.cb = cb,
		.cb_ctx = cb_ctx,
	};

	if (from->ptr != from->end) {
		r = __tee_write(tee, from->ptr, from->end - from->ptr);
		if (r < 0) goto err_free;
	}

	return &tee->is;
err_free:
	free(tee);
err:
	if (!IS_ERR(to)) {
		apk_ostream_cancel(to, r);
		apk_ostream_close(to);
	}
	if (IS_ERR(from)) return ERR_CAST(from);
	if (flags & APK_ISTREAM_TEE_OPTIONAL) return from;
	return ERR_PTR(apk_istream_close_error(from, r));
}

struct apk_mmap_istream {
	struct apk_istream is;
	int fd;
};

static void mmap_get_meta(struct apk_istream *is, struct apk_file_meta *meta)
{
	struct apk_mmap_istream *mis = container_of(is, struct apk_mmap_istream, is);
	return apk_file_meta_from_fd(mis->fd, meta);
}

static ssize_t mmap_read(struct apk_istream *is, void *ptr, size_t size)
{
	return 0;
}

static int mmap_close(struct apk_istream *is)
{
	int r = is->err;
	struct apk_mmap_istream *mis = container_of(is, struct apk_mmap_istream, is);

	munmap(mis->is.buf, mis->is.buf_size);
	close(mis->fd);
	free(mis);
	return r < 0 ? r : 0;
}

static const struct apk_istream_ops mmap_istream_ops = {
	.get_meta = mmap_get_meta,
	.read = mmap_read,
	.close = mmap_close,
};

static inline struct apk_istream *apk_mmap_istream_from_fd(int fd)
{
	struct apk_mmap_istream *mis;
	struct stat st;
	void *ptr;

	if (fstat(fd, &st) < 0) return ERR_PTR(-errno);

	ptr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) return ERR_PTR(-errno);

	mis = malloc(sizeof *mis);
	if (mis == NULL) {
		munmap(ptr, st.st_size);
		return ERR_PTR(-ENOMEM);
	}

	*mis = (struct apk_mmap_istream) {
		.is.flags = APK_ISTREAM_SINGLE_READ,
		.is.err = 1,
		.is.ops = &mmap_istream_ops,
		.is.buf = ptr,
		.is.buf_size = st.st_size,
		.is.ptr = ptr,
		.is.end = ptr + st.st_size,
		.fd = fd,
	};
	return &mis->is;
}

struct apk_fd_istream {
	struct apk_istream is;
	int fd;
};

static void fdi_get_meta(struct apk_istream *is, struct apk_file_meta *meta)
{
	struct apk_fd_istream *fis = container_of(is, struct apk_fd_istream, is);
	apk_file_meta_from_fd(fis->fd, meta);
}

static ssize_t fdi_read(struct apk_istream *is, void *ptr, size_t size)
{
	struct apk_fd_istream *fis = container_of(is, struct apk_fd_istream, is);
	ssize_t r;

	r = read(fis->fd, ptr, size);
	if (r < 0) return -errno;
	return r;
}

static int fdi_close(struct apk_istream *is)
{
	int r = is->err;
	struct apk_fd_istream *fis = container_of(is, struct apk_fd_istream, is);

	close(fis->fd);
	free(fis);
	return r < 0 ? r : 0;
}

static const struct apk_istream_ops fd_istream_ops = {
	.get_meta = fdi_get_meta,
	.read = fdi_read,
	.close = fdi_close,
};

struct apk_istream *apk_istream_from_fd(int fd)
{
	struct apk_fd_istream *fis;

	if (fd < 0) return ERR_PTR(-EBADF);

	fis = malloc(sizeof(*fis) + apk_io_bufsize);
	if (fis == NULL) {
		close(fd);
		return ERR_PTR(-ENOMEM);
	}

	*fis = (struct apk_fd_istream) {
		.is.ops = &fd_istream_ops,
		.is.buf = (uint8_t *)(fis + 1),
		.is.buf_size = apk_io_bufsize,
		.fd = fd,
	};

	return &fis->is;
}

struct apk_istream *apk_istream_from_fd_url_if_modified(int atfd, const char *url, time_t since)
{
	const char *fn = apk_url_local_file(url);
	if (fn != NULL) return apk_istream_from_file(atfd, fn);
	return apk_io_url_istream(url, since);
}

struct apk_istream *__apk_istream_from_file(int atfd, const char *file, int try_mmap)
{
	int fd;

	if (atfd_error(atfd)) return ERR_PTR(atfd);

	fd = openat(atfd, file, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return ERR_PTR(-errno);

	if (try_mmap) {
		struct apk_istream *is = apk_mmap_istream_from_fd(fd);
		if (!IS_ERR(is)) return is;
	}
	return apk_istream_from_fd(fd);
}

ssize_t apk_stream_copy(struct apk_istream *is, struct apk_ostream *os, size_t size,
			apk_progress_cb cb, void *cb_ctx, struct apk_digest_ctx *dctx)
{
	size_t done = 0;
	apk_blob_t d;
	int r;

	if (IS_ERR(is)) return PTR_ERR(is);
	if (IS_ERR(os)) return PTR_ERR(os);

	while (done < size) {
		if (cb != NULL) cb(cb_ctx, done);

		r = apk_istream_get_max(is, size - done, &d);
		if (r < 0) {
			if (r == -APKE_EOF && size == APK_IO_ALL) break;
			apk_ostream_cancel(os, r);
			return r;
		}
		if (dctx) apk_digest_ctx_update(dctx, d.ptr, d.len);

		r = apk_ostream_write(os, d.ptr, d.len);
		if (r < 0) return r;

		done += d.len;
	}
	return done;
}

int apk_blob_from_istream(struct apk_istream *is, size_t size, apk_blob_t *b)
{
	void *ptr;
	int r;

	*b = APK_BLOB_NULL;

	ptr = malloc(size);
	if (!ptr) return -ENOMEM;

	r = apk_istream_read(is, ptr, size);
	if (r < 0) {
		free(ptr);
		return r;
	}
	*b = APK_BLOB_PTR_LEN(ptr, size);
	return r;
}

int apk_blob_from_file(int atfd, const char *file, apk_blob_t *b)
{
	struct stat st;
	char *buf;
	ssize_t n;
	int fd;

	*b = APK_BLOB_NULL;

	if (atfd_error(atfd)) return atfd;

	fd = openat(atfd, file, O_RDONLY | O_CLOEXEC);
	if (fd < 0) goto err;
	if (fstat(fd, &st) < 0) goto err_fd;

	buf = malloc(st.st_size);
	if (!buf) goto err_fd;

	n = read(fd, buf, st.st_size);
	if (n != st.st_size) {
		if (n >= 0) errno = EIO;
		goto err_read;
	}

	close(fd);
	*b = APK_BLOB_PTR_LEN(buf, st.st_size);
	return 0;

err_read:
	free(buf);
err_fd:
	close(fd);
err:
	return -errno;
}

static int cmp_xattr(const void *p1, const void *p2)
{
	const struct apk_xattr *d1 = p1, *d2 = p2;
	return strcmp(d1->name, d2->name);
}

static void hash_len_data(struct apk_digest_ctx *ctx, uint32_t len, const void *ptr)
{
	uint32_t belen = htobe32(len);
	apk_digest_ctx_update(ctx, &belen, sizeof(belen));
	apk_digest_ctx_update(ctx, ptr, len);
}

static void apk_fileinfo_hash_xattr_array(struct apk_xattr_array *xattrs, uint8_t alg, struct apk_digest *d)
{
	struct apk_xattr *xattr;
	struct apk_digest_ctx dctx;

	apk_digest_reset(d);
	if (apk_array_len(xattrs) == 0) return;
	if (apk_digest_ctx_init(&dctx, alg)) return;

	apk_array_qsort(xattrs, cmp_xattr);
	foreach_array_item(xattr, xattrs) {
		hash_len_data(&dctx, strlen(xattr->name), xattr->name);
		hash_len_data(&dctx, xattr->value.len, xattr->value.ptr);
	}
	apk_digest_ctx_final(&dctx, d);
	apk_digest_ctx_free(&dctx);
}

void apk_fileinfo_hash_xattr(struct apk_file_info *fi, uint8_t alg)
{
	apk_fileinfo_hash_xattr_array(fi->xattrs, alg, &fi->xattr_digest);
}

int apk_fileinfo_get(int atfd, const char *filename, unsigned int flags,
		     struct apk_file_info *fi, struct apk_atom_pool *atoms)
{
	struct stat st;
	unsigned int hash_alg = flags & 0xff;
	unsigned int xattr_hash_alg = (flags >> 8) & 0xff;
	int atflags = 0;

	memset(fi, 0, sizeof *fi);

	if (atfd_error(atfd)) return atfd;
	if (flags & APK_FI_NOFOLLOW) atflags |= AT_SYMLINK_NOFOLLOW;
	if (fstatat(atfd, filename, &st, atflags) != 0) return -errno;

	*fi = (struct apk_file_info) {
		.size = st.st_size,
		.uid = st.st_uid,
		.gid = st.st_gid,
		.mode = st.st_mode,
		.mtime = st.st_mtime,
		.device = st.st_rdev,
		.data_device = st.st_dev,
		.data_inode = st.st_ino,
		.num_links = st.st_nlink,
	};

	if (xattr_hash_alg != APK_DIGEST_NONE && !S_ISLNK(fi->mode) && !S_ISFIFO(fi->mode)) {
		ssize_t len, vlen;
		int fd, i, r;
		char val[1024], buf[1024];

		r = 0;
		fd = openat(atfd, filename, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) {
			len = apk_flistxattr(fd, buf, sizeof(buf));
			if (len > 0) {
				struct apk_xattr_array *xattrs = NULL;
				apk_xattr_array_init(&xattrs);
				for (i = 0; i < len; i += strlen(&buf[i]) + 1) {
					vlen = apk_fgetxattr(fd, &buf[i], val, sizeof(val));
					if (vlen < 0) {
						r = errno;
						if (r == ENODATA) continue;
						break;
					}
					apk_xattr_array_add(&xattrs, (struct apk_xattr) {
						.name = &buf[i],
						.value = *apk_atomize_dup(atoms, APK_BLOB_PTR_LEN(val, vlen)),
					});
				}
				apk_fileinfo_hash_xattr_array(xattrs, xattr_hash_alg, &fi->xattr_digest);
				apk_xattr_array_free(&xattrs);
			} else if (r < 0) r = errno;
			close(fd);
		} else r = errno;

		if (r && r != ENOTSUP) return -r;
	}

	if (hash_alg == APK_DIGEST_NONE) return 0;
	if (S_ISDIR(st.st_mode)) return 0;

	/* Checksum file content */
	if ((flags & APK_FI_NOFOLLOW) && S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		if (st.st_size > sizeof target) return -ENOMEM;
		if (readlinkat(atfd, filename, target, st.st_size) < 0)
			return -errno;
		apk_digest_calc(&fi->digest, hash_alg, target, st.st_size);
	} else {
		struct apk_istream *is = apk_istream_from_file(atfd, filename);
		if (!IS_ERR(is)) {
			struct apk_digest_ctx dctx;
			apk_blob_t blob;

			if (apk_digest_ctx_init(&dctx, hash_alg) == 0) {
				while (apk_istream_get_all(is, &blob) == 0)
					apk_digest_ctx_update(&dctx, blob.ptr, blob.len);
				apk_digest_ctx_final(&dctx, &fi->digest);
				apk_digest_ctx_free(&dctx);
			}
			return apk_istream_close(is);
		}
	}

	return 0;
}

int apk_dir_foreach_file(int dirfd, apk_dir_file_cb cb, void *ctx)
{
	struct dirent *de;
	DIR *dir;
	int ret = 0;

	if (dirfd < 0)
		return -1;

	dir = fdopendir(dirfd);
	if (!dir) {
		close(dirfd);
		return -1;
	}

	/* We get called here with dup():ed fd. Since they all refer to
	 * same object, we need to rewind so subsequent calls work. */
	rewinddir(dir);

	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			if (de->d_name[1] == 0 ||
			    (de->d_name[1] == '.' && de->d_name[2] == 0))
				continue;
		}
		ret = cb(ctx, dirfd, de->d_name);
		if (ret) break;
	}
	closedir(dir);
	return ret;
}

struct apk_fd_ostream {
	struct apk_ostream os;
	int fd;

	const char *file;
	int atfd;

	size_t bytes;
	char buffer[1024];
};

static ssize_t fdo_flush(struct apk_fd_ostream *fos)
{
	ssize_t r;

	if (fos->os.rc < 0) return fos->os.rc;
	if (fos->bytes == 0) return 0;
	if ((r = apk_write_fully(fos->fd, fos->buffer, fos->bytes)) != fos->bytes)
		return apk_ostream_cancel(&fos->os, r < 0 ? r : -ENOSPC);

	fos->bytes = 0;
	return 0;
}


static void fdo_set_meta(struct apk_ostream *os, struct apk_file_meta *meta)
{
	struct apk_fd_ostream *fos = container_of(os, struct apk_fd_ostream, os);
	struct timespec times[2] = {
		{ .tv_sec = meta->atime, .tv_nsec = meta->atime ? 0 : UTIME_OMIT },
		{ .tv_sec = meta->mtime, .tv_nsec = meta->mtime ? 0 : UTIME_OMIT }
	};
	futimens(fos->fd, times);
}

static int fdo_write(struct apk_ostream *os, const void *ptr, size_t size)
{
	struct apk_fd_ostream *fos = container_of(os, struct apk_fd_ostream, os);
	ssize_t r;

	if (size + fos->bytes >= sizeof(fos->buffer)) {
		r = fdo_flush(fos);
		if (r != 0) return r;
		if (size >= sizeof(fos->buffer) / 2) {
			r = apk_write_fully(fos->fd, ptr, size);
			if (r == size) return 0;
			return apk_ostream_cancel(&fos->os, r < 0 ? r : -ENOSPC);
		}
	}

	memcpy(&fos->buffer[fos->bytes], ptr, size);
	fos->bytes += size;

	return 0;
}

static int fdo_close(struct apk_ostream *os)
{
	struct apk_fd_ostream *fos = container_of(os, struct apk_fd_ostream, os);
	char tmpname[PATH_MAX];
	int rc;

	fdo_flush(fos);
	if (fos->fd > STDERR_FILENO && close(fos->fd) < 0)
		apk_ostream_cancel(os, -errno);

	rc = fos->os.rc;
	if (fos->file && apk_fmt(tmpname, sizeof tmpname, "%s.tmp", fos->file) > 0) {
		if (rc == 0) {
			if (renameat(fos->atfd, tmpname,
				     fos->atfd, fos->file) < 0)
				rc = -errno;
		} else {
			unlinkat(fos->atfd, tmpname, 0);
		}
	}
	free(fos);

	return rc;
}

static const struct apk_ostream_ops fd_ostream_ops = {
	.set_meta = fdo_set_meta,
	.write = fdo_write,
	.close = fdo_close,
};

struct apk_ostream *apk_ostream_to_fd(int fd)
{
	struct apk_fd_ostream *fos;

	if (fd < 0) return ERR_PTR(-EBADF);

	fos = malloc(sizeof(struct apk_fd_ostream));
	if (fos == NULL) {
		close(fd);
		return ERR_PTR(-ENOMEM);
	}

	*fos = (struct apk_fd_ostream) {
		.os.ops = &fd_ostream_ops,
		.fd = fd,
	};

	return &fos->os;
}

struct apk_ostream *apk_ostream_to_file(int atfd, const char *file, mode_t mode)
{
	char tmpname[PATH_MAX];
	struct apk_ostream *os;
	int fd;

	if (atfd_error(atfd)) return ERR_PTR(atfd);
	if (apk_fmt(tmpname, sizeof tmpname, "%s.tmp", file) < 0) return ERR_PTR(-ENAMETOOLONG);

	fd = openat(atfd, tmpname, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0) return ERR_PTR(-errno);

	os = apk_ostream_to_fd(fd);
	if (IS_ERR(os)) return ERR_CAST(os);

	struct apk_fd_ostream *fos = container_of(os, struct apk_fd_ostream, os);
	fos->file = file;
	fos->atfd = atfd;

	return os;
}

struct apk_counter_ostream {
	struct apk_ostream os;
	off_t *counter;
};

static int co_write(struct apk_ostream *os, const void *ptr, size_t size)
{
	struct apk_counter_ostream *cos = container_of(os, struct apk_counter_ostream, os);
	*cos->counter += size;
	return 0;
}

static int co_close(struct apk_ostream *os)
{
	struct apk_counter_ostream *cos = container_of(os, struct apk_counter_ostream, os);
	int rc = os->rc;

	free(cos);
	return rc;
}

static const struct apk_ostream_ops counter_ostream_ops = {
	.write = co_write,
	.close = co_close,
};

struct apk_ostream *apk_ostream_counter(off_t *counter)
{
	struct apk_counter_ostream *cos;

	cos = malloc(sizeof(struct apk_counter_ostream));
	if (cos == NULL)
		return NULL;

	*cos = (struct apk_counter_ostream) {
		.os.ops = &counter_ostream_ops,
		.counter = counter,
	};

	return &cos->os;
}

ssize_t apk_ostream_write_string(struct apk_ostream *os, const char *string)
{
	size_t len;
	ssize_t r;

	len = strlen(string);
	r = apk_ostream_write(os, string, len);
	if (r < 0) return r;
	return len;
}

void apk_ostream_copy_meta(struct apk_ostream *os, struct apk_istream *is)
{
	struct apk_file_meta meta;
	apk_istream_get_meta(is, &meta);
	os->ops->set_meta(os, &meta);
}

struct cache_item {
	struct hlist_node by_id, by_name;
	unsigned long id;
	unsigned short len;
	char name[];
};

static void idhash_init(struct apk_id_hash *idh)
{
	memset(idh, 0, sizeof *idh);
	idh->empty = 1;
}

static void idhash_reset(struct apk_id_hash *idh)
{
	struct hlist_node *iter, *next;
	struct cache_item *ci;
	int i;

	for (i = 0; i < ARRAY_SIZE(idh->by_id); i++)
		hlist_for_each_entry_safe(ci, iter, next, &idh->by_id[i], by_id)
			free(ci);
	idhash_init(idh);
}

static void idcache_add(struct apk_id_hash *hash, apk_blob_t name, unsigned long id)
{
	struct cache_item *ci;
	unsigned long h;

	ci = calloc(1, sizeof(struct cache_item) + name.len);
	if (!ci) return;

	ci->id = id;
	ci->len = name.len;
	memcpy(ci->name, name.ptr, name.len);

	h = apk_blob_hash(name);
	hlist_add_head(&ci->by_id, &hash->by_id[id % ARRAY_SIZE(hash->by_id)]);
	hlist_add_head(&ci->by_name, &hash->by_name[h % ARRAY_SIZE(hash->by_name)]);
}

static struct cache_item *idcache_by_name(struct apk_id_hash *hash, apk_blob_t name)
{
	struct cache_item *ci;
	struct hlist_node *pos;
	unsigned long h = apk_blob_hash(name);

	hlist_for_each_entry(ci, pos, &hash->by_name[h % ARRAY_SIZE(hash->by_name)], by_name)
		if (apk_blob_compare(name, APK_BLOB_PTR_LEN(ci->name, ci->len)) == 0)
			return ci;
	return 0;
}

static struct cache_item *idcache_by_id(struct apk_id_hash *hash, unsigned long id)
{
	struct cache_item *ci;
	struct hlist_node *pos;

	hlist_for_each_entry(ci, pos, &hash->by_id[id % ARRAY_SIZE(hash->by_name)], by_id)
		if (ci->id == id) return ci;
	return 0;
}

const char *apk_url_local_file(const char *url)
{
	if (strncmp(url, "file:", 5) == 0) {
		if (strncmp(url, "file://localhost/", 17) == 0)
			return &url[16];
		return &url[5];
	}

	if (strncmp(url, "http:", 5) != 0 &&
	    strncmp(url, "https:", 6) != 0 &&
	    strncmp(url, "ftp:", 4) != 0)
		return url;

	return NULL;
}

void apk_id_cache_init(struct apk_id_cache *idc, int root_fd)
{
	idc->root_fd = root_fd;
	idhash_init(&idc->uid_cache);
	idhash_init(&idc->gid_cache);
}

void apk_id_cache_reset(struct apk_id_cache *idc)
{
	idhash_reset(&idc->uid_cache);
	idhash_reset(&idc->gid_cache);
}

void apk_id_cache_free(struct apk_id_cache *idc)
{
	apk_id_cache_reset(idc);
	idc->root_fd = 0;
}

static FILE *fopenat(int dirfd, const char *pathname)
{
	FILE *f;
	int fd;

	fd = openat(dirfd, pathname, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return NULL;

	f = fdopen(fd, "r");
	if (!f) close(fd);
	return f;
}

static void idcache_load_users(int root_fd, struct apk_id_hash *idh)
{
#ifdef HAVE_FGETPWENT_R
	char buf[1024];
	struct passwd pwent;
#endif
	struct passwd *pwd;
	FILE *in;

	if (!idh->empty) return;
	idh->empty = 0;

	in = fopenat(root_fd, "etc/passwd");
	if (!in) return;

	do {
#ifdef HAVE_FGETPWENT_R
		fgetpwent_r(in, &pwent, buf, sizeof(buf), &pwd);
#elif !defined(__APPLE__)
		pwd = fgetpwent(in);
#else
# warning macOS does not support nested /etc/passwd databases, using system one.
		pwd = getpwent();
#endif
		if (!pwd) break;
		idcache_add(idh, APK_BLOB_STR(pwd->pw_name), pwd->pw_uid);
	} while (1);
	fclose(in);
#ifndef HAVE_FGETPWENT_R
	endpwent();
#endif
}

static void idcache_load_groups(int root_fd, struct apk_id_hash *idh)
{
#ifdef HAVE_FGETGRENT_R
	char buf[1024];
	struct group grent;
#endif
	struct group *grp;
	FILE *in;

	if (!idh->empty) return;
	idh->empty = 0;

	in = fopenat(root_fd, "etc/group");
	if (!in) return;

	do {
#ifdef HAVE_FGETGRENT_R
		fgetgrent_r(in, &grent, buf, sizeof(buf), &grp);
#elif !defined(__APPLE__)
		grp = fgetgrent(in);
#else
# warning macOS does not support nested /etc/group databases, using system one.
		grp = getgrent();
#endif
		if (!grp) break;
		idcache_add(idh, APK_BLOB_STR(grp->gr_name), grp->gr_gid);
	} while (1);
	fclose(in);
#ifndef HAVE_FGETGRENT_R
	endgrent();
#endif
}

uid_t apk_id_cache_resolve_uid(struct apk_id_cache *idc, apk_blob_t username, uid_t default_uid)
{
	struct cache_item *ci;
	idcache_load_users(idc->root_fd, &idc->uid_cache);
	ci = idcache_by_name(&idc->uid_cache, username);
	if (ci) return ci->id;
	if (!apk_blob_compare(username, APK_BLOB_STRLIT("root"))) return 0;
	return default_uid;
}

gid_t apk_id_cache_resolve_gid(struct apk_id_cache *idc, apk_blob_t groupname, gid_t default_gid)
{
	struct cache_item *ci;
	idcache_load_groups(idc->root_fd, &idc->gid_cache);
	ci = idcache_by_name(&idc->gid_cache, groupname);
	if (ci) return ci->id;
	if (!apk_blob_compare(groupname, APK_BLOB_STRLIT("root"))) return 0;
	return default_gid;
}

apk_blob_t apk_id_cache_resolve_user(struct apk_id_cache *idc, uid_t uid)
{
	struct cache_item *ci;
	idcache_load_users(idc->root_fd, &idc->uid_cache);
	ci = idcache_by_id(&idc->uid_cache, uid);
	if (ci) return APK_BLOB_PTR_LEN(ci->name, ci->len);
	if (uid == 0) return APK_BLOB_STRLIT("root");
	return APK_BLOB_STRLIT("nobody");
}

apk_blob_t apk_id_cache_resolve_group(struct apk_id_cache *idc, gid_t gid)
{
	struct cache_item *ci;
	idcache_load_groups(idc->root_fd, &idc->gid_cache);
	ci = idcache_by_id(&idc->gid_cache, gid);
	if (ci) return APK_BLOB_PTR_LEN(ci->name, ci->len);
	if (gid == 0) return APK_BLOB_STRLIT("root");
	return APK_BLOB_STRLIT("nobody");
}
