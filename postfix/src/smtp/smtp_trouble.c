/*++
/* NAME
/*	smtp_trouble 3
/* SUMMARY
/*	error handler policies
/* SYNOPSIS
/*	#include "smtp.h"
/*
/*	int	smtp_site_fail(state, code, format, ...)
/*	SMTP_STATE *state;
/*	int	code;
/*	char	*format;
/*
/*	int	smtp_mesg_fail(state, code, format, ...)
/*	SMTP_STATE *state;
/*	int	code;
/*	char	*format;
/*
/*	void	smtp_rcpt_fail(state, code, recipient, format, ...)
/*	SMTP_STATE *state;
/*	int	code;
/*	RECIPIENT *recipient;
/*	char	*format;
/*
/*	int	smtp_stream_except(state, exception, description)
/*	SMTP_STATE *state;
/*	int	exception;
/*	char	*description;
/* DESCRIPTION
/*	This module handles all non-fatal errors that can happen while
/*	attempting to deliver mail via SMTP, and implements the policy
/*	of how to deal with the error. Depending on the nature of
/*	the problem, delivery of a single message is deferred, delivery
/*	of all messages to the same domain is deferred, or one or more
/*	recipients are given up as non-deliverable and a bounce log is
/*	updated.
/*
/*	In addition, when an unexpected response code is seen such
/*	as 3xx where only 4xx or 5xx are expected, or any error code
/*	that suggests a syntax error or something similar, the
/*	protocol error flag is set so that the postmaster receives
/*	a transcript of the session. No notification is generated for
/*	what appear to be configuration errors - very likely, they
/*	would suffer the same problem and just cause more trouble.
/*
/*	In case of a soft error, action depends on whether there are
/*	more mail servers (log an informational record only and try
/*	the other servers) or whether this is the final server (log
/*	recipient delivery status records).
/*
/*	In the case of a hard error that affects all recipients,
/*	recipient delivery status records are logged, and the
/*	final server flag is raised so that any remaining mail
/*	servers are skipped.
/*
/*	smtp_site_fail() handles the case where the program fails to
/*	complete the initial SMTP handshake: the server is not reachable,
/*	is not running, does not want talk to us, or we talk to ourselves.
/*	The \fIcode\fR gives an error status code; the \fIformat\fR
/*	argument gives a textual description.
/*	The policy is: soft error, non-final server: log an informational
/*	record why the host is being skipped; soft error, final server:
/*	defer delivery of all remaining recipients; hard error: bounce all
/*	remaining recipients and set the "final server" flag so that any
/*	remaining mail servers will be skipped.
/*	The result is non-zero.
/*
/*	smtp_mesg_fail() handles the case where the smtp server
/*	does not accept the sender address or the message data.
/*	The policy is: soft error, non-final server: log an informational
/*	record why the host is being skipped; soft error, final server:
/*	defer delivery of all remaining recipients; hard error: bounce all
/*	remaining recipients and set the "final server" flag so that any
/*	remaining mail servers will be skipped.
/*	The result is non-zero.
/*
/*	smtp_rcpt_fail() handles the case where a recipient is not
/*	accepted by the server for reasons other than that the server
/*	recipient limit is reached.
/*	The policy is: soft error, non-final server: log an informational
/*	record why the recipient is being skipped; soft error, final server:
/*	defer delivery of this recipient; hard error: bounce this
/*	recipient. This routine does not change the "final server" flag.
/*
/*	smtp_stream_except() handles the exceptions generated by
/*	the smtp_stream(3) module (i.e. timeouts and I/O errors).
/*	The \fIexception\fR argument specifies the type of problem.
/*	The \fIdescription\fR argument describes at what stage of
/*	the SMTP dialog the problem happened.
/*	The policy is: non-final server: log an informational record
/*	with the reason why the host is being skipped; final server:
/*	defer delivery of all remaining recipients.
/*	The result is non-zero.
/* DIAGNOSTICS
/*	Panic: unknown exception code.
/* SEE ALSO
/*	smtp_proto(3) smtp high-level protocol
/*	smtp_stream(3) smtp low-level protocol
/*	defer(3) basic message defer interface
/*	bounce(3) basic message bounce interface
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <stdlib.h>			/* 44BSD stdarg.h uses abort() */
#include <stdarg.h>

/* Utility library. */

#include <msg.h>
#include <vstring.h>
#include <stringops.h>
#include <mymalloc.h>

/* Global library. */

#include <smtp_stream.h>
#include <deliver_request.h>
#include <deliver_completed.h>
#include <bounce.h>
#include <defer.h>
#include <mail_error.h>

/* Application-specific. */

#include "smtp.h"

#define SMTP_SOFT(code) (((code) / 100) == 4)
#define SMTP_HARD(code) (((code) / 100) == 5)

/* smtp_check_code - check response code */

static void smtp_check_code(SMTP_STATE *state, int code)
{

    /*
     * The intention of this stuff is to alert the postmaster when the local
     * Postfix SMTP client screws up, protocol wise. RFC 821 says that x0z
     * replies "refer to syntax errors, syntactically correct commands that
     * don't fit any functional category, and unimplemented or superfluous
     * commands". Unfortunately, this also triggers postmaster notices when
     * remote servers screw up, protocol wise. This is becoming a common
     * problem now that response codes are configured manually as part of
     * anti-UCE systems, by people who aren't aware of RFC details.
     */
    if ((!SMTP_SOFT(code) && !SMTP_HARD(code))
	|| code == 555			/* RFC 1869, section 6.1. */
	|| (code >= 500 && code < 510))
	state->error_mask |= MAIL_ERROR_PROTOCOL;
}

/* smtp_site_fail - skip site, defer all recipients, or bounce all recipients */

int     smtp_site_fail(SMTP_STATE *state, int code, char *format,...)
{
    DELIVER_REQUEST *request = state->request;
    SMTP_SESSION *session = state->session;
    RECIPIENT *rcpt;
    int     status;
    int     nrcpt;
    int     soft_error = SMTP_SOFT(code);
    va_list ap;
    VSTRING *why = vstring_alloc(100);

    /*
     * Initialize.
     */
    va_start(ap, format);
    vstring_vsprintf(why, format, ap);
    va_end(ap);

    /*
     * Don't defer the recipients just yet when there are still more mail
     * servers. Just log something informative to show why we're skipping
     * this host.
     */
    if (soft_error && state->final_server == 0) {
	msg_info("%s: %s", request->queue_id, vstring_str(why));
	state->status |= -1;
    }

    /*
     * Defer or bounce all the remaining recipients and raise the final mail
     * server flag.
     */
    else {
	for (nrcpt = 0; nrcpt < request->rcpt_list.len; nrcpt++) {
	    rcpt = request->rcpt_list.info + nrcpt;
	    if (rcpt->offset == 0)
		continue;
	    status = (soft_error ? defer_append : bounce_append)
		(DEL_REQ_TRACE_FLAGS(request->flags), request->queue_id,
		 rcpt->orig_addr, rcpt->address, rcpt->offset,
		 session ? session->namaddr : "none",
		 request->arrival_time, "%s", vstring_str(why));
	    if (status == 0) {
		deliver_completed(state->src, rcpt->offset);
		rcpt->offset = 0;
	    }
	    state->status |= status;
	}
	if (soft_error && request->hop_status == 0)
	    request->hop_status = mystrdup(vstring_str(why));
	state->final_server = 1;
    }
    smtp_check_code(state, code);

    /*
     * Cleanup.
     */
    vstring_free(why);
    return (-1);
}

/* smtp_mesg_fail - skip site, defer all recipients, or bounce all recipients */

int     smtp_mesg_fail(SMTP_STATE *state, int code, char *format,...)
{
    DELIVER_REQUEST *request = state->request;
    SMTP_SESSION *session = state->session;
    RECIPIENT *rcpt;
    int     status;
    int     nrcpt;
    int     soft_error = SMTP_SOFT(code);
    va_list ap;
    VSTRING *why = vstring_alloc(100);

    /*
     * Initialize.
     */
    va_start(ap, format);
    vstring_vsprintf(why, format, ap);
    va_end(ap);

    /*
     * Don't defer the recipients just yet when there are still more mail
     * servers. Just log something informative to show why we're skipping
     * this host.
     */
    if (soft_error && state->final_server == 0) {
	msg_info("%s: %s", request->queue_id, vstring_str(why));
	state->status |= -1;
    }

    /*
     * Defer or bounce all the remaining recipients and raise the final mail
     * server flag.
     */
    else {
	for (nrcpt = 0; nrcpt < request->rcpt_list.len; nrcpt++) {
	    rcpt = request->rcpt_list.info + nrcpt;
	    if (rcpt->offset == 0)
		continue;
	    status = (soft_error ? defer_append : bounce_append)
		(DEL_REQ_TRACE_FLAGS(request->flags), request->queue_id,
		 rcpt->orig_addr, rcpt->address, rcpt->offset,
		 session->namaddr, request->arrival_time,
		 "%s", vstring_str(why));
	    if (status == 0) {
		deliver_completed(state->src, rcpt->offset);
		rcpt->offset = 0;
	    }
	    state->status |= status;
	}
	state->final_server = 1;
    }
    smtp_check_code(state, code);

    /*
     * Cleanup.
     */
    vstring_free(why);
    return (-1);
}

/* smtp_rcpt_fail - skip, defer, or bounce recipient */

void    smtp_rcpt_fail(SMTP_STATE *state, int code, RECIPIENT *rcpt,
		               char *format,...)
{
    DELIVER_REQUEST *request = state->request;
    SMTP_SESSION *session = state->session;
    int     status;
    int     soft_error = SMTP_SOFT(code);
    va_list ap;

    /*
     * Don't defer this recipient record just yet when there are still more
     * mail servers. Just log something informative to show why we're
     * skipping this recipient now.
     */
    if (soft_error && state->final_server == 0) {
	VSTRING *buf = vstring_alloc(10);

	va_start(ap, format);
	vstring_vsprintf(buf, format, ap);
	va_end(ap);
	msg_info("%s: %s", request->queue_id, vstring_str(buf));
	vstring_free(buf);
	status = -1;
    }

    /*
     * Defer or bounce this specific recipient.
     * 
     * If this is a hard error, we must not raise the final mail server flag. We
     * may still make another SMTP connection to deliver deferred recipients.
     * 
     * If this is a soft error, we got here because the final mail server flag
     * was already set.
     * 
     * So don't touch that final mail server flag!
     */
    else {
	va_start(ap, format);
	status = (soft_error ? vdefer_append : vbounce_append)
	    (DEL_REQ_TRACE_FLAGS(request->flags), request->queue_id,
	     rcpt->orig_addr, rcpt->address, rcpt->offset,
	     session->namaddr, request->arrival_time, format, ap);
	va_end(ap);
	if (status == 0) {
	    deliver_completed(state->src, rcpt->offset);
	    rcpt->offset = 0;
	}
    }
    smtp_check_code(state, code);
    state->status |= status;
}

/* smtp_stream_except - defer domain after I/O problem */

int     smtp_stream_except(SMTP_STATE *state, int code, char *description)
{
    DELIVER_REQUEST *request = state->request;
    SMTP_SESSION *session = state->session;
    RECIPIENT *rcpt;
    int     nrcpt;
    VSTRING *why = vstring_alloc(100);

    /*
     * Initialize.
     */
    switch (code) {
    default:
	msg_panic("smtp_stream_except: unknown exception %d", code);
    case SMTP_ERR_EOF:
	vstring_sprintf(why, "lost connection with %s while %s",
			session->namaddr, description);
	break;
    case SMTP_ERR_TIME:
	vstring_sprintf(why, "conversation with %s timed out while %s",
			session->namaddr, description);
	break;
    }

    /*
     * Don't defer the recipients just yet when there are still more mail
     * servers. Just log why we're abandoning this host.
     */
    if (state->final_server == 0) {
	msg_info("%s: %s", request->queue_id, vstring_str(why));
	state->status |= -1;
    }

    /*
     * Final server. Defer all the remaining recipients.
     */
    else {
	for (nrcpt = 0; nrcpt < request->rcpt_list.len; nrcpt++) {
	    rcpt = request->rcpt_list.info + nrcpt;
	    if (rcpt->offset == 0)
		continue;
	    state->status |= defer_append(DEL_REQ_TRACE_FLAGS(request->flags),
					  request->queue_id,
					  rcpt->orig_addr, rcpt->address,
					  rcpt->offset, session->namaddr,
					  request->arrival_time,
					  "%s", vstring_str(why));
	}
    }

    /*
     * Cleanup.
     */
    vstring_free(why);
    return (-1);
}
