/* 
   Copyright (C) by Ronnie Sahlberg <sahlberg@samba.org> 2008
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#include <stdio.h>
#undef _GNU_SOURCE

#include "mount.h"
#include "nfs.h"
#include "libnfs.h"
#include "dbench.h"

#define discard_const(ptr) ((void *)((intptr_t)(ptr)))

#define MAX_FILES 200

static char rw_buf[65536];


struct cb_data {
	struct nfsio *nfsio;
	char *dirname;
};

static void nfs3_deltree(struct dbench_op *op);

static void nfs3_cleanup(struct child_struct *child)
{
	char *dname;
	struct dbench_op op;
	ZERO_STRUCT(op);

	asprintf(&dname, "/clients/client%d", child->id);
	op.fname = dname;
	op.child = child;
	nfs3_deltree(&op);
	free(dname);
}

static void nfs3_setup(struct child_struct *child)
{
	const char *status = "0x00000000";
	nfsstat3 res;

	child->rate.last_time = timeval_current();
	child->rate.last_bytes = 0;


	srandom(getpid() ^ time(NULL));
	child->private = nfsio_connect(options.server, options.export, options.protocol);

	if (child->private == NULL) {
		child->failed = 1;
		printf("nfsio_connect() failed\n");
		exit(10);
	}

	/* create '/clients' */
	res = nfsio_lookup(child->private, "/clients", NULL);
	if (res == NFS3ERR_NOENT) {
		res = nfsio_mkdir(child->private, "/clients");
		if( (res != NFS3_OK) &&
		    (res != NFS3ERR_EXIST) ) {
			printf("Failed to create '/clients' directory. res:%u\n", res);
			exit(10);
		}
	}
}


static void dirent_cb(struct entryplus3 *e, void *private_data)
{
	struct cb_data *cbd = private_data;
	nfsstat3 res;
	char *objname;

	if (!strcmp(cbd->dirname,".")) {
		return;
	}
	if (!strcmp(cbd->dirname,"..")) {
		return;
	}

	asprintf(&objname, "%s/%s", cbd->dirname, e->name);
	if (objname == NULL) {
		printf("Failed to talloc ne object name in dirent_cb\n");
		exit(10);
	}

	if (e->name_attributes.post_op_attr_u.attributes.type == NF3DIR) {
		struct cb_data *new_cbd = malloc(sizeof(struct cb_data));

		new_cbd->nfsio = cbd->nfsio;
		new_cbd->dirname = strdup(objname);

		nfsio_readdirplus(cbd->nfsio, objname, dirent_cb, new_cbd);
		
		res = nfsio_rmdir(cbd->nfsio, objname);
		if (res != NFS3_OK) {
			printf("Failed to remove object : \"%s\"  %s (%d)\n", objname, nfs_error(res), res);
			free(objname);
			free(new_cbd->dirname);
			free(new_cbd);
			exit(10);
		}


		free(objname);
		free(new_cbd->dirname);
		free(new_cbd);
		return;
	}

	res = nfsio_remove(cbd->nfsio, objname);
	if (res != NFS3_OK) {
		printf("Failed to remove object : \"%s\" %s %d\n", objname, nfs_error(res), res);
		free(objname);
		exit(10);
	}

	free(objname);
}

static void nfs3_deltree(struct dbench_op *op)
{
	struct cb_data *cbd;
	nfsstat3 res;
	
	cbd = malloc(sizeof(struct cb_data));

	cbd->nfsio = op->child->private;
	cbd->dirname = discard_const(op->fname);

	res = nfsio_lookup(cbd->nfsio, cbd->dirname, NULL);
	if (res != NFS3ERR_NOENT) {
		nfsio_readdirplus(cbd->nfsio, cbd->dirname, dirent_cb, cbd);
		nfsio_rmdir(cbd->nfsio, cbd->dirname);
	}

	res = nfsio_lookup(cbd->nfsio, cbd->dirname, NULL);
	if (res != NFS3ERR_NOENT) {
		printf("Directory \"%s\" not empty. Aborting\n");
		free(cbd);
		exit(10);
	}
	free(cbd);
}

static int expected_status(const char *status)
{
	if (strncmp(status, "0x", 2) == 0) {
		return strtol(status, NULL, 16);
	}
}

static void failed(struct child_struct *child)
{
	child->failed = 1;
	printf("ERROR: child %d failed at line %d\n", child->id, child->line);
	exit(1);
}

static void nfs3_getattr(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_getattr(op->child->private, op->fname, NULL);
	if (res != expected_status(op->status)) {
		printf("[%d] GETATTR \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}


static void nfs3_lookup(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_lookup(op->child->private, op->fname, NULL);
	if (res != expected_status(op->status)) {
		printf("[%d] LOOKUP \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_create(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_create(op->child->private, op->fname);
	if (res != expected_status(op->status)) {
		printf("[%d] CREATE \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_write(struct dbench_op *op)
{
	int offset = op->params[0];
	int len = op->params[1];
	int stable = op->params[2];
	nfsstat3 res;

	res = nfsio_write(op->child->private, op->fname, rw_buf, offset, len, stable);
	if (res != expected_status(op->status)) {
		printf("[%d] WRITE \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname,
		       res, expected_status(op->status));
		failed(op->child);
	}
	op->child->bytes += len;
}

static void nfs3_commit(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_commit(op->child->private, op->fname);
	if (res != expected_status(op->status)) {
		printf("[%d] COMMIT \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}


static void nfs3_read(struct dbench_op *op)
{
	int offset = op->params[0];
	int len = op->params[1];
	nfsstat3 res = 0;

	res = nfsio_read(op->child->private, op->fname, rw_buf, offset, len, NULL, NULL);
	if (res != expected_status(op->status)) {
		printf("[%d] READ \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname,
		       res, expected_status(op->status));
		failed(op->child);
	}
	op->child->bytes += len;
}

static void nfs3_access(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_access(op->child->private, op->fname, 0, NULL);
	if (res != expected_status(op->status)) {
		printf("[%d] ACCESS \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_mkdir(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_mkdir(op->child->private, op->fname);
	if (res != expected_status(op->status)) {
		printf("[%d] MKDIR \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_rmdir(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_rmdir(op->child->private, op->fname);
	if (res != expected_status(op->status)) {
		printf("[%d] RMDIR \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_fsstat(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_fsstat(op->child->private);
	if (res != expected_status(op->status)) {
		printf("[%d] FSSTAT failed (%x) - expected %x\n", 
		       op->child->line, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_fsinfo(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_fsinfo(op->child->private);
	if (res != expected_status(op->status)) {
		printf("[%d] FSINFO failed (%x) - expected %x\n", 
		       op->child->line, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_symlink(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_symlink(op->child->private, op->fname, op->fname2);
	if (res != expected_status(op->status)) {
		printf("[%d] SYMLINK \"%s\"->\"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, op->fname2,
		       res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_remove(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_remove(op->child->private, op->fname);
	if (res != expected_status(op->status)) {
		printf("[%d] REMOVE \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_readdirplus(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_readdirplus(op->child->private, op->fname, NULL, NULL);
	if (res != expected_status(op->status)) {
		printf("[%d] READDIRPLUS \"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_link(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_link(op->child->private, op->fname, op->fname2);
	if (res != expected_status(op->status)) {
		printf("[%d] LINK \"%s\"->\"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, op->fname2,
		       res, expected_status(op->status));
		failed(op->child);
	}
}

static void nfs3_rename(struct dbench_op *op)
{
	nfsstat3 res;

	res = nfsio_rename(op->child->private, op->fname, op->fname2);
	if (res != expected_status(op->status)) {
		printf("[%d] RENAME \"%s\"->\"%s\" failed (%x) - expected %x\n", 
		       op->child->line, op->fname, op->fname2,
		       res, expected_status(op->status));
		failed(op->child);
	}
}

static struct backend_op ops[] = {
	{ "Deltree",  nfs3_deltree },
	{ "GETATTR3", nfs3_getattr },
	{ "LOOKUP3",  nfs3_lookup },
	{ "CREATE3",  nfs3_create },
	{ "WRITE3",   nfs3_write },
	{ "COMMIT3",  nfs3_commit },
	{ "READ3",    nfs3_read },
	{ "ACCESS3",  nfs3_access },
	{ "MKDIR3",   nfs3_mkdir },
	{ "RMDIR3",   nfs3_rmdir },
	{ "FSSTAT3",  nfs3_fsstat },
	{ "FSINFO3",  nfs3_fsinfo },
	{ "SYMLINK3", nfs3_symlink },
	{ "REMOVE3",  nfs3_remove },
	{ "READDIRPLUS3", nfs3_readdirplus },
	{ "RENAME3",  nfs3_rename },
	{ "LINK3",    nfs3_link },
	{ NULL, NULL}
};

struct nb_operations nfs_ops = {
	.backend_name = "nfsbench",
	.setup 	      = nfs3_setup,
	.cleanup      = nfs3_cleanup,
	.ops          = ops
};
