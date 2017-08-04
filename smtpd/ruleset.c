/*	$OpenBSD: ruleset.c,v 1.34 2017/02/13 12:23:47 gilles Exp $ */

/*
 * Copyright (c) 2009 Gilles Chehade <gilles@poolp.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "smtpd.h"
#include "log.h"


static int ruleset_check_source(struct table *,
    const struct sockaddr_storage *, int);
static int ruleset_check_mailaddr(struct table *, const struct mailaddr *);

struct rule *
ruleset_match(const struct envelope *evp)
{
	const struct mailaddr		*maddr = &evp->dest;
	const struct sockaddr_storage	*ss = &evp->ss;
	struct rule			*r;
	int				 ret;

	TAILQ_FOREACH(r, env->sc_rules, r_entry) {

		if (r->r_tag[0] != '\0') {
			ret = strcmp(r->r_tag, evp->tag);
			if (ret != 0 && !r->r_nottag)
				continue;
			if (ret == 0 && r->r_nottag)
				continue;
		}

		if ((r->r_wantauth && !r->r_negwantauth) && !(evp->flags & EF_AUTHENTICATED))
			continue;
		if ((r->r_wantauth && r->r_negwantauth) && (evp->flags & EF_AUTHENTICATED))
			continue;

		ret = ruleset_check_source(r->r_sources, ss, evp->flags);
		if (ret == -1) {
			errno = EAGAIN;
			return (NULL);
		}
		if ((ret == 0 && !r->r_notsources) || (ret != 0 && r->r_notsources))
			continue;

		if (r->r_senders) {
			ret = ruleset_check_mailaddr(r->r_senders, &evp->sender);
			if (ret == -1) {
				errno = EAGAIN;
				return (NULL);
			}
			if ((ret == 0 && !r->r_notsenders) || (ret != 0 && r->r_notsenders))
				continue;
		}

		if (r->r_recipients) {
			ret = ruleset_check_mailaddr(r->r_recipients, &evp->dest);
			if (ret == -1) {
				errno = EAGAIN;
				return (NULL);
			}
			if ((ret == 0 && !r->r_notrecipients) || (ret != 0 && r->r_notrecipients))
				continue;
		}

		ret = r->r_destination == NULL ? 1 :
		    table_lookup(r->r_destination, NULL, maddr->domain, K_DOMAIN,
			NULL);
		if (ret == -1) {
			errno = EAGAIN;
			return NULL;
		}
		if ((ret == 0 && !r->r_notdestination) || (ret != 0 && r->r_notdestination))
			continue;

		goto matched;
	}

	errno = 0;
	log_trace(TRACE_RULES, "no rule matched");
	return (NULL);

matched:
	log_trace(TRACE_RULES, "rule matched: %s", rule_to_text(r));
	return r;
}

static int
ruleset_check_source(struct table *table, const struct sockaddr_storage *ss,
    int evpflags)
{
	const char   *key;

	if (evpflags & (EF_AUTHENTICATED | EF_INTERNAL))
		key = "local";
	else
		key = ss_to_text(ss);
	switch (table_lookup(table, NULL, key, K_NETADDR, NULL)) {
	case 1:
		return 1;
	case -1:
		log_warnx("warn: failure to perform a table lookup on table %s",
		    table->t_name);
		return -1;
	default:
		break;
	}

	return 0;
}

static int
ruleset_check_mailaddr(struct table *table, const struct mailaddr *maddr)
{
	const char	*key;

	key = mailaddr_to_text(maddr);
	if (key == NULL)
		return -1;

	switch (table_lookup(table, NULL, key, K_MAILADDR, NULL)) {
	case 1:
		return 1;
	case -1:
		log_warnx("warn: failure to perform a table lookup on table %s",
		    table->t_name);
		return -1;
	default:
		break;
	}
	return 0;
}

/* --- */
static int
ruleset_match_table_lookup(struct table *table, const char *key, enum table_service service)
{
	switch (table_lookup(table, NULL, key, service, NULL)) {
	case 1:
		return 1;
	case -1:
		log_warnx("warn: failure to perform a table lookup on table %s",
		    table->t_name);
		return -1;
	default:
		break;
	}
	return 0;
}

static int
ruleset_match_tag(struct match *m, const struct envelope *evp)
{
	int		ret;
	struct table	*table = table_find(m->tag_table, NULL);

	if (!m->tag)
		return 1;
	
	if ((ret = ruleset_match_table_lookup(table, evp->tag, K_STRING)) < 0)
		return ret;

	return m->tag < 0 ? !ret : ret;
}

static int
ruleset_match_from(struct match *m, const struct envelope *evp)
{
	int		ret;
	const char	*key;
	struct table	*table = table_find(m->from_table, NULL);

	if (!m->from)
		return 1;

	if (m->from_socket) {
		/* XXX - socket needs to be distinguished from "local" */
		return -1;
	}

	/* XXX - socket should also be considered local */
	if (evp->flags & EF_INTERNAL)
		key = "local";
	else
		key = ss_to_text(&evp->ss);

	if ((ret = ruleset_match_table_lookup(table, key, K_NETADDR)) < 0)
		return -1;

	return m->from < 0 ? !ret : ret;
}

static int
ruleset_match_to(struct match *m, const struct envelope *evp)
{
	int		ret;
	struct table	*table = table_find(m->to_table, NULL);

	if (!m->to)
		return 1;

	if ((ret = ruleset_match_table_lookup(table, evp->dest.domain,
		    K_DOMAIN)) < 0)
		return -1;

	return m->to < 0 ? !ret : ret;
}

static int
ruleset_match_smtp_helo(struct match *m, const struct envelope *evp)
{
	int		ret;
	struct table	*table = table_find(m->smtp_helo_table, NULL);

	if (!m->smtp_helo)
		return 1;

	if ((ret = ruleset_match_table_lookup(table, evp->helo, K_DOMAIN)) < 0)
		return -1;

	return m->smtp_helo < 0 ? !ret : ret;
}

static int
ruleset_match_smtp_starttls(struct match *m, const struct envelope *evp)
{
	if (!m->smtp_starttls)
		return 1;

	/* XXX - not until TLS flag is added to envelope */
	return -1;
}

static int
ruleset_match_smtp_auth(struct match *m, const struct envelope *evp)
{
	int	ret;

	if (!m->smtp_auth)
		return 1;

	if (!(evp->flags & EF_AUTHENTICATED))
		ret = 0;
	else if (m->smtp_auth_table) {
		/* XXX - not until smtp_session->username is added to envelope */
		/*
		 * table = table_find(m->from_table, NULL);
		 * key = evp->username;
		 * return ruleset_match_table_lookup(table, key, K_CREDENTIALS);
		 */
		return -1;

	}
	else
		ret = 1;

	return m->smtp_auth < 0 ? !ret : ret;
}

static int
ruleset_match_smtp_mail_from(struct match *m, const struct envelope *evp)
{
	int		ret;
	const char	*key;
	struct table	*table = table_find(m->smtp_mail_from_table, NULL);

	if (!m->smtp_mail_from)
		return 1;

	if ((key = mailaddr_to_text(&evp->sender)) == NULL)
		return -1;
	if ((ret = ruleset_match_table_lookup(table, key, K_MAILADDR)) < 0)
		return -1;

	return m->smtp_mail_from < 0 ? !ret : ret;
}

static int
ruleset_match_smtp_rcpt_to(struct match *m, const struct envelope *evp)
{
	int		ret;
	const char	*key;
	struct table	*table = table_find(m->smtp_rcpt_to_table, NULL);

	if (!m->smtp_rcpt_to)
		return 1;

	if ((key = mailaddr_to_text(&evp->dest)) == NULL)
		return -1;
	if ((ret = ruleset_match_table_lookup(table, key, K_MAILADDR)) < 0)
		return -1;

	return m->smtp_rcpt_to < 0 ? !ret : ret;
}

struct match *
ruleset_match_new(const struct envelope *evp)
{
	struct match	*m;
	int		ret;
	int		i = 0;

#define	MATCH_EVAL(x)				\
	switch ((x)) {				\
	case -1:	goto tempfail;		\
	case 0:		continue;		\
	default:	break;			\
	}
	TAILQ_FOREACH(m, env->sc_matches, entry) {
		++i;
		MATCH_EVAL(ruleset_match_tag(m, evp));
		MATCH_EVAL(ruleset_match_from(m, evp));
		MATCH_EVAL(ruleset_match_to(m, evp));
		MATCH_EVAL(ruleset_match_smtp_helo(m, evp));
		MATCH_EVAL(ruleset_match_smtp_auth(m, evp));
		MATCH_EVAL(ruleset_match_smtp_starttls(m, evp));
		MATCH_EVAL(ruleset_match_smtp_mail_from(m, evp));
		MATCH_EVAL(ruleset_match_smtp_rcpt_to(m, evp));
		goto matched;
	}
#undef	MATCH_EVAL

	errno = 0;
	log_trace(TRACE_RULES, "no rule matched");
	return (NULL);

tempfail:
	errno = EAGAIN;
	log_trace(TRACE_RULES, "temporary failure in processing of a rule");
	return (NULL);

matched:
	log_trace(TRACE_RULES, "rule #%d matched: %s", i, match_to_text(m));
	return m;
}
