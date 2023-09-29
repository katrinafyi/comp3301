/*
 * Copyright 2023, The University of Queensland
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Simple test tool for vkey(4)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dev/vkeyvar.h>

#include "sshbuf.h"
#include "authfd.h"
#include "log.h"

#define	VKEY_MAX_MSG	(2*16384)
#define	VKEY_DEVFMT	"/dev/vkey%u"
#define	VKEY_MAXDEV	8

static int	cmd_list(int argc, char *argv[]);
static int	cmd_mknod(int argc, char *argv[]);
static int	cmd_sign(int fd, int argc, char *argv[]);
static int	cmd_randbytes(int fd, int argc, char *argv[]);
static int	cmd_delay(int fd, int argc, char *argv[]);

static const char 	*msg_type_str(uint8_t v);

static __dead void
usage(void)
{
	extern char *__progname;
	fprintf(stderr, "usage: %s [-v] list\n", __progname);
	fprintf(stderr, "       - lists all vkey devices and keys\n");
	fprintf(stderr, "       %s [-v] mknod\n", __progname);
	fprintf(stderr, "       - creates device nodes\n");
	fprintf(stderr, "       %s [-d DEV] [-v] sign [-k IDX] [-c COMMENT] "
	    "[FILE]\n", __progname);
	fprintf(stderr, "       - signs some data using a key\n");
	fprintf(stderr, "       %s [-d DEV] [-v] randbytes [-b BYTES]\n",
	    __progname);
	fprintf(stderr, "       - generates random bytes\n");
	fprintf(stderr, "       %s [-d DEV] [-v] delay [-t MSEC]\n",
	    __progname);
	fprintf(stderr, "       - sends a command that does a sleep before "
	    "replying\n");
	exit(1);
}

static int verbose = 0;
static char devpath[PATH_MAX];

int
main(int argc, char *argv[])
{
	const char *subcmd;
	int fd;
	struct vkey_info_arg info;
	uint i;
	int ch;
	const char *errstr;

	snprintf(devpath, sizeof(devpath), VKEY_DEVFMT, 0);

	while ((ch = getopt(argc, argv, "vd:")) != -1) {
		switch (ch) {
		case 'v':
			++verbose;
			break;
		case 'd':
			if (optarg[0] == '/') {
				strlcpy(devpath, optarg, sizeof(devpath));
			} else if (isdigit(optarg[0])) {
				i = strtonum(optarg, 0, VKEY_MAXDEV, &errstr);
				if (errstr != NULL) {
					warnx("-d arg starts with a number "
					    "but is %s", errstr);
					usage();
				}
				snprintf(devpath, sizeof(devpath), VKEY_DEVFMT,
				    i);
			} else {
				strlcpy(devpath, "/dev/", sizeof(devpath));
				strlcat(devpath, optarg, sizeof(devpath));
			}
			break;
		default:
			usage();
		}
	}

	if (optind >= argc) {
		warnx("subcmd required");
		usage();
	}
	argc -= optind;
	argv += optind;

	subcmd = argv[0];
	optreset = 1;
	optind = 0;

	if (strcmp(subcmd, "list") == 0) {
		return (cmd_list(argc, argv));
	} else if (strcmp(subcmd, "mknod") == 0) {
		return (cmd_mknod(argc, argv));
	}

	fd = open(devpath, O_RDWR);
	if (fd < 0)
		err(1, "open");
	if (ioctl(fd, VKEYIOC_GET_INFO, &info))
		err(1, "ioctl(VKEYIOC_GET_INFO)");

	if (verbose) {
		fprintf(stderr, "%s: vkey v%u.%d\n", devpath,
		    info.vkey_major, info.vkey_minor);
	}

	if (strcmp(subcmd, "sign") == 0) {
		return (cmd_sign(fd, argc, argv));
	} else if (strcmp(subcmd, "randbytes") == 0) {
		return (cmd_randbytes(fd, argc, argv));
	} else if (strcmp(subcmd, "delay") == 0) {
		return (cmd_delay(fd, argc, argv));
	}

	warnx("unknown subcmd: '%s'", subcmd);
	usage();

	return (0);
}

void
cleanup_exit(int i)
{
	_exit(i);
}

static int
do_ioctl_cmd(int fd, struct vkey_cmd_arg *cmd, struct sshbuf *inbuf,
    struct sshbuf *outbuf)
{
	int rc;
	size_t adjust;
	if (inbuf != NULL) {
		cmd->vkey_in[0].iov_len = sshbuf_len(inbuf);
		cmd->vkey_in[0].iov_base = (void *)sshbuf_ptr(inbuf);
	}
	if (outbuf != NULL) {
		rc = sshbuf_reserve(outbuf, VKEY_MAX_MSG,
		    (u_char **)&cmd->vkey_out[0].iov_base);
		if (rc != 0)
			fatal_fr(rc, "sshbuf_reserve");
		cmd->vkey_out[0].iov_len = VKEY_MAX_MSG;
	}
	if ((rc = ioctl(fd, VKEYIOC_CMD, cmd)))
		return (rc);
	adjust = VKEY_MAX_MSG - cmd->vkey_rlen;
	if (outbuf != NULL && adjust != 0) {
		if ((rc = sshbuf_consume_end(outbuf, adjust)))
			fatal_fr(rc, "sshbuf_consume");
	}
	return (0);
}

static int
cmd_mknod(int argc, char *argv[])
{
	uint i;

	if (argc > 1) {
		warnx("no extra arguments to mknod");
		usage();
	}

	for (i = 0; i < VKEY_MAXDEV; ++i) {
		snprintf(devpath, sizeof(devpath), VKEY_DEVFMT, i);
		if (verbose)
			fprintf(stderr, "%s\n", devpath);
		if (mknod(devpath, S_IFCHR | 0666, makedev(101, i))) {
			switch (errno) {
			case EEXIST:
				continue;
			default:
				warn("mknod(%s)", devpath);
			}
		}
	}

	return (0);
}

static int
cmd_list(int argc, char *argv[])
{
	struct sshbuf *buf, *sbuf;
	uint i, j;
	int fd;
	struct vkey_info_arg info;
	struct vkey_cmd_arg cmd;
	uint32_t count;
	char *ktype, *comment;
	size_t len;
	int rc;

	if (argc > 1) {
		warnx("no extra arguments to list");
		usage();
	}

	buf = sshbuf_new();
	sbuf = sshbuf_new();

	for (i = 0; i < VKEY_MAXDEV; ++i) {
		snprintf(devpath, sizeof(devpath), VKEY_DEVFMT, i);
		fd = open(devpath, O_RDWR);
		if (fd < 0) {
			switch (errno) {
			case ENXIO:
				if (verbose)
					warn("open(%s)", devpath);
				continue;
			default:
				warn("open(%s)", devpath);
				continue;
			}
		}
		if (ioctl(fd, VKEYIOC_GET_INFO, &info)) {
			warn("ioctl(%s, VKEYIOC_GET_INFO)", devpath);
			continue;
		}
		if (verbose) {
			fprintf(stderr, "%s: vkey v%u.%u\n", devpath,
			    info.vkey_major, info.vkey_minor);
		}
		bzero(&cmd, sizeof(cmd));
		cmd.vkey_cmd = SSH2_AGENTC_REQUEST_IDENTITIES;
		if ((rc = do_ioctl_cmd(fd, &cmd, NULL, buf))) {
			warn("ioctl(%s, VKEYIOC_CMD)", devpath);
			continue;
		}

		if (cmd.vkey_reply != SSH2_AGENT_IDENTITIES_ANSWER) {
			warnx("%s: replied with msg type %d (%s)", devpath,
			    cmd.vkey_reply, msg_type_str(cmd.vkey_reply));
			continue;
		}

		if ((rc = sshbuf_get_u32(buf, &count)))
			fatal_fr(rc, "sshbuf_get_u32");
		fprintf(stderr, "%s: has %d keys\n", devpath, count);

		for (j = 0; j < count; ++j) {
			sshbuf_reset(sbuf);
			if ((rc = sshbuf_get_stringb(buf, sbuf)))
				fatal_fr(rc, "sshbuf_get_stringb");
			if ((rc = sshbuf_get_cstring(sbuf, &ktype, &len)))
				fatal_fr(rc, "sshbuf_get_cstring");
			if ((rc = sshbuf_get_cstring(buf, &comment, &len)))
				fatal_fr(rc, "sshbuf_get_cstring");
			fprintf(stderr, "    key %u: type = %s, comment = %s\n",
			    j, ktype, comment);
			free(ktype);
			free(comment);
		}
	}

	return (0);
}

static int
cmd_sign(int fd, int argc, char *argv[])
{
	int ch;
	const char *findcmt = NULL;
	int kidx = -1;
	const char *errstr;
	const char *fname = NULL;
	int sfd;
	struct vkey_cmd_arg cmd;
	uint i;
	struct sshbuf *buf, *ibuf, *kbuf, *tbuf;
	uint32_t count;
	size_t len;
	char *ktype, *comment;
	int found;
	char *b64;
	int rc;

	while ((ch = getopt(argc, argv, "k:c:")) != -1) {
		switch (ch) {
		case 'k':
			kidx = strtonum(optarg, 0, UINT32_MAX, &errstr);
			if (errstr != NULL) {
				warnx("-k arg must be a number but is %s",
				    errstr);
				usage();
			}
			break;
		case 'c':
			findcmt = optarg;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;
	if (argc == 1) {
		fname = argv[0];
		sfd = open(fname, O_RDONLY);
		if (sfd < 0)
			err(1, "open(%s)", fname);
	} else if (argc > 1) {
		warnx("too many arguments to sign");
		usage();
	} else {
		sfd = STDIN_FILENO;
	}

	buf = sshbuf_new();
	kbuf = sshbuf_new();
	ibuf = sshbuf_new();

	bzero(&cmd, sizeof(cmd));
	cmd.vkey_cmd = SSH2_AGENTC_REQUEST_IDENTITIES;
	if ((rc = do_ioctl_cmd(fd, &cmd, NULL, buf))) {
		err(1, "ioctl(%s, VKEYIOC_CMD)", devpath);
	}

	if (cmd.vkey_reply != SSH2_AGENT_IDENTITIES_ANSWER) {
		errx(1, "%s: replied with msg type %d (%s)", devpath,
		    cmd.vkey_reply, msg_type_str(cmd.vkey_reply));
	}

	if ((rc = sshbuf_get_u32(buf, &count)))
		fatal_fr(rc, "sshbuf_get_u32");

	found = 0;
	for (i = 0; i < count; ++i) {
		sshbuf_reset(kbuf);
		if ((rc = sshbuf_get_stringb(buf, kbuf)))
			fatal_fr(rc, "sshbuf_get_stringb");
		tbuf = sshbuf_fromb(kbuf);
		if ((rc = sshbuf_get_cstring(tbuf, &ktype, &len)))
			fatal_fr(rc, "sshbuf_get_cstring");
		sshbuf_free(tbuf);
		if ((rc = sshbuf_get_cstring(buf, &comment, &len)))
			fatal_fr(rc, "sshbuf_get_cstring");
		if (kidx > -1) {
			if ((uint)kidx == i) {
				found = 1;
				break;
			}
		} else if (findcmt != NULL) {
			if (strstr(comment, findcmt) != NULL) {
				found = 1;
				break;
			}
		} else {
			/* user gave no filters: use first key */
			found = 1;
			break;
		}
		free(ktype);
		free(comment);
	}
	if (!found)
		errx(1, "%s: failed to find key", devpath);

	if (verbose) {
		fprintf(stderr, "%s: using key %u (type = %s, comment = %s)\n",
		    devpath, i, ktype, comment);
	}

	sshbuf_reset(buf);
	do {
		if ((rc = sshbuf_read(sfd, buf, VKEY_MAX_MSG, &len)))
			fatal_fr(rc, "sshbuf_read");
	} while (len == VKEY_MAX_MSG);

	if ((rc = sshbuf_put_stringb(ibuf, kbuf)))
		fatal_fr(rc, "sshbuf_put_stringb");
	if ((rc = sshbuf_put_stringb(ibuf, buf)))
		fatal_fr(rc, "sshbuf_put_stringb");
	if ((rc = sshbuf_put_u32(ibuf, 0)))
		fatal_fr(rc, "sshbuf_put_u32");

	if (verbose) {
		fprintf(stderr, "sending cmd with %zu byte body\n",
		    sshbuf_len(ibuf));
	}

	bzero(&cmd, sizeof(cmd));
	cmd.vkey_cmd = SSH2_AGENTC_SIGN_REQUEST;
	sshbuf_reset(buf);
	if ((rc = do_ioctl_cmd(fd, &cmd, ibuf, buf))) {
		err(1, "ioctl(%s, VKEYIOC_CMD)", devpath);
	}

	if (cmd.vkey_reply != SSH2_AGENT_SIGN_RESPONSE) {
		errx(1, "%s: replied with msg type %d (%s)", devpath,
		    cmd.vkey_reply, msg_type_str(cmd.vkey_reply));
	}

	sshbuf_reset(kbuf);
	if ((rc = sshbuf_get_stringb(buf, kbuf)))
		fatal_fr(rc, "sshbuf_get_stringb");

	b64 = sshbuf_dtob64_string(kbuf, 0);
	fprintf(stdout, "%s\n", b64);

	return (0);
}

static int
cmd_randbytes(int fd, int argc, char *argv[])
{
	struct sshbuf *buf, *obuf;
	int rc;
	uint32_t bytes = 16;
	char *b64;
	int ch;
	const char *errstr;
	struct vkey_cmd_arg cmd;

	while ((ch = getopt(argc, argv, "b:")) != -1) {
		switch (ch) {
		case 'b':
			bytes = strtonum(optarg, 1, UINT32_MAX, &errstr);
			if (errstr != NULL) {
				warnx("-b arg must be a number but is %s",
				    errstr);
				usage();
			}
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 0) {
		warnx("too many arguments to randbytes");
		usage();
	}

	buf = sshbuf_new();
	obuf = sshbuf_new();

	if ((rc = sshbuf_put_cstring(buf, "randbytes@comp3301.uq.edu.au")))
		fatal_fr(rc, "sshbuf_put_cstring");
	if ((rc = sshbuf_put_u32(buf, bytes)))
		fatal_fr(rc, "sshbuf_put_u32");

	bzero(&cmd, sizeof(cmd));
	cmd.vkey_cmd = SSH_AGENTC_EXTENSION;
	if ((rc = do_ioctl_cmd(fd, &cmd, buf, obuf))) {
		err(1, "ioctl(%s, VKEYIOC_CMD)", devpath);
	}

	if (cmd.vkey_reply != SSH_AGENT_SUCCESS) {
		errx(1, "%s: replied with msg type %d (%s)", devpath,
		    cmd.vkey_reply, msg_type_str(cmd.vkey_reply));
	}

	b64 = sshbuf_dtob64_string(obuf, 0);
	fprintf(stdout, "%s\n", b64);

	return (0);
}

static int
cmd_delay(int fd, int argc, char *argv[])
{
	struct sshbuf *buf;
	int rc;
	uint32_t msec = 1000;
	int ch;
	const char *errstr;
	struct vkey_cmd_arg cmd;

	while ((ch = getopt(argc, argv, "t:")) != -1) {
		switch (ch) {
		case 't':
			msec = strtonum(optarg, 1, UINT32_MAX, &errstr);
			if (errstr != NULL) {
				warnx("-t arg must be a number but is %s",
				    errstr);
				usage();
			}
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 0) {
		warnx("too many arguments to delay");
		usage();
	}

	buf = sshbuf_new();

	if ((rc = sshbuf_put_cstring(buf, "delay@comp3301.uq.edu.au")))
		fatal_fr(rc, "sshbuf_put_cstring");
	if ((rc = sshbuf_put_u32(buf, msec)))
		fatal_fr(rc, "sshbuf_put_u32");

	bzero(&cmd, sizeof(cmd));
	cmd.vkey_cmd = SSH_AGENTC_EXTENSION;
	if ((rc = do_ioctl_cmd(fd, &cmd, buf, NULL))) {
		err(1, "ioctl(%s, VKEYIOC_CMD)", devpath);
	}

	if (cmd.vkey_reply != SSH_AGENT_SUCCESS) {
		errx(1, "%s: replied with msg type %d (%s)", devpath,
		    cmd.vkey_reply, msg_type_str(cmd.vkey_reply));
	}

	return (0);
}

static const char *
msg_type_str(uint8_t v)
{
	switch (v) {
	case SSH_AGENTC_REQUEST_RSA_IDENTITIES:
		return ("SSH_AGENTC_REQUEST_RSA_IDENTITIES");
	case SSH_AGENT_RSA_IDENTITIES_ANSWER:
		return ("SSH_AGENT_RSA_IDENTITIES_ANSWER");
	case SSH_AGENTC_RSA_CHALLENGE:
		return ("SSH_AGENTC_RSA_CHALLENGE");
	case SSH_AGENT_RSA_RESPONSE:
		return ("SSH_AGENT_RSA_RESPONSE");
	case SSH_AGENT_FAILURE:
		return ("SSH_AGENT_FAILURE");
	case SSH_AGENT_SUCCESS:
		return ("SSH_AGENT_SUCCESS");
	case SSH_AGENTC_ADD_RSA_IDENTITY:
		return ("SSH_AGENTC_ADD_RSA_IDENTITY");
	case SSH_AGENTC_REMOVE_RSA_IDENTITY:
		return ("SSH_AGENTC_REMOVE_RSA_IDENTITY");
	case SSH_AGENTC_REMOVE_ALL_RSA_IDENTITIES:
		return ("SSH_AGENTC_REMOVE_ALL_RSA_IDENTITIES");
	case SSH2_AGENTC_REQUEST_IDENTITIES:
		return ("SSH2_AGENTC_REQUEST_IDENTITIES");
	case SSH2_AGENT_IDENTITIES_ANSWER:
		return ("SSH2_AGENT_IDENTITIES_ANSWER");
	case SSH2_AGENTC_SIGN_REQUEST:
		return ("SSH2_AGENTC_SIGN_REQUEST");
	case SSH2_AGENT_SIGN_RESPONSE:
		return ("SSH2_AGENT_SIGN_RESPONSE");
	case SSH2_AGENTC_ADD_IDENTITY:
		return ("SSH2_AGENTC_ADD_IDENTITY");
	case SSH2_AGENTC_REMOVE_IDENTITY:
		return ("SSH2_AGENTC_REMOVE_IDENTITY");
	case SSH2_AGENTC_REMOVE_ALL_IDENTITIES:
		return ("SSH2_AGENTC_REMOVE_ALL_IDENTITIES");
	case SSH_AGENTC_ADD_SMARTCARD_KEY:
		return ("SSH_AGENTC_ADD_SMARTCARD_KEY");
	case SSH_AGENTC_REMOVE_SMARTCARD_KEY:
		return ("SSH_AGENTC_REMOVE_SMARTCARD_KEY");
	case SSH_AGENTC_LOCK:
		return ("SSH_AGENTC_LOCK");
	case SSH_AGENTC_UNLOCK:
		return ("SSH_AGENTC_UNLOCK");
	case SSH_AGENTC_ADD_RSA_ID_CONSTRAINED:
		return ("SSH_AGENTC_ADD_RSA_ID_CONSTRAINED");
	case SSH2_AGENTC_ADD_ID_CONSTRAINED:
		return ("SSH2_AGENTC_ADD_ID_CONSTRAINED");
	case SSH_AGENTC_ADD_SMARTCARD_KEY_CONSTRAINED:
		return ("SSH_AGENTC_ADD_SMARTCARD_KEY_CONSTRAINED");
	case SSH_AGENTC_EXTENSION:
		return ("SSH_AGENTC_EXTENSION");
	case SSH2_AGENT_FAILURE:
		return ("SSH2_AGENT_FAILURE");
	case SSH_COM_AGENT2_FAILURE:
		return ("SSH_COM_AGENT2_FAILURE");
	default:
		return ("???");
	}
}
