/* commit.c - Alpine Package Keeper (APK)
 * Apply solver calculated changes to database.
 *
 * Copyright (C) 2008-2013 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include "apk_defines.h"
#include "apk_database.h"
#include "apk_package.h"
#include "apk_solver.h"
#include "apk_print.h"

struct apk_stats {
	size_t bytes;
	unsigned int changes;
	unsigned int packages;
};

struct progress {
	struct apk_progress prog;
	struct apk_stats done;
	struct apk_stats total;
	struct apk_package *pkg;
	int total_changes_digits;
};

static inline int pkg_available(struct apk_database *db, struct apk_package *pkg)
{
	if (pkg->repos & db->available_repos)
		return TRUE;
	return FALSE;
}

static int print_change(struct apk_database *db, struct apk_change *change,
			struct progress *prog)
{
	struct apk_out *out = &db->ctx->out;
	struct apk_name *name;
	struct apk_package *oldpkg = change->old_pkg;
	struct apk_package *newpkg = change->new_pkg;
	const char *msg = NULL, *status;
	char statusbuf[32];
	apk_blob_t *oneversion = NULL;
	int r;

	status = apk_fmts(statusbuf, sizeof statusbuf, "(%*i/%i)",
		prog->total_changes_digits, prog->done.changes+1,
		prog->total.changes) ?: "(?)";

	name = newpkg ? newpkg->name : oldpkg->name;
	if (oldpkg == NULL) {
		msg = "Installing";
		oneversion = newpkg->version;
	} else if (newpkg == NULL) {
		msg = "Purging";
		oneversion = oldpkg->version;
	} else if (newpkg == oldpkg) {
		if (change->reinstall) {
			if (pkg_available(db, newpkg))
				msg = "Reinstalling";
			else
				msg = "[APK unavailable, skipped] Reinstalling";
		} else if (change->old_repository_tag != change->new_repository_tag) {
			msg = "Updating pinning";
		}
		oneversion = newpkg->version;
	} else {
		r = apk_pkg_version_compare(newpkg, oldpkg);
		switch (r) {
		case APK_VERSION_LESS:
			msg = "Downgrading";
			break;
		case APK_VERSION_EQUAL:
			msg = "Replacing";
			break;
		case APK_VERSION_GREATER:
			msg = "Upgrading";
			break;
		}
	}
	if (msg == NULL)
		return FALSE;

	if (oneversion) {
		apk_msg(out, "%s %s %s" BLOB_FMT " (" BLOB_FMT ")",
			status, msg,
			name->name,
			BLOB_PRINTF(db->repo_tags[change->new_repository_tag].tag),
			BLOB_PRINTF(*oneversion));
	} else {
		apk_msg(out, "%s %s %s" BLOB_FMT " (" BLOB_FMT " -> " BLOB_FMT ")",
			status, msg,
			name->name,
			BLOB_PRINTF(db->repo_tags[change->new_repository_tag].tag),
			BLOB_PRINTF(*oldpkg->version),
			BLOB_PRINTF(*newpkg->version));
	}
	return TRUE;
}

static void count_change(struct apk_change *change, struct apk_stats *stats)
{
	if (change->new_pkg != change->old_pkg || change->reinstall) {
		if (change->new_pkg != NULL) {
			stats->bytes += change->new_pkg->installed_size;
			stats->packages++;
		}
		if (change->old_pkg != NULL)
			stats->packages++;
		stats->changes++;
	} else if (change->new_repository_tag != change->old_repository_tag) {
		stats->packages++;
		stats->changes++;
	}
}

static void progress_cb(void *ctx, size_t installed_bytes)
{
	struct progress *prog = (struct progress *) ctx;
	apk_print_progress(&prog->prog,
			   prog->done.bytes + prog->done.packages + installed_bytes,
			   prog->total.bytes + prog->total.packages);
}

static int dump_packages(struct apk_out *out, struct apk_change_array *changes,
			 int (*cmp)(struct apk_change *change),
			 const char *msg)
{
	struct apk_change *change;
	struct apk_name *name;
	struct apk_indent indent;
	int match = 0;

	apk_print_indented_init(&indent, out, 0);
	foreach_array_item(change, changes) {
		if (!cmp(change)) continue;
		if (!match) apk_print_indented_group(&indent, 2, "%s:\n", msg);
		if (change->new_pkg != NULL)
			name = change->new_pkg->name;
		else
			name = change->old_pkg->name;

		apk_print_indented(&indent, APK_BLOB_STR(name->name));
		match++;
	}
	apk_print_indented_end(&indent);
	return match;
}

static int sort_change(const void *a, const void *b)
{
	const struct apk_change *ca = a;
	const struct apk_change *cb = b;
	const struct apk_name *na = ca->old_pkg ? ca->old_pkg->name : ca->new_pkg->name;
	const struct apk_name *nb = cb->old_pkg ? cb->old_pkg->name : cb->new_pkg->name;
	return apk_name_cmp_display(na, nb);
}

static int cmp_remove(struct apk_change *change)
{
	return change->new_pkg == NULL;
}

static int cmp_new(struct apk_change *change)
{
	return change->old_pkg == NULL;
}

static int cmp_reinstall(struct apk_change *change)
{
	return change->reinstall;
}

static int cmp_downgrade(struct apk_change *change)
{
	if (change->new_pkg == NULL || change->old_pkg == NULL)
		return 0;
	if (apk_pkg_version_compare(change->new_pkg, change->old_pkg)
	    & APK_VERSION_LESS)
		return 1;
	return 0;
}

static int cmp_upgrade(struct apk_change *change)
{
	if (change->new_pkg == NULL || change->old_pkg == NULL)
		return 0;

	/* Count swapping package as upgrade too - this can happen if
	 * same package version is used after it was rebuilt against
	 * newer libraries. Basically, different (and probably newer)
	 * package, but equal version number. */
	if ((apk_pkg_version_compare(change->new_pkg, change->old_pkg) &
	     (APK_VERSION_GREATER | APK_VERSION_EQUAL)) &&
	    (change->new_pkg != change->old_pkg))
		return 1;

	return 0;
}

static int run_triggers(struct apk_database *db, struct apk_changeset *changeset)
{
	struct apk_change *change;
	struct apk_installed_package *ipkg;
	int errors = 0;

	if (apk_db_fire_triggers(db) == 0)
		return 0;

	foreach_array_item(change, changeset->changes) {
		struct apk_package *pkg = change->new_pkg;
		if (pkg == NULL)
			continue;
		ipkg = pkg->ipkg;
		if (ipkg == NULL || apk_array_len(ipkg->pending_triggers) == 0)
			continue;

		apk_string_array_add(&ipkg->pending_triggers, NULL);
		errors += apk_ipkg_run_script(ipkg, db, APK_SCRIPT_TRIGGER,
					      ipkg->pending_triggers->item) != 0;
		apk_string_array_free(&ipkg->pending_triggers);
	}
	return errors;
}

#define PRE_COMMIT_HOOK		0
#define POST_COMMIT_HOOK	1

struct apk_commit_hook {
	struct apk_database *db;
	int type;
};

static int run_commit_hook(void *ctx, int dirfd, const char *file)
{
	static char *const commit_hook_str[] = { "pre-commit", "post-commit" };
	struct apk_commit_hook *hook = (struct apk_commit_hook *) ctx;
	struct apk_database *db = hook->db;
	struct apk_out *out = &db->ctx->out;
	char fn[PATH_MAX], *argv[] = { fn, (char *) commit_hook_str[hook->type], NULL };
	int ret = 0;

	if (file[0] == '.') return 0;
	if ((db->ctx->flags & (APK_NO_SCRIPTS | APK_SIMULATE)) != 0) return 0;
	if (apk_fmt(fn, sizeof fn, "etc/apk/commit_hooks.d/%s", file) < 0) return 0;

	if ((db->ctx->flags & APK_NO_COMMIT_HOOKS) != 0) {
		apk_msg(out, "Skipping: %s %s", fn, commit_hook_str[hook->type]);
		return 0;
	}
	apk_dbg(out, "Executing: %s %s", fn, commit_hook_str[hook->type]);

	if (apk_db_run_script(db, -1, argv) < 0 && hook->type == PRE_COMMIT_HOOK)
		ret = -2;

	return ret;
}

static int run_commit_hooks(struct apk_database *db, int type)
{
	struct apk_commit_hook hook = { .db = db, .type = type };
	return apk_dir_foreach_file(openat(db->root_fd, "etc/apk/commit_hooks.d", O_DIRECTORY | O_RDONLY | O_CLOEXEC),
				    run_commit_hook, &hook);
}

static int calc_precision(unsigned int num)
{
	int precision = 1;
	while (num >= 10) {
		precision++;
		num /= 10;
	}
	return precision;
}

int apk_solver_commit_changeset(struct apk_database *db,
				struct apk_changeset *changeset,
				struct apk_dependency_array *world)
{
	struct apk_out *out = &db->ctx->out;
	struct progress prog = { .prog = db->ctx->progress };
	struct apk_change *change;
	const char *size_unit;
	off_t humanized, size_diff = 0, download_size = 0;
	int r, errors = 0, pkg_diff = 0;

	assert(world);
	if (apk_db_check_world(db, world) != 0) {
		apk_err(out, "Not committing changes due to missing repository tags. "
			"Use --force-broken-world to override.");
		return -1;
	}

	if (changeset->changes == NULL)
		goto all_done;

	/* Count what needs to be done */
	foreach_array_item(change, changeset->changes) {
		count_change(change, &prog.total);
		if (change->new_pkg) {
			size_diff += change->new_pkg->installed_size;
			pkg_diff++;
			if (change->new_pkg != change->old_pkg &&
			    !(change->new_pkg->repos & db->local_repos))
				download_size += change->new_pkg->size;
		}
		if (change->old_pkg) {
			size_diff -= change->old_pkg->installed_size;
			pkg_diff--;
		}
	}
	prog.total_changes_digits = calc_precision(prog.total.changes);

	if ((apk_out_verbosity(out) > 1 || (db->ctx->flags & APK_INTERACTIVE)) &&
	    !(db->ctx->flags & APK_SIMULATE)) {
		struct apk_change_array *sorted;

		apk_change_array_init(&sorted);
		apk_change_array_copy(&sorted, changeset->changes);
		apk_array_qsort(sorted, sort_change);

		r = dump_packages(out, sorted, cmp_remove,
				  "The following packages will be REMOVED");
		r += dump_packages(out, sorted, cmp_downgrade,
				   "The following packages will be DOWNGRADED");
		if (r || (db->ctx->flags & APK_INTERACTIVE) || apk_out_verbosity(out) > 2) {
			r += dump_packages(out, sorted, cmp_new,
					   "The following NEW packages will be installed");
			r += dump_packages(out, sorted, cmp_upgrade,
					   "The following packages will be upgraded");
			r += dump_packages(out, sorted, cmp_reinstall,
					   "The following packages will be reinstalled");
			if (download_size) {
				size_unit = apk_get_human_size(download_size, &humanized);
				apk_msg(out, "Need to download %lld %s of packages.",
					(long long)humanized, size_unit);
			}
			size_unit = apk_get_human_size(llabs(size_diff), &humanized);
			apk_msg(out, "After this operation, %lld %s of %s.",
				(long long)humanized,
				size_unit,
				(size_diff < 0) ?
				"disk space will be freed" :
				"additional disk space will be used");
		}
		apk_change_array_free(&sorted);

		if (r > 0 && (db->ctx->flags & APK_INTERACTIVE)) {
			printf("Do you want to continue [Y/n]? ");
			fflush(stdout);
			r = fgetc(stdin);
			if (r != 'y' && r != 'Y' && r != '\n' && r != EOF)
				return -1;
		}
	}

	if (run_commit_hooks(db, PRE_COMMIT_HOOK) == -2)
		return -1;

	/* Go through changes */
	foreach_array_item(change, changeset->changes) {
		r = change->old_pkg &&
			(change->old_pkg->ipkg->broken_files ||
			 change->old_pkg->ipkg->broken_script);
		if (print_change(db, change, &prog)) {
			prog.pkg = change->new_pkg;
			progress_cb(&prog, 0);

			if (!(db->ctx->flags & APK_SIMULATE) &&
			    ((change->old_pkg != change->new_pkg) ||
			     (change->reinstall && pkg_available(db, change->new_pkg)))) {
				r = apk_db_install_pkg(db, change->old_pkg, change->new_pkg,
						       progress_cb, &prog) != 0;
			}
			if (r == 0 && change->new_pkg && change->new_pkg->ipkg)
				change->new_pkg->ipkg->repository_tag = change->new_repository_tag;
		}
		errors += r;
		count_change(change, &prog.done);
	}
	apk_print_progress(&prog.prog, prog.total.bytes + prog.total.packages,
			   prog.total.bytes + prog.total.packages);

	errors += db->num_dir_update_errors;
	errors += run_triggers(db, changeset);

all_done:
	apk_dependency_array_copy(&db->world, world);
	if (apk_db_write_config(db) != 0) errors++;
	run_commit_hooks(db, POST_COMMIT_HOOK);

	if (!db->performing_self_upgrade) {
		char buf[32];
		const char *msg = "OK:";

		if (errors) msg = apk_fmts(buf, sizeof buf, "%d error%s;",
				errors, errors > 1 ? "s" : "") ?: "ERRORS;";

		off_t installed_bytes = db->installed.stats.bytes;
		int installed_packages = db->installed.stats.packages;
		if (db->ctx->flags & APK_SIMULATE) {
			installed_bytes += size_diff;
			installed_packages += pkg_diff;
		}

		if (apk_out_verbosity(out) > 1) {
			apk_msg(out, "%s %d packages, %d dirs, %d files, %llu MiB",
				msg,
				installed_packages,
				db->installed.stats.dirs,
				db->installed.stats.files,
				(unsigned long long)installed_bytes / (1024 * 1024)
				);
		} else {
			apk_msg(out, "%s %llu MiB in %d packages",
				msg,
				(unsigned long long)installed_bytes / (1024 * 1024),
				installed_packages);
		}
	}
	return errors;
}

enum {
	STATE_PRESENT		= 0x80000000,
	STATE_MISSING		= 0x40000000,
	STATE_VIRTUAL_ONLY	= 0x20000000,
	STATE_INSTALLIF		= 0x10000000,
	STATE_COUNT_MASK	= 0x0000ffff,
};

struct print_state {
	struct apk_database *db;
	struct apk_dependency_array *world;
	struct apk_indent i;
	const char *label;
	int num_labels;
	int match;
};

static void label_start(struct print_state *ps, const char *text)
{
	if (ps->label) {
		apk_print_indented_line(&ps->i, "  %s:\n", ps->label);
		ps->label = NULL;
		ps->num_labels++;
	}
	if (!ps->i.x) apk_print_indented_group(&ps->i, 0, "    %s", text);
}
static void label_end(struct print_state *ps)
{
	apk_print_indented_end(&ps->i);
}

static void print_pinning_errors(struct print_state *ps, struct apk_package *pkg, unsigned int tag)
{
	struct apk_database *db = ps->db;
	int i;

	if (pkg->ipkg != NULL)
		return;

	if (!(pkg->repos & db->available_repos)) {
		label_start(ps, "masked in:");
		apk_print_indented_fmt(&ps->i, "--no-network");
	} else if (!(BIT(pkg->layer) & db->active_layers)) {
		label_start(ps, "masked in:");
		apk_print_indented_fmt(&ps->i, "layer");
	} else if (pkg->repos == BIT(APK_REPOSITORY_CACHED) && !pkg->filename_ndx) {
		label_start(ps, "masked in:");
		apk_print_indented_fmt(&ps->i, "cache");
	} else {
		if (pkg->repos & apk_db_get_pinning_mask_repos(db, APK_DEFAULT_PINNING_MASK | BIT(tag)))
			return;
		for (i = 0; i < db->num_repo_tags; i++) {
			if (pkg->repos & db->repo_tags[i].allowed_repos) {
				label_start(ps, "masked in:");
				apk_print_indented(&ps->i, db->repo_tags[i].tag);
			}
		}
	}
	label_end(ps);
}

static void print_conflicts(struct print_state *ps, struct apk_package *pkg)
{
	struct apk_provider *p;
	struct apk_dependency *d;
	int once;

	foreach_array_item(p, pkg->name->providers) {
		if (p->pkg == pkg || !p->pkg->marked)
			continue;
		label_start(ps, "conflicts:");
		apk_print_indented_fmt(&ps->i, PKG_VER_FMT, PKG_VER_PRINTF(p->pkg));
	}
	foreach_array_item(d, pkg->provides) {
		once = 1;
		foreach_array_item(p, d->name->providers) {
			if (!p->pkg->marked)
				continue;
			if (d->version == &apk_atom_null &&
			    p->version == &apk_atom_null)
				continue;
			if (once && p->pkg == pkg &&
			    p->version == d->version) {
				once = 0;
				continue;
			}
			label_start(ps, "conflicts:");
			apk_print_indented_fmt(
				&ps->i, PKG_VER_FMT "[" DEP_FMT "]",
				PKG_VER_PRINTF(p->pkg),
				DEP_PRINTF(d));
		}
	}
	label_end(ps);
}

struct matched_dep {
	struct apk_package *pkg;
	struct apk_dependency *dep;
};
APK_ARRAY(matched_dep_array, struct matched_dep);

static void match_dep(struct apk_package *pkg0, struct apk_dependency *d0, struct apk_package *pkg, void *ctx)
{
	struct matched_dep_array **deps = ctx;
	matched_dep_array_add(deps, (struct matched_dep) {
		.pkg = pkg0,
		.dep = d0,
	});
}

static int matched_dep_sort(const void *p1, const void *p2)
{
	const struct matched_dep *m1 = p1, *m2 = p2;
	int r;

	if (m1->pkg && m2->pkg) {
		r = apk_pkg_cmp_display(m1->pkg, m2->pkg);
		if (r != 0) return r;
	}
	return m1->dep->op - m2->dep->op;
}

static void print_mdeps(struct print_state *ps, const char *label, struct matched_dep_array *deps)
{
	const struct matched_dep *dep;

	if (apk_array_len(deps) == 0) return;

	label_start(ps, label);
	apk_array_qsort(deps, matched_dep_sort);
	foreach_array_item(dep, deps) {
		if (dep->pkg == NULL)
			apk_print_indented_fmt(&ps->i, "world[" DEP_FMT "]", DEP_PRINTF(dep->dep));
		else
			apk_print_indented_fmt(&ps->i, PKG_VER_FMT "[" DEP_FMT "]",
					       PKG_VER_PRINTF(dep->pkg),
					       DEP_PRINTF(dep->dep));
	}
	apk_array_reset(deps);
}

static void print_deps(struct print_state *ps, struct apk_package *pkg, int match)
{
	const char *label = (match & APK_DEP_SATISFIES) ? "satisfies:" : "breaks:";
	struct matched_dep_array *deps;

	matched_dep_array_init(&deps);

	ps->match = match;
	match |= APK_FOREACH_MARKED | APK_FOREACH_DEP;
	apk_pkg_foreach_matching_dependency(NULL, ps->world, match|apk_foreach_genid(), pkg, match_dep, &deps);
	print_mdeps(ps, label, deps);
	apk_pkg_foreach_reverse_dependency(pkg, match|apk_foreach_genid(), match_dep, &deps);
	print_mdeps(ps, label, deps);
	label_end(ps);

	matched_dep_array_free(&deps);
}

static void print_broken_deps(struct print_state *ps, struct apk_dependency_array *deps, const char *label)
{
	struct apk_dependency *dep;

	foreach_array_item(dep, deps) {
		if (!dep->broken) continue;
		label_start(ps, label);
		apk_print_indented_fmt(&ps->i, DEP_FMT, DEP_PRINTF(dep));
	}
	label_end(ps);
}

static void analyze_package(struct print_state *ps, struct apk_package *pkg, unsigned int tag)
{
	char pkgtext[256];

	ps->label = apk_fmts(pkgtext, sizeof pkgtext, PKG_VER_FMT, PKG_VER_PRINTF(pkg));

	if (pkg->uninstallable) {
		label_start(ps, "error:");
		apk_print_indented_fmt(&ps->i, "uninstallable");
		label_end(ps);
		if (!apk_db_arch_compatible(ps->db, pkg->arch)) {
			label_start(ps, "arch:");
			apk_print_indented_fmt(&ps->i, BLOB_FMT, BLOB_PRINTF(*pkg->arch));
			label_end(ps);
		}
		print_broken_deps(ps, pkg->depends, "depends:");
		print_broken_deps(ps, pkg->provides, "provides:");
		print_broken_deps(ps, pkg->install_if, "install_if:");
	}

	print_pinning_errors(ps, pkg, tag);
	print_conflicts(ps, pkg);
	print_deps(ps, pkg, APK_DEP_CONFLICTS);
	if (ps->label == NULL)
		print_deps(ps, pkg, APK_DEP_SATISFIES);
}

static void analyze_missing_name(struct print_state *ps, struct apk_name *name)
{
	struct apk_name **pname0, *name0;
	struct apk_provider *p0;
	struct apk_dependency *d0;
	char label[256];
	unsigned int genid;
	int refs;

	if (apk_array_len(name->providers) != 0) {
		ps->label = apk_fmts(label, sizeof label, "%s (virtual)", name->name);

		label_start(ps, "note:");
		apk_print_indented_words(&ps->i, "please select one of the 'provided by' packages explicitly");
		label_end(ps);

		label_start(ps, "provided by:");
		foreach_array_item(p0, name->providers)
			p0->pkg->name->state_int++;
		foreach_array_item(p0, name->providers) {
			name0 = p0->pkg->name;
			refs = (name0->state_int & STATE_COUNT_MASK);
			if (refs == apk_array_len(name0->providers)) {
				/* name only */
				apk_print_indented(&ps->i, APK_BLOB_STR(name0->name));
				name0->state_int &= ~STATE_COUNT_MASK;
			} else if (refs > 0) {
				/* individual package */
				apk_print_indented_fmt(&ps->i, PKG_VER_FMT, PKG_VER_PRINTF(p0->pkg));
				name0->state_int--;
			}
		}
		label_end(ps);
	} else {
		ps->label = apk_fmts(label, sizeof label, "%s (no such package)", name->name);
	}

	label_start(ps, "required by:");
	foreach_array_item(d0, ps->world) {
		if (d0->name != name || apk_dep_conflict(d0))
			continue;
		apk_print_indented_fmt(&ps->i, "world[" DEP_FMT "]",
			DEP_PRINTF(d0));
	}
	genid = apk_foreach_genid();
	foreach_array_item(pname0, name->rdepends) {
		name0 = *pname0;
		foreach_array_item(p0, name0->providers) {
			if (!p0->pkg->marked)
				continue;
			if (p0->pkg->foreach_genid == genid)
				continue;
			p0->pkg->foreach_genid = genid;
			foreach_array_item(d0, p0->pkg->depends) {
				if (d0->name != name || apk_dep_conflict(d0))
					continue;
				apk_print_indented_fmt(&ps->i,
					PKG_VER_FMT "[" DEP_FMT "]",
					PKG_VER_PRINTF(p0->pkg),
					DEP_PRINTF(d0));
				break;
			}
			if (d0 != NULL)
				break;
		}
	}
	label_end(ps);
}

static void analyze_deps(struct print_state *ps, struct apk_dependency_array *deps)
{
	struct apk_dependency *d0;
	struct apk_name *name0;

	foreach_array_item(d0, deps) {
		name0 = d0->name;
		if (apk_dep_conflict(d0)) continue;
		if ((name0->state_int & (STATE_INSTALLIF | STATE_PRESENT | STATE_MISSING)) != 0)
			continue;
		name0->state_int |= STATE_MISSING;
		analyze_missing_name(ps, name0);
	}
}

static void discover_deps(struct apk_dependency_array *deps);
static void discover_name(struct apk_name *name, int pkg_state);

static void discover_reverse_iif(struct apk_name *name)
{
	struct apk_name **pname0, *name0;
	struct apk_dependency *d;
	struct apk_provider *p;

	foreach_array_item(pname0, name->rinstall_if) {
		name0 = *pname0;

		foreach_array_item(p, name0->providers) {
			int ok = 1;
			if (!p->pkg->marked) continue;
			if (apk_array_len(p->pkg->install_if) == 0) continue;
			foreach_array_item(d, p->pkg->install_if) {
				if (apk_dep_conflict(d) == !!(d->name->state_int & (STATE_PRESENT|STATE_INSTALLIF))) {
					ok = 0;
					break;
				}
			}
			if (ok) {
				discover_name(p->pkg->name, STATE_INSTALLIF);
				foreach_array_item(d, p->pkg->provides)
					discover_name(d->name, STATE_INSTALLIF);
			}
		}
	}
}

static int is_name_concrete(struct apk_package *pkg, struct apk_name *name)
{
	struct apk_dependency *d;
	if (pkg->name == name) return 1;
	foreach_array_item(d, pkg->provides) {
		if (d->name != name) continue;
		if (d->version == &apk_atom_null) continue;
		return 1;
	}
	return 0;
}

static void discover_name(struct apk_name *name, int pkg_state)
{
	struct apk_provider *p;
	struct apk_dependency *d;

	foreach_array_item(p, name->providers) {
		int state = pkg_state;
		if (!p->pkg->marked) continue;
		if ((state == STATE_PRESENT || state == STATE_INSTALLIF) &&
		    !p->pkg->provider_priority && !is_name_concrete(p->pkg, name))
			state = STATE_VIRTUAL_ONLY;
		if (p->pkg->state_int & state) continue;
		p->pkg->state_int |= state;

		p->pkg->name->state_int |= state;
		foreach_array_item(d, p->pkg->provides) {
			int dep_state = state;
			if (dep_state == STATE_INSTALLIF && d->version == &apk_atom_null)
				dep_state = STATE_VIRTUAL_ONLY;
			d->name->state_int |= dep_state;
		}

		discover_deps(p->pkg->depends);
		if (state == STATE_PRESENT || state == STATE_INSTALLIF) {
			discover_reverse_iif(p->pkg->name);
			foreach_array_item(d, p->pkg->provides)
				discover_reverse_iif(d->name);
		}
	}
}

static void discover_deps(struct apk_dependency_array *deps)
{
	struct apk_dependency *d;

	foreach_array_item(d, deps) {
		if (apk_dep_conflict(d)) continue;
		discover_name(d->name, STATE_PRESENT);
	}
}

void apk_solver_print_errors(struct apk_database *db,
			     struct apk_changeset *changeset,
			     struct apk_dependency_array *world)
{
	struct apk_out *out = &db->ctx->out;
	struct print_state ps;
	struct apk_change *change;

	/* ERROR: unsatisfiable dependencies:
	 *   name:
	 *     required by: a b c d e
	 *     not available in any repository
	 *   name (virtual):
	 *     required by: a b c d e
	 *     provided by: foo bar zed
	 *   pkg-1.2:
	 *     masked by: @testing
	 *     satisfies: a[pkg]
	 *     conflicts: pkg-2.0 foo-1.2 bar-1.2
	 *     breaks: b[pkg>2] c[foo>2] d[!pkg]
	 *
	 * When two packages provide same name 'foo':
	 *   a-1:
	 *     satisfies: world[a]
	 *     conflicts: b-1[foo]
	 *   b-1:
	 *     satisfies: world[b]
	 *     conflicts: a-1[foo]
	 * 
	 *   c-1:
	 *     satisfies: world[a]
	 *     conflicts: c-1[foo]  (self-conflict by providing foo twice)
	 *
	 * When two packages get pulled in:
	 *   a-1:
	 *     satisfies: app1[so:a.so.1]
	 *     conflicts: a-2
	 *   a-2:
	 *     satisfies: app2[so:a.so.2]
	 *     conflicts: a-1
	 *
	 * satisfies lists all dependencies that is not satisfiable by
	 * any other selected version. or all of them with -v.
	 */
 
	/* Construct information about names */
	foreach_array_item(change, changeset->changes) {
		struct apk_package *pkg = change->new_pkg;
		if (pkg) pkg->marked = 1;
	}
	discover_deps(world);

	/* Analyze is package, and missing names referred to */
	ps = (struct print_state) {
		.db = db,
		.world = world,
	};
	apk_err(out, "unable to select packages:");
	apk_print_indented_init(&ps.i, out, 1);
	analyze_deps(&ps, world);
	foreach_array_item(change, changeset->changes) {
		struct apk_package *pkg = change->new_pkg;
		if (!pkg) continue;
		analyze_package(&ps, pkg, change->new_repository_tag);
		analyze_deps(&ps, pkg->depends);
	}

	if (!ps.num_labels)
		apk_print_indented_line(&ps.i, "Huh? Error reporter did not find the broken constraints.\n");
}

int apk_solver_commit(struct apk_database *db,
		      unsigned short solver_flags,
		      struct apk_dependency_array *world)
{
	struct apk_out *out = &db->ctx->out;
	struct apk_changeset changeset = {};
	int r;

	if (apk_db_check_world(db, world) != 0) {
		apk_err(out, "Not committing changes due to missing repository tags. "
			"Use --force-broken-world to override.");
		return -1;
	}

	apk_change_array_init(&changeset.changes);
	r = apk_solver_solve(db, solver_flags, world, &changeset);
	if (r == 0)
		r = apk_solver_commit_changeset(db, &changeset, world);
	else
		apk_solver_print_errors(db, &changeset, world);
	apk_change_array_free(&changeset.changes);
	return r;
}
