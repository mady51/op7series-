/*
 * kernel/freezer.c - Function to freeze a process
 *
 * Originally from kernel/power/process.c
 */

#include <linux/interrupt.h>
#include <linux/suspend.h>
#include <linux/export.h>
#include <linux/syscalls.h>
#include <linux/freezer.h>
#include <linux/kthread.h>

/* total number of freezing conditions in effect */
atomic_t system_freezing_cnt = ATOMIC_INIT(0);
EXPORT_SYMBOL(system_freezing_cnt);

/* indicate whether PM freezing is in effect, protected by pm_mutex */
bool pm_freezing;
bool pm_nosig_freezing;

/*
 * Temporary export for the deadlock workaround in ata_scsi_hotplug().
 * Remove once the hack becomes unnecessary.
 */
EXPORT_SYMBOL_GPL(pm_freezing);

/* protects freezing and frozen transitions */
static DEFINE_SPINLOCK(freezer_lock);

/**
 * freezing_slow_path - slow path for testing whether a task needs to be frozen
 * @p: task to be tested
 *
 * This function is called by freezing() if system_freezing_cnt isn't zero
 * and tests whether @p needs to enter and stay in frozen state.  Can be
 * called under any context.  The freezers are responsible for ensuring the
 * target tasks see the updated state.
 */
bool freezing_slow_path(struct task_struct *p)
{
	if (p->flags & (PF_NOFREEZE | PF_SUSPEND_TASK))
		return false;

	if (test_tsk_thread_flag(p, TIF_MEMDIE))
		return false;

	if (cgroup_freezer_killable(p) && (fatal_signal_pending(p)
				|| (p->flags & PF_SIGNALED)))
		return false;

	if (pm_nosig_freezing || cgroup_freezing(p))
		return true;

	if (pm_freezing && !(p->flags & PF_KTHREAD))
		return true;

	return false;
}
EXPORT_SYMBOL(freezing_slow_path);

/* Refrigerator is place where frozen processes are stored :-). */
bool __refrigerator(bool check_kthr_stop)
{
	/* Hmm, should we be allowed to suspend when there are realtime
	   processes around? */
	bool was_frozen = false;
	long save = current->state;

	pr_debug("%s entered refrigerator\n", current->comm);

	for (;;) {
		bool killable = cgroup_freezer_killable(current);

		if (killable)
			set_current_state(TASK_INTERRUPTIBLE);
		else
			set_current_state(TASK_UNINTERRUPTIBLE);

		spin_lock_irq(&freezer_lock);
		current->flags |= PF_FROZEN;
		if (!freezing(current) ||
		     /* huruihuan add for kill task in D status */
		     (check_kthr_stop && kthread_should_stop()) || current->kill_flag)
			current->flags &= ~PF_FROZEN;
		spin_unlock_irq(&freezer_lock);

		if (!(current->flags & PF_FROZEN))
			break;
		was_frozen = true;

		/*
		 * Now we're sure that there is no pending fatal signal.
		 * Clear TIF_SIGPENDING to not get out of schedule()
		 * immediately (if there is a non-fatal signal pending), and
		 * put the task into sleep.
		 */
		if (killable) {
			long flags;

			if (lock_task_sighand(current, &flags)) {
				if (!sigismember(&current->pending.signal,
						SIGKILL))
					clear_thread_flag(TIF_SIGPENDING);
				unlock_task_sighand(current, &flags);
			}
		}

		schedule();
	}

	pr_debug("%s left refrigerator\n", current->comm);

	/*
	 * Restore saved task state before returning.  The mb'd version
	 * needs to be used; otherwise, it might silently break
	 * synchronization which depends on ordered task state change.
	 */
	set_current_state(save);

	return was_frozen;
}
EXPORT_SYMBOL(__refrigerator);

static void fake_signal_wake_up(struct task_struct *p)
{
	unsigned long flags;

	if (lock_task_sighand(p, &flags)) {
		signal_wake_up(p, 0);
		unlock_task_sighand(p, &flags);
	}
}

/* huruihuan add for freezing task in cgroup despite of PF_FREEZER_SKIP flag */
bool freeze_cgroup_task(struct task_struct *p)
{
	unsigned long flags;

	spin_lock_irqsave(&freezer_lock, flags);
	if (!freezing(p) || frozen(p)) {
		spin_unlock_irqrestore(&freezer_lock, flags);
		return false;
	}

	if (!(p->flags & PF_KTHREAD))
		fake_signal_wake_up(p);
	else
		wake_up_state(p, TASK_INTERRUPTIBLE);

	spin_unlock_irqrestore(&freezer_lock, flags);
	return true;
}

/**
 * freeze_task - send a freeze request to given task
 * @p: task to send the request to
 *
 * If @p is freezing, the freeze request is sent either by sending a fake
 * signal (if it's not a kernel thread) or waking it up (if it's a kernel
 * thread).
 *
 * RETURNS:
 * %false, if @p is not freezing or already frozen; %true, otherwise
 */
bool freeze_task(struct task_struct *p)
{
	unsigned long flags;

	/*
	 * This check can race with freezer_do_not_count, but worst case that
	 * will result in an extra wakeup being sent to the task.  It does not
	 * race with freezer_count(), the barriers in freezer_count() and
	 * freezer_should_skip() ensure that either freezer_count() sees
	 * freezing == true in try_to_freeze() and freezes, or
	 * freezer_should_skip() sees !PF_FREEZE_SKIP and freezes the task
	 * normally.
	 */
	if (freezer_should_skip(p))
		return false;

	spin_lock_irqsave(&freezer_lock, flags);
	if (!freezing(p) || frozen(p)) {
		spin_unlock_irqrestore(&freezer_lock, flags);
		return false;
	}

	if (!(p->flags & PF_KTHREAD))
		fake_signal_wake_up(p);
	else
		wake_up_state(p, TASK_INTERRUPTIBLE);

	spin_unlock_irqrestore(&freezer_lock, flags);
	return true;
}

void __thaw_task(struct task_struct *p)
{
	unsigned long flags;

	spin_lock_irqsave(&freezer_lock, flags);
	if (frozen(p))
		wake_up_process(p);
	spin_unlock_irqrestore(&freezer_lock, flags);
}

/**
 * set_freezable - make %current freezable
 *
 * Mark %current freezable and enter refrigerator if necessary.
 */
bool set_freezable(void)
{
	might_sleep();

	/*
	 * Modify flags while holding freezer_lock.  This ensures the
	 * freezer notices that we aren't frozen yet or the freezing
	 * condition is visible to try_to_freeze() below.
	 */
	spin_lock_irq(&freezer_lock);
	current->flags &= ~PF_NOFREEZE;
	spin_unlock_irq(&freezer_lock);

	return try_to_freeze();
}
EXPORT_SYMBOL(set_freezable);
