/*++
/* NAME
/*	virtual8_maps_find 3
/* SUMMARY
/*	virtual delivery agent map lookups
/* SYNOPSIS
/*	#include <virtual8.h>
/*
/*	const char *virtual8_maps_find(maps, recipient)
/*	MAPS	*maps;
/*	const char *recipient;
/* DESCRIPTION
/*	virtual8_maps_find() does user lookups for the virtual delivery
/*	agent. The code is made available as a library routine so that
/*	other programs can perform compatible queries.
/*
/*	A zero result means that the named user was not found.
/*
/*	Arguments:
/* .IP maps
/*	List of pre-opened lookup tables.
/* .IP recipient
/*	Recipient address. An optional address extension is ignored.
/* DIAGNOSTICS
/*	The dict_errno variable is non-zero in case of problems.
/* BUGS
/*	This code is a temporary solution that implements a hard-coded
/*	lookup strategy. In a future version of Postfix, the lookup
/*	strategy should become configurable.
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
#include <string.h>

/* Utility library. */

#include <msg.h>
#include <mymalloc.h>

/* Global library. */

#include <maps.h>
#include <mail_params.h>
#include <strip_addr.h>
#include <virtual8.h>

/* Application-specific. */

/* virtual8_maps_find - lookup for virtual delivery agent */

const char *virtual8_maps_find(MAPS *maps, const char *recipient)
{
    const char *ratsign;
    const char *result;
    char   *bare;

    /*
     * Look up the address minus the optional extension. This is done first,
     * to avoid hammering the database with extended address lookups, and to
     * have straightforward semantics (extensions are always ignored).
     */
    if (*var_rcpt_delim
     && (bare = strip_addr(recipient, (char **) 0, *var_rcpt_delim)) != 0) {
	result = maps_find(maps, bare, DICT_FLAG_FIXED);
	myfree(bare);
	if (result != 0 || dict_errno != 0)
	    return (result);
    }

    /*
     * Look up the full address.
     */
    result = maps_find(maps, recipient, DICT_FLAG_FIXED);
    if (result != 0 || dict_errno != 0)
	return (result);

    /*
     * Look up the @domain catch-all.
     */
    if ((ratsign = strrchr(recipient, '@')) == 0)
	return (0);
    return (maps_find(maps, ratsign, DICT_FLAG_FIXED));
}