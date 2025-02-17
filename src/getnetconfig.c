/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1989 by Sun Microsystems, Inc.
 */

#include "config.h"
#include <pthread.h>
#include <reentrant.h>
#include <sys/cdefs.h>
#include <stdio.h>
#include <errno.h>
#include <netconfig.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <rpc/rpc.h>
#include <unistd.h>

#include "rpc_com.h"

/*
 * The five library routines in this file provide application access to the
 * system network configuration database, /etc/netconfig.  In addition to the
 * netconfig database and the routines for accessing it, the environment
 * variable NETPATH and its corresponding routines in getnetpath.c may also be
 * used to specify the network transport to be used.
 */

/*
 * netconfig errors
 */

#define NC_NONETCONFIG    ENOENT
#define NC_NOMEM          ENOMEM
#define NC_NOTINIT        EINVAL	/* setnetconfig was not called first */
#define NC_BADFILE        EBADF	/* format for netconfig file is bad */
#define NC_NOTFOUND       ENOPROTOOPT	/* specified netid was not found */

/*
 * semantics as strings (should be in netconfig.h)
 */
#define NC_TPI_CLTS_S     "tpi_clts"
#define NC_TPI_COTS_S     "tpi_cots"
#define NC_TPI_COTS_ORD_S "tpi_cots_ord"
#define NC_TPI_RAW_S      "tpi_raw"

/*
 * flags as characters (also should be in netconfig.h)
 */
#define NC_NOFLAG_C       '-'
#define NC_VISIBLE_C      'v'
#define NC_BROADCAST_C    'b'

/*
 * Character used to indicate there is no name-to-address lookup library
 */
#define NC_NOLOOKUP "-"

/* serialize globals */
static mutex_t nc_mtx = MUTEX_INITIALIZER;

static const char *const _nc_errors[] = {
	"Netconfig database not found",
	"Not enough memory",
	"Not initialized",
	"Netconfig database has invalid format",
	"Netid not found in netconfig database"
};

struct netconfig_info {
	int eof;		/* all entries has been read */
	int ref;		/* # of times setnetconfig() has been called */
	struct netconfig_list *head;	/* head of the list */
	struct netconfig_list *tail;	/* last of the list */
};

struct netconfig_list {
	char *linep;		/* hold line read from netconfig */
	struct netconfig *ncp;
	struct netconfig_list *next;
};

struct netconfig_vars {
	int valid; /* token that indicates a valid netconfig_vars */
	int flag;		/* first time flag */
	struct netconfig_list *nc_configs; /* pointer to the current netconfig
					    * entry */
};

#define NC_VALID 0xfeed
#define NC_STORAGE 0xf00d
#define NC_INVALID 0

static int *__nc_error(void);
static int parse_ncp(char *, struct netconfig *);
static struct netconfig *dup_ncp(struct netconfig *);

static FILE *nc_file;		/* for netconfig db */
static struct netconfig_info ni = { 0, 0, NULL, NULL };

#define MAXNETCONFIGLINE    1000

static int *
__nc_error(void)
{
	static pthread_mutex_t nc_lock = MUTEX_INITIALIZER;
	extern thread_key_t nc_key;
	static int nc_error;
	int error, *nc_addr;

	/*
	 * Use the static `nc_error' if we are the main thread
	 * (including non-threaded programs), or if an allocation
	 * fails.
	 */
	if (nc_key == -1) {
		error = 0;
		mutex_lock(&nc_lock);
		if (nc_key == -1)
			error = thr_keycreate(&nc_key, thr_keyfree);
		mutex_unlock(&nc_lock);
		if (error)
			return (&nc_error);
	}
	nc_addr = (int *)thr_getspecific(nc_key);
	if (nc_addr == NULL) {
		nc_addr = (int *)mem_zalloc(sizeof(int));
		if (thr_setspecific(nc_key, (void *)nc_addr) != 0) {
			if (nc_addr)
				mem_free(nc_addr, sizeof(int));
			return (&nc_error);
		}
		*nc_addr = 0;
	}
	return (nc_addr);
}

#define nc_error        (*(__nc_error()))
/*
 * A call to setnetconfig() establishes a /etc/netconfig "session".  A session
 * "handle" is returned on a successful call.  At the start of a session (after
 * a call to setnetconfig()) searches through the /etc/netconfig database will
 * proceed from the start of the file.  The session handle must be passed to
 * getnetconfig() to parse the file.  Each call to getnetconfig() using the
 * current handle will process one subsequent entry in /etc/netconfig.
 * setnetconfig() must be called before the first call to getnetconfig().
 * (Handles are used to allow for nested calls to setnetpath()).
 *
 * A new session is established with each call to setnetconfig(), with a new
 * handle being returned on each call.  Previously established sessions remain
 * active until endnetconfig() is called with that session's handle as an
 * argument.
 *
 * setnetconfig() need *not* be called before a call to getnetconfigent().
 * setnetconfig() returns a NULL pointer on failure (for example, if
 * the netconfig database is not present).
 */
void *
setnetconfig(void)
{
	struct netconfig_vars *nc_vars = (struct netconfig_vars *)
		mem_zalloc(sizeof(struct netconfig_vars));

	/*
	 * For multiple calls, i.e. nc_file is not NULL, we just return the
	 * handle without reopening the netconfig db.
	 */
	mutex_lock(&nc_mtx);
	ni.ref++;

	if (!nc_file) {
		nc_file = fopen(NETCONFIG, "r");
		if (!nc_file) {
			ni.ref--;
			mutex_unlock(&nc_mtx);
			nc_error = NC_NONETCONFIG;
			mem_free(nc_vars, sizeof(*nc_vars));
			return (NULL);
		}
	}
	nc_vars->valid = NC_VALID;
	nc_vars->flag = 0;
	nc_vars->nc_configs = ni.head;
	mutex_unlock(&nc_mtx);
	return ((void *)nc_vars);
}

/*
 * When first called, getnetconfig() returns a pointer to the first entry in
 * the netconfig database, formatted as a struct netconfig.  On each subsequent
 * call, getnetconfig() returns a pointer to the next entry in the database.
 * getnetconfig() can thus be used to search the entire netconfig file.
 * getnetconfig() returns NULL at end of file.
 */

struct netconfig *
getnetconfig(void *handlep)
{
	struct netconfig_vars *ncp = (struct netconfig_vars *)handlep;
	char *stringp;		/* tmp string pointer */
	struct netconfig_list *list;
	struct netconfig *np;

	/*
	 * Verify that handle is valid
	 */
	mutex_lock(&nc_mtx);

	if (ncp == NULL || nc_file == NULL) {
		nc_error = NC_NOTINIT;
		mutex_unlock(&nc_mtx);
		return (NULL);
	}

	switch (ncp->valid) {
	case NC_VALID:
		/*
		 * If entry has already been read into the list,
		 * we return the entry in the linked list.
		 * If this is the first time call, check if there are any
		 * entries in linked list.  If no entries, we need to read the
		 * netconfig db.  If we have been here and the next entry is
		 * there, we just return it.
		 */
		if (ncp->flag == 0) {	/* first time */
			ncp->flag = 1;
			ncp->nc_configs = ni.head;
			if (ncp->nc_configs != NULL) { /* entry already exist */
				mutex_unlock(&nc_mtx);
				return (ncp->nc_configs->ncp);
			}
		} else if (ncp->nc_configs != NULL
			   && ncp->nc_configs->next != NULL) {
			ncp->nc_configs = ncp->nc_configs->next;
			mutex_unlock(&nc_mtx);
			return (ncp->nc_configs->ncp);
		}

		/*
		 * If we cannot find the entry in the list and is end of file,
		 * we give up.
		 */
		if (ni.eof == 1) {
			mutex_unlock(&nc_mtx);
			return (NULL);
		}
		break;
	default:
		nc_error = NC_NOTINIT;
		mutex_unlock(&nc_mtx);
		return (NULL);
	}

	stringp = (char *)mem_zalloc(MAXNETCONFIGLINE);

#ifdef MEM_CHK
	if (malloc_verify() == 0) {
		fprintf(stderr, "memory heap corrupted in getnetconfig\n");
		exit(1);
	}
#endif

	/*
	 * Read a line from netconfig file.
	 */
	do {
		if (fgets(stringp, MAXNETCONFIGLINE, nc_file) == NULL) {
			mem_free(stringp, MAXNETCONFIGLINE);
			ni.eof = 1;
			mutex_unlock(&nc_mtx);
			return (NULL);
		}
	} while (*stringp == '#');

	np = (struct netconfig *)mem_zalloc(sizeof(*np));
	list = (struct netconfig_list *)mem_zalloc(sizeof(*list));

	list->ncp = np;
	list->next = NULL;
	list->ncp->nc_lookups = NULL;
	list->linep = stringp;
	if (parse_ncp(stringp, list->ncp) == -1) {
		mem_free(stringp, MAXNETCONFIGLINE);
		mem_free(np, sizeof(*np));
		mem_free(list, sizeof(*list));
		mutex_unlock(&nc_mtx);
		return (NULL);
	}

	/*
	 * If this is the first entry that's been read, it is the head
	 * of the list.  If not, put the entry at the end of the list.
	 * Reposition the current pointer of the handle to the last
	 * entry in the list.
	 */
	if (ni.head == NULL) {	/* first entry */
		ni.head = ni.tail = list;
	} else {
		ni.tail->next = list;
		ni.tail = ni.tail->next;
	}
	ncp->nc_configs = ni.tail;
	mutex_unlock(&nc_mtx);
	return (ni.tail->ncp);
}

/*
 * endnetconfig() may be called to "unbind" or "close" the netconfig database
 * when processing is complete, releasing resources for reuse.  endnetconfig()
 * may not be called before setnetconfig().  endnetconfig() returns 0 on
 * success and -1 on failure (for example, if setnetconfig() was not called
 * previously).
 */
int
endnetconfig(void *handlep)
{
	struct netconfig_vars *nc_handlep = (struct netconfig_vars *)handlep;
	struct netconfig_list *q, *p;

	/*
	 * Verify that handle is valid
	 */
	if (nc_handlep == NULL
	    || (nc_handlep->valid != NC_VALID
		&& nc_handlep->valid != NC_STORAGE)) {
		nc_error = NC_NOTINIT;
		return (-1);
	}

	/*
	 * Return 0 if anyone still needs it.
	 */
	mutex_lock(&nc_mtx);
	nc_handlep->valid = NC_INVALID;
	nc_handlep->flag = 0;
	nc_handlep->nc_configs = NULL;
	if (--ni.ref > 0) {
		mutex_unlock(&nc_mtx);
		mem_free(nc_handlep, 0);
		return (0);
	}

	/*
	 * Noone needs these entries anymore, then frees them.
	 * Make sure all info in netconfig_info structure has been
	 * reinitialized. */
	q = p = ni.head;
	ni.eof = ni.ref = 0;
	ni.head = NULL;
	ni.tail = NULL;
	while (q) {
		p = q->next;
		if (q->ncp->nc_lookups != NULL)
			mem_free(q->ncp->nc_lookups, 0);
		mem_free(q->ncp, 0);
		mem_free(q->linep, 0);
		mem_free(q, 0);
		q = p;
	}
	fclose(nc_file);
	nc_file = NULL;
	mutex_unlock(&nc_mtx);
	mem_free(nc_handlep, 0);
	return (0);
}

/*
 * getnetconfigent(netid) returns a pointer to the struct netconfig structure
 * corresponding to netid.  It returns NULL if netid is invalid (that is, does
 * not name an entry in the netconfig database).  It returns NULL and sets
 * errno in case of failure (for example, if the netconfig database cannot be
 * opened).
 */

struct netconfig *
getnetconfigent(const char *netid)
{
	FILE *file;		/* NETCONFIG db's file pointer */
	char *linep;		/* holds current netconfig line */
	char *stringp;		/* temporary string pointer */
	struct netconfig *ncp = NULL;	/* returned value */
	struct netconfig_list *list;	/* pointer to cache list */

	nc_error = NC_NOTFOUND;	/* default error. */
	if (netid == NULL || strlen(netid) == 0)
		return (NULL);

	/*
	 * Look up table if the entries have already been read and parsed in
	 * getnetconfig(), then copy this entry into a buffer and return it.
	 * If we cannot find the entry in the current list and there are more
	 * entries in the netconfig db that has not been read, we then read the
	 * db and try find the match netid.
	 * If all the netconfig db has been read and placed into the list and
	 * there is no match for the netid, return NULL.
	 */
	mutex_lock(&nc_mtx);

	if (ni.head != NULL) {
		for (list = ni.head; list; list = list->next) {
			if (strcmp(list->ncp->nc_netid, netid) == 0) {
				ncp = dup_ncp(list->ncp);
				mutex_unlock(&nc_mtx);
				return ncp;
			}
		}
		if (ni.eof == 1) {	/* that's all the entries */
			mutex_unlock(&nc_mtx);
			return (NULL);
		}
	}

	file = fopen(NETCONFIG, "r");
	if (!file) {
		nc_error = NC_NONETCONFIG;
		mutex_unlock(&nc_mtx);
		return (NULL);
	}

	linep = mem_zalloc(MAXNETCONFIGLINE);

	do {
		ptrdiff_t len;
		char *tmpp;	/* tmp string pointer */

		do {
			stringp =
				fgets(linep, MAXNETCONFIGLINE, file);
			if (!stringp)
				break;
		} while (*stringp == '#');

		if (!stringp) /* eof */
			break;

		tmpp = strpbrk(stringp, "\t ");
		if (!tmpp) {
			/* can't parse file */
			nc_error = NC_BADFILE;
			break;
		}

		len = tmpp - stringp;
		if (strlen(netid) == (size_t) len &&
			/* a match */
		    strncmp(stringp, netid, (size_t) len) == 0) {
			ncp = (struct netconfig *)mem_zalloc(sizeof(*ncp));

			ncp->nc_lookups = NULL;
			if (parse_ncp(linep, ncp) == -1) {
				mem_free(ncp, sizeof(*ncp));
				ncp = NULL;
			}
			break;
		}

	} while (stringp != NULL);
	if (!ncp)
		mem_free(linep, MAXNETCONFIGLINE);
	fclose(file);
	mutex_unlock(&nc_mtx);
	return (ncp);
}

/*
 * freenetconfigent(netconfigp) frees the netconfig structure pointed to by
 * netconfigp (previously returned by getnetconfigent()).
 */

void
freenetconfigent(struct netconfig *netconfigp)
{
	if (netconfigp != NULL) {
		mem_free(netconfigp->nc_netid, 0);
			/* holds all netconfigp's strings */
		if (netconfigp->nc_lookups != NULL)
			mem_free(netconfigp->nc_lookups, 0);
		mem_free(netconfigp, sizeof(*netconfigp));
	}
	return;
}

/*
 * Parse line and stuff it in a struct netconfig
 * Typical line might look like:
 * udp tpi_cots vb inet udp /dev/udp /usr/lib/ip.so,/usr/local/ip.so
 *
 * We return -1 if any of the tokens don't parse, or malloc fails.
 *
 * Note that we modify stringp (putting NULLs after tokens) and
 * we set the ncp's string field pointers to point to these tokens within
 * stringp.
 */

static int
parse_ncp(char *stringp,	/* string to parse */
	  struct netconfig *ncp /* where to put results */)
{
	char *tokenp;		/* for processing tokens */
	char *lasts;

	/* nearly anything that breaks is for this reason */
	nc_error = NC_BADFILE;
	stringp[strlen(stringp) - 1] = '\0';	/* get rid of newline */

	/* netid */
	ncp->nc_netid = strtok_r(stringp, "\t ", &lasts);
	if (!ncp->nc_netid)
		return (-1);

	/* semantics */
	tokenp = strtok_r(NULL, "\t ", &lasts);
	if (!tokenp)
		return (-1);

	if (strcmp(tokenp, NC_TPI_COTS_ORD_S) == 0)
		ncp->nc_semantics = NC_TPI_COTS_ORD;
	else if (strcmp(tokenp, NC_TPI_COTS_S) == 0)
		ncp->nc_semantics = NC_TPI_COTS;
	else if (strcmp(tokenp, NC_TPI_CLTS_S) == 0)
		ncp->nc_semantics = NC_TPI_CLTS;
	else if (strcmp(tokenp, NC_TPI_RAW_S) == 0)
		ncp->nc_semantics = NC_TPI_RAW;
	else
		return (-1);

	/* flags */
	tokenp = strtok_r(NULL, "\t ", &lasts);
	if (!tokenp)
		return (-1);

	for (ncp->nc_flag = NC_NOFLAG; *tokenp != '\0'; tokenp++) {
		switch (*tokenp) {
		case NC_NOFLAG_C:
			break;
		case NC_VISIBLE_C:
			ncp->nc_flag |= NC_VISIBLE;
			break;
		case NC_BROADCAST_C:
			ncp->nc_flag |= NC_BROADCAST;
			break;
		default:
			return (-1);
		}
	}
	/* protocol family */
	ncp->nc_protofmly = strtok_r(NULL, "\t ", &lasts);
	if (!ncp->nc_protofmly)
		return (-1);
	/* protocol name */
	ncp->nc_proto = strtok_r(NULL, "\t ", &lasts);
	if (!ncp->nc_proto)
		return (-1);
	/* network device */
	ncp->nc_device = strtok_r(NULL, "\t ", &lasts);
	if (!ncp->nc_device)
		return (-1);
	tokenp = strtok_r(NULL, "\t ", &lasts);
	if (!tokenp)
		return (-1);
	if (strcmp(tokenp, NC_NOLOOKUP) == 0) {
		ncp->nc_nlookups = 0;
		ncp->nc_lookups = NULL;
	} else {
		char *cp;	/* tmp string */

		if (ncp->nc_lookups != NULL)	/* from last visit */
			mem_free(ncp->nc_lookups, 0);	/* XXX */
		/* preallocate one string pointer */
		ncp->nc_lookups = (char **)mem_zalloc(sizeof(char *));
		ncp->nc_nlookups = 0;
		while ((cp = tokenp) != NULL) {
			tokenp = _get_next_token(cp, ',');
			ncp->nc_lookups[(size_t) ncp->nc_nlookups++] = cp;
			/* XXXX ok, this works, but should be fixed */
			ncp->nc_lookups =
				(char **)mem_realloc(
					ncp->nc_lookups,
					(size_t) (ncp->nc_nlookups + 1) *
					sizeof(char *)); /* for next loop */
		}
	}
	return (0);
}

/*
 * Returns a string describing the reason for failure.
 */
char *
nc_sperror(void)
{
	const char *message;

	switch (nc_error) {
	case NC_NONETCONFIG:
		message = _nc_errors[0];
		break;
	case NC_NOMEM:
		message = _nc_errors[1];
		break;
	case NC_NOTINIT:
		message = _nc_errors[2];
		break;
	case NC_BADFILE:
		message = _nc_errors[3];
		break;
	case NC_NOTFOUND:
		message = _nc_errors[4];
		break;
	default:
		message = "Unknown network selection error";
	}
	/* LINTED const castaway */
	return ((char *)message);
}

/*
 * Prints a message onto standard error describing the reason for failure.
 */
void nc_perror(const char *s)
{
	fprintf(stderr, "%s: %s\n", s, nc_sperror());
}

/*
 * Duplicates the matched netconfig buffer.
 */
static struct netconfig *
dup_ncp(struct netconfig *ncp)
{
	struct netconfig *p;
	char *tmp;
	u_int i;

	tmp = mem_zalloc(MAXNETCONFIGLINE);
	p = (struct netconfig *)mem_zalloc(sizeof(struct netconfig));

	/*
	 * First we dup all the data from matched netconfig buffer.  Then we
	 * adjust some of the member pointer to a pre-allocated buffer where
	 * contains part of the data.
	 * To follow the convention used in parse_ncp(), we store all the
	 * necessary information in the pre-allocated buffer and let each
	 * of the netconfig char pointer member point to the right address
	 * in the buffer.
	 */
	*p = *ncp;
	p->nc_netid = (char *)strcpy(tmp, ncp->nc_netid);
	tmp = strchr(tmp, 0) + 1;
	p->nc_protofmly = (char *)strcpy(tmp, ncp->nc_protofmly);
	tmp = strchr(tmp, 0) + 1;
	p->nc_proto = (char *)strcpy(tmp, ncp->nc_proto);
	tmp = strchr(tmp, 0) + 1;
	p->nc_device = (char *)strcpy(tmp, ncp->nc_device);
	p->nc_lookups = (char **)mem_zalloc(p->nc_nlookups + 1);

	for (i = 0; i < p->nc_nlookups; i++) {
		tmp = strchr(tmp, 0) + 1;
		p->nc_lookups[i] = (char *)strcpy(tmp, ncp->nc_lookups[i]);
	}
	return (p);
}
