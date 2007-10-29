/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <smbsrv/nterror.h>
#include <sys/synch.h>
#include <smbsrv/smb_incl.h>
#include <smbsrv/smb_fsops.h>

static int smb_do_rename(struct smb_request *sr,
				struct smb_fqi *src_fqi,
				struct smb_fqi *dst_fqi);


/*
 * smb_com_rename
 *
 * Rename a file. Files OldFileName must exist and NewFileName must not.
 * Both pathnames must be relative to the Tid specified in the request.
 * Open files may be renamed.
 *
 * Multiple files may be renamed in response to a single request as Rename
 * File supports wildcards in the file name (last component of the path).
 * NOTE: we don't support rename with wildcards.
 *
 * SearchAttributes indicates the attributes that the target file(s) must
 * have. If SearchAttributes is zero then only normal files are renamed.
 * If the system file or hidden attributes are specified then the rename
 * is inclusive - both the specified type(s) of files and normal files are
 * renamed. The encoding of SearchAttributes is described in section 3.10
 * - File Attribute Encoding.
 */
int
smb_com_rename(struct smb_request *sr)
{
	static kmutex_t mutex;
	struct smb_fqi *src_fqi;
	struct smb_fqi *dst_fqi;
	struct smb_node *dst_node;
	int rc;

	if (!STYPE_ISDSK(sr->tid_tree->t_res_type)) {
		smbsr_raise_cifs_error(sr, NT_STATUS_ACCESS_DENIED,
		    ERRDOS, ERROR_ACCESS_DENIED);
		/* NOTREACHED */
	}

	src_fqi = &sr->arg.dirop.fqi;
	dst_fqi = &sr->arg.dirop.dst_fqi;

	if (smbsr_decode_vwv(sr, "w", &src_fqi->srch_attr) != 0) {
		smbsr_decode_error(sr);
		/* NOTREACHED */
	}

	rc = smbsr_decode_data(sr, "%SS", sr, &src_fqi->path, &dst_fqi->path);
	if (rc != 0) {
		smbsr_decode_error(sr);
		/* NOTREACHED */
	}

	dst_fqi->srch_attr = 0;

	mutex_enter(&mutex);
	rc = smb_do_rename(sr, src_fqi, dst_fqi);
	mutex_exit(&mutex);

	if (rc != 0) {
		/*
		 * ERROR_FILE_EXISTS doesn't work for Windows98 clients.
		 *
		 * Windows95 clients don't see this problem because the target
		 * is deleted before the rename request.
		 *
		 * The following values are based on observed WFWG, Win9x,
		 * NT and W2K client behaviour.
		 */
		if (rc == EEXIST) {
			smbsr_raise_cifs_error(sr,
			    NT_STATUS_OBJECT_NAME_COLLISION,
			    ERRDOS, ERROR_ALREADY_EXISTS);
			/* NOTREACHED */
		}

		if (rc == EPIPE) {
			smbsr_raise_cifs_error(sr, NT_STATUS_SHARING_VIOLATION,
			    ERRDOS, ERROR_SHARING_VIOLATION);
			/* NOTREACHED */
		}

		smbsr_raise_errno(sr, rc);
		/* NOTREACHED */
	}

	if (src_fqi->dir_snode)
		smb_node_release(src_fqi->dir_snode);

	dst_node = dst_fqi->dir_snode;
	if (dst_node) {
		if (dst_node->flags & NODE_FLAGS_NOTIFY_CHANGE) {
			dst_node->flags |= NODE_FLAGS_CHANGED;
			smb_process_node_notify_change_queue(dst_node);
		}
		smb_node_release(dst_node);
	}

	SMB_NULL_FQI_NODES(*src_fqi);
	SMB_NULL_FQI_NODES(*dst_fqi);

	smbsr_encode_empty_result(sr);

	return (SDRC_NORMAL_REPLY);
}

/*
 * smb_rename_share_check
 *
 * An open file can be renamed if
 *
 *      1. isn't opened for data writing or deleting
 *
 *  2. Opened with "Deny Delete" share mode
 *         But not opened for data reading or executing
 *         (opened for accessing meta data)
 */
DWORD
smb_rename_share_check(struct smb_node *node)
{
	struct smb_ofile *open;

	if (node == 0 || node->n_refcnt <= 1)
		return (NT_STATUS_SUCCESS);

	smb_llist_enter(&node->n_ofile_list, RW_READER);
	open = smb_llist_head(&node->n_ofile_list);
	while (open) {
		if (open->f_granted_access &
		    (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) {
			smb_llist_exit(&node->n_ofile_list);
			return (NT_STATUS_SHARING_VIOLATION);
		}

		if ((open->f_share_access & FILE_SHARE_DELETE) == 0) {
			if (open->f_granted_access &
			    (FILE_READ_DATA | FILE_EXECUTE)) {
				smb_llist_exit(&node->n_ofile_list);
				return (NT_STATUS_SHARING_VIOLATION);
			}
		}
		open = smb_llist_next(&node->n_ofile_list, open);
	}
	smb_llist_exit(&node->n_ofile_list);
	return (NT_STATUS_SUCCESS);
}


/*
 * smb_do_rename
 *
 * Backend to smb_com_rename to ensure that the rename operation is atomic.
 * This function should be called within a mutual exclusion region. If the
 * source and destination are identical, we don't actually do a rename, we
 * just check that the conditions are right. If the source and destination
 * files differ only in case, we a case-sensitive rename. Otherwise, we do
 * a full case-insensitive rename.
 *
 * This function should always return errno values.
 *
 * Upon success, the last_snode's and dir_snode's of both src_fqi and dst_fqi
 * are not released in this routine but in smb_com_rename().
 */
static int
smb_do_rename(
    struct smb_request *sr,
    struct smb_fqi *src_fqi,
    struct smb_fqi *dst_fqi)
{
	struct smb_node *src_node;
	char *dstname;
	DWORD status;
	int rc;
	int count;

	if ((rc = smbd_fs_query(sr, src_fqi, FQM_PATH_MUST_EXIST)) != 0) {
		return (rc);
	}

	src_node = src_fqi->last_snode;

	/*
	 * Break the oplock before access checks. If a client
	 * has a file open, this will force a flush or close,
	 * which may affect the outcome of any share checking.
	 */
	if (OPLOCKS_IN_FORCE(src_node)) {
		status = smb_break_oplock(sr, src_node);

		if (status != NT_STATUS_SUCCESS) {
			smb_node_release(src_node);
			smb_node_release(src_fqi->dir_snode);

			SMB_NULL_FQI_NODES(*src_fqi);
			SMB_NULL_FQI_NODES(*dst_fqi);
			return (EACCES);
		}
	}

	status = smb_lock_range_access(sr, src_node, 0, 0, FILE_WRITE_DATA);
	if (status != NT_STATUS_SUCCESS) {
		smb_node_release(src_node);
		smb_node_release(src_fqi->dir_snode);

		SMB_NULL_FQI_NODES(*src_fqi);
		SMB_NULL_FQI_NODES(*dst_fqi);
		return (EACCES);
	}


	for (count = 0; count <= 3; count++) {
		if (count)
			delay(MSEC_TO_TICK(400));
		status = smb_rename_share_check(src_node);
		if (status != NT_STATUS_SHARING_VIOLATION)
			break;
	}

	smb_node_release(src_node);

	if (status == NT_STATUS_SHARING_VIOLATION) {
		smb_node_release(src_fqi->dir_snode);

		SMB_NULL_FQI_NODES(*src_fqi);
		SMB_NULL_FQI_NODES(*dst_fqi);
		return (EPIPE); /* = ERRbadshare */
	}

	if (utf8_strcasecmp(src_fqi->path, dst_fqi->path) == 0) {
		if ((rc = smbd_fs_query(sr, dst_fqi, 0)) != 0) {
			smb_node_release(src_fqi->dir_snode);

			SMB_NULL_FQI_NODES(*src_fqi);
			SMB_NULL_FQI_NODES(*dst_fqi);
			return (rc);
		}

		/*
		 * Because the fqm parameter to smbd_fs_query() was 0,
		 * a successful return value means that dst_fqi->last_snode
		 * may be NULL.
		 */
		if (dst_fqi->last_snode)
			smb_node_release(dst_fqi->last_snode);

		rc = strcmp(src_fqi->last_comp_od, dst_fqi->last_comp);
		if (rc == 0) {
			smb_node_release(src_fqi->dir_snode);
			smb_node_release(dst_fqi->dir_snode);

			SMB_NULL_FQI_NODES(*src_fqi);
			SMB_NULL_FQI_NODES(*dst_fqi);
			return (0);
		}

		rc = smb_fsop_rename(sr, sr->user_cr,
		    src_fqi->dir_snode,
		    src_fqi->last_comp_od,
		    dst_fqi->dir_snode,
		    dst_fqi->last_comp);

		if (rc != 0) {
			smb_node_release(src_fqi->dir_snode);
			smb_node_release(dst_fqi->dir_snode);

			SMB_NULL_FQI_NODES(*src_fqi);
			SMB_NULL_FQI_NODES(*dst_fqi);
		}
		return (rc);
	}

	rc = smbd_fs_query(sr, dst_fqi, FQM_PATH_MUST_NOT_EXIST);
	if (rc != 0) {
		smb_node_release(src_fqi->dir_snode);

		SMB_NULL_FQI_NODES(*src_fqi);
		SMB_NULL_FQI_NODES(*dst_fqi);
		return (rc);
	}

	/*
	 * Because of FQM_PATH_MUST_NOT_EXIST and the successful return
	 * value, only dst_fqi->dir_snode is valid (dst_fqi->last_snode
	 * is NULL).
	 */

	/*
	 * Use the unmangled form of the destination name if the
	 * source and destination names are the same and the source
	 * name is mangled.  (We are taking a chance here, assuming
	 * that this is what the user wants.)
	 */

	if ((smb_maybe_mangled_name(src_fqi->last_comp)) &&
	    (strcmp(src_fqi->last_comp, dst_fqi->last_comp) == 0)) {
		dstname = src_fqi->last_comp_od;
	} else {
		dstname = dst_fqi->last_comp;
	}

	rc = smb_fsop_rename(sr, sr->user_cr,
	    src_fqi->dir_snode,
	    src_fqi->last_comp_od,
	    dst_fqi->dir_snode,
	    dstname);

	if (rc != 0) {
		smb_node_release(src_fqi->dir_snode);
		smb_node_release(dst_fqi->dir_snode);

		SMB_NULL_FQI_NODES(*src_fqi);
		SMB_NULL_FQI_NODES(*dst_fqi);
	}

	return (rc);
}