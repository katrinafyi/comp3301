#if !defined(_SYS_ADD2_H_)
#define _SYS_ADD2_H_

#include <sys/types.h>

#define	ADD2_MODE_ADD		1
#define	ADD2_MODE_SUBTRACT	2

int add2(uint mode, uint a, uint b, uint *result);

#endif	/* !_SYS_ADD2_H_ */
