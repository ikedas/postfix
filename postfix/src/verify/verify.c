/*++
/* NAME
/*	verify 8
/* SUMMARY
/*	Postfix address verification server
/* SYNOPSIS
/*	\fBverify\fR [generic Postfix daemon options]
/* DESCRIPTION
/*	The Postfix address verification server maintains a record
/*	of what recipient addresses are known to be deliverable or
/*	undeliverable.
/*
/*	Addresses are verified by submitting probe messages to the
/*	Postfix queue. Probe messages are run through all the routing
/*	and rewriting machinery except for final delivery, and are
/*	discarded rather than being deferred or bounced.
/*
/*	Address verification relies on the answer from the nearest
/*	MTA for the specified address, and will therefore not detect
/*	all undeliverable addresses.
/*
/*	This server is designed to run under control by the Postfix
/*	master server. It maintains an optional persistent database.
/*	To avoid being interrupted by "postfix stop" in the middle
/*	of a database update, the process runs in a separate process
/*	group.
/*
/*	This server implements the following requests:
/* .IP "\fBVRFY_ADDR_UPDATE\fI address status text\fR"
/*	Update the status of the specified address.
/* .IP "\fBVRFY_ADDR_QUERY\fI address\fR"
/*	Look up the \fIstatus\fR and \fItext\fR of the specified address.
/*	If the status is unknown, a probe is sent and a default status is
/*	returned.
/* .PP
/*	The server reply status is one of:
/* .IP \fBVRFYSTAT_OK\fR
/*	The request completed normally.
/* .IP \fBVRFYSTAT_BAD\fR
/*	The server rejected the request (bad request name, bad
/*	request parameter value).
/* .IP \fBVRFYSTAT_FAIL\fR
/*	The request failed.
/* .PP
/*	The recipient status is one of:
/* .IP \fBDEL_RCPT_STAT_OK\fR
/*	The address is deliverable.
/* .IP \fBDEL_RCPT_STAT_DEFER\fR
/*	The address is undeliverable due to a temporary problem.
/* .IP \fBDEL_RCPT_STAT_BOUNCE\fR
/*	The address is undeliverable due to a permanent problem.
/* .IP \fBDEL_RCPT_STAT_TODO\fR
/*	The address status is being determined.
/* SECURITY
/* .ad
/* .fi
/*	The address verification server is not security-sensitive. It does
/*	not talk to the network, and it does not talk to local users.
/*	The verify server can run chrooted at fixed low privilege.
/*
/*	The address verification server can be coerced to store
/*	unlimited amounts of garbage. Limiting the cache size
/*	trades one problem (disk space exhaustion) for another
/*	one (poor response time to client requests).
/* DIAGNOSTICS
/*	Problems and transactions are logged to \fBsyslogd\fR(8).
/* BUGS
/*	This prototype server uses synchronous submission for sending
/*	a probe message, which can be slow on a busy machine.
/*
/*	If the persistent database ever gets corrupted then the world
/*	comes to an end and human intervention is needed. This violates
/*	a basic Postfix principle.
/* CONFIGURATION PARAMETERS
/* .ad
/* .fi
/*	See the Postfix \fBmain.cf\fR file for syntax details and for
/*	default values. Use the \fBpostfix reload\fR command after a
/*	configuration change.
/* .IP \fBaddress_verify_map\fR
/*	Optional table for persistent recipient status storage. The file
/*	is opened before the process enters a chroot jail and before
/*	it drops root privileges.
/*	By default, the information is kept in volatile memory,
/*	and is lost after \fBpostfix reload\fR or \fBpostfix stop\fR.
/* .sp
/*	To recover from a corrupted address verification database,
/*	delete the file and do \fBpostfix reload\fR.
/* .IP \fBaddress_verify_sender\fR
/*	The sender address to use for probe messages. Specify an empty
/*	value (\fBaddress_verify_sender =\fR) or \fB<>\fR if you want
/*	to use the null sender address.
/* .IP \fBaddress_verify_positive_expire_time\fR
/*	The amount of time after which a known to be good address expires.
/* .IP \fBaddress_verify_positive_refresh_time\fR
/*	The minimal amount of time after which a proactive probe is sent to
/*	verify that a known to be good address is still good. The address
/*	status is not updated when the probe fails (optimistic caching).
/* .IP \fBaddress_verify_negative_cache\fR
/*	A boolean parameter that controls whether negative probe results
/*	are stored in the address verification cache. When enabled, the
/*	cache may pollute quickly with garbage. When disabled, Postfix
/*	will generate an address probe for every lookup.
/* .IP \fBaddress_verify_negative_expire_time\fR
/*	The amount of time after which a rejected address expires.
/* .IP \fBaddress_verify_negative_refresh_time\fR
/*	The minimal amount of time after which a proactive probe is sent to
/*	verify that a known to be bad address is still bad.
/* SEE ALSO
/*	verify_clnt(3) address verification client
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
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Utility library. */

#include <msg.h>
#include <mymalloc.h>
#include <htable.h>
#include <dict_ht.h>
#include <dict.h>
#include <split_at.h>

/* Global library. */

#include <mail_conf.h>
#include <mail_params.h>
#include <mail_proto.h>
#include <post_mail.h>
#include <verify_clnt.h>

/* Single server skeleton. */

#include <mail_server.h>

/* Application-specific. */

 /*
  * Tunable parameters.
  */
char   *var_verify_map;
int     var_verify_pos_exp;
int     var_verify_pos_try;
int     var_verify_neg_exp;
int     var_verify_neg_try;
char   *var_verify_sender;

 /*
  * State.
  */
static DICT *verify_map;

 /*
  * Silly little macros.
  */
#define STR(x)			vstring_str(x)
#define STREQ(x,y)		(strcmp(x,y) == 0)

 /*
  * The address verification database consists of (address, data) tuples. The
  * format of the data field is "status:probed:updated:text". The meaning of
  * each field is:
  * 
  * status: one of the four recipient status codes (OK, DEFER, BOUNCE or TODO).
  * In the case of TODO, we have no information about the address, and the
  * address is being probed.
  * 
  * probed: if non-zero, the time of the last outstanding address probe. If
  * zero, there is no outstanding address probe.
  * 
  * updated: if non-zero, the time of the last processed address probe. If zero,
  * we have no information about the address, and the address is being
  * probed.
  * 
  * text: descriptive text from delivery agents etc.
  */

 /*
  * Quick test to see status without parsing the whole entry.
  */
#define STATUS_FROM_RAW_ENTRY(e) atoi(e)

/* verify_make_entry - construct table entry */

static void verify_make_entry(VSTRING *buf, int status, long probed,
			              long updated, const char *text)
{
    vstring_sprintf(buf, "%d:%ld:%ld:%s", status, probed, updated, text);
}

/* verify_parse_entry - parse table entry */

static int verify_parse_entry(char *buf, int *status, long *probed,
			              long *updated, char **text)
{
    char   *probed_text;
    char   *updated_text;

    if ((probed_text = split_at(buf, ':')) != 0
	&& (updated_text = split_at(probed_text, ':')) != 0
	&& (*text = split_at(updated_text, ':')) != 0) {
	*probed = atol(probed_text);
	*updated = atol(updated_text);
	*status = atoi(buf);
	if ((*status == DEL_RCPT_STAT_OK
	     || *status == DEL_RCPT_STAT_DEFER
	     || *status == DEL_RCPT_STAT_BOUNCE
	     || *status == DEL_RCPT_STAT_TODO)
	    && (probed || updated))
	    return (0);
    }
    msg_warn("bad address verify table entry: %.100s", buf);
    return (-1);
}

/* verify_stat2name - status to name */

static const char *verify_stat2name(int addr_status)
{
    if (addr_status == DEL_RCPT_STAT_OK)
	return ("deliverable");
    if (addr_status == DEL_RCPT_STAT_DEFER)
	return ("undeliverable");
    if (addr_status == DEL_RCPT_STAT_BOUNCE)
	return ("undeliverable");
    return (0);
}

/* verify_update_service - update address service */

static void verify_update_service(VSTREAM *client_stream)
{
    VSTRING *buf = vstring_alloc(10);
    VSTRING *addr = vstring_alloc(10);
    int     addr_status;
    VSTRING *text = vstring_alloc(10);
    const char *status_name;
    const char *raw_data;
    long    probed;
    long    updated;

    if (attr_scan(client_stream, ATTR_FLAG_STRICT,
		  ATTR_TYPE_STR, MAIL_ATTR_ADDR, addr,
		  ATTR_TYPE_NUM, MAIL_ATTR_ADDR_STATUS, &addr_status,
		  ATTR_TYPE_STR, MAIL_ATTR_WHY, text,
		  ATTR_TYPE_END) == 3) {
	if ((status_name = verify_stat2name(addr_status)) == 0) {
	    msg_warn("bad recipient status %d for recipient %s",
		     addr_status, STR(addr));
	    attr_print(client_stream, ATTR_FLAG_NONE,
		       ATTR_TYPE_NUM, MAIL_ATTR_STATUS, VRFY_STAT_BAD,
		       ATTR_TYPE_END);
	} else {

	    /*
	     * Robustness: don't allow a failed probe to clobber an OK
	     * address before it expires. The failed probe is ignored so that
	     * the address will be re-probed upon the next query. As long as
	     * some probes succeed the address will remain cached as OK.
	     */
	    if (addr_status == DEL_RCPT_STAT_OK
		|| (raw_data = dict_get(verify_map, STR(addr))) == 0
		|| STATUS_FROM_RAW_ENTRY(raw_data) != DEL_RCPT_STAT_OK) {
		probed = 0;
		updated = (long) time((time_t *) 0);
		verify_make_entry(buf, addr_status, probed, updated, STR(text));
		if (msg_verbose)
		    msg_info("PUT %s status=%d probed=%ld updated=%ld text=%s",
			STR(addr), addr_status, probed, updated, STR(text));
		dict_put(verify_map, STR(addr), STR(buf));
	    }
	    attr_print(client_stream, ATTR_FLAG_NONE,
		       ATTR_TYPE_NUM, MAIL_ATTR_STATUS, VRFY_STAT_OK,
		       ATTR_TYPE_END);
	}
    }
    vstring_free(buf);
    vstring_free(addr);
    vstring_free(text);
}

/* verify_query_service - query address status */

static void verify_query_service(VSTREAM *client_stream)
{
    VSTRING *addr = vstring_alloc(10);
    VSTRING *get_buf = 0;
    VSTRING *put_buf = 0;
    const char *raw_data;
    int     addr_status;
    long    probed;
    long    updated;
    char   *text;
    VSTREAM *post;

    if (attr_scan(client_stream, ATTR_FLAG_STRICT,
		  ATTR_TYPE_STR, MAIL_ATTR_ADDR, addr,
		  ATTR_TYPE_END) == 1) {
	long    now = (long) time((time_t *) 0);

	/*
	 * Produce a default record when no usable record exists.
	 * 
	 * If negative caching is disabled, purge an expired record from the
	 * database.
	 * 
	 * XXX Assume that a probe is lost if no response is received in 1000
	 * seconds. If this number is too small the queue will slowly fill up
	 * with delayed probes.
	 * 
	 * XXX Maintain a moving average for the probe turnaround time, and
	 * allow probe "retransmission" when a probe is outstanding for, say
	 * some minimal amount of time (1000 sec) plus several times the
	 * observed probe turnaround time. This causes probing to back off
	 * when the mail system becomes congested.
	 */
#define POSITIVE_ENTRY_EXPIRED(addr_status, updated) \
    (addr_status == DEL_RCPT_STAT_OK && updated + var_verify_pos_exp < now)
#define NEGATIVE_ENTRY_EXPIRED(addr_status, updated) \
    (addr_status != DEL_RCPT_STAT_OK && updated + var_verify_neg_exp < now)
#define PROBE_TTL	1000

	if ((raw_data = dict_get(verify_map, STR(addr))) == 0	/* not found */
	    || ((get_buf = vstring_alloc(10)),
		vstring_strcpy(get_buf, raw_data),	/* malformed */
		verify_parse_entry(STR(get_buf), &addr_status, &probed,
				   &updated, &text) < 0)
	    || (now - probed > PROBE_TTL	/* safe to probe */
		&& (POSITIVE_ENTRY_EXPIRED(addr_status, updated)
		    || NEGATIVE_ENTRY_EXPIRED(addr_status, updated)))) {
	    addr_status = DEL_RCPT_STAT_TODO;
	    probed = 0;
	    updated = 0;
	    text = "Address verification in progress";
	    if (raw_data != 0 && var_verify_neg_cache == 0)
		dict_del(verify_map, STR(addr));
	}
	if (msg_verbose)
	    msg_info("GOT %s status=%d probed=%ld updated=%ld text=%s",
		     STR(addr), addr_status, probed, updated, text);

	/*
	 * Respond to the client.
	 */
	attr_print(client_stream, ATTR_FLAG_NONE,
		   ATTR_TYPE_NUM, MAIL_ATTR_STATUS, VRFY_STAT_OK,
		   ATTR_TYPE_NUM, MAIL_ATTR_ADDR_STATUS, addr_status,
		   ATTR_TYPE_STR, MAIL_ATTR_WHY, text,
		   ATTR_TYPE_END);

	/*
	 * Send a new probe when the information needs to be refreshed.
	 * 
	 * XXX For an initial proof of concept implementation, use synchronous
	 * mail submission. This needs to be made async for high-volume
	 * sites, which makes it even more interesting to eliminate duplicate
	 * queries while a probe is being built.
	 * 
	 * If negative caching is turned off, update the database only when
	 * refreshing an existing entry.
	 */
#define POSITIVE_REFRESH_NEEDED(addr_status, updated) \
    (addr_status == DEL_RCPT_STAT_OK && updated + var_verify_pos_try < now)
#define NEGATIVE_REFRESH_NEEDED(addr_status, updated) \
    (addr_status != DEL_RCPT_STAT_OK && updated + var_verify_neg_try < now)
#define NULL_CLEANUP_FLAGS	0

	if (now - probed > PROBE_TTL
	    && (POSITIVE_REFRESH_NEEDED(addr_status, updated)
		|| NEGATIVE_REFRESH_NEEDED(addr_status, updated))) {
	    if (msg_verbose)
		msg_info("PROBE %s status=%d probed=%ld updated=%ld",
			 STR(addr), addr_status, now, updated);
	    if ((post = post_mail_fopen(strcmp(var_verify_sender, "<>") == 0 ?
					"" : var_verify_sender, STR(addr),
					NULL_CLEANUP_FLAGS,
					DEL_REQ_FLAG_VERIFY)) != 0
		&& post_mail_fclose(post) == 0
		&& (updated != 0 || var_verify_neg_cache != 0)) {
		put_buf = vstring_alloc(10);
		verify_make_entry(put_buf, addr_status, now, updated, text);
		if (msg_verbose)
		    msg_info("PUT %s status=%d probed=%ld updated=%ld text=%s",
			     STR(addr), addr_status, now, updated, text);
		dict_put(verify_map, STR(addr), STR(put_buf));
	    }
	}
    }
    vstring_free(addr);
    if (get_buf)
	vstring_free(get_buf);
    if (put_buf)
	vstring_free(put_buf);
}

/* verify_service - perform service for client */

static void verify_service(VSTREAM *client_stream, char *unused_service,
			           char **argv)
{
    VSTRING *request = vstring_alloc(10);

    /*
     * Sanity check. This service takes no command-line arguments.
     */
    if (argv[0])
	msg_fatal("unexpected command-line argument: %s", argv[0]);

    /*
     * This routine runs whenever a client connects to the socket dedicated
     * to the address verification service. All connection-management stuff
     * is handled by the common code in multi_server.c.
     */
    if (attr_scan(client_stream,
		  ATTR_FLAG_MORE | ATTR_FLAG_STRICT,
		  ATTR_TYPE_STR, MAIL_ATTR_REQ, request,
		  ATTR_TYPE_END) == 1) {
	if (STREQ(STR(request), VRFY_REQ_UPDATE)) {
	    verify_update_service(client_stream);
	} else if (STREQ(STR(request), VRFY_REQ_QUERY)) {
	    verify_query_service(client_stream);
	} else {
	    msg_warn("unrecognized request: \"%s\", ignored", STR(request));
	    attr_print(client_stream, ATTR_FLAG_NONE,
		       ATTR_TYPE_NUM, MAIL_ATTR_STATUS, VRFY_STAT_BAD,
		       ATTR_TYPE_END);
	}
    }
    vstream_fflush(client_stream);
    vstring_free(request);
}

/* post_jail_init - post-jail initialization */

static void post_jail_init(char *unused_name, char **unused_argv)
{

    /*
     * If the database is in volatile memory only, prevent automatic process
     * suicide after a limited number of client requests or after a limited
     * amount of idle time.
     */
    if (*var_verify_map == 0) {
	var_use_limit = 0;
	var_idle_limit = 0;
    }
}

/* pre_jail_init - pre-jail initialization */

static void pre_jail_init(char *unused_name, char **unused_argv)
{
    mode_t  saved_mask;

    /*
     * Keep state in persistent (external) or volatile (internal) map.
     */
#define VERIFY_DICT_OPEN_FLAGS (DICT_FLAG_DUP_REPLACE | DICT_FLAG_SYNC_UPDATE)

    if (*var_verify_map) {
	saved_mask = umask(022);
	verify_map = dict_open(var_verify_map,
			       O_CREAT | O_RDWR,
			       VERIFY_DICT_OPEN_FLAGS);
	(void) umask(saved_mask);
    } else {
	verify_map = dict_ht_open("verify", htable_create(0), myfree);
    }

    /*
     * Never, ever, get killed by a master signal, as that would corrupt the
     * database when we're in the middle of an update.
     */
    setsid();
}

/* main - pass control to the single-threaded skeleton */

int     main(int argc, char **argv)
{
    static CONFIG_STR_TABLE str_table[] = {
	VAR_VERIFY_MAP, DEF_VERIFY_MAP, &var_verify_map, 0, 0,
	VAR_VERIFY_SENDER, DEF_VERIFY_SENDER, &var_verify_sender, 0, 0,
	0,
    };
    static CONFIG_TIME_TABLE time_table[] = {
	VAR_VERIFY_POS_EXP, DEF_VERIFY_POS_EXP, &var_verify_pos_exp, 1, 0,
	VAR_VERIFY_POS_TRY, DEF_VERIFY_POS_TRY, &var_verify_pos_try, 1, 0,
	VAR_VERIFY_NEG_EXP, DEF_VERIFY_NEG_EXP, &var_verify_neg_exp, 1, 0,
	VAR_VERIFY_NEG_TRY, DEF_VERIFY_NEG_TRY, &var_verify_neg_try, 1, 0,
	0,
    };

    multi_server_main(argc, argv, verify_service,
		      MAIL_SERVER_STR_TABLE, str_table,
		      MAIL_SERVER_TIME_TABLE, time_table,
		      MAIL_SERVER_PRE_INIT, pre_jail_init,
		      MAIL_SERVER_POST_INIT, post_jail_init,
		      MAIL_SERVER_SOLITARY,
		      0);
}