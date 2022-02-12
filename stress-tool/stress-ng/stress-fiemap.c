/*
 * Copyright (C) 2016-2021 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#include "stress-ng.h"

#define MAX_FIEMAP_PROCS	(4)		/* Number of FIEMAP stressors */
#define COUNT_MAX		(128)

static const stress_help_t help[] = {
	{ NULL,	"fiemap N",	  "start N workers exercising the FIEMAP ioctl" },
	{ NULL,	"fiemap-ops N",	  "stop after N FIEMAP ioctl bogo operations" },
	{ NULL,	"fiemap-bytes N", "specify size of file to fiemap" },
	{ NULL,	NULL,		   NULL }
};

static int stress_set_fiemap_bytes(const char *opt)
{
	uint64_t fiemap_bytes;

	fiemap_bytes = stress_get_uint64_byte_filesystem(opt, 1);
	stress_check_range_bytes("fiemap-bytes", fiemap_bytes,
		MIN_FIEMAP_SIZE, MAX_FIEMAP_SIZE);
	return stress_set_setting("fiemap-bytes", TYPE_ID_UINT64, &fiemap_bytes);
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_fiemap_bytes,	stress_set_fiemap_bytes },
	{ 0,			NULL }
};

#if defined(HAVE_LINUX_FS_H) &&		\
    defined(HAVE_LINUX_FIEMAP_H) && 	\
    defined(FS_IOC_FIEMAP)


/*
 *  stress_fiemap_count()
 *     racy bogo counter across child processes, avoids
 *     locking but is not accurate
 */
static bool stress_fiemap_count(const stress_args_t *args, uint64_t *counters)
{
	size_t i;
	uint64_t counter;

	for (counter = 0, i = 0; i < MAX_FIEMAP_PROCS; i++) {
		counter += counters[i];
	}

	set_counter(args, counter);
	return keep_stressing(args);
}

/*
 *  stress_fiemap_writer()
 *	write data in random places and punch holes
 *	in data in random places to try and maximize
 *	extents in the file
 */
static int stress_fiemap_writer(
	const stress_args_t *args,
	const int fd,
	const uint64_t fiemap_bytes,
	uint64_t *counters)
{
	uint8_t buf[1];
	const uint64_t len = fiemap_bytes - sizeof(buf);
	int rc = EXIT_FAILURE;
#if defined(FALLOC_FL_PUNCH_HOLE) && \
    defined(FALLOC_FL_KEEP_SIZE)
	bool punch_hole = true;
#endif

	stress_strnrnd((char *)buf, sizeof(buf));

	do {
		uint64_t offset;

		offset = (stress_mwc64() % len) & ~0x1fffUL;
		if (lseek(fd, (off_t)offset, SEEK_SET) < 0)
			break;
		if (!stress_fiemap_count(args, counters))
			break;
		if (write(fd, buf, sizeof(buf)) < 0) {
			if (errno == ENOSPC)
				continue;
			if ((errno != EAGAIN) && (errno != EINTR)) {
				pr_fail("%s: write failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
				goto tidy;
			}
		}
		if (!stress_fiemap_count(args, counters))
			break;
#if defined(FALLOC_FL_PUNCH_HOLE) && \
    defined(FALLOC_FL_KEEP_SIZE)
		if (!punch_hole)
			continue;
		(void)shim_usleep(1000);

		offset = stress_mwc64() % len;
		if (shim_fallocate(fd, FALLOC_FL_PUNCH_HOLE |
				  FALLOC_FL_KEEP_SIZE, (off_t)offset, 8192) < 0) {
			if (errno == ENOSPC)
				continue;
			if (errno == EOPNOTSUPP)
				punch_hole = false;
		}
		(void)shim_usleep(1000);
		if (!stress_fiemap_count(args, counters))
			break;
#endif
	} while (keep_stressing(args));
	rc = EXIT_SUCCESS;
tidy:
	(void)close(fd);

	return rc;
}

/*
 *  stress_fiemap_ioctl()
 *	exercise the FIEMAP ioctl
 */
static void stress_fiemap_ioctl(
	const stress_args_t *args,
	uint64_t *counter,
	const int fd)
{
	int c = stress_mwc32() % COUNT_MAX;

	do {
		struct fiemap *fiemap, *tmp;
		size_t extents_size;

		fiemap = (struct fiemap *)calloc(1, sizeof(*fiemap));
		if (!fiemap) {
			pr_err("Out of memory allocating fiemap\n");
			break;
		}
		fiemap->fm_length = ~0UL;

		/* Find out how many extents there are */
		if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
			if (errno == EOPNOTSUPP) {
				if (args->instance == 0)
					pr_inf_skip("%s: FS_IOC_FIEMAP not supported on the file system, skipping stressor\n",
						args->name);
				free(fiemap);
				break;
			}
			pr_fail("%s: ioctl FS_IOC_FIEMAP failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			free(fiemap);
			break;
		}
		if (!keep_stressing(args)) {
			free(fiemap);
			break;
		}

		/* Read in the extents */
		extents_size = sizeof(struct fiemap_extent) *
			(fiemap->fm_mapped_extents);

		/* Resize fiemap to allow us to read in the extents */
		tmp = (struct fiemap *)realloc(fiemap,
			sizeof(*fiemap) + extents_size);
		if (!tmp) {
			pr_fail("%s: ioctl FS_IOC_FIEMAP failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			free(fiemap);
			break;
		}
		fiemap = tmp;

		(void)memset(fiemap->fm_extents, 0, extents_size);
		fiemap->fm_extent_count = fiemap->fm_mapped_extents;
		fiemap->fm_mapped_extents = 0;

		if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
			pr_fail("%s: ioctl FS_IOC_FIEMAP failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			free(fiemap);
			break;
		}
		free(fiemap);
		(*counter)++;
		if (c++ > COUNT_MAX) {
			c = 0;
			fdatasync(fd);
		}
	} while (keep_stressing(args));
}

/*
 *  stress_fiemap_spawn()
 *	helper to spawn off fiemap stressor
 */
static inline pid_t stress_fiemap_spawn(
	const stress_args_t *args,
	uint64_t *counter,
	const int fd)
{
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		(void)setpgid(0, g_pgrp);
		stress_parent_died_alarm();
		(void)sched_settings_apply(true);
		stress_mwc_reseed();
		stress_fiemap_ioctl(args, counter, fd);
		_exit(EXIT_SUCCESS);
	}
	(void)setpgid(pid, g_pgrp);
	return pid;
}

/*
 *  stress_fiemap
 *	stress fiemap IOCTL
 */
static int stress_fiemap(const stress_args_t *args)
{
	pid_t pids[MAX_FIEMAP_PROCS];
	int ret, fd, rc = EXIT_FAILURE, status;
	char filename[PATH_MAX];
	size_t i, n;
	uint64_t *counters;
	const size_t counters_sz = sizeof(*counters) * MAX_FIEMAP_PROCS;
	uint64_t fiemap_bytes = DEFAULT_FIEMAP_SIZE;
	struct fiemap fiemap;

	if (!stress_get_setting("fiemap-bytes", &fiemap_bytes)) {
		if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
			fiemap_bytes = MAXIMIZED_FILE_SIZE;
		if (g_opt_flags & OPT_FLAGS_MINIMIZE)
			fiemap_bytes = MIN_FIEMAP_SIZE;
	}
	fiemap_bytes /= args->num_instances;
	if (fiemap_bytes < MIN_FIEMAP_SIZE)
		fiemap_bytes = MIN_FIEMAP_SIZE;

	/* We need some share memory for counter accounting */
	counters = mmap(NULL, counters_sz, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (counters == MAP_FAILED) {
		pr_err("%s: mmap failed: errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}
	(void)memset(counters, 0, counters_sz);

	ret = stress_temp_dir_mk_args(args);
	if (ret < 0) {
		rc = exit_status(-ret);
		goto clean;
	}

	(void)stress_temp_filename_args(args,
		filename, sizeof(filename), stress_mwc32());
	if ((fd = open(filename, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR)) < 0) {
		rc = exit_status(errno);
		pr_fail("%s: open %s failed, errno=%d (%s)\n",
			args->name, filename, errno, strerror(errno));
		goto dir_clean;
	}
	(void)unlink(filename);

	memset(&fiemap, 0, sizeof(fiemap));
	fiemap.fm_length = ~0UL;
	if (ioctl(fd, FS_IOC_FIEMAP, &fiemap) < 0) {
		errno = EOPNOTSUPP;
		if (errno == EOPNOTSUPP) {
			if (args->instance == 0)
				pr_inf_skip("%s: FS_IOC_FIEMAP not supported on the file system, skipping stressor\n",
					args->name);
			rc = EXIT_NOT_IMPLEMENTED;
			goto close_clean;
		}
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	for (n = 0; n < MAX_FIEMAP_PROCS; n++) {
		if (!keep_stressing(args)) {
			rc = EXIT_SUCCESS;
			goto reap;
		}

		pids[n] = stress_fiemap_spawn(args, &counters[n], fd);
		if (pids[n] < 0)
			goto reap;
	}
	rc = stress_fiemap_writer(args, fd, fiemap_bytes, counters);
reap:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	/* And reap stressors */
	for (i = 0; i < n; i++) {
		(void)kill(pids[i], SIGKILL);
		(void)shim_waitpid(pids[i], &status, 0);
	}
close_clean:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	(void)close(fd);
dir_clean:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	(void)stress_temp_dir_rm_args(args);
clean:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	(void)munmap(counters, counters_sz);

	return rc;
}

stressor_info_t stress_fiemap_info = {
	.stressor = stress_fiemap,
	.class = CLASS_FILESYSTEM | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
#else
stressor_info_t stress_fiemap_info = {
	.stressor = stress_not_implemented,
	.class = CLASS_FILESYSTEM | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
#endif