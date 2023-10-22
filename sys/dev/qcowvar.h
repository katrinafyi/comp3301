
/*
 * Copyright (c) 2023 The University of Queensland
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

#ifndef _DEV_QCOWVAR_H_
#define _DEV_QCOWVAR_H_

/*
 * QCOW2 file format
 */

struct qcow2_file_header {
	uint32_t		magic;
#define QCOW2_MAGIC			0x514649fb /* "QFI\xfb" */
	uint32_t		version;
#define QCOW2_VERSION_3			3
	uint64_t		backing_file_offset;
	uint32_t		backing_file_size;
#define QCOW2_FILE_SIZE_MAX		1023
	uint32_t		cluster_bits;
#define QCOW2_CLUSTER_BITS_MIN		9
#define QCOW2_CLUSTER_BITS_MAX		21
	uint64_t		size;
	uint32_t		crypt_method;
#define QCOW2_CRYPT_METHOD_NONE		0
#define QCOW2_CRYPT_METHOD_AES		1
	uint32_t		l1_num_entries;
	uint64_t		l1_table_offset;
	uint64_t		refcount_table_offset;
	uint32_t		refcount_table_clusters;
	uint32_t		nb_snapshots;
	uint32_t		snapshots_offset;

	/* v3 bits here */
	uint64_t		incompatible_features;
	uint64_t		compatible_features;
	uint64_t		autoclear_features;
	uint32_t		refcount_order;
	uint32_t		header_length;

	// although a v3 header is _at least_ 104 bytes large, we don't
	// really care about anything beyond the 104 bytes.
};

#define QCOW2_FEAT_DIRTY		(1ULL << 0)
#define QCOW2_FEAT_CORRUPT		(1ULL << 1)

struct qcow2_l1_entry {
	// reserved lower bytes are within offset.
	uint64_t val;
#define QCOW2_L1E_OFFSET_MASK ((1ULL << 63) - 1)
#define QCOW2_L1E_BIT_MASK (1ULL << 63)
};

struct qcow2_l2_entry {
	uint64_t val;
};

/*
 * ioctl interface
 */

struct qcow_attach {
	char			*qc_file;
	unsigned int		 qc_readonly;
	unsigned int		 qc_secbits;
	unsigned int		 qc_secbits_phys;
};

#define QCOW_SECBITS_DEFAULT	 0
#define QCOW_SECBITS_MIN	 9	/* 512 */
#define QCOW_SECBITS_MAX	 16	/* 64k */

struct qcow_fname {
	char			 qc_name[1024];
};

#define QCOWIOCATTACH		_IOW('Q', 1, struct qcow_attach)
#define QCOWIOCDETACH		_IOW('Q', 2, int)
#define QCOWIOCFNAME		_IOR('Q', 3, struct qcow_fname)
#define QCOWIOCSTAT		_IOR('Q', 4, struct stat)

#endif /* _DEV_QCOWVAR_H_H */
