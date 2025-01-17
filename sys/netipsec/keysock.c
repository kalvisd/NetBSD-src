/*	$NetBSD: keysock.c,v 1.72 2024/07/05 04:31:54 rin Exp $	*/
/*	$FreeBSD: keysock.c,v 1.3.2.1 2003/01/24 05:11:36 sam Exp $	*/
/*	$KAME: keysock.c,v 1.25 2001/08/13 20:07:41 itojun Exp $	*/

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: keysock.c,v 1.72 2024/07/05 04:31:54 rin Exp $");

/* This code has derived from sys/net/rtsock.c on FreeBSD2.2.5 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/domain.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/mbuf.h>
#include <sys/protosw.h>
#include <sys/signalvar.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/cpu.h>
#include <sys/syslog.h>

#include <net/raw_cb.h>
#include <net/route.h>

#include <net/pfkeyv2.h>
#include <netipsec/key.h>
#include <netipsec/keysock.h>
#include <netipsec/key_debug.h>

#include <netipsec/ipsec_private.h>

struct key_cb {
	int key_count;
	int any_count;
};
static struct key_cb key_cb;

static struct sockaddr key_dst = {
    .sa_len = 2,
    .sa_family = PF_KEY,
};
static struct sockaddr key_src = {
    .sa_len = 2,
    .sa_family = PF_KEY,
};

static const struct protosw keysw[];

static int key_sendup0(struct rawcb *, struct mbuf *, int, int);

int key_registered_sb_max = (2048 * MHLEN); /* XXX arbitrary */

static kmutex_t *key_so_mtx;
static struct rawcbhead key_rawcb;

void
key_init_so(void)
{

	key_so_mtx = mutex_obj_alloc(MUTEX_DEFAULT, IPL_NONE);
}

static void
key_pr_init(void)
{

	LIST_INIT(&key_rawcb);
}

/*
 * key_output()
 */
static int
key_output(struct mbuf *m, struct socket *so)
{
	struct sadb_msg *msg;
	int len, error = 0;
	int s;

	KASSERT(m != NULL);

	{
		net_stat_ref_t ps = PFKEY_STAT_GETREF();
		_NET_STATINC_REF(ps, PFKEY_STAT_OUT_TOTAL);
		_NET_STATADD_REF(ps, PFKEY_STAT_OUT_BYTES, m->m_pkthdr.len);
		PFKEY_STAT_PUTREF();
	}

	len = m->m_pkthdr.len;
	if (len < sizeof(struct sadb_msg)) {
		PFKEY_STATINC(PFKEY_STAT_OUT_TOOSHORT);
		error = EINVAL;
		goto end;
	}

	if (m->m_len < sizeof(struct sadb_msg)) {
		if ((m = m_pullup(m, sizeof(struct sadb_msg))) == 0) {
			PFKEY_STATINC(PFKEY_STAT_OUT_NOMEM);
			error = ENOBUFS;
			goto end;
		}
	}

	KASSERT((m->m_flags & M_PKTHDR) != 0);

	if (KEYDEBUG_ON(KEYDEBUG_KEY_DUMP))
		kdebug_mbuf(__func__, m);

	msg = mtod(m, struct sadb_msg *);
	PFKEY_STATINC(PFKEY_STAT_OUT_MSGTYPE + msg->sadb_msg_type);
	if (len != PFKEY_UNUNIT64(msg->sadb_msg_len)) {
		PFKEY_STATINC(PFKEY_STAT_OUT_INVLEN);
		error = EINVAL;
		goto end;
	}

	/*XXX giant lock*/
	s = splsoftnet();
	error = key_parse(m, so);
	m = NULL;
	splx(s);
end:
	m_freem(m);
	return error;
}

/*
 * send message to the socket.
 */
static int
key_sendup0(
    struct rawcb *rp,
    struct mbuf *m,
    int promisc,
    int sbprio
)
{
	int error;
	int ok;

	if (promisc) {
		struct sadb_msg *pmsg;

		M_PREPEND(m, sizeof(struct sadb_msg), M_DONTWAIT);
		if (m && m->m_len < sizeof(struct sadb_msg))
			m = m_pullup(m, sizeof(struct sadb_msg));
		if (!m) {
			PFKEY_STATINC(PFKEY_STAT_IN_NOMEM);
			return ENOBUFS;
		}
		m->m_pkthdr.len += sizeof(*pmsg);

		pmsg = mtod(m, struct sadb_msg *);
		memset(pmsg, 0, sizeof(*pmsg));
		pmsg->sadb_msg_version = PF_KEY_V2;
		pmsg->sadb_msg_type = SADB_X_PROMISC;
		pmsg->sadb_msg_len = PFKEY_UNIT64(m->m_pkthdr.len);
		/* pid and seq? */

		PFKEY_STATINC(PFKEY_STAT_IN_MSGTYPE + pmsg->sadb_msg_type);
	}

	if (sbprio == 0)
		ok = sbappendaddr(&rp->rcb_socket->so_rcv,
			       (struct sockaddr *)&key_src, m, NULL);
	else
		ok = sbappendaddrchain(&rp->rcb_socket->so_rcv,
			       (struct sockaddr *)&key_src, m, sbprio);

	if (!ok) {
		log(LOG_WARNING,
		    "%s: couldn't send PF_KEY message to the socket\n",
		    __func__);
		PFKEY_STATINC(PFKEY_STAT_IN_NOMEM);
		m_freem(m);
		/* Don't call soroverflow because we're returning this
		 * error directly to the sender. */
		rp->rcb_socket->so_rcv.sb_overflowed++;
		error = ENOBUFS;
	} else {
		sorwakeup(rp->rcb_socket);
		error = 0;
	}
	return error;
}

/* so can be NULL if target != KEY_SENDUP_ONE */
static int
_key_sendup_mbuf(struct socket *so, struct mbuf *m,
		int target/*, sbprio */)
{
	struct mbuf *n;
	struct keycb *kp;
	int sendup;
	struct rawcb *rp;
	int error = 0;
	int sbprio = 0; /* XXX should be a parameter */

	KASSERT(m != NULL);
	KASSERT(so != NULL || target != KEY_SENDUP_ONE);

	/*
	 * RFC 2367 says ACQUIRE and other kernel-generated messages
	 * are special. We treat all KEY_SENDUP_REGISTERED messages
	 * as special, delivering them to all registered sockets
	 * even if the socket is at or above its so->so_rcv.sb_max limits.
	 * The only constraint is that the  so_rcv data fall below
	 * key_registered_sb_max.
	 * Doing that check here avoids reworking every key_sendup_mbuf()
	 * in the short term. . The rework will be done after a technical
	 * conensus that this approach is appropriate.
 	 */
	if (target == KEY_SENDUP_REGISTERED) {
		sbprio = SB_PRIO_BESTEFFORT;
	}

	{
		net_stat_ref_t ps = PFKEY_STAT_GETREF();
		_NET_STATINC_REF(ps, PFKEY_STAT_IN_TOTAL);
		_NET_STATADD_REF(ps, PFKEY_STAT_IN_BYTES, m->m_pkthdr.len);
		PFKEY_STAT_PUTREF();
	}
	if (m->m_len < sizeof(struct sadb_msg)) {
#if 1
		m = m_pullup(m, sizeof(struct sadb_msg));
		if (m == NULL) {
			PFKEY_STATINC(PFKEY_STAT_IN_NOMEM);
			return ENOBUFS;
		}
#else
		/* don't bother pulling it up just for stats */
#endif
	}
	if (m->m_len >= sizeof(struct sadb_msg)) {
		struct sadb_msg *msg;
		msg = mtod(m, struct sadb_msg *);
		PFKEY_STATINC(PFKEY_STAT_IN_MSGTYPE + msg->sadb_msg_type);
	}

	LIST_FOREACH(rp, &key_rawcb, rcb_list)
	{
		struct socket * kso = rp->rcb_socket;
		if (rp->rcb_proto.sp_family != PF_KEY)
			continue;
		if (rp->rcb_proto.sp_protocol
		 && rp->rcb_proto.sp_protocol != PF_KEY_V2) {
			continue;
		}

		kp = (struct keycb *)rp;

		/*
		 * If you are in promiscuous mode, and when you get broadcasted
		 * reply, you'll get two PF_KEY messages.
		 * (based on pf_key@inner.net message on 14 Oct 1998)
		 */
		if (((struct keycb *)rp)->kp_promisc) {
			if ((n = m_copym(m, 0, (int)M_COPYALL, M_DONTWAIT)) != NULL) {
				(void)key_sendup0(rp, n, 1, 0);
				n = NULL;
			}
		}

		/* the exact target will be processed later */
		if (so && sotorawcb(so) == rp)
			continue;

		sendup = 0;
		switch (target) {
		case KEY_SENDUP_ONE:
			/* the statement has no effect */
			if (so && sotorawcb(so) == rp)
				sendup++;
			break;
		case KEY_SENDUP_ALL:
			sendup++;
			break;
		case KEY_SENDUP_REGISTERED:
			if (kp->kp_registered) {
				if (kso->so_rcv.sb_cc <= key_registered_sb_max)
					sendup++;
			  	else
			  		printf("keysock: "
					       "registered sendup dropped, "
					       "sb_cc %ld max %d\n",
					       kso->so_rcv.sb_cc,
					       key_registered_sb_max);
			}
			break;
		}
		PFKEY_STATINC(PFKEY_STAT_IN_MSGTARGET + target);

		if (!sendup)
			continue;

		if ((n = m_copym(m, 0, (int)M_COPYALL, M_DONTWAIT)) == NULL) {
			m_freem(m);
			PFKEY_STATINC(PFKEY_STAT_IN_NOMEM);
			return ENOBUFS;
		}

		if ((error = key_sendup0(rp, n, 0, 0)) != 0) {
			m_freem(m);
			return error;
		}

		n = NULL;
	}

	/* The 'later' time for processing the exact target has arrived */
	if (so) {
		error = key_sendup0(sotorawcb(so), m, 0, sbprio);
		m = NULL;
	} else {
		error = 0;
		m_freem(m);
	}
	return error;
}

int
key_sendup_mbuf(struct socket *so, struct mbuf *m,
		int target/*, sbprio */)
{
	int error;

	if (so == NULL)
		mutex_enter(key_so_mtx);
	else
		KASSERT(solocked(so));

	error = _key_sendup_mbuf(so, m, target);

	if (so == NULL)
		mutex_exit(key_so_mtx);
	return error;
}

static int
key_attach(struct socket *so, int proto)
{
	struct keycb *kp;
	int s, error;

	KASSERT(sotorawcb(so) == NULL);
	kp = kmem_zalloc(sizeof(*kp), KM_SLEEP);
	kp->kp_raw.rcb_len = sizeof(*kp);
	so->so_pcb = kp;

	s = splsoftnet();

	if (so->so_lock != key_so_mtx) {
		KASSERT(so->so_lock == NULL);
		mutex_obj_hold(key_so_mtx);
		so->so_lock = key_so_mtx;
		solock(so);
	}

	error = raw_attach(so, proto, &key_rawcb);
	if (error) {
		PFKEY_STATINC(PFKEY_STAT_SOCKERR);
		kmem_free(kp, sizeof(*kp));
		so->so_pcb = NULL;
		goto out;
	}

	kp->kp_promisc = kp->kp_registered = 0;

	if (kp->kp_raw.rcb_proto.sp_protocol == PF_KEY) /* XXX: AF_KEY */
		key_cb.key_count++;
	key_cb.any_count++;
	kp->kp_raw.rcb_laddr = &key_src;
	kp->kp_raw.rcb_faddr = &key_dst;
	soisconnected(so);
	so->so_options |= SO_USELOOPBACK;
out:
	KASSERT(solocked(so));
	splx(s);
	return error;
}

static void
key_detach(struct socket *so)
{
	struct keycb *kp = (struct keycb *)sotorawcb(so);
	int s;

	KASSERT(!cpu_softintr_p());
	KASSERT(solocked(so));
	KASSERT(kp != NULL);

	s = splsoftnet();
	if (kp->kp_raw.rcb_proto.sp_protocol == PF_KEY) /* XXX: AF_KEY */
		key_cb.key_count--;
	key_cb.any_count--;
	key_freereg(so);
	raw_detach(so);
	splx(s);
}

static int
key_accept(struct socket *so, struct sockaddr *nam)
{
	KASSERT(solocked(so));

	panic("%s: unsupported", __func__);

	return EOPNOTSUPP;
}

static int
key_bind(struct socket *so, struct sockaddr *nam, struct lwp *l)
{
	KASSERT(solocked(so));

	return EOPNOTSUPP;
}

static int
key_listen(struct socket *so, struct lwp *l)
{
	KASSERT(solocked(so));

	return EOPNOTSUPP;
}

static int
key_connect(struct socket *so, struct sockaddr *nam, struct lwp *l)
{
	KASSERT(solocked(so));

	return EOPNOTSUPP;
}

static int
key_connect2(struct socket *so, struct socket *so2)
{
	KASSERT(solocked(so));

	return EOPNOTSUPP;
}

static int
key_disconnect(struct socket *so)
{
	struct rawcb *rp = sotorawcb(so);
	int s;
        
	KASSERT(solocked(so));
	KASSERT(rp != NULL);

	s = splsoftnet();
	soisdisconnected(so);
	raw_disconnect(rp);
	splx(s);
 
	return 0;                               
}

static int
key_shutdown(struct socket *so)
{
	int s;

	KASSERT(solocked(so));

	/*
	 * Mark the connection as being incapable of further input.
	 */
	s = splsoftnet();
	socantsendmore(so);
	splx(s);

	return 0;
}

static int
key_abort(struct socket *so)
{
	KASSERT(solocked(so));

	panic("%s: unsupported", __func__);

	return EOPNOTSUPP;
}

static int
key_ioctl(struct socket *so, u_long cmd, void *nam, struct ifnet *ifp)
{
	return EOPNOTSUPP;
}

static int
key_stat(struct socket *so, struct stat *ub)
{
	KASSERT(solocked(so));

	return 0;
}

static int
key_peeraddr(struct socket *so, struct sockaddr *nam)
{
	struct rawcb *rp = sotorawcb(so);

	KASSERT(solocked(so));
	KASSERT(rp != NULL);
	KASSERT(nam != NULL);

	if (rp->rcb_faddr == NULL)
		return ENOTCONN;

	raw_setpeeraddr(rp, nam);
	return 0;
}

static int
key_sockaddr(struct socket *so, struct sockaddr *nam)
{
	struct rawcb *rp = sotorawcb(so);

	KASSERT(solocked(so));
	KASSERT(rp != NULL);
	KASSERT(nam != NULL);

	if (rp->rcb_faddr == NULL)
		return ENOTCONN;

	raw_setsockaddr(rp, nam);
	return 0;
}

static int
key_rcvd(struct socket *so, int flags, struct lwp *l)
{
	KASSERT(solocked(so));

	return EOPNOTSUPP;
}

static int
key_recvoob(struct socket *so, struct mbuf *m, int flags)
{
	KASSERT(solocked(so));

	return EOPNOTSUPP;
}

static int
key_send(struct socket *so, struct mbuf *m, struct sockaddr *nam,
    struct mbuf *control, struct lwp *l)
{
	int error = 0;
	int s;

	KASSERT(solocked(so));
	KASSERT(so->so_proto == &keysw[0]);

	s = splsoftnet();
	error = raw_send(so, m, nam, control, l, &key_output);
	splx(s);

	return error;
}

static int
key_sendoob(struct socket *so, struct mbuf *m, struct mbuf *control)
{
	KASSERT(solocked(so));

	m_freem(m);
	m_freem(control);

	return EOPNOTSUPP;
}

static int
key_purgeif(struct socket *so, struct ifnet *ifa)
{

	panic("%s: unsupported", __func__);

	return EOPNOTSUPP;
}

/*
 * Definitions of protocols supported in the KEY domain.
 */

DOMAIN_DEFINE(keydomain);

PR_WRAP_USRREQS(key)
#define	key_attach	key_attach_wrapper
#define	key_detach	key_detach_wrapper
#define	key_accept	key_accept_wrapper
#define	key_bind	key_bind_wrapper
#define	key_listen	key_listen_wrapper
#define	key_connect	key_connect_wrapper
#define	key_connect2	key_connect2_wrapper
#define	key_disconnect	key_disconnect_wrapper
#define	key_shutdown	key_shutdown_wrapper
#define	key_abort	key_abort_wrapper
#define	key_ioctl	key_ioctl_wrapper
#define	key_stat	key_stat_wrapper
#define	key_peeraddr	key_peeraddr_wrapper
#define	key_sockaddr	key_sockaddr_wrapper
#define	key_rcvd	key_rcvd_wrapper
#define	key_recvoob	key_recvoob_wrapper
#define	key_send	key_send_wrapper
#define	key_sendoob	key_sendoob_wrapper
#define	key_purgeif	key_purgeif_wrapper

static const struct pr_usrreqs key_usrreqs = {
	.pr_attach	= key_attach,
	.pr_detach	= key_detach,
	.pr_accept	= key_accept,
	.pr_bind	= key_bind,
	.pr_listen	= key_listen,
	.pr_connect	= key_connect,
	.pr_connect2	= key_connect2,
	.pr_disconnect	= key_disconnect,
	.pr_shutdown	= key_shutdown,
	.pr_abort	= key_abort,
	.pr_ioctl	= key_ioctl,
	.pr_stat	= key_stat,
	.pr_peeraddr	= key_peeraddr,
	.pr_sockaddr	= key_sockaddr,
	.pr_rcvd	= key_rcvd,
	.pr_recvoob	= key_recvoob,
	.pr_send	= key_send,
	.pr_sendoob	= key_sendoob,
	.pr_purgeif	= key_purgeif,
};

static const struct protosw keysw[] = {
    {
	.pr_type = SOCK_RAW,
	.pr_domain = &keydomain,
	.pr_protocol = PF_KEY_V2,
	.pr_flags = PR_ATOMIC|PR_ADDR,
	.pr_ctlinput = raw_ctlinput,
	.pr_usrreqs = &key_usrreqs,
	.pr_init = key_pr_init,
    }
};

struct domain keydomain = {
    .dom_family = PF_KEY,
    .dom_name = "key",
    .dom_init = key_init,
    .dom_protosw = keysw,
    .dom_protoswNPROTOSW = &keysw[__arraycount(keysw)],
};
