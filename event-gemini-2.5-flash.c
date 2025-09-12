/*
 * This file implements a generic process event publish/subscribe facility.
 * The facility for use by non-core system services that implement part of the
 * userland system call interface.  Currently, it supports two events: a
 * process catching a signal, and a process being terminated.  A subscribing
 * service would typically use such events to interrupt a blocking system call
 * and/or clean up process-bound resources.  As of writing, the only service
 * that uses this facility is the System V IPC server.
 *
 * Each of these events will be published to subscribing services right after
 * VFS has acknowledged that it has processed the same event.  For each
 * subscriber, in turn, the process will be blocked (with the EVENT_CALL flag
 * set) until the subscriber acknowledges the event or PM learns that the
 * subscriber has died.  Thus, each subscriber adds a serialized messaging
 * roundtrip for each subscribed event.
 *
 * The one and only reason for this synchronous, serialized approach is that it
 * avoids PM queuing up too many asynchronous messages.  In theory, each
 * running process may have an event pending, and thus, the serial synchronous
 * approach requires NR_PROCS asynsend slots.  For a parallel synchronous
 * approach, this would increase to (NR_PROCS*NR_SUBS).  Worse yet, for an
 * asynchronous event notification approach, the number of messages that PM can
 * end up queuing is potentially unbounded, so that is certainly not an option.
 * At this moment, we expect only one subscriber (the IPC server) which makes
 * the serial vs parallel point less relevant.
 *
 * It is not possible to subscribe to events from certain processes only.  If
 * a service were to subscribe to process events as part of a system call by
 * a process (e.g., semop(2) in the case of the IPC server), it may subscribe
 * "too late" and already have missed a signal event for the process calling
 * semop(2), for example.  Resolving such race conditions would require major
 * infrastructure changes.
 *
 * A server may however change its event subscription mask at runtime, so as to
 * limit the number of event messages it receives in a crude fashion.  For the
 * same race-condition reasons, new subscriptions must always be made when
 * processing a message that is *not* a system call potentially affected by
 * events.  In the case of the IPC server, it may subscribe to events from
 * semget(2) but not semop(2).  For signal events, the delay call system
 * guarantees the safety of this approach; for exit events, the message type
 * prioritization does (which is not great; see the TODO item in forkexit.c).
 *
 * After changing its mask, a subscribing service may still receive messages
 * for events it is no longer subscribed to.  It should acknowledge these
 * messages by sending a reply as usual.
 */

#include "pm.h"
#include "mproc.h"
#include <assert.h>

/*
 * A realistic upper bound for the number of subscribing services.  The process
 * event notification system adds a round trip to a service for each subscriber
 * and uses asynchronous messaging to boot, so clearly it does not scale to
 * numbers larger than this.
 */
#define NR_SUBS		4

static struct {
	endpoint_t endpt;		/* endpoint of subscriber */
	unsigned int mask;		/* interests bit mask (PROC_EVENT_) */
	unsigned int waiting;		/* # procs blocked on reply from it */
} subs[NR_SUBS];

static unsigned int nsubs = 0;
static unsigned int nested = 0;

/*
 * For the current event of the given process, as determined by its flags, send
 * a process event message to the next subscriber, or resume handling the
 * event itself if there are no more subscribers to notify.
 */
static void
resume_event(struct mproc * rmp)
{
	message m;
	int r;
	unsigned int event;

	assert(rmp->mp_flags & IN_USE);
	assert(rmp->mp_flags & EVENT_CALL);
	assert(rmp->mp_eventsub != NO_EVENTSUB);

	/* Determine which event we are processing. */
	if (rmp->mp_flags & EXITING) {
		event = PROC_EVENT_EXIT;
	} else if (rmp->mp_flags & UNPAUSED) {
		event = PROC_EVENT_SIGNAL;
	} else {
		panic("unknown event for flags %x", rmp->mp_flags);
	}

	/*
	 * Iterate through subscribers starting from `rmp->mp_eventsub`.
	 * `rmp->mp_eventsub` is continuously advanced to track the next subscriber to check,
	 * regardless of whether the current subscriber matches the event or not.
	 * If a matching subscriber is found, a message is sent, and the function returns,
	 * notifying only one subscriber per call to `resume_event`.
	 */
	while (rmp->mp_eventsub < nsubs) {
		unsigned int current_sub_idx = rmp->mp_eventsub;
		rmp->mp_eventsub++; /* Advance for the next check/call */

		if (subs[current_sub_idx].mask & event) {
			memset(&m, 0, sizeof(m));
			m.m_type = PROC_EVENT;
			m.m_pm_lsys_proc_event.endpt = rmp->mp_endpoint;
			m.m_pm_lsys_proc_event.event = event;

			r = asynsend3(subs[current_sub_idx].endpt, &m, AMF_NOREPLY);
			if (r != OK) {
				panic("asynsend failed: %d", r);
			}

			assert(subs[current_sub_idx].waiting < NR_PROCS);
			subs[current_sub_idx].waiting++;
			return; /* Only notify one subscriber per call */
		}
	}

	/*
	 * If the loop completes, it means no more subscribers are interested
	 * or all have been checked. The event chain for this process is complete.
	 * Resume the actual underlying event.
	 */
	rmp->mp_flags &= ~EVENT_CALL;
	rmp->mp_eventsub = NO_EVENTSUB; /* Reset, as all subscribers have been processed */

	if (event == PROC_EVENT_EXIT) {
		exit_restart(rmp);
	} else if (event == PROC_EVENT_SIGNAL) {
		restart_sigs(rmp);
	}
}

/*
 * Remove a subscriber from the set, forcefully if we have to.  Ensure that
 * any processes currently subject to process event notification are updated
 * accordingly, in a way that no services are skipped for process events.
 */
static void
remove_sub(unsigned int slot)
{
	struct mproc *rmp;

	if (slot >= nsubs) {
		return;
	}

	if (slot < nsubs - 1) {
		memmove(&subs[slot], &subs[slot + 1], (nsubs - 1 - slot) * sizeof(subs[0]));
	}
	nsubs--;

	for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++) {
		if ((rmp->mp_flags & (IN_USE | EVENT_CALL)) != (IN_USE | EVENT_CALL)) {
			continue;
		}

		assert(rmp->mp_eventsub != NO_EVENTSUB);

		unsigned int current_sub_index = (unsigned int)rmp->mp_eventsub;

		if (current_sub_index == slot) {
			nested++;
			resume_event(rmp);
			nested--;
		} else if (current_sub_index > slot) {
			rmp->mp_eventsub--;
		}
	}
}

/*
 * Subscribe to process events.  The given event mask denotes the events in
 * which the caller is interested.  Multiple calls will each replace the mask,
 * and a mask of zero will unsubscribe the service from events altogether.
 * Return OK on success, EPERM if the caller may not register for events, or
 * ENOMEM if all subscriber slots are in use already.
 */
int
do_proceventmask(void)
{
	unsigned int i;
	unsigned int new_mask;

	if (!(mp->mp_flags & PRIV_PROC)) {
		return EPERM;
	}

	new_mask = m_in.m_lsys_pm_proceventmask.mask;

	for (i = 0; i < nsubs; i++) {
		if (subs[i].endpt == who_e) {
			if (new_mask == 0 && subs[i].waiting == 0) {
				remove_sub(i);
			} else {
				subs[i].mask = new_mask;
			}
			return OK;
		}
	}

	if (new_mask == 0) {
		return OK;
	}

	if (nsubs >= __arraycount(subs)) {
		printf("PM: too many process event subscribers!\n");
		return ENOMEM;
	}

	subs[nsubs].endpt = who_e;
	subs[nsubs].mask = new_mask;
	nsubs++;

	return OK;
}

/*
 * A subscribing service has replied to a process event message from us, or at
 * least that is what should have happened.  First make sure of this, and then
 * resume event handling for the affected process.
 */
#include <assert.h>
#include <stdio.h> /* For printf */

/*
 * Assume the following are externally defined or included from system headers:
 * - struct mproc, struct subs_entry, message
 * - endpoint_t type
 * - External variables: mproc[], subs[], nsubs, who_e, m_in, nested, mp
 * - External functions: pm_isokendpt(), remove_sub(), resume_event()
 * - Constants: ENOSYS, SUSPEND, OK, NO_EVENTSUB, PRIV_PROC, EVENT_CALL,
 *   EXITING, UNPAUSED, PROC_EVENT_EXIT, PROC_EVENT_SIGNAL
 */

/* Helper macro for consistent error logging and suspension. */
#define LOG_AND_SUSPEND(fmt, ...) \
    do { \
        printf("PM: " fmt, ##__VA_ARGS__); \
        return SUSPEND; \
    } while (0)

int
do_proc_event_reply(void)
{
	struct mproc *rmp;
	endpoint_t endpt;
	unsigned int sub_idx;
	int event_type;
	int slot;

	assert(nested == 0);

	/* Check if the caller has sufficient privilege. */
	if (!(mp->mp_flags & PRIV_PROC)) {
		return ENOSYS;
	}

	/* Validate the endpoint provided in the reply message. */
	endpt = m_in.m_pm_lsys_proc_event.endpt;
	if (pm_isokendpt(endpt, &slot) != OK) {
		LOG_AND_SUSPEND("proc event reply from %d for invalid endpt %d\n",
		    who_e, endpt);
	}
	rmp = &mproc[slot];

	/* Ensure the process was indeed waiting for an event reply. */
	if (!(rmp->mp_flags & EVENT_CALL)) {
		LOG_AND_SUSPEND("proc event reply from %d for endpt %d, no event pending\n",
		    who_e, endpt);
	}

	/* Validate the event subscriber index associated with the process. */
	if (rmp->mp_eventsub == NO_EVENTSUB ||
	    (unsigned int)rmp->mp_eventsub >= nsubs) {
		LOG_AND_SUSPEND("proc event reply from %d for endpt %d with invalid sub index %d\n",
		    who_e, endpt, rmp->mp_eventsub);
	}
	sub_idx = (unsigned int)rmp->mp_eventsub;

	/* Verify the reply came from the expected subscriber endpoint. */
	if (subs[sub_idx].endpt != who_e) {
		LOG_AND_SUSPEND("proc event reply for %d from %d instead of expected subscriber %d\n",
		    endpt, who_e, subs[sub_idx].endpt);
	}

	/* Determine the expected event type based on the process's flags. */
	if (rmp->mp_flags & EXITING) {
		event_type = PROC_EVENT_EXIT;
	} else if (rmp->mp_flags & UNPAUSED) {
		event_type = PROC_EVENT_SIGNAL;
	} else {
		LOG_AND_SUSPEND("proc event reply from %d for %d, unexpected process flags %x\n",
		    who_e, endpt, rmp->mp_flags);
	}

	/* Compare the received event with the internally determined expected event type. */
	if (m_in.m_pm_lsys_proc_event.event != event_type) {
		LOG_AND_SUSPEND("proc event reply from %d for %d with event %d, expected %d\n",
		    who_e, endpt, m_in.m_pm_lsys_proc_event.event, event_type);
	}

	/*
	 * Do NOT check the event against the subscriber's event mask, since a
	 * service may have unsubscribed from an event while it has yet to
	 * process some leftover notifications for that event.  We could decide
	 * not to wait for the replies to those leftover notifications upon
	 * unsubscription, but that could result in problems upon quick
	 * resubscription, and such cases may in fact happen in practice.
	 */

	assert(subs[sub_idx].waiting > 0); /* Ensure waiting count is positive before decrement. */
	subs[sub_idx].waiting--;

	/*
	 * If no more replies are pending from this subscriber and it's no
	 * longer actively subscribed, remove it. Otherwise, advance the process
	 * to handle the next event or subscriber.
	 */
	if (subs[sub_idx].mask == 0 && subs[sub_idx].waiting == 0) {
		remove_sub(sub_idx);
	} else {
		rmp->mp_eventsub++; /* Move to the next subscriber or event processing step. */
		resume_event(rmp);
	}

	/* In any case, do not reply to this reply message itself. */
	return SUSPEND;
}

/*
 * Publish a process event to interested subscribers.  The event is determined
 * from the process flags.  In addition, if the event is a process exit, also
 * check if it is a subscribing service that died.
 */
static void handle_exiting_privileged_process_subscription(struct mproc *rmp)
{
	unsigned int i;

	for (i = 0; i < nsubs; i++) {
		if (subs[i].endpt == rmp->mp_endpoint) {
			remove_sub(i);
			break;
		}
	}
}

void
publish_event(struct mproc * rmp)
{
	assert(nested == 0);
	assert((rmp->mp_flags & (IN_USE | EVENT_CALL)) == IN_USE);
	assert(rmp->mp_eventsub == NO_EVENTSUB);

	if ((rmp->mp_flags & (PRIV_PROC | EXITING)) == (PRIV_PROC | EXITING)) {
		handle_exiting_privileged_process_subscription(rmp);
	}

	rmp->mp_flags |= EVENT_CALL;
	rmp->mp_eventsub = 0;

	resume_event(rmp);
}
