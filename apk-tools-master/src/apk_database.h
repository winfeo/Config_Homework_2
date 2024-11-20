/* apk_database.h - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008-2011 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef APK_PKGDB_H
#define APK_PKGDB_H

#include "apk_version.h"
#include "apk_hash.h"
#include "apk_atom.h"
#include "apk_balloc.h"
#include "apk_package.h"
#include "apk_io.h"
#include "apk_context.h"

#include "apk_provider_data.h"
#include "apk_solver_data.h"

struct apk_name;
APK_ARRAY(apk_name_array, struct apk_name *);

struct apk_db_acl {
	mode_t mode;
	uid_t uid;
	gid_t gid;
	uint8_t xattr_hash_len;
	uint8_t xattr_hash[];
} __attribute__((packed));

static inline apk_blob_t apk_acl_digest_blob(struct apk_db_acl *acl) {
	return APK_BLOB_PTR_LEN((char*) acl->xattr_hash, acl->xattr_hash_len);
}

struct apk_db_file {
	struct hlist_node hash_node;
	struct hlist_node diri_files_list;

	struct apk_db_dir_instance *diri;
	struct apk_db_acl *acl;

	unsigned char audited : 1;
	unsigned char broken : 1;
	unsigned char digest_alg : 6;
	unsigned char namelen;
	uint8_t digest[20]; // sha1 length
	char name[];
};

static inline apk_blob_t apk_dbf_digest_blob(struct apk_db_file *file) {
	return APK_BLOB_PTR_LEN((char*) file->digest, apk_digest_alg_len(file->digest_alg));
}
static inline void apk_dbf_digest_set(struct apk_db_file *file, uint8_t alg, const uint8_t *data) {
	uint8_t len = apk_digest_alg_len(alg);
	if (len > sizeof file->digest) {
		file->digest_alg = APK_DIGEST_NONE;
		return;
	}
	file->digest_alg = alg;
	memcpy(file->digest, data, len);
}

enum apk_protect_mode {
	APK_PROTECT_NONE = 0,
	APK_PROTECT_IGNORE,
	APK_PROTECT_CHANGED,
	APK_PROTECT_SYMLINKS_ONLY,
	APK_PROTECT_ALL,
};

static inline int apk_protect_mode_none(enum apk_protect_mode mode)
{
	return mode == APK_PROTECT_NONE || mode == APK_PROTECT_IGNORE;
}

struct apk_protected_path {
	char *relative_pattern;
	unsigned protect_mode : 3;
};
APK_ARRAY(apk_protected_path_array, struct apk_protected_path);

struct apk_db_dir {
	apk_hash_node hash_node;
	unsigned long hash;

	struct apk_db_dir *parent;
	struct apk_db_dir_instance *owner;
	struct list_head diris;
	struct apk_protected_path_array *protected_paths;

	unsigned short refs;
	unsigned short namelen;

	unsigned char protect_mode : 3;
	unsigned char has_protected_children : 1;

	unsigned char created : 1;
	unsigned char modified : 1;
	unsigned char permissions_ok : 1;

	char rooted_name[1];
	char name[];
};

#define DIR_FILE_FMT			"%s%s%s"
#define DIR_FILE_PRINTF(dir,file)	(dir)->name, (dir)->namelen ? "/" : "", (file)->name

struct apk_db_dir_instance {
	struct list_head dir_diri_list;
	struct hlist_node pkg_dirs_list;
	struct hlist_head owned_files;
	struct apk_package *pkg;
	struct apk_db_dir *dir;
	struct apk_db_acl *acl;
};

struct apk_name {
	apk_hash_node hash_node;
	struct apk_provider_array *providers;
	struct apk_name_array *rdepends;
	struct apk_name_array *rinstall_if;
	unsigned is_dependency : 1;
	unsigned auto_select_virtual: 1;
	unsigned priority : 2;
	unsigned solver_flags_set : 1;
	unsigned providers_sorted : 1;
	unsigned int foreach_genid;
	union {
		struct apk_solver_name_state ss;
		unsigned long state_buf[4];
		int state_int;
	};
	char name[];
};

struct apk_repository {
	const char *url;
	struct apk_digest hash;
	apk_blob_t description;
};

#define APK_DB_LAYER_ROOT		0
#define APK_DB_LAYER_UVOL		1
#define APK_DB_LAYER_NUM		2

#define APK_REPOSITORY_CACHED		0
#define APK_REPOSITORY_FIRST_CONFIGURED	1

#define APK_DEFAULT_REPOSITORY_TAG	0
#define APK_DEFAULT_PINNING_MASK	BIT(APK_DEFAULT_REPOSITORY_TAG)

struct apk_repository_tag {
	unsigned int allowed_repos;
	apk_blob_t tag, plain_name;
};

struct apk_database {
	struct apk_ctx *ctx;
	struct apk_balloc ba_names;
	struct apk_balloc ba_pkgs;
	struct apk_balloc ba_files;
	struct apk_balloc ba_deps;
	int root_fd, lock_fd, cache_fd;
	unsigned num_repos, num_repo_tags;
	const char *cache_dir;
	char *cache_remount_dir, *root_proc_dir;
	apk_blob_t *noarch;
	unsigned long cache_remount_flags;
	unsigned int local_repos, available_repos;
	unsigned int pending_triggers;
	unsigned int extract_flags;
	unsigned int active_layers;
	unsigned int num_dir_update_errors;

	unsigned int root_dev_works : 1;
	unsigned int performing_self_upgrade : 1;
	unsigned int usermode : 1;
	unsigned int permanent : 1;
	unsigned int autoupdate : 1;
	unsigned int write_arch : 1;
	unsigned int script_dirs_checked : 1;
	unsigned int open_complete : 1;
	unsigned int compat_newfeatures : 1;
	unsigned int compat_notinstallable : 1;
	unsigned int compat_depversions : 1;
	unsigned int sorted_names : 1;
	unsigned int sorted_installed_packages : 1;

	struct apk_dependency_array *world;
	struct apk_id_cache *id_cache;
	struct apk_protected_path_array *protected_paths;
	struct apk_blobptr_array *arches;
	struct apk_repository repos[APK_MAX_REPOS];
	struct apk_repository_tag repo_tags[APK_MAX_TAGS];
	struct apk_atom_pool atoms;
	struct apk_string_array *filename_array;
	struct apk_package_tmpl overlay_tmpl;

	struct {
		unsigned stale, updated, unavailable;
	} repositories;

	struct {
		struct apk_name_array *sorted_names;
		struct apk_hash names;
		struct apk_hash packages;
	} available;

	struct {
		struct apk_package_array *sorted_packages;
		struct list_head packages;
		struct list_head triggers;
		struct apk_hash dirs;
		struct apk_hash files;
		struct {
			unsigned files;
			unsigned dirs;
			unsigned packages;
			size_t bytes;
		} stats;
	} installed;
};

static inline int apk_name_cmp_display(const struct apk_name *a, const struct apk_name *b) {
	return strcasecmp(a->name, b->name) ?: strcmp(a->name, b->name);
}
struct apk_provider_array *apk_name_sorted_providers(struct apk_name *);

struct apk_name *apk_db_get_name(struct apk_database *db, apk_blob_t name);
struct apk_name *apk_db_query_name(struct apk_database *db, apk_blob_t name);
int apk_db_get_tag_id(struct apk_database *db, apk_blob_t tag);

void apk_db_dir_update_permissions(struct apk_database *db, struct apk_db_dir_instance *diri);
void apk_db_dir_prepare(struct apk_database *db, struct apk_db_dir *dir, struct apk_db_acl *expected_acl, struct apk_db_acl *new_acl);
void apk_db_dir_unref(struct apk_database *db, struct apk_db_dir *dir, int allow_rmdir);
struct apk_db_dir *apk_db_dir_ref(struct apk_db_dir *dir);
struct apk_db_dir *apk_db_dir_get(struct apk_database *db, apk_blob_t name);
struct apk_db_dir *apk_db_dir_query(struct apk_database *db, apk_blob_t name);
struct apk_db_file *apk_db_file_query(struct apk_database *db,
				      apk_blob_t dir, apk_blob_t name);

const char *apk_db_layer_name(int layer);
void apk_db_init(struct apk_database *db);
int apk_db_open(struct apk_database *db, struct apk_ctx *ctx);
void apk_db_close(struct apk_database *db);
int apk_db_write_config(struct apk_database *db);
int apk_db_permanent(struct apk_database *db);
int apk_db_check_world(struct apk_database *db, struct apk_dependency_array *world);
int apk_db_fire_triggers(struct apk_database *db);
int apk_db_run_script(struct apk_database *db, int fd, char **argv);
static inline time_t apk_db_url_since(struct apk_database *db, time_t since) {
	return apk_ctx_since(db->ctx, since);
}

bool apk_db_arch_compatible(struct apk_database *db, apk_blob_t *arch);

struct apk_package *apk_db_pkg_add(struct apk_database *db, struct apk_package_tmpl *tmpl);
struct apk_package *apk_db_get_pkg(struct apk_database *db, struct apk_digest *id);
struct apk_package *apk_db_get_file_owner(struct apk_database *db, apk_blob_t filename);

int apk_db_index_read(struct apk_database *db, struct apk_istream *is, int repo);
int apk_db_index_read_file(struct apk_database *db, const char *file, int repo);

int apk_db_repository_check(struct apk_database *db);
int apk_db_add_repository(struct apk_database *db, apk_blob_t repository);
struct apk_repository *apk_db_select_repo(struct apk_database *db,
					  struct apk_package *pkg);

int apk_repo_format_cache_index(apk_blob_t to, struct apk_repository *repo);
int apk_repo_format_item(struct apk_database *db, struct apk_repository *repo, struct apk_package *pkg,
			 int *fd, char *buf, size_t len);

unsigned int apk_db_get_pinning_mask_repos(struct apk_database *db, unsigned short pinning_mask);

int apk_db_cache_active(struct apk_database *db);
int apk_cache_download(struct apk_database *db, struct apk_repository *repo,
		       struct apk_package *pkg, int autoupdate,
		       apk_progress_cb cb, void *cb_ctx);

typedef void (*apk_cache_item_cb)(struct apk_database *db, int static_cache,
				  int dirfd, const char *name,
				  struct apk_package *pkg);
int apk_db_cache_foreach_item(struct apk_database *db, apk_cache_item_cb cb, int static_cache);

int apk_db_install_pkg(struct apk_database *db,
		       struct apk_package *oldpkg,
		       struct apk_package *newpkg,
		       apk_progress_cb cb, void *cb_ctx);


struct apk_package_array *apk_db_sorted_installed_packages(struct apk_database *db);

typedef int (*apk_db_foreach_name_cb)(struct apk_database *db, const char *match, struct apk_name *name, void *ctx);

int apk_db_foreach_matching_name(struct apk_database *db, struct apk_string_array *filter,
				 apk_db_foreach_name_cb cb, void *ctx);

int apk_db_foreach_sorted_name(struct apk_database *db, struct apk_string_array *filter,
			       apk_db_foreach_name_cb cb, void *ctx);

typedef int (*apk_db_foreach_package_cb)(struct apk_database *db, const char *match, struct apk_package *pkg, void *ctx);

int __apk_db_foreach_sorted_package(struct apk_database *db, struct apk_string_array *filter,
				    apk_db_foreach_package_cb cb, void *cb_ctx, int provides);

static inline int apk_db_foreach_sorted_package(struct apk_database *db, struct apk_string_array *filter,
						apk_db_foreach_package_cb cb, void *cb_ctx) {
	return __apk_db_foreach_sorted_package(db, filter, cb, cb_ctx, 0);
}

static inline int apk_db_foreach_sorted_providers(struct apk_database *db, struct apk_string_array *filter,
						  apk_db_foreach_package_cb cb, void *cb_ctx) {
	return __apk_db_foreach_sorted_package(db, filter, cb, cb_ctx, 1);
}

#endif
