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
static unsigned int get_event_type(struct mproc *rmp)
{
	if (rmp->mp_flags & EXITING)
		return PROC_EVENT_EXIT;
	if (rmp->mp_flags & UNPAUSED)
		return PROC_EVENT_SIGNAL;
	panic("unknown event for flags %x", rmp->mp_flags);
}

static void send_event_notification(struct mproc *rmp, unsigned int event, unsigned int sub_index)
{
	message m;
	int r;

	memset(&m, 0, sizeof(m));
	m.m_type = PROC_EVENT;
	m.m_pm_lsys_proc_event.endpt = rmp->mp_endpoint;
	m.m_pm_lsys_proc_event.event = event;

	r = asynsend3(subs[sub_index].endpt, &m, AMF_NOREPLY);
	if (r != OK)
		panic("asynsend failed: %d", r);

	assert(subs[sub_index].waiting < NR_PROCS);
	subs[sub_index].waiting++;
}

static int notify_next_subscriber(struct mproc *rmp, unsigned int event)
{
	unsigned int i;

	for (i = rmp->mp_eventsub; i < nsubs; i++) {
		if (subs[i].mask & event) {
			send_event_notification(rmp, event, i);
			rmp->mp_eventsub = i + 1;
			return 1;
		}
	}
	return 0;
}

static void complete_event_processing(struct mproc *rmp, unsigned int event)
{
	rmp->mp_flags &= ~EVENT_CALL;
	rmp->mp_eventsub = NO_EVENTSUB;

	if (event == PROC_EVENT_EXIT)
		exit_restart(rmp);
	else if (event == PROC_EVENT_SIGNAL)
		restart_sigs(rmp);
}

static void resume_event(struct mproc *rmp)
{
	unsigned int event;

	assert(rmp->mp_flags & IN_USE);
	assert(rmp->mp_flags & EVENT_CALL);
	assert(rmp->mp_eventsub != NO_EVENTSUB);

	event = get_event_type(rmp);

	if (!notify_next_subscriber(rmp, event))
		complete_event_processing(rmp, event);
}

/*
 * Remove a subscriber from the set, forcefully if we have to.  Ensure that
 * any processes currently subject to process event notification are updated
 * accordingly, in a way that no services are skipped for process events.
 */
static void shift_subscribers_after_slot(unsigned int slot)
{
	unsigned int i;
	for (i = slot; i < nsubs - 1; i++)
		subs[i] = subs[i + 1];
	nsubs--;
}

static int is_process_with_event_call(struct mproc *rmp)
{
	return (rmp->mp_flags & (IN_USE | EVENT_CALL)) == (IN_USE | EVENT_CALL);
}

static void handle_process_at_removed_slot(struct mproc *rmp)
{
	nested++;
	resume_event(rmp);
	nested--;
}

static void update_process_event_subscriber(struct mproc *rmp, unsigned int removed_slot)
{
	if (!is_process_with_event_call(rmp))
		return;
		
	assert(rmp->mp_eventsub != NO_EVENTSUB);
	
	if ((unsigned int)rmp->mp_eventsub == removed_slot) {
		handle_process_at_removed_slot(rmp);
	} else if ((unsigned int)rmp->mp_eventsub > removed_slot) {
		rmp->mp_eventsub--;
	}
}

static void remove_sub(unsigned int slot)
{
	struct mproc *rmp;
	
	shift_subscribers_after_slot(slot);
	
	for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++) {
		update_process_event_subscriber(rmp, slot);
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
	unsigned int mask;

	if (!(mp->mp_flags & PRIV_PROC))
		return EPERM;

	mask = m_in.m_lsys_pm_proceventmask.mask;

	int index = find_subscriber(who_e);
	if (index >= 0)
		return update_subscriber(index, mask);

	if (mask == 0)
		return OK;

	return add_subscriber(who_e, mask);
}

static int
find_subscriber(endpoint_t endpt)
{
	unsigned int i;
	for (i = 0; i < nsubs; i++) {
		if (subs[i].endpt == endpt)
			return i;
	}
	return -1;
}

static int
update_subscriber(unsigned int index, unsigned int mask)
{
	if (mask == 0 && subs[index].waiting == 0)
		remove_sub(index);
	else
		subs[index].mask = mask;
	return OK;
}

static int
add_subscriber(endpoint_t endpt, unsigned int mask)
{
	if (nsubs == __arraycount(subs)) {
		printf("PM: too many process event subscribers!\n");
		return ENOMEM;
	}

	subs[nsubs].endpt = endpt;
	subs[nsubs].mask = mask;
	nsubs++;

	return OK;
}

/*
 * A subscribing service has replied to a process event message from us, or at
 * least that is what should have happened.  First make sure of this, and then
 * resume event handling for the affected process.
 */
#define EVENT_REPLY_SUCCESS SUSPEND
#define INVALID_EVENTSUB NO_EVENTSUB

static int validate_caller_privileges(void)
{
	if (!(mp->mp_flags & PRIV_PROC))
		return ENOSYS;
	return EVENT_REPLY_SUCCESS;
}

static int validate_endpoint(endpoint_t endpt, int *slot)
{
	if (pm_isokendpt(endpt, slot) != OK) {
		printf("PM: proc event reply from %d for invalid endpt %d\n",
		    who_e, endpt);
		return 0;
	}
	return 1;
}

static int validate_event_call(struct mproc *rmp, endpoint_t endpt)
{
	if (!(rmp->mp_flags & EVENT_CALL)) {
		printf("PM: proc event reply from %d for endpt %d, no event\n",
		    who_e, endpt);
		return 0;
	}
	return 1;
}

static int validate_subscription_index(struct mproc *rmp, endpoint_t endpt)
{
	if (rmp->mp_eventsub == INVALID_EVENTSUB ||
	    (unsigned int)rmp->mp_eventsub >= nsubs) {
		printf("PM: proc event reply from %d for endpt %d index %d\n",
		    who_e, endpt, rmp->mp_eventsub);
		return 0;
	}
	return 1;
}

static int validate_sender(unsigned int i, endpoint_t endpt)
{
	if (subs[i].endpt != who_e) {
		printf("PM: proc event reply for %d from %d instead of %d\n",
		    endpt, who_e, subs[i].endpt);
		return 0;
	}
	return 1;
}

static int determine_event_type(struct mproc *rmp, endpoint_t endpt)
{
	if (rmp->mp_flags & EXITING)
		return PROC_EVENT_EXIT;
	if (rmp->mp_flags & UNPAUSED)
		return PROC_EVENT_SIGNAL;
	
	printf("PM: proc event reply from %d for %d, bad flags %x\n",
	    who_e, endpt, rmp->mp_flags);
	return -1;
}

static int validate_event_type(unsigned int event, endpoint_t endpt)
{
	if (m_in.m_pm_lsys_proc_event.event != event) {
		printf("PM: proc event reply from %d for %d for event %d "
		    "instead of %d\n", who_e, endpt,
		    m_in.m_pm_lsys_proc_event.event, event);
		return 0;
	}
	return 1;
}

static void process_subscription_reply(struct mproc *rmp, unsigned int i)
{
	assert(subs[i].waiting > 0);
	subs[i].waiting--;

	if (subs[i].mask == 0 && subs[i].waiting == 0) {
		remove_sub(i);
	} else {
		rmp->mp_eventsub++;
		resume_event(rmp);
	}
}

int
do_proc_event_reply(void)
{
	struct mproc *rmp;
	endpoint_t endpt;
	unsigned int i, event;
	int slot;
	int result;

	assert(nested == 0);

	result = validate_caller_privileges();
	if (result != EVENT_REPLY_SUCCESS)
		return result;

	endpt = m_in.m_pm_lsys_proc_event.endpt;
	
	if (!validate_endpoint(endpt, &slot))
		return EVENT_REPLY_SUCCESS;
	
	rmp = &mproc[slot];
	
	if (!validate_event_call(rmp, endpt))
		return EVENT_REPLY_SUCCESS;
	
	if (!validate_subscription_index(rmp, endpt))
		return EVENT_REPLY_SUCCESS;
	
	i = rmp->mp_eventsub;
	
	if (!validate_sender(i, endpt))
		return EVENT_REPLY_SUCCESS;

	event = determine_event_type(rmp, endpt);
	if (event == -1)
		return EVENT_REPLY_SUCCESS;
	
	if (!validate_event_type(event, endpt))
		return EVENT_REPLY_SUCCESS;

	process_subscription_reply(rmp, i);

	return EVENT_REPLY_SUCCESS;
}

/*
 * Publish a process event to interested subscribers.  The event is determined
 * from the process flags.  In addition, if the event is a process exit, also
 * check if it is a subscribing service that died.
 */
void publish_event(struct mproc *rmp)
{
	assert(nested == 0);
	assert((rmp->mp_flags & (IN_USE | EVENT_CALL)) == IN_USE);
	assert(rmp->mp_eventsub == NO_EVENTSUB);

	handle_exiting_service(rmp);
	initiate_event_processing(rmp);
}

static void handle_exiting_service(struct mproc *rmp)
{
	if (!is_exiting_service(rmp))
		return;

	remove_service_subscription(rmp->mp_endpoint);
}

static int is_exiting_service(struct mproc *rmp)
{
	return (rmp->mp_flags & (PRIV_PROC | EXITING)) == (PRIV_PROC | EXITING);
}

static void remove_service_subscription(endpoint_t endpt)
{
	unsigned int i;

	for (i = 0; i < nsubs; i++) {
		if (subs[i].endpt == endpt) {
			remove_sub(i);
			break;
		}
	}
}

static void initiate_event_processing(struct mproc *rmp)
{
	rmp->mp_flags |= EVENT_CALL;
	rmp->mp_eventsub = 0;
	resume_event(rmp);
}
