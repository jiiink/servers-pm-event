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
static void resume_event(struct mproc *rmp) {
    message m;
    unsigned int i, event = 0;
    int r;

    assert(rmp->mp_flags & IN_USE);
    assert(rmp->mp_flags & EVENT_CALL);
    assert(rmp->mp_eventsub != NO_EVENTSUB);

    if (rmp->mp_flags & EXITING) {
        event = PROC_EVENT_EXIT;
    } else if (rmp->mp_flags & UNPAUSED) {
        event = PROC_EVENT_SIGNAL;
    } else {
        panic("unknown event for flags %x", rmp->mp_flags);
    }

    for (i = rmp->mp_eventsub; i < nsubs; i++, rmp->mp_eventsub++) {
        if (subs[i].mask & event) {
            memset(&m, 0, sizeof(m));
            m.m_type = PROC_EVENT;
            m.m_pm_lsys_proc_event.endpt = rmp->mp_endpoint;
            m.m_pm_lsys_proc_event.event = event;

            r = asynsend3(subs[i].endpt, &m, AMF_NOREPLY);
            if (r != OK) {
                panic("asynsend failed: %d", r);
            }

            assert(subs[i].waiting < NR_PROCS);
            subs[i].waiting++;
            return;
        }
    }

    rmp->mp_flags &= ~EVENT_CALL;
    rmp->mp_eventsub = NO_EVENTSUB;

    switch (event) {
        case PROC_EVENT_EXIT:
            exit_restart(rmp);
            break;
        case PROC_EVENT_SIGNAL:
            restart_sigs(rmp);
            break;
    }
}

/*
 * Remove a subscriber from the set, forcefully if we have to.  Ensure that
 * any processes currently subject to process event notification are updated
 * accordingly, in a way that no services are skipped for process events.
 */
static void remove_sub(unsigned int slot) {
    if (slot >= nsubs) {
        return;
    }

    memmove(&subs[slot], &subs[slot + 1], (nsubs - slot - 1) * sizeof(subs[0]));
    nsubs--;

    for (struct mproc *rmp = mproc; rmp < &mproc[NR_PROCS]; rmp++) {
        if ((rmp->mp_flags & (IN_USE | EVENT_CALL)) != (IN_USE | EVENT_CALL)) {
            continue;
        }

        assert(rmp->mp_eventsub != NO_EVENTSUB);

        if ((unsigned int)rmp->mp_eventsub == slot) {
            nested++;
            resume_event(rmp);
            nested--;
        } else if ((unsigned int)rmp->mp_eventsub > slot) {
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
int do_proceventmask(void) {
    unsigned int i;
    unsigned int mask;

    if (!(mp->mp_flags & PRIV_PROC)) {
        return EPERM;
    }

    mask = m_in.m_lsys_pm_proceventmask.mask;

    for (i = 0; i < nsubs; i++) {
        if (subs[i].endpt == who_e) {
            if (mask == 0 && subs[i].waiting == 0) {
                remove_sub(i);
            } else {
                subs[i].mask = mask;
            }
            return OK;
        }
    }

    if (mask == 0) {
        return OK;
    }

    if (nsubs >= __arraycount(subs)) {
        fprintf(stderr, "PM: too many process event subscribers!\n");
        return ENOMEM;
    }

    subs[nsubs].endpt = who_e;
    subs[nsubs].mask = mask;
    nsubs++;

    return OK;
}

/*
 * A subscribing service has replied to a process event message from us, or at
 * least that is what should have happened.  First make sure of this, and then
 * resume event handling for the affected process.
 */
#include <assert.h>
#include <stdio.h>

int do_proc_event_reply(void) {
    struct mproc *rmp;
    endpoint_t endpt;
    unsigned int i, event;
    int slot;

    assert(nested == 0);

    if (!(mp->mp_flags & PRIV_PROC)) {
        return ENOSYS;
    }

    endpt = m_in.m_pm_lsys_proc_event.endpt;
    if (pm_isokendpt(endpt, &slot) != OK) {
        fprintf(stderr, "Invalid endpoint: %d from %d\n", endpt, who_e);
        return SUSPEND;
    }

    rmp = &mproc[slot];
    if (!(rmp->mp_flags & EVENT_CALL) || rmp->mp_eventsub == NO_EVENTSUB || 
        (unsigned int)rmp->mp_eventsub >= nsubs || subs[rmp->mp_eventsub].endpt != who_e) {
        fprintf(stderr, "Mismatched proc event reply from %d for endpoint %d\n", who_e, endpt);
        return SUSPEND;
    }

    i = rmp->mp_eventsub;
    if (rmp->mp_flags & EXITING) {
        event = PROC_EVENT_EXIT;
    } else if (rmp->mp_flags & UNPAUSED) {
        event = PROC_EVENT_SIGNAL;
    } else {
        fprintf(stderr, "Bad flags %x from %d for %d\n", rmp->mp_flags, who_e, endpt);
        return SUSPEND;
    }

    if (m_in.m_pm_lsys_proc_event.event != event) {
        fprintf(stderr, "Mismatched event %d from %d for %d, expected %d\n", 
                m_in.m_pm_lsys_proc_event.event, who_e, endpt, event);
        return SUSPEND;
    }

    assert(subs[i].waiting > 0);
    subs[i].waiting--;

    if (subs[i].mask == 0 && subs[i].waiting == 0) {
        remove_sub(i);
    } else {
        rmp->mp_eventsub++;
        resume_event(rmp);
    }

    return SUSPEND;
}

/*
 * Publish a process event to interested subscribers.  The event is determined
 * from the process flags.  In addition, if the event is a process exit, also
 * check if it is a subscribing service that died.
 */
void publish_event(struct mproc * rmp) {
    assert(nested == 0);
    assert((rmp->mp_flags & (IN_USE | EVENT_CALL)) == IN_USE);
    assert(rmp->mp_eventsub == NO_EVENTSUB);

    if ((rmp->mp_flags & (PRIV_PROC | EXITING)) == (PRIV_PROC | EXITING)) {
        for (unsigned int i = 0; i < nsubs; i++) {
            if (subs[i].endpt == rmp->mp_endpoint) {
                remove_sub(i);
                break;
            }
        }
    }

    rmp->mp_flags |= EVENT_CALL;
    rmp->mp_eventsub = 0;
    resume_event(rmp);
}
