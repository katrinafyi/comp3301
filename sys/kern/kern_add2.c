/*
 * COMP3301 example syscall: add2
 */

#include <sys/param.h>

#include <sys/types.h>
#include <sys/errno.h>

#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/syscall.h>

/* These are required for sys/syscallargs.h */
#include <sys/socket.h>
#include <sys/mount.h>

#include <sys/syscallargs.h>

#include <sys/add2.h>

int
sys_add2(struct proc *p, void *v, register_t *retval)
{
	struct sys_add2_args /* {
		syscallarg(uint) mode;
		syscallarg(uint) a;
		syscallarg(uint) b;
		syscallarg(uint*) result;
	} */	*uap = v;
	uint mode 	= SCARG(uap, mode);
	uint a 		= SCARG(uap, a);
	uint b 		= SCARG(uap, b);
	uint *result 	= (uint *)SCARG(uap, result);

  if (!(mode == 0 || mode == 1)) {
    return EINVAL;
  }

  uint kresult = mode == 0 ? a + b : a - b;
  return copyout(&kresult, result, sizeof(kresult));
}
