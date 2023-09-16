#if !defined(_SYS_VKEY_H)
#define _SYS_VKEY_H

#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/types.h>
#include <sys/uio.h>

// #include <dev/vkeyvar.h>

// TEMPORARY
struct vkey_info {
uint32_t vkey_major;
uint32_t vkey_minor;
};

struct vkey_cmd {
/* input */
uint vkey_flags;
uint8_t vkey_cmd;
struct iovec vkey_in[4];
/* output */
uint8_t vkey_reply;
size_t vkey_rlen;
/* input + output */
struct iovec vkey_out[4];
};

#define	VKEYIOC_GET_INFO	_IOR('v', 1, struct vkey_info)
#define	VKEYIOC_CMD		_IOWR('v', 1, struct vkey_cmd)

#endif /* _SYS_VKEY_H */
