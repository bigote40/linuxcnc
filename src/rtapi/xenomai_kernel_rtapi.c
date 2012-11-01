/** RTAPI is a library providing a uniform API for several real time
    operating systems.  As of ver 2.0, RTLinux and RTAI are supported.
*/
/********************************************************************
* Description:  rtai_rtapi.c
*               Realtime RTAPI implementation for the RTAI platform.
*
* Author: John Kasunich, Paul Corner
* License: GPL Version 2
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change: 
********************************************************************/

/** This file, 'rtai_rtapi.c', implements the realtime portion of the
    API for the RTAI platform.  The API is defined in rtapi.h, which
    includes documentation for all of the API functions.  The non-
    real-time portion of the API is implemented in rtai_ulapi.c (for
    the RTAI platform).  This implementation attempts to prevent
    kernel panics, 'oops'es, and other nasty stuff that can happen
    when writing and testing realtime code.  Wherever possible,
    common errors are detected and corrected before they can cause a
    crash.  This implementation also includes several /proc filesystem
    entries and numerous debugging print statements.
*/

/** Copyright (C) 2003 John Kasunich
                       <jmkasunich AT users DOT sourceforge DOT net>
    Copyright (C) 2003 Paul Corner
                       <paul_c AT users DOT sourceforge DOT net>
    This library is based on version 1.0, which was released into
    the public domain by its author, Fred Proctor.  Thanks Fred!
*/

/* This library is free software; you can redistribute it and/or
   modify it under the terms of version 2 of the GNU General Public
   License as published by the Free Software Foundation.
   This library is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA
*/

/** THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
    ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE
    TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of
    harming persons must have provisions for completely removing power
    from all motors, etc, before persons enter any danger area.  All
    machinery must be designed to comply with local and national safety
    codes, and the authors of this software can not, and do not, take
    any responsibility for such compliance.

    This code was written as part of the EMC HAL project.  For more
    information, go to www.linuxcnc.org.
*/

#include "config.h"	

#include <stdarg.h>		/* va_* */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>		/* replaces malloc.h in recent kernels */
#include <linux/ctype.h>	/* isdigit */
#include <linux/delay.h>	/* udelay */
#include <asm/uaccess.h>	/* copy_from_user() */
#include <asm/msr.h>		/* rdtscll() */
#include <linux/time.h>		/* timeval & do_gettimeofday() */

#ifndef LINUX_VERSION_CODE
#include <linux/version.h>
#endif
#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#endif

/* get inb(), outb(), ioperm() */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,4,17)
#include <asm/io.h>
#else
#include <sys/io.h>
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,0)
#include <linux/cpumask.h>	/* NR_CPUS, cpu_online() */
#endif

#include "vsnprintf.h"

#if defined(RTAPI_RTAI)
#include <rtai.h>
#include <rtai_sched.h>
#if RTAI > 2
#include <rtai_sem.h>
#endif
#include <rtai_shm.h>
#include <rtai_fifos.h>
#endif


#if defined(RTAPI_XENOMAI_KERNEL)
#include <native/heap.h>
#include <native/timer.h>
#include <native/task.h>
#include <native/intr.h>
#include <native/sem.h>
#include  <rtdk.h>
#include "xenomai_common.h"
#include "rtapi.h"		/* public RTAPI decls */
#include "rtapi_common.h"	/* shared realtime/nonrealtime stuff */

static RT_HEAP master_heap;
static RT_HEAP shmem_heap_array[RTAPI_MAX_SHMEMS + 1];        
static RT_INTR xeno_irq_array[RTAPI_MAX_IRQS+1];
static rthal_trap_handler_t old_trap_handler;
static int rtapi_trap_handler(unsigned event, rthal_pipeline_stage_t *stage, void *data);
#else
#include "rtapi.h"		/* public RTAPI decls */
#include "rtapi_common.h"	/* shared realtime/nonrealtime stuff */
#endif

/* resource data unique to kernel space */
static RT_TASK *ostask_array[RTAPI_MAX_TASKS + 1];

static void *shmem_addr_array[RTAPI_MAX_SHMEMS + 1];
#if defined(RTAPI_XENOMAI_KERNEL)
static RT_SEM ossem_array[RTAPI_MAX_SEMS + 1];
#endif
#if defined(RTAPI_RTAI)
static SEM ossem_array[RTAPI_MAX_SEMS + 1];
#endif

#define DEFAULT_MAX_DELAY 10000
static long int max_delay = DEFAULT_MAX_DELAY;

// Actual number of RTAI timer counts of the periodic timer
static unsigned long timer_counts; 

/* module parameters */

static int msg_level = RTAPI_MSG_DBG;	/* message printing level */
RTAPI_MP_INT(msg_level, "debug message level (default=1)");

/* other module information */
MODULE_AUTHOR("John Kasunich, Fred Proctor, Paul Corner & Michael Haberler");
MODULE_DESCRIPTION("Portable Real Time API for RTAI and Xenomai");
MODULE_LICENSE("GPL");


#include "rtapi_proc.h"		/* proc filesystem decls & code */

/* the following are internal functions that do the real work associated
   with deleting tasks, etc.  They do not check the mutex that protects
   the internal data structures.  When someone calls an rtapi_xxx_delete()
   function, the rtapi funct gets the mutex before calling one of these
   internal functions.  When internal code that already has the mutex
   needs to delete something, it calls these functions directly.
*/
static int module_delete(int module_id);
static int task_delete(int task_id);
static int shmem_delete(int shmem_id, int module_id);
static int sem_delete(int sem_id, int module_id);
#if defined(RTAPI_FIFO)
static int fifo_delete(int fifo_id, int module_id);
#endif
static int irq_delete(unsigned int irq_num);

/***********************************************************************
*                   INIT AND SHUTDOWN FUNCTIONS                        *
************************************************************************/

int init_module(void)
{
    int n;

    /* say hello */
    rtapi_print_msg(RTAPI_MSG_INFO, "RTAPI: Init\n");
    /* get master shared memory block from OS and save its address */
#if defined(RTAPI_XENOMAI_KERNEL)
    if ((n = rt_heap_create(&master_heap, MASTER_HEAP, 
			    sizeof(rtapi_data_t), H_SHARED)) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: ERROR: rt_heap_create() returns %d\n", n);
	return -EINVAL;
    }
    if ((n = rt_heap_alloc(&master_heap, 0, TM_INFINITE , (void **)&rtapi_data)) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: ERROR: rt_heap_alloc() returns %d\n", n);
	return -EINVAL;
    }
#endif
#if defined(RTAPI_RTAI)
    rtapi_data = rtai_kmalloc(RTAPI_KEY, sizeof(rtapi_data_t));
    if (rtapi_data == NULL) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "RTAPI: ERROR: could not open shared memory\n");
	return -ENOMEM;
    }
#endif
    /* perform a global init if needed */
    init_rtapi_data(rtapi_data);
    /* check revision code */
    if (rtapi_data->rev_code != rev_code) {
	/* mismatch - release master shared memory block */
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: ERROR: version mismatch %d vs %d\n", rtapi_data->rev_code, rev_code);
#if defined(RTAPI_RTAI)
	rtai_kfree(RTAPI_KEY);
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
	rt_heap_delete(&master_heap);
#endif
	return -EINVAL;
    }
    /* set up local pointers to global data */
    module_array = rtapi_data->module_array;
    task_array = rtapi_data->task_array;
    shmem_array = rtapi_data->shmem_array;
    sem_array = rtapi_data->sem_array;
    fifo_array = rtapi_data->fifo_array;
    irq_array = rtapi_data->irq_array;
    /* perform local init */
    for (n = 0; n <= RTAPI_MAX_TASKS; n++) {
	ostask_array[n] = NULL;
    }
    for (n = 0; n <= RTAPI_MAX_SHMEMS; n++) {
	shmem_addr_array[n] = NULL;
    }
    rtapi_data->timer_running = 0;
    rtapi_data->timer_period = 0;
    max_delay = DEFAULT_MAX_DELAY;

    /* rt_linux_use_fpu informs the scheduler that floating point arithmetic */
    /* operations will be used also by foreground Linux processes, i.e. the */
    /* Linux kernel itself (unlikely) and any of its processes. */

#if defined(RTAPI_RTAI)
    // FIXME unsure how this relates to Xenomai
    rt_linux_use_fpu(1);
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,0)
    /* on SMP machines, we want to put RT code on the last CPU */
    n = NR_CPUS-1;
    while ( ! cpu_online(n) ) {
	n--;
    }
    rtapi_data->rt_cpu = n;
#else
    /* old kernel, the SMP hooks aren't available, so use CPU 0 */
    rtapi_data->rt_cpu = 0;
#endif


#ifdef CONFIG_PROC_FS
    /* set up /proc/rtapi */
    if (proc_init() != 0) {
	rtapi_print_msg(RTAPI_MSG_WARN,
	    "RTAPI: WARNING: Could not activate /proc entries\n");
	proc_clean();
    }
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    old_trap_handler = rthal_trap_catch((rthal_trap_handler_t) rtapi_trap_handler);
#endif

    /* done */

    rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: magic=%x revcode=%x\n", rtapi_data->magic,rtapi_data->rev_code);
    rtapi_print_msg(RTAPI_MSG_INFO, "RTAPI: Init complete\n");
    return 0;
}

/* This cleanup code attempts to fix any messes left by modules
that fail to load properly, or fail to clean up after themselves */

void cleanup_module(void)
{
    int n;

    if (rtapi_data == NULL) {
	/* never got inited, nothing to do */
	return;
    }
    /* grab the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    rtapi_print_msg(RTAPI_MSG_INFO, "RTAPI: Exiting\n");

    /* clean up leftover modules (start at 1, we don't use ID 0 */
    for (n = 1; n <= RTAPI_MAX_MODULES; n++) {
	if (module_array[n].state == REALTIME) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
		"RTAPI: WARNING: module '%s' (ID: %02d) did not call rtapi_exit()\n",
		module_array[n].name, n);
	    module_delete(n);
	}
    }
    /* cleaning up modules should clean up everything, if not there has
       probably been an unrecoverable internal error.... */
    for (n = 1; n <= RTAPI_MAX_IRQS; n++) {
	if (irq_array[n].irq_num != 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"RTAPI: ERROR: interrupt handler %02d not deleted (IRQ %d)\n",
		n, irq_array[n].irq_num);
	    /* probably un-recoverable, but try anyway */
	    irq_delete(irq_array[n].irq_num);
	}
    }
    for (n = 1; n <= RTAPI_MAX_FIFOS; n++) {
	if (fifo_array[n].state != UNUSED) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"RTAPI: ERROR: FIFO %02d not deleted\n", n);
	}
    }
    for (n = 1; n <= RTAPI_MAX_SEMS; n++) {
	while (sem_array[n].users > 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"RTAPI: ERROR: semaphore %02d not deleted\n", n);
	}
    }
    for (n = 1; n <= RTAPI_MAX_SHMEMS; n++) {
	if (shmem_array[n].rtusers > 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"RTAPI: ERROR: shared memory block %02d not deleted\n", n);
	}
    }
    for (n = 1; n <= RTAPI_MAX_TASKS; n++) {
	if (task_array[n].state != EMPTY) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"RTAPI: ERROR: task %02d not deleted\n", n);
	    /* probably un-recoverable, but try anyway */
	    rtapi_task_pause(n);
	    task_delete(n);
	}
    }
    if (rtapi_data->timer_running != 0) {
#if defined(RTAPI_RTAI)
	stop_rt_timer();
	rt_free_timer();
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
	// nothing to do here
#endif
	rtapi_data->timer_period = 0;
	timer_counts = 0;
	rtapi_data->timer_running = 0;
	max_delay = DEFAULT_MAX_DELAY;
    }
    rtapi_mutex_give(&(rtapi_data->mutex));
#ifdef CONFIG_PROC_FS
    proc_clean();
#endif
    /* release master shared memory block */
#if defined(RTAPI_RTAI)
    rtai_kfree(RTAPI_KEY);
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    rt_heap_delete(&master_heap);
    rthal_trap_catch(old_trap_handler);
#endif
    rtapi_print_msg(RTAPI_MSG_INFO, "RTAPI: Exit complete\n");
    return;
}

/***********************************************************************
*                   GENERAL PURPOSE FUNCTIONS                          *
************************************************************************/

/* all RTAPI init is done when the rtapi kernel module
is insmoded.  The rtapi_init() and rtapi_exit() functions
simply register that another module is using the RTAPI.
For other RTOSes, things might be different, especially
if the RTOS does not use modules. */

int rtapi_init(const char *modname)
{
    int n, module_id;
    module_data *module;

    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: initing module %s\n", modname);
    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* find empty spot in module array */
    n = 1;
    while ((n <= RTAPI_MAX_MODULES) && (module_array[n].state != NO_MODULE)) {
	n++;
    }
    if (n > RTAPI_MAX_MODULES) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: ERROR: reached module limit %d\n",
	    n);
	return -EMFILE;
    }
    /* we have space for the module */
    module_id = n;
    module = &(module_array[n]);
    /* update module data */
    module->state = REALTIME;
    if (modname != NULL) {
	/* use name supplied by caller, truncating if needed */
	rtapi_snprintf(module->name, RTAPI_NAME_LEN, "%s", modname);
    } else {
	/* make up a name */
	rtapi_snprintf(module->name, RTAPI_NAME_LEN, "RTMOD%03d", module_id);
    }
    rtapi_data->rt_module_count++;
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: module '%s' loaded, ID: %d\n",
	module->name, module_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return module_id;
}

int rtapi_exit(int module_id)
{
    int retval;

    rtapi_mutex_get(&(rtapi_data->mutex));
    retval = module_delete(module_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return retval;
}

static int module_delete(int module_id)
{
    module_data *module;
    char name[RTAPI_NAME_LEN + 1];
    int n;

    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: module %d exiting\n", module_id);
    /* validate module ID */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	return -EINVAL;
    }
    /* point to the module's data */
    module = &(module_array[module_id]);
    /* check module status */
    if (module->state != REALTIME) {
	/* not an active realtime module */
	return -EINVAL;
    }
    /* clean up any mess left behind by the module */
    for (n = 1; n <= RTAPI_MAX_TASKS; n++) {
	if ((task_array[n].state != EMPTY)
	    && (task_array[n].owner == module_id)) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
		"RTAPI: WARNING: module '%s' failed to delete task %02d\n",
		module->name, n);
	    task_delete(n);
	}
    }
    for (n = 1; n <= RTAPI_MAX_SHMEMS; n++) {
	if (test_bit(module_id, shmem_array[n].bitmap)) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
		"RTAPI: WARNING: module '%s' failed to delete shmem %02d\n",
		module->name, n);
	    shmem_delete(n, module_id);
	}
    }
    for (n = 1; n <= RTAPI_MAX_SEMS; n++) {
	if (test_bit(module_id, sem_array[n].bitmap)) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
		"RTAPI: WARNING: module '%s' failed to delete sem %02d\n",
		module->name, n);
	    sem_delete(n, module_id);
	}
    }
#if defined(RTAPI_FIFO)
    for (n = 1; n <= RTAPI_MAX_FIFOS; n++) {
	if ((fifo_array[n].reader == module_id) ||
	    (fifo_array[n].writer == module_id)) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
		"RTAPI: WARNING: module '%s' failed to delete fifo %02d\n",
		module->name, n);
	    fifo_delete(n, module_id);
	}
    }
#endif
    for (n = 1; n <= RTAPI_MAX_IRQS; n++) {
	if (irq_array[n].owner == module_id) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
		"RTAPI: WARNING: module '%s' failed to delete handler for IRQ %d\n",
		module->name, irq_array[n].irq_num);
	    irq_delete(irq_array[n].irq_num);
	}
    }
    /* use snprintf() to do strncpy(), since we don't have string.h */
    rtapi_snprintf(name, RTAPI_NAME_LEN, "%s", module->name);
    /* update module data */
    module->state = NO_MODULE;
    module->name[0] = '\0';
    rtapi_data->rt_module_count--;
    if (rtapi_data->rt_module_count == 0) {
	if (rtapi_data->timer_running != 0) {

#if defined(RTAPI_RTAI)
	    stop_rt_timer();
	    rt_free_timer();
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
	// nothing to do here
#endif	    
	    rtapi_data->timer_period = 0;
	    timer_counts = 0;
	    max_delay = DEFAULT_MAX_DELAY;
	    rtapi_data->timer_running = 0;
	}
    }
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: module %d exited, name: '%s'\n",
	module_id, name);
    return 0;
}

int rtapi_snprintf(char *buf, unsigned long int size, const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    i = rtapi_vsnprintf(buf, size, fmt, args);
    va_end(args);
    return i;
}

#define RTPRINTBUFFERLEN 1024

void default_rtapi_msg_handler(msg_level_t level, const char *fmt, va_list ap) {
    char buf[RTPRINTBUFFERLEN];
    rtapi_vsnprintf(buf, RTPRINTBUFFERLEN, fmt, ap);

#if defined(RTAPI_RTAI)
    rt_printk(buf);
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    // FIXME unsure
    printk(buf);
#endif
}
static rtapi_msg_handler_t rtapi_msg_handler = default_rtapi_msg_handler;

rtapi_msg_handler_t rtapi_get_msg_handler(void) {
    return rtapi_msg_handler;
}

void rtapi_set_msg_handler(rtapi_msg_handler_t handler) {
    if(handler == NULL) rtapi_msg_handler = default_rtapi_msg_handler;
    else rtapi_msg_handler = handler;
}

void rtapi_print(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    rtapi_msg_handler(RTAPI_MSG_ALL, fmt, args);
    va_end(args);
}


void rtapi_print_msg(int level, const char *fmt, ...)
{
    va_list args;

    if ((level <= msg_level) && (msg_level != RTAPI_MSG_NONE)) {
	va_start(args, fmt);
	rtapi_msg_handler(level, fmt, args);
	va_end(args);
    }
}

int rtapi_set_msg_level(int level)
{
    if ((level < RTAPI_MSG_NONE) || (level > RTAPI_MSG_ALL)) {
	return -EINVAL;
    }
    msg_level = level;
    return 0;
}

int rtapi_get_msg_level(void)
{
    return msg_level;
}

/***********************************************************************
*                     CLOCK RELATED FUNCTIONS                          *
************************************************************************/

long int rtapi_clock_set_period(long int nsecs)
{
    RTIME counts, got_counts;

    if (nsecs == 0) {
	/* it's a query, not a command */
	return rtapi_data->timer_period;
    }
    if (rtapi_data->timer_running) {
	/* already started, can't restart */
	return -EINVAL;
    }
    /* limit period to 2 micro-seconds min, 1 second max */
    if ((nsecs < 2000) || (nsecs > 1000000000L)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "RTAPI: ERR: clock_set_period: %ld nsecs,  out of range\n",
	    nsecs);
	return -EINVAL;
    }
#if defined(RTAPI_RTAI)
    rt_set_periodic_mode();
    counts = nano2count((RTIME) nsecs);
    if(count2nano(counts) > nsecs) counts--;
    got_counts = start_rt_timer(counts);
    rtapi_data->timer_period = count2nano(got_counts);
    timer_counts = got_counts;
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    counts = rt_timer_ns2ticks((RTIME) nsecs);
    rt_timer_set_mode(counts);
    rtapi_data->timer_period = got_counts = rt_timer_ticks2ns(counts);
    timer_counts = got_counts;
#endif

    rtapi_print_msg(RTAPI_MSG_DBG,
	"RTAPI: clock_set_period requested: %ld  actual: %ld  counts requested: %d  actual: %d\n",
	nsecs, rtapi_data->timer_period, (int)counts, (int)got_counts);
    rtapi_data->timer_running = 1;
    max_delay = rtapi_data->timer_period / 4;
    return rtapi_data->timer_period;
}

long long int rtapi_get_time(void)
{
    //struct timeval tv;

    //AJ: commenting the following code out, as it seems on some systems it
    // really breaks
    
    /* call the kernel's internal implementation of gettimeofday() */
    /* unfortunately timeval has only usec, struct timespec would be
       better, it has nsec resolution.  Doing this right probably
       involves a number of ifdefs based on kernel version and such */
    /*do_gettimeofday(&tv);*/
    /* convert to nanoseconds */
    /*return (tv.tv_sec * 1000000000LL) + (tv.tv_usec * 1000L);*/
    
    //reverted to old code for now
    /* this is a monstrosity that seems to take several MICROSECONDS!!!
       on some boxes.  Why the RTAI folks even bothered I have no idea!
       If you have any need for speed at all use rtapi_get_clocks()!!
    */
#if defined(RTAPI_RTAI)
    return rt_get_cpu_time_ns();    
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    /* The value returned will represent a count of jiffies if the native  */
    /* skin is bound to a periodic time base (see CONFIG_XENO_OPT_NATIVE_PERIOD),  */
    /* or nanoseconds otherwise.  */
    return  rt_timer_read();
#endif
}

/* This returns a result in clocks instead of nS, and needs to be used
   with care around CPUs that change the clock speed to save power and
   other disgusting, non-realtime oriented behavior.  But at least it
   doesn't take a week every time you call it.
*/

long long int rtapi_get_clocks(void)
{
    long long int retval;

    rdtscll(retval);
    return retval;    
}

void rtapi_delay(long int nsec)
{
    if (nsec > max_delay) {
	nsec = max_delay;
    }
    udelay(nsec / 1000);
}

long int rtapi_delay_max(void)
{
    return max_delay;
}

/***********************************************************************
*                     TASK RELATED FUNCTIONS                           *
************************************************************************/

/* Priority functions.  RTAI uses 0 as the highest priority, as the
number increases, the actual priority of the task decreases. */

int rtapi_prio_highest(void)
{
#if defined(RTAPI_RTAI)
    return 0;
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    /* The base priority of the new task. This value must range from  */
    /* [0..99] (inclusive) where 0 is the lowest effective priority. */
    return 99;
#endif
}

int rtapi_prio_lowest(void)
{
#if defined(RTAPI_RTAI)
    /* RTAI has LOTS of different priorities - RT_LOWEST_PRIORITY is
       0x3FFFFFFF! I don't want such ugly numbers, and we only need a few
       levels, so we use 0xFFF (4095) instead */
    return 0xFFF;
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    return 0;
#endif
}

#if defined(RTAPI_RTAI)
int rtapi_prio_next_higher(int prio)
{
    /* return a valid priority for out of range arg */
    if (prio <= rtapi_prio_highest()) {
	return rtapi_prio_highest();
    }
    if (prio > rtapi_prio_lowest()) {
	return rtapi_prio_lowest();
    }

    /* return next higher priority for in-range arg */
    return prio - 1;
}
int rtapi_prio_next_lower(int prio)
{
    /* return a valid priority for out of range arg */
    if (prio >= rtapi_prio_lowest()) {
	return rtapi_prio_lowest();
    }
    if (prio < rtapi_prio_highest()) {
	return rtapi_prio_highest();
    }
    /* return next lower priority for in-range arg */
    return prio + 1;
}
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
int rtapi_prio_next_higher(int prio)
{
  /* return a valid priority for out of range arg */
  if (prio >= rtapi_prio_highest())
    return rtapi_prio_highest();
  if (prio <= rtapi_prio_lowest())
    return rtapi_prio_lowest();

  /* return next higher priority for in-range arg */
  return prio + 1;
}

int rtapi_prio_next_lower(int prio)
{
  /* return a valid priority for out of range arg */
  if (prio <= rtapi_prio_lowest())
    return rtapi_prio_lowest();
  if (prio >= rtapi_prio_highest())
    return rtapi_prio_highest();
  /* return next lower priority for in-range arg */
  return prio - 1;
}
#endif

#if defined(RTAPI_RTAI)
/* We define taskcode as taking a void pointer and returning void, but
   rtai wants it to take an int and return void.
   We solve this with a wrapper function that meets rtai's needs.
   The wrapper functions also properly deals with tasks that return.
   (Most tasks are infinite loops, and don't return.)
*/
static void wrapper(long task_id)
{
    task_data *task;

    /* point to the task data */
    task = &task_array[task_id];
    /* call the task function with the task argument */
    (task->taskcode) (task->arg);
    /* if the task ever returns, we record that fact */
    task->state = ENDED;
    /* and return to end the thread */
    return;
}
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
#define IP(x) ((x)->ip)
#elif defined(__i386__)
#define IP(x) ((x)->eip)
#else
#define IP(x) ((x)->rip)
#endif

#if defined(RTAPI_RTAI)
static int rtapi_trap_handler(int vec, int signo, struct pt_regs *regs,
        void *task) {
    int self = rtapi_task_self();
    rtapi_print_msg(RTAPI_MSG_ERR,
	"RTAPI: Task %d[%p]: Fault with vec=%d, signo=%d ip=%08lx.\n"
	"RTAPI: This fault may not be recoverable without rebooting.\n",
	self, task, vec, signo, IP(regs));
    rtapi_task_pause(self);
    return 0;
}
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
// not better than the builtin Xenomai handler, but at least
// hook into to rtapi_print
static int rtapi_trap_handler(unsigned event, rthal_pipeline_stage_t *stage, void *data)
{
    struct pt_regs *regs = data;
    xnthread_t *thread = xnpod_current_thread(); ;

    rtapi_print_msg(RTAPI_MSG_ERR, 
		    "RTAPI: trap event=%d thread=%s ip:%lx sp:%lx userpid=%d errcode=%d\n",
		    event, thread->name,
		    regs->ip, regs->sp, 
		    xnthread_user_pid(thread), thread->errcode);
    // forward to default Xenomai trap handler
    return ((rthal_trap_handler_t) old_trap_handler)(event, stage, data);
}
#endif

int rtapi_task_new(void (*taskcode) (void *), void *arg,
		   int prio, int owner, unsigned long int stacksize, int uses_fp,
		   char *name)
{
    int n;
    long task_id;
    int retval;
    task_data *task;

    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* validate owner */
    if ((owner < 1) || (owner > RTAPI_MAX_MODULES)) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    if (module_array[owner].state != REALTIME) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    /* find empty spot in task array */
    n = 1;
    while ((n <= RTAPI_MAX_TASKS) && (task_array[n].state != EMPTY)) {
	n++;
    }
    if (n > RTAPI_MAX_TASKS) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EMFILE;
    }
    /* we have space for the task */
    task_id = n;
    task = &(task_array[n]);
    /* check requested priority */

#if defined(RTAPI_RTAI)
    if ((prio < rtapi_prio_highest()) || (prio > rtapi_prio_lowest())) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    if ((prio < rtapi_prio_lowest()) || (prio > rtapi_prio_highest())) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
#endif

    /* get space for the OS's task data - this is around 900 bytes, */
    /* so we don't want to statically allocate it for unused tasks. */
    ostask_array[task_id] = kmalloc(sizeof(RT_TASK), GFP_USER);
    if (ostask_array[task_id] == NULL) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -ENOMEM;
    }
    task->taskcode = taskcode;
    task->arg = arg;
    /* call OS to initialize the task - use predetermined CPU */

#if defined(RTAPI_RTAI)
    retval = rt_task_init_cpuid(ostask_array[task_id], wrapper, task_id,
	 stacksize, prio, uses_fp, 0 /* signal */, rtapi_data->rt_cpu );
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    rtapi_print_msg(RTAPI_MSG_ERR, "rt_task_create %ld \"%s\" cpu=%d fpu=%d prio=%d\n", 
		    task_id, name, rtapi_data->rt_cpu, uses_fp, task->prio );

    retval = rt_task_create(ostask_array[task_id], name, stacksize, task->prio, 
			    (uses_fp ? T_FPU : 0) | T_CPU(rtapi_data->rt_cpu));
    if (retval) {
	rtapi_print_msg(RTAPI_MSG_ERR, "rt_task_create failed, rc = %d\n", retval );
    }
#endif

    if (retval != 0) {
	/* couldn't create task, free task data memory */
	kfree(ostask_array[task_id]);
	rtapi_mutex_give(&(rtapi_data->mutex));
	if (retval == ENOMEM) {
	    /* not enough space for stack */
	    return -ENOMEM;
	}
	/* unknown error */
	return -EINVAL;
    }

#if defined(RTAPI_RTAI)
    /* request to handle traps in the new task */
    {
    int v;
    for(v=0; v<HAL_NR_FAULTS; v++)
        rt_set_task_trap_handler(ostask_array[task_id], v, rtapi_trap_handler);
    }
#endif

    /* the task has been created, update data */
    task->state = PAUSED;
    task->prio = prio;
    task->owner = owner;
    task->taskcode = taskcode;
    rtapi_data->task_count++;
    /* announce the birth of a brand new baby task */
    rtapi_print_msg(RTAPI_MSG_DBG,
	"RTAPI: task %02ld installed by module %02d, priority %d, code: %p\n",
	task_id, task->owner, task->prio, taskcode);
    /* and return the ID to the proud parent */
    rtapi_mutex_give(&(rtapi_data->mutex));
    return task_id;
}

int rtapi_task_delete(int task_id)
{
    int retval;

    rtapi_mutex_get(&(rtapi_data->mutex));
    retval = task_delete(task_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return retval;
}

static int task_delete(int task_id)
{
    task_data *task;

    /* validate task ID */
    if ((task_id < 1) || (task_id > RTAPI_MAX_TASKS)) {
	return -EINVAL;
    }
    /* point to the task's data */
    task = &(task_array[task_id]);
    /* check task status */
    if (task->state == EMPTY) {
	/* nothing to delete */
	return -EINVAL;
    }
    if ((task->state == PERIODIC) || (task->state == FREERUN)) {
	/* task is running, need to stop it */
	rtapi_print_msg(RTAPI_MSG_WARN,
	    "RTAPI: WARNING: tried to delete task %02d while running\n",
	    task_id);
	rtapi_task_pause(task_id);
    }
    /* get rid of it */
    rt_task_delete(ostask_array[task_id]); // ok for both RTAI and Xenomai
    /* free kernel memory */
    kfree(ostask_array[task_id]);
    /* update data */
    task->state = EMPTY;
    task->prio = 0;
    task->owner = 0;
    task->taskcode = NULL;
    ostask_array[task_id] = NULL;
    rtapi_data->task_count--;
    /* if no more tasks, stop the timer */
    if (rtapi_data->task_count == 0) {
	if (rtapi_data->timer_running != 0) {

#if defined(RTAPI_RTAI)
	    stop_rt_timer();
	    rt_free_timer();
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
	    // nothing to do
#endif
	    rtapi_data->timer_period = 0;
	    max_delay = DEFAULT_MAX_DELAY;
	    rtapi_data->timer_running = 0;
	}
    }
    /* done */
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: task %02d deleted\n", task_id);
    return 0;
}

int rtapi_task_start(int task_id, unsigned long int period_nsec)
{
    int retval;
#if defined(RTAPI_RTAI)
    unsigned long int period_counts;
    unsigned long int quo;
#endif
    task_data *task;

    /* validate task ID */
    if ((task_id < 1) || (task_id > RTAPI_MAX_TASKS)) {
	return -EINVAL;
    }
    /* point to the task's data */
    task = &(task_array[task_id]);
    /* is task ready to be started? */
    if (task->state != PAUSED) {
	return -EINVAL;
    }
    /* can't start periodic tasks if timer isn't running */
    if ((rtapi_data->timer_running == 0) || (rtapi_data->timer_period == 0)) {
        rtapi_print_msg(RTAPI_MSG_ERR, 
                "RTAPI: could not start task: timer isn't running\n");
	return -EINVAL;
    }

#if defined(RTAPI_RTAI)
    period_counts = nano2count((RTIME)period_nsec);  
    quo = (period_counts + timer_counts / 2) / timer_counts;
    period_counts = quo * timer_counts;
    period_nsec = count2nano(period_counts);

    /* start the task */

    retval = rt_task_make_periodic(ostask_array[task_id],
	rt_get_time() + period_counts, period_counts);

    if (retval != 0) {
	return -EINVAL;
    }
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    // we get a single Unexpected realtime delay here it seems
    if ((retval = rt_task_set_periodic( ostask_array[task_id], TM_NOW, period_nsec)) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: rt_task_set_periodic() task_id %d periodns=%ld returns %d\n", 
			task_id, period_nsec, retval);
	return -EINVAL;
    }
    if ((retval = rt_task_start( ostask_array[task_id], task->taskcode, (void*)task->arg )) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: rt_task_start() task_id %d returns %d\n", 
			task_id, retval);
	return -EINVAL;
    }
#endif

    /* ok, task is started */
    task->state = PERIODIC;
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: start_task id: %02d\n", task_id);
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: period_nsec: %ld\n", period_nsec);
#if defined(RTAPI_RTAI)
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: count: %ld\n", period_counts);
#endif
    return retval;
}

#if defined(RTAPI_XENOMAI_KERNEL)
void rtapi_wait(void)
{
    unsigned long overruns;
    static int error_printed = 0;

    int result =  rt_task_wait_period(&overruns);
    switch (result) {
    case 0: // ok - no overruns;
	break;

    case -ETIMEDOUT: // release point was missed
	rtapi_data->rt_wait_error++;
	rtapi_data->rt_last_overrun = overruns;
	rtapi_data->rt_total_overruns += overruns;

	rtapi_print_msg(error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
			"RTAPI: ERROR: Unexpected realtime delay on task %d (%lu overruns)\n" 
			"This Message will only display once per session.\n"
			"Run the Latency Test and resolve before continuing.\n", 
			rtapi_task_self(), overruns);

	error_printed++;
	if(error_printed == 10)
	    rtapi_print_msg(error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
			    "RTAPI: (further messages will be suppressed)\n");
	break;

    case -EWOULDBLOCK:
	rtapi_print_msg(error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
			"RTAPI: ERROR: rt_task_wait_period() without previous rt_task_set_periodic()\n");
	error_printed++;
	break;

    case -EINTR:
	rtapi_print_msg(error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
			"RTAPI: ERROR: rt_task_unblock() called before release point\n");
	error_printed++;
	break;

    case -EPERM:
	rtapi_print_msg(error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
			"RTAPI: ERROR: cannot rt_task_wait_period() from this context\n");
	error_printed++;
	break;
    default:
	rtapi_print_msg(error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
			"RTAPI: ERROR: unknown error code %d\n", result);
	error_printed++;
	break;
    }
}
#endif

#if defined(RTAPI_RTAI)
void rtapi_wait(void)
{
    int result = rt_task_wait_period();
    if(result != 0) {
	static int error_printed = 0;
	if(error_printed < 10) {
#ifdef RTE_TMROVRN
	    if(result == RTE_TMROVRN) {
		rtapi_print_msg(
		    error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
		    "RTAPI: ERROR: Unexpected realtime delay on task %d\n" 
		    "This Message will only display once per session.\n"
		    "Run the Latency Test and resolve before continuing.\n", 
		    rtapi_task_self());
	    } else
#endif
#ifdef RTE_UNBLKD
		    if(result == RTE_UNBLKD) {
		rtapi_print_msg(
		    error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
		    "RTAPI: ERROR: rt_task_wait_period() returned RTE_UNBLKD (%d).\n", result);
	    } else
#endif
	    {
		rtapi_print_msg(
		    error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
		    "RTAPI: ERROR: rt_task_wait_period() returned %d.\n", result);
	    }
	    error_printed++;
	    if(error_printed == 10)
	        rtapi_print_msg(
		    error_printed == 0 ? RTAPI_MSG_ERR : RTAPI_MSG_WARN,
		    "RTAPI: (further messages will be suppressed)\n");
	}
    }
}
#endif

int rtapi_task_resume(int task_id)
{
    int retval;
    task_data *task;

    /* validate task ID */
    if ((task_id < 1) || (task_id > RTAPI_MAX_TASKS)) {
	return -EINVAL;
    }
    /* point to the task's data */
    task = &(task_array[task_id]);
    /* is task ready to be started? */
    if (task->state != PAUSED) {
	return -EINVAL;
    }
    /* start the task */
    // ok for both RTAI and Xenomai
    retval = rt_task_resume(ostask_array[task_id]);
    if (retval != 0) {
	return -EINVAL;
    }
    /* update task data */
    task->state = FREERUN;
    return 0;
}

int rtapi_task_pause(int task_id)
{
    int retval;
    int oldstate;
    task_data *task;

    /* validate task ID */
    if ((task_id < 1) || (task_id > RTAPI_MAX_TASKS)) {
	return -EINVAL;
    }
    /* point to the task's data */
    task = &(task_array[task_id]);
    /* is it running? */
    if ((task->state != PERIODIC) && (task->state != FREERUN)) {
	return -EINVAL;
    }
    /* pause the task */
    oldstate = task->state;
    task->state = PAUSED;
    // ok for both RTAI and Xenomai
    retval = rt_task_suspend(ostask_array[task_id]);
    if (retval != 0) {
        task->state = oldstate;
	return -EINVAL;
    }
    /* update task data */
    return 0;
}

int rtapi_task_self(void)
{
    RT_TASK *ptr;
    int n;

    /* ask OS for pointer to its data for the current task */

#if defined(RTAPI_RTAI)
    ptr = rt_whoami();
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    ptr = rt_task_self();
#endif

    if (ptr == NULL) {
	/* called from outside a task? */
	return -EINVAL;
    }
    /* find matching entry in task array */
    n = 1;
    while (n <= RTAPI_MAX_TASKS) {
	if (ostask_array[n] == ptr) {
	    /* found a match */
	    return n;
	}
	n++;
    }
    return -EINVAL;
}

/***********************************************************************
*                  SHARED MEMORY RELATED FUNCTIONS                     *
************************************************************************/

int rtapi_shmem_new(int key, int module_id, unsigned long int size)
{
    int n;
    int shmem_id;
    shmem_data *shmem;
    char shm_name[20];

    /* key must be non-zero, and also cannot match the key that RTAPI uses */
    if ((key == 0) || (key == RTAPI_KEY)) {
	return -EINVAL;
    }
    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    if (module_array[module_id].state != REALTIME) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }

    /* check if a block is already open for this key */
    for (n = 1; n <= RTAPI_MAX_SHMEMS; n++) {
	if (shmem_array[n].key == key) {
	    /* found a match */
	    shmem_id = n;
	    shmem = &(shmem_array[n]);
	    /* is it big enough? */
	    if (shmem->size < size) {
		rtapi_mutex_give(&(rtapi_data->mutex));
		return -EINVAL;
	    }
	    /* yes, has it been mapped into kernel space? */
	    if (shmem->rtusers == 0) {
		/* no, map it and save the address */

#if defined(RTAPI_XENOMAI_KERNEL)
		rtapi_print_msg(RTAPI_MSG_ERR, 
				"RTAPI: UNSUPPORTED - cannott map user segment %d into kernel\n",n);
#endif
#if defined(RTAPI_RTAI)
		shmem_addr_array[shmem_id] = rtai_kmalloc(key, shmem->size);
#endif
		if (shmem_addr_array[shmem_id] == NULL) {
		    rtapi_mutex_give(&(rtapi_data->mutex));
		    return -ENOMEM;
		}
	    }
	    /* is this module already using it? */
	    if (test_bit(module_id, shmem->bitmap)) {
		rtapi_mutex_give(&(rtapi_data->mutex));
		return -EINVAL;
	    }
	    /* update usage data */
	    set_bit(module_id, shmem->bitmap);
	    shmem->rtusers++;
	    /* announce another user for this shmem */
	    rtapi_print_msg(RTAPI_MSG_DBG,
		"RTAPI: shmem %02d opened by module %02d\n",
		shmem_id, module_id);
	    rtapi_mutex_give(&(rtapi_data->mutex));
	    return shmem_id;
	}
    }
    /* find empty spot in shmem array */
    n = 1;
    while ((n <= RTAPI_MAX_SHMEMS) && (shmem_array[n].key != 0)) {
	n++;
    }
    if (n > RTAPI_MAX_SHMEMS) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EMFILE;
    }
    /* we have space for the block data */
    shmem_id = n;
    shmem = &(shmem_array[n]);

    /* get shared memory block from OS and save its address */

#if defined(RTAPI_RTAI)
    shmem_addr_array[shmem_id] = rtai_kmalloc(key, size);
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    snprintf(shm_name, sizeof(shm_name), "shm-%d", shmem_id);

    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: rtapi_shmem_new %s module_id=%d key=%d mname=%s size=%ld\n", 
		    shm_name, module_id, key,module_array[module_id].name, size);

    if ((n = rt_heap_create(&shmem_heap_array[shmem_id], shm_name, 
			    size, H_SHARED)) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: ERROR: rt_heap_create() returns %d\n", n);
	return -EINVAL;
    }
    if ((n = rt_heap_alloc(&shmem_heap_array[shmem_id], 0, TM_INFINITE , 
			   (void **)&shmem_addr_array[shmem_id])) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: ERROR: rt_heap_alloc() returns %d\n", n);
	return -EINVAL;
    }
#endif

    if (shmem_addr_array[shmem_id] == NULL) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -ENOMEM;
    }
    /* the block has been created, update data */
    set_bit(module_id, shmem->bitmap);
    shmem->key = key;
    shmem->rtusers = 1;
    shmem->ulusers = 0;
    shmem->size = size;
    rtapi_data->shmem_count++;
    /* zero the first word of the shmem area */
    *((long int *) (shmem_addr_array[shmem_id])) = 0;
    /* announce the birth of a brand new baby shmem */
    rtapi_print_msg(RTAPI_MSG_DBG,
	"RTAPI: shmem %02d created by module %02d, key: %d, size: %lu\n",
	shmem_id, module_id, key, size);

    /* and return the ID to the proud parent */
    rtapi_mutex_give(&(rtapi_data->mutex));
    return shmem_id;
}

int rtapi_shmem_delete(int shmem_id, int module_id)
{
    int retval;

    rtapi_mutex_get(&(rtapi_data->mutex));
    retval = shmem_delete(shmem_id, module_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return retval;
}

static int shmem_delete(int shmem_id, int module_id)
{
    shmem_data *shmem;

    /* validate shmem ID */
    if ((shmem_id < 1) || (shmem_id > RTAPI_MAX_SHMEMS)) {
	return -EINVAL;
    }
    /* point to the shmem's data */
    shmem = &(shmem_array[shmem_id]);
    /* is the block valid? */
    if (shmem->key == 0) {
	return -EINVAL;
    }
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	return -EINVAL;
    }
    if (module_array[module_id].state != REALTIME) {
	return -EINVAL;
    }
    /* is this module using the block? */
    if (test_bit(module_id, shmem->bitmap) == 0) {
	return -EINVAL;
    }
    /* OK, we're no longer using it */
    clear_bit(module_id, shmem->bitmap);
    shmem->rtusers--;
    /* is somebody else still using the block? */
    if (shmem->rtusers > 0) {
	/* yes, we're done for now */
	rtapi_print_msg(RTAPI_MSG_DBG,
	    "RTAPI: shmem %02d closed by module %02d\n", shmem_id, module_id);
	return 0;
    }
    /* no other realtime users, free the shared memory from kernel space */
#if defined(RTAPI_RTAI)
    rtai_kfree(shmem->key);
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    rt_heap_delete(&shmem_heap_array[shmem_id]);
#endif
    shmem_addr_array[shmem_id] = NULL;
    shmem->rtusers = 0;
    /* are any user processes using the block? */
    if (shmem->ulusers > 0) {
	/* yes, we're done for now */
	rtapi_print_msg(RTAPI_MSG_DBG,
	    "RTAPI: shmem %02d unmapped by module %02d\n", shmem_id,
	    module_id);
	return 0;
    }
    /* no other users at all, this ID is now free */
    /* update the data array and usage count */
    shmem->key = 0;
    shmem->size = 0;
    rtapi_data->shmem_count--;
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: shmem %02d freed by module %02d\n",
	shmem_id, module_id);
    return 0;
}

int rtapi_shmem_getptr(int shmem_id, void **ptr)
{
    /* validate shmem ID */
    if ((shmem_id < 1) || (shmem_id > RTAPI_MAX_SHMEMS)) {
	return -EINVAL;
    }
    /* is the block mapped? */
    if (shmem_addr_array[shmem_id] == NULL) {
	return -EINVAL;
    }
    /* pass memory address back to caller */
    *ptr = shmem_addr_array[shmem_id];
    return 0;
}

/***********************************************************************
*                    SEMAPHORE RELATED FUNCTIONS                       *
************************************************************************/

int rtapi_sem_new(int key, int module_id)
{
    int n, retval;
    int sem_id;
    sem_data *sem;
    char sem_name[20];

    /* key must be non-zero */
    if (key == 0) {
	return -EINVAL;
    }
    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    if (module_array[module_id].state != REALTIME) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    /* check if a semaphore already exists for this key */
    for (n = 1; n <= RTAPI_MAX_SEMS; n++) {
	if ((sem_array[n].users > 0) && (sem_array[n].key == key)) {
	    /* found a match */
	    sem_id = n;
	    sem = &(sem_array[n]);
	    /* is this module already using it? */
	    if (test_bit(module_id, sem->bitmap)) {
		/* yes, can't open it again */
		rtapi_mutex_give(&(rtapi_data->mutex));
		return -EINVAL;
	    }
	    /* update usage data */
	    set_bit(module_id, sem->bitmap);
	    sem->users++;
	    /* announce another user for this semaphore */
	    rtapi_print_msg(RTAPI_MSG_DBG,
		"RTAPI: sem %02d opened by module %02d\n", sem_id, module_id);
	    rtapi_mutex_give(&(rtapi_data->mutex));
	    return sem_id;
	}
    }
    /* find empty spot in sem array */
    n = 1;
    while ((n <= RTAPI_MAX_SEMS) && (sem_array[n].users != 0)) {
	n++;
    }
    if (n > RTAPI_MAX_SEMS) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EMFILE;
    }
    /* we have space for the semaphore */
    sem_id = n;
    sem = &(sem_array[n]);
    /* ask the OS to initialize the semaphore */

#if defined(RTAPI_XENOMAI_KERNEL)
    snprintf(sem_name, sizeof(sem_name), "sem-%d", sem_id);
    if ((retval = rt_sem_create(&(ossem_array[sem_id]), sem_name, 0, S_FIFO))) {
	rtapi_print_msg(RTAPI_MSG_ERR,
		"RTAPI: cannot create semaphore %d: %d\n", sem_id, retval);
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
#endif

#if defined(RTAPI_RTAI)
    rt_sem_init(&(ossem_array[sem_id]), 0);
#endif

    /* the semaphore has been created, update data */
    set_bit(module_id, sem->bitmap);
    sem->users = 1;
    sem->key = key;
    rtapi_data->sem_count++;
    /* announce the birth of a brand new baby semaphore */
    rtapi_print_msg(RTAPI_MSG_DBG,
	"RTAPI: sem %02d created by module %02d, key: %d\n",
	sem_id, module_id, key);
    /* and return the ID to the proud parent */
    rtapi_mutex_give(&(rtapi_data->mutex));
    return sem_id;
}

int rtapi_sem_delete(int sem_id, int module_id)
{
    int retval;

    rtapi_mutex_get(&(rtapi_data->mutex));
    retval = sem_delete(sem_id, module_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return retval;
}

static int sem_delete(int sem_id, int module_id)
{
    sem_data *sem;

    /* validate sem ID */
    if ((sem_id < 1) || (sem_id > RTAPI_MAX_SEMS)) {
	return -EINVAL;
    }
    /* point to the semaphores's data */
    sem = &(sem_array[sem_id]);
    /* is the semaphore valid? */
    if (sem->users == 0) {
	return -EINVAL;
    }
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	return -EINVAL;
    }
    if (module_array[module_id].state != REALTIME) {
	return -EINVAL;
    }
    /* is this module using the semaphore? */
    if (test_bit(module_id, sem->bitmap) == 0) {
	return -EINVAL;
    }
    /* OK, we're no longer using it */
    clear_bit(module_id, sem->bitmap);
    sem->users--;
    /* is somebody else still using the semaphore */
    if (sem->users > 0) {
	/* yes, we're done for now */
	rtapi_print_msg(RTAPI_MSG_DBG,
	    "RTAPI: sem %02d closed by module %02d\n", sem_id, module_id);
	return 0;
    }
    /* no other users, ask the OS to shut down the semaphore */
    // ok for RTAI and Xenomai
    rt_sem_delete(&(ossem_array[sem_id]));
    /* update the data array and usage count */
    sem->users = 0;
    sem->key = 0;
    rtapi_data->sem_count--;
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: sem %02d deleted by module %02d\n",
	sem_id, module_id);
    return 0;
}

int rtapi_sem_give(int sem_id)
{
    sem_data *sem;

    /* validate sem ID */
    if ((sem_id < 1) || (sem_id > RTAPI_MAX_SEMS)) {
	return -EINVAL;
    }
    /* point to the semaphores's data */
    sem = &(sem_array[sem_id]);
    /* is the semaphore valid? */
    if (sem->users == 0) {
	return -EINVAL;
    }
    /* give up the semaphore */
#if defined(RTAPI_RTAI)
    rt_sem_signal(&(ossem_array[sem_id]));
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    rt_sem_v(&(ossem_array[sem_id]));
#endif
    return 0;
}

int rtapi_sem_take(int sem_id)
{
    sem_data *sem;

    /* validate sem ID */
    if ((sem_id < 1) || (sem_id > RTAPI_MAX_SEMS)) {
	return -EINVAL;
    }
    /* point to the semaphores's data */
    sem = &(sem_array[sem_id]);
    /* is the semaphore valid? */
    if (sem->users == 0) {
	return -EINVAL;
    }
    /* get the semaphore */

#if defined(RTAPI_RTAI)
    rt_sem_wait(&(ossem_array[sem_id]));
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    rt_sem_p(&(ossem_array[sem_id]), TM_INFINITE);
#endif
    return 0;
}

int rtapi_sem_try(int sem_id)
{
    sem_data *sem;

    /* validate sem ID */
    if ((sem_id < 1) || (sem_id > RTAPI_MAX_SEMS)) {
	return -EINVAL;
    }
    /* point to the semaphores's data */
    sem = &(sem_array[sem_id]);
    /* is the semaphore valid? */
    if (sem->users == 0) {
	return -EINVAL;
    }
    /* try the semaphore */
#if defined(RTAPI_RTAI)
    if (rt_sem_wait_if(&(ossem_array[sem_id])) <= 0) {
	return -EBUSY;
    }
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    if (rt_sem_p(&(ossem_array[sem_id]), TM_NONBLOCK) == -EWOULDBLOCK) {
	return -EBUSY;
    }
#endif
    return 0;
}

/***********************************************************************
*                        FIFO RELATED FUNCTIONS                        *
************************************************************************/
#if defined(RTAPI_FIFO)
int rtapi_fifo_new(int key, int module_id, unsigned long int size, char mode)
{
    int n, retval;
    int fifo_id;
    fifo_data *fifo;

    /* key must be non-zero */
    if (key == 0) {
	return -EINVAL;
    }
    /* mode must be "R" or "W" */
    if ((mode != 'R') && (mode != 'W')) {
	return -EINVAL;
    }
    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    if (module_array[module_id].state != REALTIME) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    /* check if a fifo already exists for this key */
    for (n = 1; n <= RTAPI_MAX_FIFOS; n++) {
	if ((fifo_array[n].state != UNUSED) && (fifo_array[n].key == key)) {
	    /* found a match */
	    fifo_id = n;
	    fifo = &(fifo_array[n]);
	    /* is the desired mode available */
	    if (mode == 'R') {
		if (fifo->state & HAS_READER) {
		    rtapi_mutex_give(&(rtapi_data->mutex));
		    return -EBUSY;
		}
		/* available, update status */
		fifo->state |= HAS_READER;
		fifo->reader = module_id;
		/* announce */
		rtapi_print_msg(RTAPI_MSG_DBG,
		    "RTAPI: fifo %02d opened for read by module %02d\n",
		    fifo_id, module_id);
		rtapi_mutex_give(&(rtapi_data->mutex));
		return fifo_id;
	    } else {		/* mode == 'W' */

		if (fifo->state & HAS_WRITER) {
		    rtapi_mutex_give(&(rtapi_data->mutex));
		    return -EBUSY;
		}
		/* available, update status */
		fifo->state |= HAS_WRITER;
		fifo->writer = module_id;
		/* announce */
		rtapi_print_msg(RTAPI_MSG_DBG,
		    "RTAPI: fifo %02d opened for write by module %02d\n",
		    fifo_id, module_id);
		rtapi_mutex_give(&(rtapi_data->mutex));
		return fifo_id;
	    }
	}
    }
    /* find empty spot in fifo array */
    n = 1;
    while ((n <= RTAPI_MAX_FIFOS) && (fifo_array[n].state != UNUSED)) {
	n++;
    }
    if (n > RTAPI_MAX_FIFOS) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EMFILE;
    }
    /* we have a free ID for the fifo */
    fifo_id = n;
    fifo = &(fifo_array[n]);
    /* create the fifo */
    retval = rtf_create(fifo_id, size);
    /* rtf_create() returns 0 on success */
    if (retval != 0) {
	/* create failed */
	rtapi_mutex_give(&(rtapi_data->mutex));
	if (retval == ENOMEM) {
	    /* couldn't allocate memory */
	    return -ENOMEM;
	}
	/* some other failure */
	return -EINVAL;
    }
    /* the fifo has been created, update data */
    if (mode == 'R') {
	fifo->state = HAS_READER;
	fifo->reader = module_id;
	rtapi_print_msg(RTAPI_MSG_DBG,
	    "RTAPI: fifo %02d created for read by module %02d, key: %d, size: %ld\n",
	    fifo_id, module_id, key, size);
    } else {			/* mode == 'W' */

	fifo->state = HAS_WRITER;
	fifo->writer = module_id;
	rtapi_print_msg(RTAPI_MSG_DBG,
	    "RTAPI: fifo %02d created for write by module %02d, key: %d, size: %ld\n",
	    fifo_id, module_id, key, size);
    }
    fifo->key = key;
    fifo->size = size;
    rtapi_data->fifo_count++;
    /* and return the ID */
    rtapi_mutex_give(&(rtapi_data->mutex));
    return fifo_id;
}

int rtapi_fifo_delete(int fifo_id, int module_id)
{
    int retval;

    rtapi_mutex_get(&(rtapi_data->mutex));
    retval = fifo_delete(fifo_id, module_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return retval;
}

static int fifo_delete(int fifo_id, int module_id)
{
    fifo_data *fifo;

    /* validate fifo ID */
    if ((fifo_id < 1) || (fifo_id > RTAPI_MAX_FIFOS)) {
	return -EINVAL;
    }
    /* point to the fifo's data */
    fifo = &(fifo_array[fifo_id]);
    /* is the fifo valid? */
    if (fifo->state == UNUSED) {
	return -EINVAL;
    }
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	return -EINVAL;
    }
    if (module_array[module_id].state != REALTIME) {
	return -EINVAL;
    }
    /* is this module using the fifo? */
    if ((fifo->reader != module_id) && (fifo->writer != module_id)) {
	return -EINVAL;
    }
    /* update fifo state */
    if (fifo->reader == module_id) {
	fifo->state &= ~HAS_READER;
	fifo->reader = 0;
    }
    if (fifo->writer == module_id) {
	fifo->state &= ~HAS_WRITER;
	fifo->writer = 0;
    }
    /* is somebody else still using the fifo */
    if (fifo->state != UNUSED) {
	/* yes, done for now */
	rtapi_print_msg(RTAPI_MSG_DBG,
	    "RTAPI: fifo %02d closed by module %02d\n", fifo_id, module_id);
	return 0;
    }
    /* no other users, call the OS to destroy the fifo */
    /* OS returns open count, loop until truly destroyed */
    while (rtf_destroy(fifo_id) > 0);
    /* update the data array and usage count */
    fifo->state = UNUSED;
    fifo->key = 0;
    fifo->size = 0;
    rtapi_data->fifo_count--;
    rtapi_print_msg(RTAPI_MSG_DBG,
	"RTAPI: fifo %02d deleted by module %02d\n", fifo_id, module_id);
    return 0;
}

int rtapi_fifo_read(int fifo_id, char *buf, unsigned long int size)
{
    int retval;
    fifo_data *fifo;

    /* validate fifo ID */
    if ((fifo_id < 1) || (fifo_id > RTAPI_MAX_FIFOS)) {
	return -EINVAL;
    }
    /* point to the fifo's data */
    fifo = &(fifo_array[fifo_id]);
    /* is the fifo valid? */
    if ((fifo->state & HAS_READER) == 0) {
	return -EINVAL;
    }
    /* get whatever data is available */
    retval = rtf_get(fifo_id, &buf, size);
    if (retval < 0) {
	return -EINVAL;
    }
    return retval;
}

int rtapi_fifo_write(int fifo_id, char *buf, unsigned long int size)
{
    int retval;
    fifo_data *fifo;

    /* validate fifo ID */
    if ((fifo_id < 1) || (fifo_id > RTAPI_MAX_FIFOS)) {
	return -EINVAL;
    }
    /* point to the fifo's data */
    fifo = &(fifo_array[fifo_id]);
    /* is the fifo valid? */
    if ((fifo->state & HAS_WRITER) == 0) {
	return -EINVAL;
    }
    /* put as much data as possible */
    retval = rtf_put(fifo_id, buf, size);
    if (retval < 0) {
	return -EINVAL;
    }
    return retval;
}
#else // RTAPI_FIFO undefined
int rtapi_fifo_new(int key, int module_id, unsigned long int size, char mode)

{
  return -ENOSYS;
}

int rtapi_fifo_delete(int fifo_id, int module_id)
{
  return -ENOSYS;
}

int rtapi_fifo_read(int fifo_id, char *buf, unsigned long size)
{
  return -ENOSYS;
}

int rtapi_fifo_write(int fifo_id, char *buf, unsigned long int size)
{
  return -ENOSYS;
}
#endif

/***********************************************************************
*                    INTERRUPT RELATED FUNCTIONS                       *
************************************************************************/

int rtapi_irq_new(unsigned int irq_num, int owner, void (*handler) (void))
{
    int n, retval;
    int irq_id;
    irq_data *irq;
    char irq_name[20];

    /* validate irq */
    if ((irq_num < 1) || (irq_num > 255)) {
	return -EINVAL;
    }
    /* validate handler */
    if (handler == NULL) {
	return -EINVAL;
    }
    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* validate owner */
    if ((owner < 1) || (owner > RTAPI_MAX_MODULES)) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    if (module_array[owner].state != REALTIME) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EINVAL;
    }
    /* check if a handler already exists for this irq */
    for (n = 1; n <= RTAPI_MAX_IRQS; n++) {
	if (irq_array[n].irq_num == irq_num) {
	    /* found a match */
	    rtapi_mutex_give(&(rtapi_data->mutex));
	    return -EBUSY;
	}
    }
    /* find empty spot in irq array */
    n = 1;
    while ((n <= RTAPI_MAX_IRQS) && (irq_array[n].irq_num != 0)) {
	n++;
    }
    if (n > RTAPI_MAX_IRQS) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return -EMFILE;
    }
    /* we have space for the irq */
    irq_id = n;
    irq = &(irq_array[n]);
    /* install the handler */

#if defined(RTAPI_RTAI)
    retval = rt_request_global_irq(irq_num, handler);
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    snprintf(irq_name, sizeof(irq_name), "irq-%d", irq_num);
    retval = rt_intr_create(&xeno_irq_array[irq_id], irq_name,
			    irq_num, handler, 0, 0);
#endif

    if (retval != 0) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	if (retval == EBUSY) {
	    return -EBUSY;
	} else {
	    return -EINVAL;
	}
    }
    /* update data */
    irq->irq_num = irq_num;
    irq->owner = owner;
    irq->handler = handler;
    rtapi_data->irq_count++;
    /* announce the new interrupt handler */
    rtapi_print_msg(RTAPI_MSG_DBG,
	"RTAPI: handler for IRQ %d installed by module %02d\n",
	irq_num, owner);
    /* and return success */
    rtapi_mutex_give(&(rtapi_data->mutex));
    return 0;
}

int rtapi_irq_delete(unsigned int irq_num)
{
    int retval;

    rtapi_mutex_get(&(rtapi_data->mutex));
    retval = irq_delete(irq_num);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return retval;
}

static int irq_delete(unsigned int irq_num)
{
    int n, retval;
    int irq_id;
    irq_data *irq;

    /* validate irq */
    if ((irq_num < 1) || (irq_num > 255)) {
	return -EINVAL;
    }
    /* check if a handler exists for this irq */
    n = 1;
    while ((n <= RTAPI_MAX_IRQS) && (irq_array[n].irq_num != irq_num)) {
	n++;
    }
    if (n > RTAPI_MAX_IRQS) {
	/* not found */
	return -EINVAL;
    }
    /* found the irq */
    irq_id = n;
    irq = &(irq_array[n]);
    /* get rid of the handler */

#if defined(RTAPI_RTAI)
    rt_shutdown_irq(irq_num);
    retval = rt_free_global_irq(irq_num);
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    retval = rt_intr_delete(&xeno_irq_array[irq_id]);
#endif

    if (retval != 0) {
	return -EINVAL;
    }
    /* update data */
    irq->irq_num = 0;
    irq->owner = 0;
    irq->handler = NULL;
    rtapi_data->irq_count--;
    rtapi_print_msg(RTAPI_MSG_DBG,
	"RTAPI: handler for IRQ %d deleted\n", irq_num);
    return 0;
}

int rtapi_enable_interrupt(unsigned int irq)
{
#if defined(RTAPI_RTAI)
    rt_startup_irq(irq);
#endif

#if defined(RTAPI_XENOMAI_KERNEL)
    int n;

    // FIXME this is dubious - cant do a 
    // rtapi_mutex_get(&(rtapi_data->mutex)), this might be RT
    for (n = 1; n <= RTAPI_MAX_IRQS; n++) {
	if (irq_array[n].irq_num == irq) {
	    rt_intr_enable(&xeno_irq_array[n]);
	}
    }
#endif

    return 0;
}

int rtapi_disable_interrupt(unsigned int irq)
{
#if defined(RTAPI_RTAI)
    rt_shutdown_irq(irq);
#endif
#if defined(RTAPI_XENOMAI_KERNEL)
    int n;

    // FIXME this is dubious - cant do a 
    // rtapi_mutex_get(&(rtapi_data->mutex)), this might be RT
    for (n = 1; n <= RTAPI_MAX_IRQS; n++) {
	if (irq_array[n].irq_num == irq) {
	    rt_intr_disable(&xeno_irq_array[n]);
	}
    }
#endif
    return 0;
}

/***********************************************************************
*                        I/O RELATED FUNCTIONS                         *
************************************************************************/

void rtapi_outb(unsigned char byte, unsigned int port)
{
    outb(byte, port);
}

unsigned char rtapi_inb(unsigned int port)
{
    return inb(port);
}


/* starting with kernel 2.6, symbols that are used by other modules
   _must_ be explicitly exported.  2.4 and earlier kernels exported
   all non-static global symbols by default, so these explicit exports
   were not needed.  For 2.4 and older, you should define EXPORT_SYMTAB
   (before including module.h) to make these explicit exports work and 
   minimize pollution of the kernel namespace.  But EXPORT_SYMTAB
   must not be defined for 2.6, so the best place to do it is 
   probably in the makefiles somewhere (as a -D option to gcc).
*/

EXPORT_SYMBOL(rtapi_init);
EXPORT_SYMBOL(rtapi_exit);
EXPORT_SYMBOL(rtapi_snprintf);
EXPORT_SYMBOL(rtapi_vsnprintf);
EXPORT_SYMBOL(rtapi_print);
EXPORT_SYMBOL(rtapi_print_msg);
EXPORT_SYMBOL(rtapi_set_msg_level);
EXPORT_SYMBOL(rtapi_get_msg_level);
EXPORT_SYMBOL(rtapi_set_msg_handler);
EXPORT_SYMBOL(rtapi_get_msg_handler);
EXPORT_SYMBOL(rtapi_clock_set_period);
EXPORT_SYMBOL(rtapi_get_time);
EXPORT_SYMBOL(rtapi_get_clocks);
EXPORT_SYMBOL(rtapi_delay);
EXPORT_SYMBOL(rtapi_delay_max);
EXPORT_SYMBOL(rtapi_prio_highest);
EXPORT_SYMBOL(rtapi_prio_lowest);
EXPORT_SYMBOL(rtapi_prio_next_higher);
EXPORT_SYMBOL(rtapi_prio_next_lower);
EXPORT_SYMBOL(rtapi_task_new);
EXPORT_SYMBOL(rtapi_task_delete);
EXPORT_SYMBOL(rtapi_task_start);
EXPORT_SYMBOL(rtapi_wait);
EXPORT_SYMBOL(rtapi_task_resume);
EXPORT_SYMBOL(rtapi_task_pause);
EXPORT_SYMBOL(rtapi_task_self);
EXPORT_SYMBOL(rtapi_shmem_new);
EXPORT_SYMBOL(rtapi_shmem_delete);
EXPORT_SYMBOL(rtapi_shmem_getptr);
EXPORT_SYMBOL(rtapi_sem_new);
EXPORT_SYMBOL(rtapi_sem_delete);
EXPORT_SYMBOL(rtapi_sem_give);
EXPORT_SYMBOL(rtapi_sem_take);
EXPORT_SYMBOL(rtapi_sem_try);
EXPORT_SYMBOL(rtapi_fifo_new);
EXPORT_SYMBOL(rtapi_fifo_delete);
EXPORT_SYMBOL(rtapi_fifo_read);
EXPORT_SYMBOL(rtapi_fifo_write);
EXPORT_SYMBOL(rtapi_irq_new);
EXPORT_SYMBOL(rtapi_irq_delete);
EXPORT_SYMBOL(rtapi_enable_interrupt);
EXPORT_SYMBOL(rtapi_disable_interrupt);
EXPORT_SYMBOL(rtapi_outb);
EXPORT_SYMBOL(rtapi_inb);
