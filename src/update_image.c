/*
 * update_image.c - Update a WIM image.
 */

/*
 * Copyright (C) 2013, 2014 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib/capture.h"
#include "wimlib/dentry.h"
#include "wimlib/encoding.h"
#include "wimlib/error.h"
#include "wimlib/lookup_table.h"
#include "wimlib/metadata.h"
#ifdef WITH_NTFS_3G
#  include "wimlib/ntfs_3g.h" /* for do_ntfs_umount() */
#endif
#include "wimlib/paths.h"
#include "wimlib/xml.h"

#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>

#ifdef HAVE_ALLOCA_H
#  include <alloca.h>
#endif

/* Saved specification of a "primitive" update operation that was performed.  */
struct update_primitive {
	enum {
		/* Unlinked a dentry from its parent directory.  */
		UNLINK_DENTRY,

		/* Linked a dentry into its parent directory.  */
		LINK_DENTRY,

		/* Changed the file name of a dentry.  */
		CHANGE_FILE_NAME,

		/* Changed the short name of a dentry.  */
		CHANGE_SHORT_NAME,
	} type;

	union {
		/* For UNLINK_DENTRY and LINK_DENTRY operations  */
		struct {
			/* Dentry that was linked or unlinked.  */
			struct wim_dentry *subject;

			/* For link operations, the directory into which
			 * @subject was linked, or NULL if @subject was set as
			 * the root of the image.
			 *
			 * For unlink operations, the directory from which
			 * @subject was unlinked, or NULL if @subject was unset
			 * as the root of the image.  */
			struct wim_dentry *parent;
		} link;

		/* For CHANGE_FILE_NAME and CHANGE_SHORT_NAME operations  */
		struct {
			/* Dentry that had its name changed.  */
			struct wim_dentry *subject;

			/* The old name.  */
			utf16lechar *old_name;
		} name;
	};
};

/* Chronological list of primitive operations that were executed for a single
 * logical update command, such as 'add', 'delete', or 'rename'.  */
struct update_primitive_list {
	struct update_primitive *entries;
	struct update_primitive inline_entries[4];
	size_t num_entries;
	size_t num_alloc_entries;
};

/* Journal for managing the executing of zero or more logical update commands,
 * such as 'add', 'delete', or 'rename'.  This allows either committing or
 * rolling back the commands.  */
struct update_command_journal {
	/* Number of update commands this journal contains.  */
	size_t num_cmds;

	/* Index of currently executing update command.  */
	size_t cur_cmd;

	/* Location of the WIM image's root pointer.  */
	struct wim_dentry **root_p;

	/* Pointer to the lookup table of the WIM (may needed for rollback)  */
	struct wim_lookup_table *lookup_table;

	/* List of dentries that are currently unlinked from the WIM image.
	 * These must be freed when no longer needed for commit or rollback.  */
	struct list_head orphans;

	/* Per-command logs.  */
	struct update_primitive_list cmd_prims[];
};

static void
init_update_primitive_list(struct update_primitive_list *l)
{
	l->entries = l->inline_entries;
	l->num_entries = 0;
	l->num_alloc_entries = ARRAY_LEN(l->inline_entries);
}

/* Allocates a new journal for managing the execution of up to @num_cmds update
 * commands.  */
static struct update_command_journal *
new_update_command_journal(size_t num_cmds, struct wim_dentry **root_p,
			   struct wim_lookup_table *lookup_table)
{
	struct update_command_journal *j;

	j = MALLOC(sizeof(*j) + num_cmds * sizeof(j->cmd_prims[0]));
	if (j) {
		j->num_cmds = num_cmds;
		j->cur_cmd = 0;
		j->root_p = root_p;
		j->lookup_table = lookup_table;
		INIT_LIST_HEAD(&j->orphans);
		for (size_t i = 0; i < num_cmds; i++)
			init_update_primitive_list(&j->cmd_prims[i]);
	}
	return j;
}

/* Don't call this directly; use commit_update() or rollback_update() instead.
 */
static void
free_update_command_journal(struct update_command_journal *j)
{
	struct wim_dentry *orphan;

	/* Free orphaned dentry trees  */
	while (!list_empty(&j->orphans)) {
		orphan = list_first_entry(&j->orphans,
					  struct wim_dentry, tmp_list);
		list_del(&orphan->tmp_list);
		free_dentry_tree(orphan, j->lookup_table);
	}

	for (size_t i = 0; i < j->num_cmds; i++)
		if (j->cmd_prims[i].entries != j->cmd_prims[i].inline_entries)
			FREE(j->cmd_prims[i].entries);
	FREE(j);
}

/* Add the entry @prim to the update command journal @j.  */
static int
record_update_primitive(struct update_command_journal *j,
			struct update_primitive prim)
{
	struct update_primitive_list *l;

	l = &j->cmd_prims[j->cur_cmd];

	if (l->num_entries == l->num_alloc_entries) {
		struct update_primitive *new_entries;
		size_t new_num_alloc_entries;
		size_t new_size;

		new_num_alloc_entries = l->num_alloc_entries * 2;
		new_size = new_num_alloc_entries * sizeof(new_entries[0]);
		if (l->entries == l->inline_entries) {
			new_entries = MALLOC(new_size);
			if (!new_entries)
				return WIMLIB_ERR_NOMEM;
			memcpy(new_entries, l->inline_entries,
			       sizeof(l->inline_entries));
		} else {
			new_entries = REALLOC(l->entries, new_size);
			if (!new_entries)
				return WIMLIB_ERR_NOMEM;
		}
		l->entries = new_entries;
		l->num_alloc_entries = new_num_alloc_entries;
	}
	l->entries[l->num_entries++] = prim;
	return 0;
}

static void
do_unlink(struct wim_dentry *subject, struct wim_dentry *parent,
	  struct wim_dentry **root_p)
{
	if (parent) {
		/* Unlink @subject from its @parent.  */
		wimlib_assert(subject->parent == parent);
		unlink_dentry(subject);
	} else {
		/* Unset @subject as the root of the image.  */
		*root_p = NULL;
	}
	subject->parent = subject;
}

static void
do_link(struct wim_dentry *subject, struct wim_dentry *parent,
	struct wim_dentry **root_p)
{
	if (parent) {
		/* Link @subject to its @parent  */
		struct wim_dentry *existing;

		existing = dentry_add_child(parent, subject);
		wimlib_assert(!existing);
	} else {
		/* Set @subject as root of the image  */
		*root_p = subject;
	}
}

/* Undo a link operation.  */
static void
rollback_link(struct wim_dentry *subject, struct wim_dentry *parent,
	      struct wim_dentry **root_p, struct list_head *orphans)
{
	/* Unlink is the opposite of link  */
	do_unlink(subject, parent, root_p);

	/* @subject is now unlinked.  Add it to orphans. */
	list_add(&subject->tmp_list, orphans);
	subject->is_orphan = 1;
}

/* Undo an unlink operation.  */
static void
rollback_unlink(struct wim_dentry *subject, struct wim_dentry *parent,
		struct wim_dentry **root_p)
{
	/* Link is the opposite of unlink  */
	do_link(subject, parent, root_p);

	/* @subject is no longer unlinked.  Delete it from orphans. */
	list_del(&subject->tmp_list);
	subject->is_orphan = 0;
}

/* Rollback a name change operation.  */
static void
rollback_name_change(utf16lechar *old_name,
		     utf16lechar **name_ptr, u16 *name_nbytes_ptr)
{
	/* Free the new name, then replace it with the old name.  */
	FREE(*name_ptr);
	if (old_name) {
		*name_ptr = old_name;
		*name_nbytes_ptr = utf16le_strlen(old_name);
	} else {
		*name_ptr = NULL;
		*name_nbytes_ptr = 0;
	}
}

/* Rollback a primitive update operation.  */
static void
rollback_update_primitive(const struct update_primitive *prim,
			  struct wim_dentry **root_p,
			  struct list_head *orphans)
{
	switch (prim->type) {
	case LINK_DENTRY:
		rollback_link(prim->link.subject, prim->link.parent, root_p,
			      orphans);
		break;
	case UNLINK_DENTRY:
		rollback_unlink(prim->link.subject, prim->link.parent, root_p);
		break;
	case CHANGE_FILE_NAME:
		rollback_name_change(prim->name.old_name,
				     &prim->name.subject->file_name,
				     &prim->name.subject->file_name_nbytes);
		break;
	case CHANGE_SHORT_NAME:
		rollback_name_change(prim->name.old_name,
				     &prim->name.subject->short_name,
				     &prim->name.subject->short_name_nbytes);
		break;
	}
}

/* Rollback a logical update command  */
static void
rollback_update_command(const struct update_primitive_list *l,
			struct wim_dentry **root_p,
			struct list_head *orphans)
{
	size_t i = l->num_entries;

	/* Rollback each primitive operation, in reverse order.  */
	while (i--)
		rollback_update_primitive(&l->entries[i], root_p, orphans);
}

/****************************************************************************/

/* Link @subject into the directory @parent; or, if @parent is NULL, set
 * @subject as the root of the WIM image.
 *
 * This is the journaled version, so it can be rolled back.  */
static int
journaled_link(struct update_command_journal *j,
	       struct wim_dentry *subject, struct wim_dentry *parent)
{
	struct update_primitive prim;
	int ret;

	prim.type = LINK_DENTRY;
	prim.link.subject = subject;
	prim.link.parent = parent;

	ret = record_update_primitive(j, prim);
	if (ret)
		return ret;

	do_link(subject, parent, j->root_p);

	if (subject->is_orphan) {
		list_del(&subject->tmp_list);
		subject->is_orphan = 0;
	}
	return 0;
}

/* Unlink @subject from the WIM image.
 *
 * This is the journaled version, so it can be rolled back.  */
static int
journaled_unlink(struct update_command_journal *j, struct wim_dentry *subject)
{
	struct wim_dentry *parent;
	struct update_primitive prim;
	int ret;

	if (dentry_is_root(subject))
		parent = NULL;
	else
		parent = subject->parent;

	prim.type = UNLINK_DENTRY;
	prim.link.subject = subject;
	prim.link.parent = parent;

	ret = record_update_primitive(j, prim);
	if (ret)
		return ret;

	do_unlink(subject, parent, j->root_p);

	list_add(&subject->tmp_list, &j->orphans);
	subject->is_orphan = 1;
	return 0;
}

/* Change the name of @dentry to @new_name_tstr.
 *
 * This is the journaled version, so it can be rolled back.  */
static int
journaled_change_name(struct update_command_journal *j,
		      struct wim_dentry *dentry, const tchar *new_name_tstr)
{
	int ret;
	utf16lechar *new_name = NULL;
	u16 new_name_nbytes = 0;
	struct update_primitive prim;

	/* Set the long name.  */
	ret = get_utf16le_string(new_name_tstr, &new_name, &new_name_nbytes);
	if (ret)
		return ret;

	prim.type = CHANGE_FILE_NAME;
	prim.name.subject = dentry;
	prim.name.old_name = dentry->file_name;
	ret = record_update_primitive(j, prim);
	if (ret)
		return ret;

	dentry->file_name = new_name;
	dentry->file_name_nbytes = new_name_nbytes;

	/* Clear the short name.  */
	prim.type = CHANGE_SHORT_NAME;
	prim.name.subject = dentry;
	prim.name.old_name = dentry->short_name;
	ret = record_update_primitive(j, prim);
	if (ret)
		return ret;

	dentry->short_name = NULL;
	dentry->short_name_nbytes = 0;
	return 0;
}

static void
next_command(struct update_command_journal *j)
{
	j->cur_cmd++;
}

static void
commit_update(struct update_command_journal *j)
{
	for (size_t i = 0; i < j->num_cmds; i++)
	{
		for (size_t k = 0; k < j->cmd_prims[i].num_entries; k++)
		{
			if (j->cmd_prims[i].entries[k].type == CHANGE_FILE_NAME ||
			    j->cmd_prims[i].entries[k].type == CHANGE_SHORT_NAME)
			{
				FREE(j->cmd_prims[i].entries[k].name.old_name);
			}
		}
	}
	free_update_command_journal(j);
}

static void
rollback_update(struct update_command_journal *j)
{
	/* Rollback each logical update command, in reverse order.  */
	size_t i = j->cur_cmd;
	if (i < j->num_cmds)
		i++;
	while (i--)
		rollback_update_command(&j->cmd_prims[i], j->root_p, &j->orphans);
	free_update_command_journal(j);
}

/*
 * Set the name of @branch for placing it at @target in the WIM image.  This
 * assumes that @target is in "canonical form", as produced by
 * canonicalize_wim_path().
 *
 * Note: for the root target this produces the empty name.
 */
static int
set_branch_name(struct wim_dentry *branch, const utf16lechar *target)
{
	const utf16lechar *p;

	/* Find end of string.  (We can assume it contains at least one
	 * character, the leading slash.)   */
	wimlib_assert(target[0] == cpu_to_le16(WIM_PATH_SEPARATOR));
	p = target;
	do {
		p++;
	} while (*p);

	while (*(p - 1) != cpu_to_le16(WIM_PATH_SEPARATOR))
		p--;


	/* We're assuming no trailing slashes.  */
	wimlib_assert(*p || p == &target[1]);

	return dentry_set_name_utf16le(branch, p);
}

static int
handle_conflict(struct wim_dentry *branch, struct wim_dentry *existing,
		struct update_command_journal *j,
		int add_flags, wimlib_progress_func_t progress_func)
{
	bool branch_is_dir = dentry_is_directory(branch);
	bool existing_is_dir = dentry_is_directory(existing);

	if (branch_is_dir != existing_is_dir) {
		if (existing_is_dir)  {
			ERROR("\"%"TS"\" is a directory!\n"
			      "        Specify the path at which "
			      "to place the file inside this directory.",
			      dentry_full_path(existing));
			return WIMLIB_ERR_IS_DIRECTORY;
		} else {
			ERROR("Can't place directory at \"%"TS"\" because "
			      "a nondirectory file already exists there!",
			      dentry_full_path(existing));
			return WIMLIB_ERR_NOTDIR;
		}
	}

	if (branch_is_dir) {
		/* Directory overlay  */
		while (dentry_has_children(branch)) {
			struct wim_dentry *new_child;
			struct wim_dentry *existing_child;
			int ret;

			new_child = dentry_any_child(branch);

			existing_child =
				get_dentry_child_with_utf16le_name(existing,
								   new_child->file_name,
								   new_child->file_name_nbytes,
								   WIMLIB_CASE_PLATFORM_DEFAULT);
			unlink_dentry(new_child);
			if (existing_child) {
				ret = handle_conflict(new_child, existing_child,
						      j, add_flags, progress_func);
			} else {
				ret = journaled_link(j, new_child, existing);
			}
			if (ret) {
				dentry_add_child(branch, new_child);
				return ret;
			}
		}
		free_dentry(branch);
		return 0;
	} else if (add_flags & WIMLIB_ADD_FLAG_NO_REPLACE) {
		/* Can't replace nondirectory file  */
		ERROR("Refusing to overwrite nondirectory file \"%"TS"\"",
		      dentry_full_path(existing));
		return WIMLIB_ERR_INVALID_OVERLAY;
	} else {
		/* Replace nondirectory file  */
		struct wim_dentry *parent;
		int ret;

		parent = existing->parent;

		ret = calculate_dentry_full_path(existing);
		if (ret)
			return ret;

		ret = journaled_unlink(j, existing);
		if (ret)
			return ret;

		ret = journaled_link(j, branch, parent);
		if (ret)
			return ret;

		if (progress_func && (add_flags & WIMLIB_ADD_FLAG_VERBOSE)) {
			union wimlib_progress_info info;

			info.replace.path_in_wim = existing->_full_path;
			progress_func(WIMLIB_PROGRESS_MSG_REPLACE_FILE_IN_WIM, &info);
		}
		return 0;
	}
}

static int
do_attach_branch(struct wim_dentry *branch, utf16lechar *target,
		 struct update_command_journal *j,
		 int add_flags, wimlib_progress_func_t progress_func)
{
	struct wim_dentry *parent;
	struct wim_dentry *existing;
	utf16lechar empty_name[1] = {0};
	utf16lechar *cur_component_name;
	utf16lechar *next_component_name;
	int ret;

	/* Attempt to create root directory before proceeding to the "real"
	 * first component  */
	parent = NULL;
	existing = *j->root_p;
	cur_component_name = empty_name;

	/* Skip leading slashes  */
	next_component_name = target;
	while (*next_component_name == cpu_to_le16(WIM_PATH_SEPARATOR))
		next_component_name++;

	while (*next_component_name) { /* While not the last component ... */
		utf16lechar *end;

		if (existing) {
			/* Descend into existing directory  */
			if (!dentry_is_directory(existing)) {
				ERROR("\"%"TS"\" in the WIM image "
				      "is not a directory!",
				      dentry_full_path(existing));
				return WIMLIB_ERR_NOTDIR;
			}
		} else {
			/* A parent directory of the target didn't exist.  Make
			 * the way by creating a filler directory.  */
			struct wim_dentry *filler;

			ret = new_filler_directory(T(""), &filler);
			if (ret)
				return ret;
			ret = dentry_set_name_utf16le(filler,
						      cur_component_name);
			if (ret) {
				free_dentry(filler);
				return ret;
			}
			ret = journaled_link(j, filler, parent);
			if (ret) {
				free_dentry(filler);
				return ret;
			}
			existing = filler;
		}

		/* Advance to next component  */

		cur_component_name = next_component_name;
		end = cur_component_name + 1;
		while (*end && *end != cpu_to_le16(WIM_PATH_SEPARATOR))
			end++;

		next_component_name = end;
		if (*end) {
			/* There will still be more components after this.  */
			*end = 0;
			do {
			} while (*++next_component_name == cpu_to_le16(WIM_PATH_SEPARATOR));
			wimlib_assert(*next_component_name);  /* No trailing slashes  */
		} else {
			/* This will be the last component  */
			next_component_name = end;
		}
		parent = existing;
		existing = get_dentry_child_with_utf16le_name(
					parent,
					cur_component_name,
					(end - cur_component_name) * sizeof(utf16lechar),
					WIMLIB_CASE_PLATFORM_DEFAULT);
	}

	/* Last component  */
	if (existing) {
		return handle_conflict(branch, existing, j,
				       add_flags, progress_func);
	} else {
		return journaled_link(j, branch, parent);
	}
}

/*
 * Place the directory entry tree @branch at the path @target_tstr in the WIM
 * image.
 *
 * @target_tstr cannot contain trailing slashes, and all path separators must be
 * WIM_PATH_SEPARATOR.
 *
 * On success, @branch is committed to the journal @j.
 * Otherwise @branch is freed.
 *
 * The relevant @add_flags are WIMLIB_ADD_FLAG_NO_REPLACE and
 * WIMLIB_ADD_FLAG_VERBOSE.
 */
static int
attach_branch(struct wim_dentry *branch, const tchar *target_tstr,
	      struct update_command_journal *j,
	      int add_flags, wimlib_progress_func_t progress_func)
{
	int ret;
	utf16lechar *target;

	if (unlikely(!branch))
		return 0;

#if TCHAR_IS_UTF16LE
	target = memdup(target_tstr,
			(tstrlen(target_tstr) + 1) * sizeof(target_tstr[0]));
	if (!target) {
		ret = WIMLIB_ERR_NOMEM;
		goto out_free_branch;
	}
#else
	{
		size_t target_nbytes;
		ret = tstr_to_utf16le(target_tstr,
				      tstrlen(target_tstr) * sizeof(target_tstr[0]),
				      &target, &target_nbytes);
		if (ret)
			goto out_free_branch;
	}
#endif

	ret = set_branch_name(branch, target);
	if (ret)
		goto out_free_target;

	ret = do_attach_branch(branch, target, j, add_flags, progress_func);
	if (ret)
		goto out_free_target;
	/* branch was successfully committed to the journal  */
	branch = NULL;
out_free_target:
	FREE(target);
out_free_branch:
	free_dentry_tree(branch, j->lookup_table);
	return ret;
}

static const char wincfg[] =
"[ExclusionList]\n"
"/$ntfs.log\n"
"/hiberfil.sys\n"
"/pagefile.sys\n"
"/System Volume Information\n"
"/RECYCLER\n"
"/Windows/CSC\n";

static const tchar *wimboot_cfgfile =
	    WIMLIB_WIM_PATH_SEPARATOR_STRING T("Windows")
	    WIMLIB_WIM_PATH_SEPARATOR_STRING T("System32")
	    WIMLIB_WIM_PATH_SEPARATOR_STRING T("WimBootCompress.ini");

static int
get_capture_config(const tchar *config_file, struct capture_config *config,
		   int add_flags, const tchar *fs_source_path)
{
	int ret;
	tchar *tmp_config_file = NULL;

	memset(config, 0, sizeof(*config));

	/* For WIMBoot capture, check for default capture configuration file
	 * unless one was explicitly specified.  */
	if (!config_file && (add_flags & WIMLIB_ADD_FLAG_WIMBOOT)) {

		/* XXX: Handle loading file correctly when in NTFS volume.  */

		size_t len = tstrlen(fs_source_path) +
			     tstrlen(wimboot_cfgfile);
		tmp_config_file = MALLOC((len + 1) * sizeof(tchar));
		struct stat st;

		tsprintf(tmp_config_file, T("%"TS"%"TS),
			 fs_source_path, wimboot_cfgfile);
		if (!tstat(tmp_config_file, &st)) {
			config_file = tmp_config_file;
			add_flags &= ~WIMLIB_ADD_FLAG_WINCONFIG;
		} else {
			WARNING("\"%"TS"\" does not exist.\n"
				"          Using default capture configuration!",
				tmp_config_file);
		}
	}

	if (add_flags & WIMLIB_ADD_FLAG_WINCONFIG) {
		/* Use Windows default.  */
		if (config_file)
			return WIMLIB_ERR_INVALID_PARAM;
		ret = do_read_capture_config_file(T("wincfg"), wincfg,
						  sizeof(wincfg) - 1, config);
	} else if (config_file) {
		/* Use the specified configuration file.  */
		ret = do_read_capture_config_file(config_file, NULL, 0, config);
	} else {
		/* ... Or don't use any configuration file at all.  No files
		 * will be excluded from capture, all files will be compressed,
		 * etc.  */
		ret = 0;
	}
	FREE(tmp_config_file);
	return ret;
}

static int
execute_add_command(struct update_command_journal *j,
		    WIMStruct *wim,
		    const struct wimlib_update_command *add_cmd,
		    struct wim_inode_table *inode_table,
		    struct wim_sd_set *sd_set,
		    struct list_head *unhashed_streams,
		    wimlib_progress_func_t progress_func)
{
	int ret;
	int add_flags;
	tchar *fs_source_path;
	tchar *wim_target_path;
	const tchar *config_file;
	struct add_image_params params;
	struct capture_config config;
	capture_tree_t capture_tree = platform_default_capture_tree;
#ifdef WITH_NTFS_3G
	struct _ntfs_volume *ntfs_vol = NULL;
#endif
	void *extra_arg = NULL;
	struct wim_dentry *branch;

	add_flags = add_cmd->add.add_flags;
	fs_source_path = add_cmd->add.fs_source_path;
	wim_target_path = add_cmd->add.wim_target_path;
	config_file = add_cmd->add.config_file;

	DEBUG("fs_source_path=\"%"TS"\", wim_target_path=\"%"TS"\", add_flags=%#x",
	      fs_source_path, wim_target_path, add_flags);

	memset(&params, 0, sizeof(params));

#ifdef WITH_NTFS_3G
	if (add_flags & WIMLIB_ADD_FLAG_NTFS) {
		capture_tree = build_dentry_tree_ntfs;
		extra_arg = &ntfs_vol;
		if (wim_get_current_image_metadata(wim)->ntfs_vol != NULL) {
			ERROR("NTFS volume already set");
			ret = WIMLIB_ERR_INVALID_PARAM;
			goto out;
		}
	}
#endif

	ret = get_capture_config(config_file, &config,
				 add_flags, fs_source_path);
	if (ret)
		goto out;

	params.lookup_table = wim->lookup_table;
	params.unhashed_streams = unhashed_streams;
	params.inode_table = inode_table;
	params.sd_set = sd_set;
	params.config = &config;
	params.add_flags = add_flags;
	params.extra_arg = extra_arg;

	params.progress_func = progress_func;
	params.progress.scan.source = fs_source_path;
	params.progress.scan.wim_target_path = wim_target_path;
	if (progress_func)
		progress_func(WIMLIB_PROGRESS_MSG_SCAN_BEGIN, &params.progress);

	if (WIMLIB_IS_WIM_ROOT_PATH(wim_target_path))
		params.add_flags |= WIMLIB_ADD_FLAG_ROOT;
	ret = (*capture_tree)(&branch, fs_source_path, &params);
	if (ret)
		goto out_destroy_config;

	if (progress_func)
		progress_func(WIMLIB_PROGRESS_MSG_SCAN_END, &params.progress);

	if (WIMLIB_IS_WIM_ROOT_PATH(wim_target_path) &&
	    branch && !dentry_is_directory(branch))
	{
		ERROR("\"%"TS"\" is not a directory!", fs_source_path);
		ret = WIMLIB_ERR_NOTDIR;
		free_dentry_tree(branch, wim->lookup_table);
		goto out_cleanup_after_capture;
	}

	ret = attach_branch(branch, wim_target_path, j,
			    add_flags, params.progress_func);
	if (ret)
		goto out_cleanup_after_capture;

	if (config_file && (add_flags & WIMLIB_ADD_FLAG_WIMBOOT) &&
	    WIMLIB_IS_WIM_ROOT_PATH(wim_target_path))
	{
		params.add_flags = 0;
		params.progress_func = NULL;
		params.config = NULL;

		/* If a capture configuration file was explicitly specified when
		 * capturing an image in WIMBoot mode, save it as
		 * /Windows/System32/WimBootCompress.ini in the WIM image. */
		ret = platform_default_capture_tree(&branch, config_file, &params);
		if (ret)
			goto out_cleanup_after_capture;

		ret = attach_branch(branch, wimboot_cfgfile, j, 0, NULL);
		if (ret)
			goto out_cleanup_after_capture;
	}

#ifdef WITH_NTFS_3G
	wim_get_current_image_metadata(wim)->ntfs_vol = ntfs_vol;
#endif
	if (add_flags & WIMLIB_ADD_FLAG_RPFIX)
		wim->hdr.flags |= WIM_HDR_FLAG_RP_FIX;
	ret = 0;
	goto out_destroy_config;
out_cleanup_after_capture:
#ifdef WITH_NTFS_3G
	if (ntfs_vol)
		do_ntfs_umount(ntfs_vol);
#endif
out_destroy_config:
	destroy_capture_config(&config);
out:
	return ret;
}

static int
execute_delete_command(struct update_command_journal *j,
		       WIMStruct *wim,
		       const struct wimlib_update_command *delete_cmd)
{
	int flags;
	const tchar *wim_path;
	struct wim_dentry *tree;

	flags = delete_cmd->delete_.delete_flags;
	wim_path = delete_cmd->delete_.wim_path;

	DEBUG("Deleting WIM path \"%"TS"\" (flags=%#x)", wim_path, flags);

	tree = get_dentry(wim, wim_path, WIMLIB_CASE_PLATFORM_DEFAULT);
	if (!tree) {
		/* Path to delete does not exist in the WIM. */
		if (flags & WIMLIB_DELETE_FLAG_FORCE) {
			return 0;
		} else {
			ERROR("Path \"%"TS"\" does not exist in WIM image %d",
			      wim_path, wim->current_image);
			return WIMLIB_ERR_PATH_DOES_NOT_EXIST;
		}
	}

	if (dentry_is_directory(tree) && !(flags & WIMLIB_DELETE_FLAG_RECURSIVE)) {
		ERROR("Path \"%"TS"\" in WIM image %d is a directory "
		      "but a recursive delete was not requested",
		      wim_path, wim->current_image);
		return WIMLIB_ERR_IS_DIRECTORY;
	}

	return journaled_unlink(j, tree);
}

static int
free_dentry_full_path(struct wim_dentry *dentry, void *_ignore)
{
	FREE(dentry->_full_path);
	dentry->_full_path = NULL;
	return 0;
}

/* Is @d1 a (possibly nonproper) ancestor of @d2?  */
static bool
is_ancestor(struct wim_dentry *d1, struct wim_dentry *d2)
{
	for (;;) {
		if (d2 == d1)
			return true;
		if (dentry_is_root(d2))
			return false;
		d2 = d2->parent;
	}
}

/* Rename a file or directory in the WIM.
 *
 * This returns a -errno value.
 *
 * The journal @j is optional.
 */
int
rename_wim_path(WIMStruct *wim, const tchar *from, const tchar *to,
		CASE_SENSITIVITY_TYPE case_type,
		struct update_command_journal *j)
{
	struct wim_dentry *src;
	struct wim_dentry *dst;
	struct wim_dentry *parent_of_dst;
	int ret;

	/* This rename() implementation currently only supports actual files
	 * (not alternate data streams) */

	src = get_dentry(wim, from, case_type);
	if (!src)
		return -errno;

	dst = get_dentry(wim, to, case_type);

	if (dst) {
		/* Destination file exists */

		if (src == dst) /* Same file */
			return 0;

		if (!dentry_is_directory(src)) {
			/* Cannot rename non-directory to directory. */
			if (dentry_is_directory(dst))
				return -EISDIR;
		} else {
			/* Cannot rename directory to a non-directory or a non-empty
			 * directory */
			if (!dentry_is_directory(dst))
				return -ENOTDIR;
			if (dentry_has_children(dst))
				return -ENOTEMPTY;
		}
		parent_of_dst = dst->parent;
	} else {
		/* Destination does not exist */
		parent_of_dst = get_parent_dentry(wim, to, case_type);
		if (!parent_of_dst)
			return -errno;

		if (!dentry_is_directory(parent_of_dst))
			return -ENOTDIR;
	}

	/* @src can't be an ancestor of @dst.  Otherwise we're unlinking @src
	 * from the tree and creating a loop...  */
	if (is_ancestor(src, parent_of_dst))
		return -EBUSY;

	if (j) {
		if (dst)
			if (journaled_unlink(j, dst))
				return -ENOMEM;
		if (journaled_unlink(j, src))
			return -ENOMEM;
		if (journaled_change_name(j, src, path_basename(to)))
			return -ENOMEM;
		if (journaled_link(j, src, parent_of_dst))
			return -ENOMEM;
	} else {
		ret = dentry_set_name(src, path_basename(to));
		if (ret)
			return -ENOMEM;
		if (dst) {
			unlink_dentry(dst);
			free_dentry_tree(dst, wim->lookup_table);
		}
		unlink_dentry(src);
		dentry_add_child(parent_of_dst, src);
	}
	if (src->_full_path)
		for_dentry_in_tree(src, free_dentry_full_path, NULL);
	return 0;
}


static int
execute_rename_command(struct update_command_journal *j,
		       WIMStruct *wim,
		       const struct wimlib_update_command *rename_cmd)
{
	int ret;

	ret = rename_wim_path(wim, rename_cmd->rename.wim_source_path,
			      rename_cmd->rename.wim_target_path,
			      WIMLIB_CASE_PLATFORM_DEFAULT, j);
	if (ret) {
		ret = -ret;
		errno = ret;
		ERROR_WITH_ERRNO("Can't rename \"%"TS"\" to \"%"TS"\"",
				 rename_cmd->rename.wim_source_path,
				 rename_cmd->rename.wim_target_path);
		switch (ret) {
		case ENOMEM:
			ret = WIMLIB_ERR_NOMEM;
			break;
		case ENOTDIR:
			ret = WIMLIB_ERR_NOTDIR;
			break;
		case ENOTEMPTY:
		case EBUSY:
			/* XXX: EBUSY is returned when the rename would create a
			 * loop.  It maybe should have its own error code.  */
			ret = WIMLIB_ERR_NOTEMPTY;
			break;
		case EISDIR:
			ret = WIMLIB_ERR_IS_DIRECTORY;
			break;
		case ENOENT:
		default:
			ret = WIMLIB_ERR_PATH_DOES_NOT_EXIST;
			break;
		}
	}
	return ret;
}

static inline const tchar *
update_op_to_str(int op)
{
	switch (op) {
	case WIMLIB_UPDATE_OP_ADD:
		return T("add");
	case WIMLIB_UPDATE_OP_DELETE:
		return T("delete");
	case WIMLIB_UPDATE_OP_RENAME:
		return T("rename");
	default:
		wimlib_assert(0);
		return NULL;
	}
}

static bool
have_command_type(const struct wimlib_update_command *cmds, size_t num_cmds,
		  enum wimlib_update_op op)
{
	for (size_t i = 0; i < num_cmds; i++)
		if (cmds[i].op == op)
			return true;
	return false;
}

static int
execute_update_commands(WIMStruct *wim,
			const struct wimlib_update_command *cmds,
			size_t num_cmds,
			int update_flags,
			wimlib_progress_func_t progress_func)
{
	struct wim_inode_table *inode_table;
	struct wim_sd_set *sd_set;
	struct list_head unhashed_streams;
	struct update_command_journal *j;
	union wimlib_progress_info info;
	int ret;

	if (have_command_type(cmds, num_cmds, WIMLIB_UPDATE_OP_ADD)) {
		/* If we have at least one "add" command, create the inode and
		 * security descriptor tables to index new inodes and new
		 * security descriptors, respectively.  */
		inode_table = alloca(sizeof(struct wim_inode_table));
		sd_set = alloca(sizeof(struct wim_sd_set));

		ret = init_inode_table(inode_table, 9001);
		if (ret)
			goto out;

		ret = init_sd_set(sd_set, wim_security_data(wim));
		if (ret)
			goto out_destroy_inode_table;

		INIT_LIST_HEAD(&unhashed_streams);
	} else {
		inode_table = NULL;
		sd_set = NULL;
	}

	/* Start an in-memory journal to allow rollback if something goes wrong
	 */
	j = new_update_command_journal(num_cmds,
				       &wim_get_current_image_metadata(wim)->root_dentry,
				       wim->lookup_table);
	if (!j) {
		ret = WIMLIB_ERR_NOMEM;
		goto out_destroy_sd_set;
	}

	info.update.completed_commands = 0;
	info.update.total_commands = num_cmds;
	ret = 0;
	for (size_t i = 0; i < num_cmds; i++) {
		DEBUG("Executing update command %zu of %zu (op=%"TS")",
		      i + 1, num_cmds, update_op_to_str(cmds[i].op));
		if (update_flags & WIMLIB_UPDATE_FLAG_SEND_PROGRESS &&
		    progress_func)
		{
			info.update.command = &cmds[i];
			(*progress_func)(WIMLIB_PROGRESS_MSG_UPDATE_BEGIN_COMMAND,
					 &info);
		}
		ret = WIMLIB_ERR_INVALID_PARAM;
		switch (cmds[i].op) {
		case WIMLIB_UPDATE_OP_ADD:
			ret = execute_add_command(j, wim, &cmds[i], inode_table,
						  sd_set, &unhashed_streams,
						  progress_func);
			break;
		case WIMLIB_UPDATE_OP_DELETE:
			ret = execute_delete_command(j, wim, &cmds[i]);
			break;
		case WIMLIB_UPDATE_OP_RENAME:
			ret = execute_rename_command(j, wim, &cmds[i]);
			break;
		}
		if (unlikely(ret))
			goto rollback;
		info.update.completed_commands++;
		if (update_flags & WIMLIB_UPDATE_FLAG_SEND_PROGRESS &&
		    progress_func)
		{
			(*progress_func)(WIMLIB_PROGRESS_MSG_UPDATE_END_COMMAND,
					 &info);
		}
		next_command(j);
	}

	commit_update(j);
	if (inode_table) {
		struct wim_image_metadata *imd;

		imd = wim_get_current_image_metadata(wim);

		list_splice_tail(&unhashed_streams, &imd->unhashed_streams);
		inode_table_prepare_inode_list(inode_table, &imd->inode_list);
	}
	goto out_destroy_sd_set;

rollback:
	if (sd_set)
		rollback_new_security_descriptors(sd_set);
	rollback_update(j);
out_destroy_sd_set:
	if (sd_set)
		destroy_sd_set(sd_set);
out_destroy_inode_table:
	if (inode_table)
		destroy_inode_table(inode_table);
out:
	return ret;
}


static int
check_add_command(struct wimlib_update_command *cmd,
		  const struct wim_header *hdr)
{
	int add_flags = cmd->add.add_flags;

	if (add_flags & ~(WIMLIB_ADD_FLAG_NTFS |
			  WIMLIB_ADD_FLAG_DEREFERENCE |
			  WIMLIB_ADD_FLAG_VERBOSE |
			  /* BOOT doesn't make sense for wimlib_update_image().  */
			  /*WIMLIB_ADD_FLAG_BOOT |*/
			  WIMLIB_ADD_FLAG_UNIX_DATA |
			  WIMLIB_ADD_FLAG_NO_ACLS |
			  WIMLIB_ADD_FLAG_STRICT_ACLS |
			  WIMLIB_ADD_FLAG_EXCLUDE_VERBOSE |
			  WIMLIB_ADD_FLAG_RPFIX |
			  WIMLIB_ADD_FLAG_NORPFIX |
			  WIMLIB_ADD_FLAG_NO_UNSUPPORTED_EXCLUDE |
			  WIMLIB_ADD_FLAG_WINCONFIG |
			  WIMLIB_ADD_FLAG_WIMBOOT |
			  WIMLIB_ADD_FLAG_NO_REPLACE))
		return WIMLIB_ERR_INVALID_PARAM;

	bool is_entire_image = WIMLIB_IS_WIM_ROOT_PATH(cmd->add.wim_target_path);

#ifndef WITH_NTFS_3G
	if (add_flags & WIMLIB_ADD_FLAG_NTFS) {
		ERROR("wimlib was compiled without support for NTFS-3g, so\n"
		      "        we cannot capture a WIM image directly "
		      "from an NTFS volume");
		return WIMLIB_ERR_UNSUPPORTED;
	}
#endif

#ifdef __WIN32__
	/* Check for flags not supported on Windows */
	if (add_flags & WIMLIB_ADD_FLAG_UNIX_DATA) {
		ERROR("Capturing UNIX-specific data is not supported on Windows");
		return WIMLIB_ERR_UNSUPPORTED;
	}
	if (add_flags & WIMLIB_ADD_FLAG_DEREFERENCE) {
		ERROR("Dereferencing symbolic links is not supported on Windows");
		return WIMLIB_ERR_UNSUPPORTED;
	}
#endif

	/* VERBOSE implies EXCLUDE_VERBOSE */
	if (add_flags & WIMLIB_ADD_FLAG_VERBOSE)
		add_flags |= WIMLIB_ADD_FLAG_EXCLUDE_VERBOSE;

	/* Check for contradictory reparse point fixup flags */
	if ((add_flags & (WIMLIB_ADD_FLAG_RPFIX |
			  WIMLIB_ADD_FLAG_NORPFIX)) ==
		(WIMLIB_ADD_FLAG_RPFIX |
		 WIMLIB_ADD_FLAG_NORPFIX))
	{
		ERROR("Cannot specify RPFIX and NORPFIX flags "
		      "at the same time!");
		return WIMLIB_ERR_INVALID_PARAM;
	}

	/* Set default behavior on reparse point fixups if requested */
	if ((add_flags & (WIMLIB_ADD_FLAG_RPFIX |
			  WIMLIB_ADD_FLAG_NORPFIX)) == 0)
	{
		/* Do reparse-point fixups by default if we are capturing an
		 * entire image and either the header flag is set from previous
		 * images, or if this is the first image being added. */
		if (is_entire_image &&
		    ((hdr->flags & WIM_HDR_FLAG_RP_FIX) || hdr->image_count == 1))
			add_flags |= WIMLIB_ADD_FLAG_RPFIX;
	}

	if (!is_entire_image) {
		if (add_flags & WIMLIB_ADD_FLAG_NTFS) {
			ERROR("Cannot add directly from a NTFS volume "
			      "when not capturing a full image!");
			return WIMLIB_ERR_INVALID_PARAM;
		}

		if (add_flags & WIMLIB_ADD_FLAG_RPFIX) {
			ERROR("Cannot do reparse point fixups when "
			      "not capturing a full image!");
			return WIMLIB_ERR_INVALID_PARAM;
		}
	}
	/* We may have modified the add flags. */
	cmd->add.add_flags = add_flags;
	return 0;
}

static int
check_delete_command(const struct wimlib_update_command *cmd)
{
	if (cmd->delete_.delete_flags & ~(WIMLIB_DELETE_FLAG_FORCE |
					  WIMLIB_DELETE_FLAG_RECURSIVE))
		return WIMLIB_ERR_INVALID_PARAM;
	return 0;
}

static int
check_rename_command(const struct wimlib_update_command *cmd)
{
	if (cmd->rename.rename_flags != 0)
		return WIMLIB_ERR_INVALID_PARAM;
	return 0;
}

static int
check_update_command(struct wimlib_update_command *cmd,
		     const struct wim_header *hdr)
{
	switch (cmd->op) {
	case WIMLIB_UPDATE_OP_ADD:
		return check_add_command(cmd, hdr);
	case WIMLIB_UPDATE_OP_DELETE:
		return check_delete_command(cmd);
	case WIMLIB_UPDATE_OP_RENAME:
		return check_rename_command(cmd);
	}
	return 0;
}

static int
check_update_commands(struct wimlib_update_command *cmds, size_t num_cmds,
		      const struct wim_header *hdr)
{
	int ret = 0;
	for (size_t i = 0; i < num_cmds; i++) {
		ret = check_update_command(&cmds[i], hdr);
		if (ret)
			break;
	}
	return ret;
}


static void
free_update_commands(struct wimlib_update_command *cmds, size_t num_cmds)
{
	if (cmds) {
		for (size_t i = 0; i < num_cmds; i++) {
			switch (cmds[i].op) {
			case WIMLIB_UPDATE_OP_ADD:
				FREE(cmds[i].add.wim_target_path);
				break;
			case WIMLIB_UPDATE_OP_DELETE:
				FREE(cmds[i].delete_.wim_path);
				break;
			case WIMLIB_UPDATE_OP_RENAME:
				FREE(cmds[i].rename.wim_source_path);
				FREE(cmds[i].rename.wim_target_path);
				break;
			}
		}
		FREE(cmds);
	}
}

static int
copy_update_commands(const struct wimlib_update_command *cmds,
		     size_t num_cmds,
		     struct wimlib_update_command **cmds_copy_ret)
{
	int ret;
	struct wimlib_update_command *cmds_copy;

	cmds_copy = CALLOC(num_cmds, sizeof(cmds[0]));
	if (!cmds_copy)
		goto oom;

	for (size_t i = 0; i < num_cmds; i++) {
		cmds_copy[i].op = cmds[i].op;
		switch (cmds[i].op) {
		case WIMLIB_UPDATE_OP_ADD:
			cmds_copy[i].add.fs_source_path = cmds[i].add.fs_source_path;
			cmds_copy[i].add.wim_target_path =
				canonicalize_wim_path(cmds[i].add.wim_target_path);
			if (!cmds_copy[i].add.wim_target_path)
				goto oom;
			cmds_copy[i].add.config_file = cmds[i].add.config_file;
			cmds_copy[i].add.add_flags = cmds[i].add.add_flags;
			break;
		case WIMLIB_UPDATE_OP_DELETE:
			cmds_copy[i].delete_.wim_path =
				canonicalize_wim_path(cmds[i].delete_.wim_path);
			if (!cmds_copy[i].delete_.wim_path)
				goto oom;
			cmds_copy[i].delete_.delete_flags = cmds[i].delete_.delete_flags;
			break;
		case WIMLIB_UPDATE_OP_RENAME:
			cmds_copy[i].rename.wim_source_path =
				canonicalize_wim_path(cmds[i].rename.wim_source_path);
			cmds_copy[i].rename.wim_target_path =
				canonicalize_wim_path(cmds[i].rename.wim_target_path);
			if (!cmds_copy[i].rename.wim_source_path ||
			    !cmds_copy[i].rename.wim_target_path)
				goto oom;
			break;
		default:
			ERROR("Unknown update operation %u", cmds[i].op);
			ret = WIMLIB_ERR_INVALID_PARAM;
			goto err;
		}
	}
	*cmds_copy_ret = cmds_copy;
	ret = 0;
out:
	return ret;
oom:
	ret = WIMLIB_ERR_NOMEM;
err:
	free_update_commands(cmds_copy, num_cmds);
	goto out;
}

/* API function documented in wimlib.h  */
WIMLIBAPI int
wimlib_update_image(WIMStruct *wim,
		    int image,
		    const struct wimlib_update_command *cmds,
		    size_t num_cmds,
		    int update_flags,
		    wimlib_progress_func_t progress_func)
{
	int ret;
	struct wimlib_update_command *cmds_copy;

	if (update_flags & ~WIMLIB_UPDATE_FLAG_SEND_PROGRESS)
		return WIMLIB_ERR_INVALID_PARAM;

	DEBUG("Updating image %d with %zu commands", image, num_cmds);

	if (have_command_type(cmds, num_cmds, WIMLIB_UPDATE_OP_DELETE))
		ret = can_delete_from_wim(wim);
	else
		ret = can_modify_wim(wim);

	if (ret)
		goto out;

	/* Load the metadata for the image to modify (if not loaded already) */
	ret = select_wim_image(wim, image);
	if (ret)
		goto out;

	DEBUG("Preparing %zu update commands", num_cmds);

	/* Make a copy of the update commands, in the process doing certain
	 * canonicalizations on paths (e.g. translating backslashes to forward
	 * slashes).  This is done to avoid modifying the caller's copy of the
	 * commands. */
	ret = copy_update_commands(cmds, num_cmds, &cmds_copy);
	if (ret)
		goto out;

	/* Perform additional checks on the update commands before we execute
	 * them. */
	ret = check_update_commands(cmds_copy, num_cmds, &wim->hdr);
	if (ret)
		goto out_free_cmds_copy;

	/* Actually execute the update commands. */
	DEBUG("Executing %zu update commands", num_cmds);
	ret = execute_update_commands(wim, cmds_copy, num_cmds, update_flags,
				      progress_func);
	if (ret)
		goto out_free_cmds_copy;

	wim->image_metadata[image - 1]->modified = 1;

	/* Statistics about the WIM image, such as the numbers of files and
	 * directories, may have changed.  Call xml_update_image_info() to
	 * recalculate these statistics. */
	xml_update_image_info(wim, image);
out_free_cmds_copy:
	free_update_commands(cmds_copy, num_cmds);
out:
	return ret;
}
