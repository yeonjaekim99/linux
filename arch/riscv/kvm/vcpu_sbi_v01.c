// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *     Atish Patra <atish.patra@wdc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/kvm_vcpu_timer.h>
#include <asm/kvm_vcpu_sbi.h>

static void kvm_sbi_system_shutdown(struct kvm_vcpu *vcpu,
				    struct kvm_run *run, u32 type)
{
	unsigned long i;
	struct kvm_vcpu *tmp;

	kvm_for_each_vcpu(i, tmp, vcpu->kvm)
		tmp->arch.power_off = true;
	kvm_make_all_cpus_request(vcpu->kvm, KVM_REQ_SLEEP);

	memset(&run->system_event, 0, sizeof(run->system_event));
	run->system_event.type = type;
	run->exit_reason = KVM_EXIT_SYSTEM_EVENT;
}

static int kvm_sbi_ext_v01_handler(struct kvm_vcpu *vcpu, struct kvm_run *run,
				      unsigned long *out_val,
				      struct kvm_cpu_trap *utrap,
				      bool *exit)
{
	ulong hmask;
	int i, ret = 0;
	u64 next_cycle;
	struct kvm_vcpu *rvcpu;
	struct cpumask cm, hm;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;

	switch (cp->a7) {
	case SBI_EXT_0_1_CONSOLE_GETCHAR:
	case SBI_EXT_0_1_CONSOLE_PUTCHAR:
		/*
		 * The CONSOLE_GETCHAR/CONSOLE_PUTCHAR SBI calls cannot be
		 * handled in kernel so we forward these to user-space
		 */
		kvm_riscv_vcpu_sbi_forward(vcpu, run);
		*exit = true;
		break;
	case SBI_EXT_0_1_SET_TIMER:
#if __riscv_xlen == 32
		next_cycle = ((u64)cp->a1 << 32) | (u64)cp->a0;
#else
		next_cycle = (u64)cp->a0;
#endif
		ret = kvm_riscv_vcpu_timer_next_event(vcpu, next_cycle);
		break;
	case SBI_EXT_0_1_CLEAR_IPI:
		ret = kvm_riscv_vcpu_unset_interrupt(vcpu, IRQ_VS_SOFT);
		break;
	case SBI_EXT_0_1_SEND_IPI:
		if (cp->a0)
			hmask = kvm_riscv_vcpu_unpriv_read(vcpu, false, cp->a0,
							   utrap);
		else
			hmask = (1UL << atomic_read(&kvm->online_vcpus)) - 1;
		if (utrap->scause)
			break;

		for_each_set_bit(i, &hmask, BITS_PER_LONG) {
			rvcpu = kvm_get_vcpu_by_id(vcpu->kvm, i);
			ret = kvm_riscv_vcpu_set_interrupt(rvcpu, IRQ_VS_SOFT);
			if (ret < 0)
				break;
		}
		break;
	case SBI_EXT_0_1_SHUTDOWN:
		kvm_sbi_system_shutdown(vcpu, run, KVM_SYSTEM_EVENT_SHUTDOWN);
		*exit = true;
		break;
	case SBI_EXT_0_1_REMOTE_FENCE_I:
	case SBI_EXT_0_1_REMOTE_SFENCE_VMA:
	case SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID:
		if (cp->a0)
			hmask = kvm_riscv_vcpu_unpriv_read(vcpu, false, cp->a0,
							   utrap);
		else
			hmask = (1UL << atomic_read(&kvm->online_vcpus)) - 1;
		if (utrap->scause)
			break;

		cpumask_clear(&cm);
		for_each_set_bit(i, &hmask, BITS_PER_LONG) {
			rvcpu = kvm_get_vcpu_by_id(vcpu->kvm, i);
			if (rvcpu->cpu < 0)
				continue;
			cpumask_set_cpu(rvcpu->cpu, &cm);
		}
		riscv_cpuid_to_hartid_mask(&cm, &hm);
		if (cp->a7 == SBI_EXT_0_1_REMOTE_FENCE_I)
			ret = sbi_remote_fence_i(cpumask_bits(&hm));
		else if (cp->a7 == SBI_EXT_0_1_REMOTE_SFENCE_VMA)
			ret = sbi_remote_hfence_vvma(cpumask_bits(&hm),
						cp->a1, cp->a2);
		else
			ret = sbi_remote_hfence_vvma_asid(cpumask_bits(&hm),
						cp->a1, cp->a2, cp->a3);
		break;
	default:
		ret = -EINVAL;
		break;
	};

	return ret;
}

const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_v01 = {
	.extid_start = SBI_EXT_0_1_SET_TIMER,
	.extid_end = SBI_EXT_0_1_SHUTDOWN,
	.handler = kvm_sbi_ext_v01_handler,
};
