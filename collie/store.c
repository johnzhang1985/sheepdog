/*
 * Copyright (C) 2009 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/xattr.h>
#include <sys/statvfs.h>

#include "collie.h"
#include "meta.h"

static char *obj_dir;
static char *mnt_dir;
static char *zero_block;

static int stat_sheep(uint64_t *store_size, uint64_t *store_free)
{
	struct statvfs vs;
	int ret;
	DIR *dir;
	struct dirent *d;
	uint64_t used = 0;
	struct stat s;
	char path[1024];

	ret = statvfs(mnt_dir, &vs);
	if (ret)
		return SD_RES_EIO;

	dir = opendir(obj_dir);
	if (!dir)
		return SD_RES_EIO;

	while ((d = readdir(dir))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		snprintf(path, sizeof(path), "%s/%s", obj_dir, d->d_name);

		ret = stat(path, &s);
		if (ret)
			continue;

		used += s.st_size;
	}

	*store_size = vs.f_frsize * vs.f_bfree;
	*store_free = vs.f_frsize * vs.f_bfree - used;

	return SD_RES_SUCCESS;
}

static int read_from_one(struct cluster_info *cluster, uint64_t oid,
			 unsigned *rlen, void *buf, uint64_t offset)
{
	int i, n, nr, fd, ret;
	unsigned wlen;
	char name[128];
	struct sheepdog_node_list_entry *e;
	struct sd_obj_req hdr;
	struct sd_obj_rsp *rsp = (struct sd_obj_rsp *)&hdr;

	e = zalloc(SD_MAX_NODES * sizeof(struct sheepdog_node_list_entry));
again:
	nr = build_node_list(&cluster->node_list, e);

	for (i = 0; i < nr; i++) {
		n = obj_to_sheep(e, nr, oid, i);

		snprintf(name, sizeof(name), "%d.%d.%d.%d",
			 e[n].addr[12], e[n].addr[13],
			 e[n].addr[14], e[n].addr[15]);

		fd = connect_to(name, e[n].port);
		if (fd < 0)
			continue;

		memset(&hdr, 0, sizeof(hdr));
		hdr.opcode = SD_OP_READ_OBJ;
		hdr.oid = oid;
		hdr.epoch = cluster->epoch;

		hdr.flags = 0;
		hdr.data_length = *rlen;
		hdr.offset = offset;

		ret = exec_req(fd, (struct sd_req *)&hdr, buf, &wlen, rlen);

		close(fd);

		if (ret)
			continue;

		switch (rsp->result) {
		case SD_RES_SUCCESS:
			return 0;
		case SD_RES_OLD_NODE_VER:
		case SD_RES_NEW_NODE_VER:
			/* waits for the node list timer */
			sleep(2);
			goto again;
			break;
		default:
			;
		}
	}

	return -1;
}

static int read_from_other_sheeps(struct cluster_info *cluster,
				  uint64_t oid, char *buf, int copies)
{
	int ret;
	unsigned int rlen;

	rlen = SD_DATA_OBJ_SIZE;

	ret = read_from_one(cluster, oid, &rlen, buf, 0);

	return ret;
}

void store_queue_request(struct work *work, int idx)
{
	struct request *req = container_of(work, struct request, work);
	struct cluster_info *cluster = req->ci->cluster;
	char path[1024];
	int fd = -1, ret = SD_RES_SUCCESS;
	int flags = O_RDWR;
	char *buf = zero_block + idx * SD_DATA_OBJ_SIZE;
	char aname[] = "user.sheepdog.copies";
	struct sd_obj_req *hdr = (struct sd_obj_req *)&req->rq;
	struct sd_obj_rsp *rsp = (struct sd_obj_rsp *)&req->rp;
	uint64_t oid = hdr->oid;
	uint32_t opcode = hdr->opcode;
	uint32_t epoch = cluster->epoch;
	uint32_t req_epoch = hdr->epoch;
	struct sd_node_rsp *nrsp = (struct sd_node_rsp *)&req->rp;
	int copies;

	/* use le_to_cpu */

	snprintf(path, sizeof(path), "%s/%" PRIx64, obj_dir, oid);

	dprintf("%d, %x, %s, %u, %u\n", idx, opcode, path, epoch, req_epoch);

	if (list_empty(&cluster->node_list)) {
		/* we haven't got SD_OP_GET_NODE_LIST response yet. */
		ret = SD_RES_SYSTEM_ERROR;
		goto out;
	}

	if (opcode != SD_OP_GET_NODE_LIST) {
		if (before(req_epoch, epoch)) {
			ret = SD_RES_OLD_NODE_VER;
			eprintf("old node version %u %u, %x %" PRIx64 "\n",
				epoch, req_epoch, opcode, oid);
			goto out;
		} else if (after(req_epoch, epoch)) {
			ret = SD_RES_NEW_NODE_VER;
			eprintf("new node version %u %u %x %" PRIx64 "\n",
				epoch, req_epoch, opcode, oid);
			goto out;
		}
	}

	switch (opcode) {
	case SD_OP_CREATE_AND_WRITE_OBJ:
	case SD_OP_WRITE_OBJ:
	case SD_OP_READ_OBJ:
	case SD_OP_SYNC_OBJ:
		if (opcode == SD_OP_CREATE_AND_WRITE_OBJ)
			flags |= O_CREAT;

		fd = open(path, flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
		if (fd < 0) {
			if (errno == ENOENT)
				ret = SD_RES_NO_OBJ;
			else
				ret = SD_RES_UNKNOWN;

			goto out;
		}

		if (opcode != SD_OP_CREATE_AND_WRITE_OBJ)
			break;

		if (!hdr->copies) {
			eprintf("zero copies is invalid\n");
			ret = SD_RES_INVALID_PARMS;
			goto out;
		}

		ret = ftruncate(fd, 0);
		if (ret) {
			ret = SD_RES_EIO;
			goto out;
		}

		ret = fsetxattr(fd, aname, &hdr->copies,
				sizeof(hdr->copies), 0);
		if (ret) {
			eprintf("use 'user_xattr' option?\n");
			ret = SD_RES_SYSTEM_ERROR;
			goto out;
		}

		if (!is_data_obj(oid))
			break;

		if (hdr->flags & SD_FLAG_CMD_COW) {
			dprintf("%" PRIu64 "\n", hdr->cow_oid);

			ret = read_from_other_sheeps(cluster,
						     hdr->cow_oid, buf,
						     hdr->copies);
			if (ret) {
				ret = 1;
				goto out;
			}
		} else {
			dprintf("%" PRIu64 "\n", oid);
			memset(buf, 0, SD_DATA_OBJ_SIZE);
		}

		dprintf("%" PRIu64 "\n", oid);

		ret = pwrite64(fd, buf, SD_DATA_OBJ_SIZE, 0);
		if (ret != SD_DATA_OBJ_SIZE) {
			ret = SD_RES_EIO;
			goto out;
		}
	default:
		break;
	}

	switch (opcode) {
	case SD_OP_REMOVE_OBJ:
		ret = unlink(path);
		if (ret)
			ret = 1;
		break;
	case SD_OP_READ_OBJ:
		/*
		 * TODO: should be optional (we can use the flags) for
		 * performance; qemu doesn't always need the copies.
		 */
		copies = 0;
		ret = fgetxattr(fd, aname, &copies, sizeof(copies));
		if (ret != sizeof(copies)) {
			ret = SD_RES_SYSTEM_ERROR;
			goto out;
		}

		ret = pread64(fd, req->data, hdr->data_length, hdr->offset);
		if (ret < 0)
			ret = SD_RES_EIO;
		else {
			rsp->data_length = ret;
			rsp->copies = copies;
			ret = SD_RES_SUCCESS;
		}
		break;
	case SD_OP_CREATE_AND_WRITE_OBJ:
	case SD_OP_WRITE_OBJ:
		ret = pwrite64(fd, req->data, hdr->data_length, hdr->offset);
		if (ret != hdr->data_length) {
			ret = SD_RES_EIO;
			goto out;
		} else
			ret = SD_RES_SUCCESS;
		break;
	case SD_OP_SYNC_OBJ:
		ret = fsync(fd);
		if (ret) {
			if (errno == EIO)
				ret = SD_RES_EIO;
			else
				ret = SD_RES_UNKNOWN;
		}
		break;
	case SD_OP_STAT_SHEEP:
		ret = stat_sheep(&nrsp->store_size, &nrsp->store_free);
		break;
	}
out:
	if (ret != SD_RES_SUCCESS) {
		dprintf("failed, %d, %d, %x, %s, %u, %u\n", ret, idx, opcode,
			path, epoch, req_epoch);

		rsp->result = ret;
	}

	if (fd != -1)
		close(fd);
}

int init_store(char *dir)
{
	int ret;
	struct mntent *mnt;
	struct stat s, ms;
	FILE *fp;

	ret = stat(dir, &s);
	if (ret) {
		if (errno == ENOENT) {
			ret = mkdir(dir, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP |
				    S_IWGRP | S_IXGRP);
			if (ret) {
				eprintf("can't create the object dir %s, %m\n",
					dir);
				return 1;
			} else {
				ret = stat(dir, &s);
				if (ret)
					return 1;

				eprintf("created the object dir %s\n", dir);
			}
		} else {
			eprintf("can't handle the object dir %s, %m\n", dir);
			return 1;
		}
	} else if (!S_ISDIR(s.st_mode)) {
		eprintf("%s is not a directory\n", dir);
		return 1;
	}

	obj_dir = dir;

	fp = setmntent(MOUNTED, "r");
	if (!fp)
		return 1;

	while ((mnt = getmntent(fp))) {
		ret = stat(mnt->mnt_dir, &ms);
		if (ret)
			continue;

		if (ms.st_dev == s.st_dev) {
			mnt_dir = strdup(mnt->mnt_dir);
			break;
		}
	}

	endmntent(fp);

	zero_block = zalloc(SD_DATA_OBJ_SIZE * NR_WORKER_THREAD);
	if (!zero_block)
		return 1;

	return ret;
}
