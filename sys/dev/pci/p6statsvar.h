#if !defined(_DEV_PCI_P6STATS_H)
#define _DEV_PCI_P6STATS_H

#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/types.h>

struct p6stats_output {
        uint64_t                 po_count;
        uint64_t                 po_sum;
        uint64_t                 po_mean;
        uint64_t                 po_median;
        uint8_t                  po_rsvd[8];
};

struct p6stats_calc {
        uint64_t                *pc_inputs;
        uint64_t                 pc_ninputs;
        struct p6stats_output   *pc_output;
};
#define P6STATS_IOC_CALC        _IOWR('6', 1, struct p6stats_calc)

#endif /* _DEV_PCI_P6STATS_H */
