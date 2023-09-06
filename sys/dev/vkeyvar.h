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

#if !defined(_DEV_VKEYVAR_H_)
#define	_DEV_VKEYVAR_H_

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/uio.h>

struct vkey_info_arg {
	uint32_t	vkey_major;
	uint32_t	vkey_minor;
};
#define	VKEYIOC_GET_INFO	_IOR('z', 0, struct vkey_info_arg)

struct vkey_cmd_arg {
	/* inputs */
#define	VKEY_FLAG_TRUNC_OK	(1<<0)
	uint		vkey_flags;

	uint8_t		vkey_cmd;
	struct iovec	vkey_in[4];

	/* outputs */
	uint8_t		vkey_reply;
	size_t		vkey_rlen;

	/* input + output */
	struct iovec	vkey_out[4];
};
#define	VKEYIOC_CMD		_IOWR('z', 1, struct vkey_cmd_arg)

#endif	/* !_DEV_VKEYVAR_H_ */
