/*
 * QEMU HAX support
 *
 * Copyright IBM, Corp. 2008
 *           Red Hat, Inc. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Glauber Costa     <gcosta@redhat.com>
 *
 * Copyright (c) 2011 Intel Corporation
 *  Written by:
 *  Jiang Yunhong<yunhong.jiang@intel.com>
 *  Xin Xiaohui<xiaohui.xin@intel.com>
 *  Zhang Xiantao<xiantao.zhang@intel.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "accel/accel-cpu-ops.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "qemu/guest-random.h"

#include "hax-accel-ops.h"

static void *hax_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int r;

    rcu_register_thread();
    bql_lock();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;
    hax_init_vcpu(cpu);
#ifdef _WIN32
    cpu->accel->hThread = qemu_thread_get_handle(cpu->thread);
#endif
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        qemu_process_cpu_events(cpu);
#ifdef _WIN32
        /* Eat dummy APC queued by hax_kick_vcpu_thread(). */
        SleepEx(0, TRUE);
#endif

        if (cpu_can_run(cpu)) {
            r = hax_smp_cpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }
    } while (!cpu->unplug || cpu_can_run(cpu));
    hax_vcpu_destroy(cpu);
    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

static void hax_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/HAX",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, hax_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static bool hax_vcpu_thread_is_idle(CPUState *cpu)
{
    return !cpu->interrupt_request;
}

static void hax_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = hax_start_vcpu_thread;
    ops->kick_vcpu_thread = hax_kick_vcpu_thread;
    ops->cpu_thread_is_idle = hax_vcpu_thread_is_idle;
    ops->handle_interrupt = generic_handle_interrupt;

    ops->synchronize_post_reset = hax_cpu_synchronize_post_reset;
    ops->synchronize_post_init = hax_cpu_synchronize_post_init;
    ops->synchronize_state = hax_cpu_synchronize_state;
    ops->synchronize_pre_loadvm = hax_cpu_synchronize_pre_loadvm;
}

static const TypeInfo hax_accel_ops_type = {
    .name = ACCEL_OPS_NAME("hax"),

    .parent = TYPE_ACCEL_OPS,
    .class_init = hax_accel_ops_class_init,
    .abstract = true,
};

static void hax_accel_ops_register_types(void)
{
    type_register_static(&hax_accel_ops_type);
}
type_init(hax_accel_ops_register_types);
