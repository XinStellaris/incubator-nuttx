/****************************************************************************
 * sched/init/nx_start.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/compiler.h>
#include <nuttx/sched.h>
#include <nuttx/fs/fs.h>
#include <nuttx/net/net.h>
#include <nuttx/mm/iob.h>
#include <nuttx/mm/kmap.h>
#include <nuttx/mm/mm.h>
#include <nuttx/kmalloc.h>
#include <nuttx/pgalloc.h>
#include <nuttx/sched_note.h>
#include <nuttx/trace.h>
#include <nuttx/binfmt/binfmt.h>
#include <nuttx/drivers/drivers.h>
#include <nuttx/init.h>

#include "sched/sched.h"
#include "signal/signal.h"
#include "semaphore/semaphore.h"
#include "mqueue/mqueue.h"
#include "mqueue/msg.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "irq/irq.h"
#include "group/group.h"
#include "init/init.h"
#include "instrument/instrument.h"
#include "tls/tls.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* This set of all CPUs */

#define SCHED_ALL_CPUS           ((1 << CONFIG_SMP_NCPUS) - 1)

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Task Lists ***************************************************************/

/* The state of a task is indicated both by the task_state field of the TCB
 * and by a series of task lists.  All of these tasks lists are declared
 * below. Although it is not always necessary, most of these lists are
 * prioritized so that common list handling logic can be used (only the
 * g_readytorun, the g_pendingtasks, and the g_waitingforsemaphore lists
 * need to be prioritized).
 */

/* This is the list of all tasks that are ready to run.  This is a
 * prioritized list with head of the list holding the highest priority
 * (unassigned) task.  In the non-SMP case, the head of this list is the
 * currently active task and the tail of this list, the lowest priority
 * task, is always the IDLE task.
 */

dq_queue_t g_readytorun;

/* In order to support SMP, the function of the g_readytorun list changes,
 * The g_readytorun is still used but in the SMP case it will contain only:
 *
 *  - Only tasks/threads that are eligible to run, but not currently running,
 *    and
 *  - Tasks/threads that have not been assigned to a CPU.
 *
 * Otherwise, the TCB will be retained in an assigned task list,
 * g_assignedtasks.  As its name suggests, on 'g_assignedtasks queue for CPU
 * 'n' would contain only tasks/threads that are assigned to CPU 'n'.  Tasks/
 * threads would be assigned a particular CPU by one of two mechanisms:
 *
 *  - (Semi-)permanently through an RTOS interfaces such as
 *    pthread_attr_setaffinity(), or
 *  - Temporarily through scheduling logic when a previously unassigned task
 *    is made to run.
 *
 * Tasks/threads that are assigned to a CPU via an interface like
 * pthread_attr_setaffinity() would never go into the g_readytorun list, but
 * would only go into the g_assignedtasks[n] list for the CPU 'n' to which
 * the thread has been assigned.  Hence, the g_readytorun list would hold
 * only unassigned tasks/threads.
 *
 * Like the g_readytorun list in in non-SMP case, each g_assignedtask[] list
 * is prioritized:  The head of the list is the currently active task on this
 * CPU.  Tasks after the active task are ready-to-run and assigned to this
 * CPU. The tail of this assigned task list, the lowest priority task, is
 * always the CPU's IDLE task.
 */

#ifdef CONFIG_SMP
dq_queue_t g_assignedtasks[CONFIG_SMP_NCPUS];
#endif

/* g_running_tasks[] holds a references to the running task for each cpu.
 * It is valid only when up_interrupt_context() returns true.
 */

FAR struct tcb_s *g_running_tasks[CONFIG_SMP_NCPUS];

/* This is the list of all tasks that are ready-to-run, but cannot be placed
 * in the g_readytorun list because:  (1) They are higher priority than the
 * currently active task at the head of the g_readytorun list, and (2) the
 * currently active task has disabled pre-emption.
 */

dq_queue_t g_pendingtasks;

/* This is the list of all tasks that are blocked waiting for a signal */

dq_queue_t g_waitingforsignal;

#ifdef CONFIG_LEGACY_PAGING
/* This is the list of all tasks that are blocking waiting for a page fill */

dq_queue_t g_waitingforfill;
#endif

#ifdef CONFIG_SIG_SIGSTOP_ACTION
/* This is the list of all tasks that have been stopped
 * via SIGSTOP or SIGTSTP
 */

dq_queue_t g_stoppedtasks;
#endif

/* This list of all tasks that have been initialized, but not yet
 * activated. NOTE:  This is the only list that is not prioritized.
 */

dq_queue_t g_inactivetasks;

/* This is the value of the last process ID assigned to a task */

volatile pid_t g_lastpid;

/* The following hash table is used for two things:
 *
 * 1. This hash table greatly speeds the determination of a new unique
 *    process ID for a task, and
 * 2. Is used to quickly map a process ID into a TCB.
 */

FAR struct tcb_s **g_pidhash;
volatile int g_npidhash;

/* This is a table of task lists.  This table is indexed by the task state
 * enumeration type (tstate_t) and provides a pointer to the associated
 * static task list (if there is one) as well as a set of attribute flags
 * indicating properties of the list, for example, if the list is an
 * ordered list or not.
 */

const struct tasklist_s g_tasklisttable[NUM_TASK_STATES] =
{
  {                                              /* TSTATE_TASK_INVALID */
    NULL,
    0
  },
  {                                              /* TSTATE_TASK_PENDING */
    &g_pendingtasks,
    TLIST_ATTR_PRIORITIZED
  },
#ifdef CONFIG_SMP
  {                                              /* TSTATE_TASK_READYTORUN */
    &g_readytorun,
    TLIST_ATTR_PRIORITIZED
  },
  {                                              /* TSTATE_TASK_ASSIGNED */
    g_assignedtasks,
    TLIST_ATTR_PRIORITIZED | TLIST_ATTR_INDEXED | TLIST_ATTR_RUNNABLE
  },
  {                                              /* TSTATE_TASK_RUNNING */
    g_assignedtasks,
    TLIST_ATTR_PRIORITIZED | TLIST_ATTR_INDEXED | TLIST_ATTR_RUNNABLE
  },
#else
  {                                              /* TSTATE_TASK_READYTORUN */
    &g_readytorun,
    TLIST_ATTR_PRIORITIZED | TLIST_ATTR_RUNNABLE
  },
  {                                              /* TSTATE_TASK_RUNNING */
    &g_readytorun,
    TLIST_ATTR_PRIORITIZED | TLIST_ATTR_RUNNABLE
  },
#endif
  {                                              /* TSTATE_TASK_INACTIVE */
    &g_inactivetasks,
    0
  },
  {                                              /* TSTATE_WAIT_SEM */
    (FAR void *)offsetof(sem_t, waitlist),
    TLIST_ATTR_PRIORITIZED | TLIST_ATTR_OFFSET
  },
  {                                              /* TSTATE_WAIT_SIG */
    &g_waitingforsignal,
    0
  }
#ifndef CONFIG_DISABLE_MQUEUE
  ,
  {                                              /* TSTATE_WAIT_MQNOTEMPTY */
    (FAR void *)offsetof(struct mqueue_inode_s, cmn.waitfornotempty),
    TLIST_ATTR_PRIORITIZED | TLIST_ATTR_OFFSET
  },
  {                                              /* TSTATE_WAIT_MQNOTFULL */
    (FAR void *)offsetof(struct mqueue_inode_s, cmn.waitfornotfull),
    TLIST_ATTR_PRIORITIZED | TLIST_ATTR_OFFSET
  }
#endif
#ifdef CONFIG_LEGACY_PAGING
  ,
  {                                              /* TSTATE_WAIT_PAGEFILL */
    &g_waitingforfill,
    TLIST_ATTR_PRIORITIZED
  }
#endif
#ifdef CONFIG_SIG_SIGSTOP_ACTION
  ,
  {                                              /* TSTATE_TASK_STOPPED */
    &g_stoppedtasks,
    0                                            /* See tcb->prev_state */
  },
#endif
};

/* This is the current initialization state.  The level of initialization
 * is only important early in the start-up sequence when certain OS or
 * hardware resources may not yet be available to the kernel logic.
 */

uint8_t g_nx_initstate;  /* See enum nx_initstate_e */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* This is an array of task control block (TCB) for the IDLE thread of each
 * CPU.  For the non-SMP case, this is a a single TCB; For the SMP case,
 * there is one TCB per CPU.  NOTE: The system boots on CPU0 into the IDLE
 * task.  The IDLE task later starts the other CPUs and spawns the user
 * initialization task.  That user initialization task is responsible for
 * bringing up the rest of the system.
 */

static struct task_tcb_s g_idletcb[CONFIG_SMP_NCPUS];

/* This is the name of the idle task */

#if CONFIG_TASK_NAME_SIZE <= 0 || !defined(CONFIG_SMP)
#  ifdef CONFIG_SMP
static const char g_idlename[] = "CPU_Idle";
#  else
static const char g_idlename[] = "Idle_Task";
#  endif
#endif

/* This is IDLE threads argument list.  NOTE: Normally the argument
 * list is created on the stack prior to starting the task.  We have to
 * do things little differently here for the IDLE tasks.
 */

static FAR char *g_idleargv[CONFIG_SMP_NCPUS][2];

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nx_start
 *
 * Description:
 *   This function is called to initialize the operating system and to spawn
 *   the user initialization thread of execution.  This is the initial entry
 *   point into NuttX.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Does not return.
 *
 ****************************************************************************/

void nx_start(void)
{
  int i;

  sinfo("Entry\n");

  /* Boot up is complete */

  g_nx_initstate = OSINIT_BOOT;

  /* Initialize RTOS Data ***************************************************/

  sched_trace_begin();

  /* Initialize the IDLE task TCB *******************************************/

  for (i = 0; i < CONFIG_SMP_NCPUS; i++)
    {
      FAR dq_queue_t *tasklist;

      /* Initialize a TCB for this thread of execution.  NOTE:  The default
       * value for most components of the g_idletcb are zero.  The entire
       * structure is set to zero.  Then only the (potentially) non-zero
       * elements are initialized. NOTE:  The idle task is the only task in
       * that has pid == 0 and sched_priority == 0.
       */

      memset((void *)&g_idletcb[i], 0, sizeof(struct task_tcb_s));
      g_idletcb[i].cmn.pid        = i;
      g_idletcb[i].cmn.task_state = TSTATE_TASK_RUNNING;

      /* Set the entry point.  This is only for debug purposes.  NOTE: that
       * the start_t entry point is not saved.  That is acceptable, however,
       * because it can be used only for restarting a task: The IDLE task
       * cannot be restarted.
       */

#ifdef CONFIG_SMP
      if (i > 0)
        {
          g_idletcb[i].cmn.start      = nx_idle_trampoline;
          g_idletcb[i].cmn.entry.main = (main_t)nx_idle_trampoline;
        }
      else
#endif
        {
          g_idletcb[i].cmn.start      = nx_start;
          g_idletcb[i].cmn.entry.main = (main_t)nx_start;
        }

      /* Set the task flags to indicate that this is a kernel thread and, if
       * configured for SMP, that this task is locked to this CPU.
       */

#ifdef CONFIG_SMP
      g_idletcb[i].cmn.flags = (TCB_FLAG_TTYPE_KERNEL | TCB_FLAG_CPU_LOCKED);
      g_idletcb[i].cmn.cpu   = i;

      /* Set the affinity mask to allow the thread to run on all CPUs.  No,
       * this IDLE thread can only run on its assigned CPU.  That is
       * enforced by the TCB_FLAG_CPU_LOCKED which overrides the affinity
       * mask.  This is essential because all tasks inherit the affinity
       * mask from their parent and, ultimately, the parent of all tasks is
       * the IDLE task.
       */

      g_idletcb[i].cmn.affinity =
        (cpu_set_t)(CONFIG_SMP_DEFAULT_CPUSET & SCHED_ALL_CPUS);
#else
      g_idletcb[i].cmn.flags = TCB_FLAG_TTYPE_KERNEL;
#endif

#if CONFIG_TASK_NAME_SIZE > 0
      /* Set the IDLE task name */

#  ifdef CONFIG_SMP
      snprintf(g_idletcb[i].cmn.name, CONFIG_TASK_NAME_SIZE, "CPU%d IDLE",
               i);
#  else
      strlcpy(g_idletcb[i].cmn.name, g_idlename, CONFIG_TASK_NAME_SIZE);
#  endif

      /* Configure the task name in the argument list.  The IDLE task does
       * not really have an argument list, but this name is still useful
       * for things like the NSH PS command.
       *
       * In the kernel mode build, the arguments are saved on the task's
       * stack and there is no support that yet.
       */

      g_idleargv[i][0] = g_idletcb[i].cmn.name;
#else
      g_idleargv[i][0] = (FAR char *)g_idlename;
#endif /* CONFIG_TASK_NAME_SIZE */

      /* Then add the idle task's TCB to the head of the current ready to
       * run list.
       */

#ifdef CONFIG_SMP
      tasklist = TLIST_HEAD(&g_idletcb[i].cmn, i);
#else
      tasklist = TLIST_HEAD(&g_idletcb[i].cmn);
#endif
      dq_addfirst((FAR dq_entry_t *)&g_idletcb[i], tasklist);

      /* Mark the idle task as the running task */

      g_running_tasks[i] = &g_idletcb[i].cmn;
    }

  /* Task lists are initialized */

  g_nx_initstate = OSINIT_TASKLISTS;

  /* Initialize RTOS facilities *********************************************/

  /* Initialize the semaphore facility.  This has to be done very early
   * because many subsystems depend upon fully functional semaphores.
   */

  nxsem_initialize();

#if defined(MM_KERNEL_USRHEAP_INIT) || defined(CONFIG_MM_KERNEL_HEAP) || \
    defined(CONFIG_MM_PGALLOC)
  /* Initialize the memory manager */

    {
      FAR void *heap_start;
      size_t heap_size;

#ifdef MM_KERNEL_USRHEAP_INIT
      /* Get the user-mode heap from the platform specific code and configure
       * the user-mode memory allocator.
       */

      up_allocate_heap(&heap_start, &heap_size);
      kumm_initialize(heap_start, heap_size);
#endif

#ifdef CONFIG_MM_KERNEL_HEAP
      /* Get the kernel-mode heap from the platform specific code and
       * configure the kernel-mode memory allocator.
       */

      up_allocate_kheap(&heap_start, &heap_size);
      kmm_initialize(heap_start, heap_size);
#endif

#ifdef CONFIG_MM_PGALLOC
      /* If there is a page allocator in the configuration, then get the page
       * heap information from the platform-specific code and configure the
       * page allocator.
       */

      up_allocate_pgheap(&heap_start, &heap_size);
      mm_pginitialize(heap_start, heap_size);
#endif
    }
#endif

#ifdef CONFIG_MM_KMAP
  /* Initialize the kernel dynamic mapping module */

  kmm_map_initialize();
#endif

#ifdef CONFIG_ARCH_HAVE_EXTRA_HEAPS
  /* Initialize any extra heap. */

  up_extraheaps_init();
#endif

#ifdef CONFIG_MM_IOB
  /* Initialize IO buffering */

  iob_initialize();
#endif

  /* Initialize the logic that determine unique process IDs. */

  g_npidhash = 4;
  while (g_npidhash <= CONFIG_SMP_NCPUS)
    {
      g_npidhash <<= 1;
    }

  g_pidhash = kmm_zalloc(sizeof(*g_pidhash) * g_npidhash);
  DEBUGASSERT(g_pidhash);

  /* IDLE Group Initialization **********************************************/

  for (i = 0; i < CONFIG_SMP_NCPUS; i++)
    {
      int hashndx;

      /* Assign the process ID(s) of ZERO to the idle task(s) */

      hashndx            = PIDHASH(i);
      g_pidhash[hashndx] = &g_idletcb[i].cmn;

      /* Allocate the IDLE group */

      DEBUGVERIFY(group_allocate(&g_idletcb[i], g_idletcb[i].cmn.flags));
      g_idletcb[i].cmn.group->tg_info->argv = &g_idleargv[i][0];

#ifdef CONFIG_SMP
      /* Create a stack for all CPU IDLE threads (except CPU0 which already
       * has a stack).
       */

      if (i > 0)
        {
          DEBUGVERIFY(up_cpu_idlestack(i, &g_idletcb[i].cmn,
                                       CONFIG_IDLETHREAD_STACKSIZE));
        }
#endif

      /* Initialize the processor-specific portion of the TCB */

      up_initial_state(&g_idletcb[i].cmn);

      /* Initialize the thread local storage */

      tls_init_info(&g_idletcb[i].cmn);

      /* Complete initialization of the IDLE group.  Suppress retention
       * of child status in the IDLE group.
       */

      group_initialize(&g_idletcb[i]);
      g_idletcb[i].cmn.group->tg_flags = GROUP_FLAG_NOCLDWAIT |
                                         GROUP_FLAG_PRIVILEGED;
    }

  g_lastpid = CONFIG_SMP_NCPUS - 1;

  /* The memory manager is available */

  g_nx_initstate = OSINIT_MEMORY;

  /* Initialize tasking data structures */

  task_initialize();

  /* Disables context switching because we need take the memory manager
   * semaphore on this CPU so that it will not be available on the other
   * CPUs until we have finished initialization.
   */

  sched_lock();

  /* Initialize the instrument function */

  instrument_initialize();

  /* Initialize the file system (needed to support device drivers) */

  fs_initialize();

  /* Initialize the interrupt handling subsystem (if included) */

  irq_initialize();

  /* Initialize the POSIX timer facility (if included in the link) */

  clock_initialize();

#ifndef CONFIG_DISABLE_POSIX_TIMERS
  timer_initialize();
#endif

  /* Initialize the signal facility (if in link) */

  nxsig_initialize();

#if !defined(CONFIG_DISABLE_MQUEUE) || !defined(CONFIG_DISABLE_MQUEUE_SYSV)
  /* Initialize the named message queue facility (if in link) */

  nxmq_initialize();
#endif

#ifndef CONFIG_DISABLE_MQUEUE_SYSV
  /* Initialize the System V message queue facility (if in link) */

  nxmsg_initialize();
#endif

#ifdef CONFIG_NET
  /* Initialize the networking system */

  net_initialize();
#endif

#ifndef CONFIG_BINFMT_DISABLE
  /* Initialize the binfmt system */

  binfmt_initialize();
#endif

  /* Initialize Hardware Facilities *****************************************/

  /* The processor specific details of running the operating system
   * will be handled here.  Such things as setting up interrupt
   * service routines and starting the clock are some of the things
   * that are different for each  processor and hardware platform.
   */

  up_initialize();

  /* Initialize common drivers */

  drivers_initialize();

#ifdef CONFIG_BOARD_EARLY_INITIALIZE
  /* Call the board-specific up_initialize() extension to support
   * early initialization of board-specific drivers and resources
   * that cannot wait until board_late_initialize.
   */

  board_early_initialize();
#endif

  /* Hardware resources are now available */

  g_nx_initstate = OSINIT_HARDWARE;

  /* Setup for Multi-Tasking ************************************************/

  /* Announce that the CPU0 IDLE task has started */

  sched_note_start(&g_idletcb[0].cmn);

  /* Initialize stdio for the IDLE task of each CPU */

  for (i = 0; i < CONFIG_SMP_NCPUS; i++)
    {
      if (i > 0)
        {
          /* Clone stdout, stderr, stdin from the CPU0 IDLE task. */

          DEBUGVERIFY(group_setuptaskfiles(&g_idletcb[i], NULL, true));
        }
      else
        {
          /* Create stdout, stderr, stdin on the CPU0 IDLE task.  These
           * will be inherited by all of the threads created by the CPU0
           * IDLE task.
           */

          DEBUGVERIFY(group_setupidlefiles());
        }
    }

#ifdef CONFIG_SMP
  /* Start all CPUs *********************************************************/

  /* A few basic sanity checks */

  DEBUGASSERT(this_cpu() == 0);

  /* Then start the other CPUs */

  DEBUGVERIFY(nx_smp_start());

#endif /* CONFIG_SMP */

  /* Bring Up the System ****************************************************/

  /* The OS is fully initialized and we are beginning multi-tasking */

  g_nx_initstate = OSINIT_OSREADY;

  /* Create initial tasks and bring-up the system */

  DEBUGVERIFY(nx_bringup());

  /* Enter to idleloop */

  g_nx_initstate = OSINIT_IDLELOOP;

  /* Let other threads have access to the memory manager */

  sched_trace_end();
  sched_unlock();

  /* The IDLE Loop **********************************************************/

  /* When control is return to this point, the system is idle. */

  sinfo("CPU0: Beginning Idle Loop\n");
#ifndef CONFIG_DISABLE_IDLE_LOOP
  for (; ; )
    {
      /* Perform any processor-specific idle state operations */

      up_idle();
    }
#endif
}
