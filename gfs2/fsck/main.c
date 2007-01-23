/*****************************************************************************
******************************************************************************
**
**  Copyright (C) 2005 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
******************************************************************************
*****************************************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libgen.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <signal.h>

#include "copyright.cf"
#include "libgfs2.h"
#include "fsck.h"
#include "osi_list.h"

struct gfs2_options opts = {0};
struct gfs2_inode *lf_dip; /* Lost and found directory inode */
osi_list_t dir_hash[FSCK_HASH_SIZE];
osi_list_t inode_hash[FSCK_HASH_SIZE];
struct gfs2_block_list *bl;
uint64_t last_fs_block, last_reported_block = -1;
int skip_this_pass = FALSE, fsck_abort = FALSE;
const char *pass = "";
uint64_t last_data_block;
uint64_t first_data_block;
osi_list_t dup_list;
char *prog_name = "gfs2_fsck"; /* needed by libgfs2 */

/* This function is for libgfs2's sake.                                      */
void print_it(const char *label, const char *fmt, const char *fmt2, ...)
{
	va_list args;

	va_start(args, fmt2);
	printf("%s: ", label);
	vprintf(fmt, args);
	va_end(args);
}

void print_map(struct gfs2_block_list *il, int count)
{
	int i, j;
	struct gfs2_block_query k;

	log_info("Printing map of blocks - 80 blocks per row\n");
	j = 0;
	for(i = 0; i < count; i++) {
		if(j > 79) {
			log_info("\n");
			j = 0;
		}
		else if(!(j %10) && j != 0) {
			log_info(" ");
		}
		j++;
		gfs2_block_check(il, i, &k);
		log_info("%X", k.block_type);

	}
	log_info("\n");
}

void usage(char *name)
{
	printf("Usage: %s [-hnqvVy] <device> \n", basename(name));
}

void version(void)
{
	printf("GFS2 fsck %s (built %s %s)\n",
	       GFS_RELEASE_NAME, __DATE__, __TIME__);
	printf("%s\n", REDHAT_COPYRIGHT);
}

int read_cmdline(int argc, char **argv, struct gfs2_options *opts)
{
	char c;

	while((c = getopt(argc, argv, "hnqvyV")) != -1) {
		switch(c) {

		case 'h':
			usage(argv[0]);
			exit(0);
			break;
		case 'n':
			opts->no = 1;
			break;
		case 'q':
			decrease_verbosity();
			break;
		case 'v':
			increase_verbosity();
			break;
		case 'V':
			version();
			exit(0);
			break;
		case 'y':
			opts->yes = 1;
			break;
		case ':':
		case '?':
			fprintf(stderr, "Please use '-h' for usage.\n");
			exit(1);
			break;
		default:
			fprintf(stderr, "Bad programmer! You forgot to catch"
				" the %c flag\n", c);
			exit(1);
			break;

		}
	}
	if(argc > optind) {
		opts->device = (argv[optind]);
		if(!opts->device) {
			fprintf(stderr, "Please use '-h' for usage.\n");
			exit(1);
		}
	} else {
		fprintf(stderr, "No device specified.  Use '-h' for usage.\n");
		exit(1);
	}
	return 0;
}

void interrupt(int sig)
{
	fd_set rfds;
	struct timeval tv;
	char response;
	int err;

	if (opts.query) /* if we're asking them a question */
		return;     /* ignore the interrupt signal */
	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	/* Make sure there isn't extraneous input before asking the
	 * user the question */
	while((err = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv))) {
		if(err < 0) {
			log_debug("Error in select() on stdin\n");
			break;
		}
		read(STDIN_FILENO, &response, sizeof(char));
	}
	while (TRUE) {
		printf("\ngfs_fsck interrupted in %s:  ", pass);
		if (!last_reported_block || last_reported_block == last_fs_block)
			printf("progress unknown.\n");
		else
			printf("processing block %" PRIu64 " out of %" PRIu64 "\n",
				   last_reported_block, last_fs_block);
		printf("Do you want to abort gfs_fsck, skip the rest of %s or continue (a/s/c)?", pass);

		/* Make sure query is printed out */
		fflush(stdout);
		read(STDIN_FILENO, &response, sizeof(char));

		if(tolower(response) == 's') {
			skip_this_pass = TRUE;
			return;
		}
		else if (tolower(response) == 'a') {
			fsck_abort = TRUE;
			return;
		}
		else if (tolower(response) == 'c')
			return;
        else {
			while(response != '\n')
				read(STDIN_FILENO, &response, sizeof(char));
			printf("Bad response, please type 'c', 'a' or 's'.\n");
			continue;
        }
	}
}

int main(int argc, char **argv)
{
	struct gfs2_sbd sb;
	struct gfs2_sbd *sbp = &sb;
	int j;

	memset(sbp, 0, sizeof(*sbp));

	if(read_cmdline(argc, argv, &opts))
		return 1;
	setbuf(stdout, NULL);
	log_notice("Initializing fsck\n");
	if (initialize(sbp))
		return 1;

	signal(SIGINT, interrupt);
	log_notice("Starting pass1\n");
	pass = "pass 1";
	last_reported_block = 0;
	if (pass1(sbp))
		return 1;
	if (skip_this_pass || fsck_abort) {
		skip_this_pass = FALSE;
		log_notice("Pass1 interrupted   \n");
	}
	else
		log_notice("Pass1 complete      \n");

	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1b";
		log_notice("Starting pass1b\n");
		if(pass1b(sbp))
			return 1;
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass1b interrupted   \n");
		}
		else
			log_notice("Pass1b complete\n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1c";
		log_notice("Starting pass1c\n");
		if(pass1c(sbp))
			return 1;
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass1c interrupted   \n");
		}
		else
			log_notice("Pass1c complete\n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 2";
		log_notice("Starting pass2\n");
		if (pass2(sbp))
			return 1;
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass2 interrupted   \n");
		}
		else
			log_notice("Pass2 complete      \n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 3";
		log_notice("Starting pass3\n");
		if (pass3(sbp))
			return 1;
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass3 interrupted   \n");
		}
		else
			log_notice("Pass3 complete      \n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 4";
		log_notice("Starting pass4\n");
		if (pass4(sbp))
			return 1;
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass4 interrupted   \n");
		}
		else
			log_notice("Pass4 complete      \n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 5";
		log_notice("Starting pass5\n");
		if (pass5(sbp))
			return 1;
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass5 interrupted   \n");
		}
		else
			log_notice("Pass5 complete      \n");
	}
	/* Free up our system inodes */
	inode_put(sbp->md.inum, updated);
	inode_put(sbp->md.statfs, updated);
	for (j = 0; j < sbp->md.journals; j++)
		inode_put(sbp->md.journal[j], updated);
	inode_put(sbp->md.jiinode, updated);
	inode_put(sbp->md.riinode, updated);
	inode_put(sbp->md.qinode, updated);
	inode_put(sbp->md.pinode, updated);
	inode_put(sbp->md.rooti, updated);
	inode_put(sbp->master_dir, updated);
	if (lf_dip)
		inode_put(lf_dip, updated);
/*	print_map(sbp->bl, sbp->last_fs_block); */

	log_notice("Writing changes to disk\n");
	bsync(sbp);
	destroy(sbp);
	log_notice("gfs2_fsck complete    \n");

	return 0;
}
