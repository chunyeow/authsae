/*
 * Copyright (c) cozybit Inc., 2011
 *
 *  Copyright holder grants permission for redistribution and use in source
 *  and binary forms, with or without modification, provided that the
 *  following conditions are met:
 *     1. Redistribution of source code must retain the above copyright
 *        notice, this list of conditions, and the following disclaimer
 *        in all source files.
 *     2. Redistribution in binary form must retain the above copyright
 *        notice, this list of conditions, and the following disclaimer
 *        in the documentation and/or other materials provided with the
 *        distribution.
 *
 *  "DISCLAIMER OF LIABILITY
 *
 *  THIS SOFTWARE IS PROVIDED BY DAN HARKINS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INDUSTRIAL LOUNGE BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE."
 *
 */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <net/if.h>
#include <assert.h>
#include <openssl/rand.h>

#include "service.h"
#include "common.h"
#include "ieee802_11.h"
#include "os_glue.h"
#include "sae.h"
#include "ampe.h"

/* Peer link cancel reasons */
#define MESH_LINK_CANCELLED                     52
#define MESH_MAX_NEIGHBORS                      53
#define MESH_CAPABILITY_POLICY_VIOLATION        54
#define MESH_CLOSE_RCVD                         55
#define MESH_MAX_RETRIES                        56
#define MESH_CONFIRM_TIMEOUT                    57
#define MESH_SECURITY_INVALID_GTK               58
#define MESH_SECURITY_INCONSISTENT_PARAMS       59
#define MESH_SECURITY_INVALID_CAPABILITY        60

unsigned char *meshid[32];
unsigned char meshid_len;

struct ampe_state {
    TAILQ_ENTRY(ampe_state) entry;
#define SHA256_DIGEST_LENGTH	32
    unsigned char pmk[SHA256_DIGEST_LENGTH];
    unsigned char kck[SHA256_DIGEST_LENGTH];
    timerid t0;
    timerid t1;
    enum plink_state state;
    struct {
        unsigned char mac[ETH_ALEN];
        unsigned short lid;
    } peer;
    struct {
        unsigned char mac[ETH_ALEN];
        unsigned short lid;
    } local;
    unsigned short reason;
    unsigned short retries;
    void *cookie;
};

/**
 * fsm_restart - restart a mesh peer link finite state machine
 *
 * @cand: mesh peer link to restart
 *
 * */
static inline void fsm_restart(struct ampe_state *cand)
{
    cand->state = PLINK_LISTEN;
    cand->local.lid = cand->peer.lid = cand->reason = 0;
    cand->retries = 0;
}




TAILQ_HEAD(nincompoop, ampe_state) candidates;
extern service_context srvctx;

enum plink_event {
        PLINK_UNDEFINED,
        OPN_ACPT,
        OPN_RJCT,
        OPN_IGNR,
        CNF_ACPT,
        CNF_RJCT,
        CNF_IGNR,
        CLS_ACPT,
        CLS_IGNR
};

struct ampe_state* find_by_address(unsigned char *mac)
{
    struct ampe_state *cand;

    TAILQ_FOREACH(cand, &candidates, entry) {
        if (memcmp(cand->peer.mac, mac, ETH_ALEN) == 0)
            return cand;
    }
    return NULL;
}

static struct ampe_state* create_candidate (unsigned char *her_mac, unsigned char *my_mac, void *cookie)
{
    struct ampe_state *cand;

    if ((cand = (struct ampe_state*) malloc(sizeof(struct ampe_state))) == NULL) {
        sae_debug(0x20, "can't malloc space for candidate!\n");
        return NULL;
    }
    memset(cand, 0, sizeof(cand));
    memcpy(cand->local.mac, my_mac, ETH_ALEN);
    memcpy(cand->peer.mac, her_mac, ETH_ALEN);
    TAILQ_INSERT_TAIL(&candidates, cand, entry);
    cand->state = PLINK_LISTEN;
    return cand;
}

static int plink_free_count() {
    sae_debug(AMPE_DEBUG_CANDIDATES, "TODO: return available peer link slots");
    return 99;
}

static inline u8* start_of_ies(struct ieee80211_mgmt_frame *frame,
    int len, u16 *ie_len)
{
    int offset;
    switch(frame->action.action_code) {
        case PLINK_OPEN:
            offset = 2;
        case PLINK_CONFIRM:
            offset = 4;
        case PLINK_CLOSE:
            offset = 0;
    }
    if (ie_len)
        *ie_len = len - 24 - sizeof(frame->action) - offset;
    return (frame->action.u.var8 + offset);
}

/* Create a short lived candidate structure used to send frames addressed to
 * candidates that don't exist in our candidate list.  These are used for
 * reporting early errors.
 */
static void create_tmp_cand_from_frame(struct ieee80211_mgmt_frame *mgmt, struct ampe_state *cand)
{
        assert(mgmt);
        assert(cand);
        memcpy(cand->peer.mac, mgmt->sa, ETH_ALEN);
        memcpy(cand->local.mac, mgmt->da, ETH_ALEN);
        memcpy(&cand->peer.lid, mgmt->action.u.var8, 2);
        cand->local.lid = 0;
}

static int plink_frame_tx(struct ampe_state *cand, enum
        plink_action_code action, unsigned short reason)
{
        unsigned char *buf;
        struct ieee80211_mgmt_frame *mgmt;
        unsigned char *plen;
        int len;
        struct mesh_peering_ie;
        unsigned char *ies;

        assert(cand);

        /* XXX: calculate the right size */
        len = 400;
        buf = calloc(1, len);
        if (!buf)
                return -1;

        sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: Sending plink action %d\n", action);

        mgmt = (struct ieee80211_mgmt_frame *) buf;
        mgmt->frame_control = htole16((IEEE802_11_FC_TYPE_MGMT << 2 |
                                    IEEE802_11_FC_STYPE_ACTION << 4));

        memcpy(mgmt->da, cand->peer.mac, ETH_ALEN);
        memcpy(mgmt->sa, cand->local.mac, ETH_ALEN);
        memcpy(mgmt->bssid, cand->local.mac, ETH_ALEN);
        mgmt->action.category = IEEE80211_CATEGORY_SELF_PROTECTED;
        mgmt->action.action_code = action;

	    ies = start_of_ies(mgmt, len, NULL);

        /* Add Mesh ID element */
        *ies++ = IEEE80211_EID_MESH_ID;
        *ies++ = meshid_len;
        memcpy((char *) ies, meshid, meshid_len);
        ies += meshid_len;

        /* Add mesh peering */
        *ies++ = IEEE80211_EID_MESH_PEERING;
        plen = ies++;
        memcpy(ies, &cand->local.lid, 2);
        ies += 2;
        *plen = 2;
        if (cand->peer.lid) {
            memcpy(ies, &cand->peer.lid, 2);
            ies += 2;
            *plen += 2;
        }
        if (reason) {
            memcpy(ies, &cand->reason, 2);
            ies += 2;
            *plen += 2;
        }

        /*  TODO: Add PMK field to mesh peering element here */

        /* Add mesh config */
        *ies++ = IEEE80211_EID_MESH_CONFIG;
        *ies++ = 8;
        /*  TODO: IIRC all the defaults are 0. Double check */
        memset(ies, 0, 8);
        ies += 8;

        /*  TODO: Add PMK field to mesh peering element here */

        len = ies - buf;

        if (meshd_write_mgmt((char *)buf, len, cand->cookie) != len) {
            sae_debug(SAE_DEBUG_ERR, "can't send an authentication "
                    "frame to " MACSTR "\n", MAC2STR(cand->peer.mac));
        }
        free(buf);
        return 0;
}

static void fsm_step(struct ampe_state *cand, enum plink_event event)
{
    unsigned short reason = 0;
    le16 plid = 0, llid = 0;

	switch (cand->state) {
	case PLINK_LISTEN:
		switch (event) {
		case CLS_ACPT:
			fsm_restart(cand);
			break;
		case OPN_ACPT:
			cand->state = PLINK_OPN_RCVD;
			cand->peer.lid = plid;
			RAND_bytes((unsigned char *) &llid, 2);
			cand->local.lid = llid;
			//mesh_plink_timer_set(cand, dot11MeshRetryTimeout(sdata));
			plink_frame_tx(cand, PLINK_OPEN, 0);
			plink_frame_tx(cand, PLINK_CONFIRM, 0);
			break;
		default:
			break;
		}
		break;

	case PLINK_OPN_SNT:
		switch (event) {
		case OPN_RJCT:
		case CNF_RJCT:
			reason = htole16(MESH_CAPABILITY_POLICY_VIOLATION);
		case CLS_ACPT:
			if (!reason)
				reason = htole16(MESH_CLOSE_RCVD);
			cand->reason = reason;
			cand->state = PLINK_HOLDING;
			//if (!mod_plink_timer(cand,
			//		     dot11MeshHoldingTimeout(sdata)))
			//	cand->ignore_plink_timer = true;

			llid = cand->local.lid;
			plink_frame_tx(cand, PLINK_CLOSE, reason);
			break;
		case OPN_ACPT:
			/* retry timer is left untouched */
			cand->state = PLINK_OPN_RCVD;
			cand->peer.lid = plid;
			llid = cand->local.lid;
			plink_frame_tx(cand, PLINK_CONFIRM, 0);
			break;
		case CNF_ACPT:
			cand->state = PLINK_CNF_RCVD;
			//if (!mod_plink_timer(cand,
			//		     dot11MeshConfirmTimeout(sdata)))
			//	cand->ignore_plink_timer = true;

			break;
		default:
			break;
		}
		break;

	case PLINK_OPN_RCVD:
		switch (event) {
		case OPN_RJCT:
		case CNF_RJCT:
			reason = htole16(MESH_CAPABILITY_POLICY_VIOLATION);
		case CLS_ACPT:
			if (!reason)
				reason = htole16(MESH_CLOSE_RCVD);
			cand->reason = reason;
			cand->state = PLINK_HOLDING;
			//if (!mod_plink_timer(cand,
			//		     dot11MeshHoldingTimeout(sdata)))
			//	cand->ignore_plink_timer = true;

			llid = cand->local.lid;
			plink_frame_tx(cand, PLINK_CLOSE, reason);
			break;
		case OPN_ACPT:
			llid = cand->local.lid;
			plink_frame_tx(cand, PLINK_CONFIRM, 0);
			break;
		case CNF_ACPT:
			//del_timer(&cand->plink_timer);
			cand->state = PLINK_ESTAB;
			//mesh_plink_inc_estab_count(sdata);
			//ieee80211_bss_info_change_notify(sdata, BSS_CHANGED_BEACON);
            sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink with %pM "
                "ESTABLISHED\n", cand->peer.mac);
			break;
		default:
			break;
		}
		break;

	case PLINK_CNF_RCVD:
		switch (event) {
		case OPN_RJCT:
		case CNF_RJCT:
			reason = htole16(MESH_CAPABILITY_POLICY_VIOLATION);
		case CLS_ACPT:
			if (!reason)
				reason = htole16(MESH_CLOSE_RCVD);
			cand->reason = reason;
			cand->state = PLINK_HOLDING;
			//if (!mod_plink_timer(cand,
			//		     dot11MeshHoldingTimeout(sdata)))
			//	cand->ignore_plink_timer = true;

			llid = cand->local.lid;
			plink_frame_tx(cand, PLINK_CLOSE, reason);
			break;
		case OPN_ACPT:
			//del_timer(&cand->plink_timer);
			cand->state = PLINK_ESTAB;
			//mesh_plink_inc_estab_count(sdata);
			//ieee80211_bss_info_change_notify(sdata, BSS_CHANGED_BEACON);
			sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink with %pM ESTABLISHED\n",
				cand->peer.mac);
			plink_frame_tx(cand, PLINK_CONFIRM, 0);
			break;
		default:
			break;
		}
		break;

	case PLINK_ESTAB:
		switch (event) {
		case CLS_ACPT:
			reason = htole16(MESH_CLOSE_RCVD);
			cand->reason = reason;
			//deactivated = __mesh_plink_deactivate(cand);
			cand->state = PLINK_HOLDING;
			llid = cand->local.lid;
			//mod_plink_timer(cand, dot11MeshHoldingTimeout(sdata));
			//if (deactivated)
		    //	ieee80211_bss_info_change_notify(sdata, BSS_CHANGED_BEACON);
			plink_frame_tx(cand, PLINK_CLOSE, reason);
			break;
		case OPN_ACPT:
			llid = cand->local.lid;
			plink_frame_tx(cand, PLINK_CONFIRM, 0);
			break;
		default:
			break;
		}
		break;
	case PLINK_HOLDING:
		switch (event) {
		case CLS_ACPT:
			//if (del_timer(&cand->plink_timer))
			//	cand->ignore_plink_timer = 1;
			fsm_restart(cand);
			break;
		case OPN_ACPT:
		case CNF_ACPT:
		case OPN_RJCT:
		case CNF_RJCT:
			llid = cand->local.lid;
			reason = cand->reason;
			plink_frame_tx(cand, PLINK_CLOSE, reason);
			break;
		default:
            break;
		}
		break;
	default:
        sae_debug(AMPE_DEBUG_CANDIDATES, "Unsupported event transition %d", event);
		break;
	}
}

#define PLINK_GET_LLID(p) (p + 0)
#define PLINK_GET_PLID(p) (p + 2)


/**
 * start_peer_link - attempt to establish a peer link
 * @peer:      MAC address of the candidate peer
 * @me:        The MAC address of the local interface
 * @cookie:    Opaque cookie that will be returned to the caller along with
 *             frames to be transmitted.
 *
 * Returns 0 or a negative error.
 */
int start_peer_link(unsigned char *peer_mac, unsigned char *me, void *cookie)
{
	le16 llid;
    struct ampe_state *cand;

/* 	if (ask_sae_about_this_candidate) {
 * 		sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: Attempt to peer with non-authed peer\n");
 * 		return -EPERM;
 * 	}
 */

    RAND_bytes((unsigned char *) &llid, 2);
    cand = create_candidate(peer_mac, me, cookie);
	cand->local.lid = llid;
	cand->state = PLINK_OPN_SNT;
	//mesh_plink_timer_set(sta, dot11MeshRetryTimeout(sdata));
	sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: starting establishment "
            "with " MACSTR "\n", MAC2STR(peer_mac));

	return plink_frame_tx(cand, PLINK_OPEN, 0);
}

/**
 * process_ampe_frame - process an ampe frame
 * @frame:     The full frame
 * @len:       The full frame length
 * @me:        The MAC address of the local interface
 * @cookie:    Opaque cookie that will be returned to the caller along with
 *             frames to be transmitted.
 *
 * Returns 0 unless something really horrible happened.  In other words, even
 * the frame could not be processed or it was corrupted, the function still
 * returns 0.
 */
int process_ampe_frame(struct ieee80211_mgmt_frame *mgmt, int len,
                        unsigned char *me, void *cookie)
{
    struct info_elems elems;
    unsigned char ftype;
	struct ampe_state *cand = NULL;
	enum plink_event event;
	int matches_local = 1;
	unsigned char ie_len = 0;
	unsigned short plid = 0, llid = 0, reason;
    unsigned char *ies;
    unsigned short ies_len;
	static const char *mplstates[] = {
		[PLINK_LISTEN] = "LISTEN",
		[PLINK_OPN_SNT] = "OPN-SNT",
		[PLINK_OPN_RCVD] = "OPN-RCVD",
		[PLINK_CNF_RCVD] = "CNF_RCVD",
		[PLINK_ESTAB] = "ESTAB",
		[PLINK_HOLDING] = "HOLDING",
		[PLINK_BLOCKED] = "BLOCKED"
	};

	/* management header, category, action code, mesh id and peering mgmt*/
	if (len < 24 + 1 + 1 + 2 + 2)
		return 0;

	//if (is_multicast_ether_addr(mgmt->da)) {
	//	sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: ignore frame to multicast address");
	//	return 0;
	//}


	ies = start_of_ies(mgmt, len, &ies_len);
	parse_ies(ies, ies_len, &elems);
	if (!elems.mesh_peering) {  // || !elems.rsn) {
		sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: missing necessary peer link ie\n");
		return 0;
	}

	ftype = mgmt->action.action_code;
	ie_len = elems.mesh_peering_len;

    /*  TODO: This hard coded lenghts need reviewing  */
	if ((ftype == PLINK_OPEN && ie_len != 4) ||
	    (ftype == PLINK_CONFIRM && ie_len != 6) ||
	    (ftype == PLINK_CLOSE && ie_len != 6 && ie_len != 10)) {
		sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: incorrect plink ie length %d %d\n",
		    ftype, ie_len);
		return 0;
	}

    if (ftype != PLINK_CLOSE && (!elems.mesh_id || !elems.mesh_config)) {
        sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: missing necessary ie %p %p\n", elems.mesh_id, elems.mesh_config);
        return 0;
    }

	/* Note the lines below are correct, the llid in the frame is the plid
	 * from the point of view of this host.
	 */
 	memcpy(&plid, PLINK_GET_LLID(elems.mesh_peering), 2);
    if (ftype == PLINK_CONFIRM || (ftype == PLINK_CLOSE && ie_len == 10))
        memcpy(&llid, PLINK_GET_PLID(elems.mesh_peering), 2);

	cand = find_by_address(mgmt->sa);
	if (!cand && ftype != PLINK_OPEN) {
		sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: cls or cnf from unknown peer\n");
		return 0;
	}

/* 	if (ask_sae_about_this_candidate) {
 * 		sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: Action frame from non-authed peer\n");
 * 		return;
 * 	}
 */

	if (cand && cand->state == PLINK_BLOCKED) {
		return 0;
	}

	/* Now we will figure out the appropriate event... */
	event = PLINK_UNDEFINED;
//	if (ftype != PLINK_CLOSE && (!mesh_matches_local(&elems, sdata))) {
	if (ftype != PLINK_CLOSE) {
		matches_local = 0;
		switch (ftype) {
		case PLINK_OPEN:
			event = OPN_RJCT;
			break;
		case PLINK_CONFIRM:
			event = CNF_RJCT;
			break;
		case PLINK_CLOSE:
			break;
		}
	}

	if (!cand && !matches_local) {
        struct ampe_state tmp;
		reason = htole16(MESH_CAPABILITY_POLICY_VIOLATION);
        create_tmp_cand_from_frame(mgmt, &tmp);
		plink_frame_tx(&tmp, PLINK_CLOSE, reason);
		return 0;
	} else if (!cand) {
        sae_debug(AMPE_DEBUG_CANDIDATES,
                "Mesh plink: !cand and ftype == PLINK_OPEN\n");
		if (!plink_free_count()) {
			sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink error: no more free plinks\n");
			return 0;
		}

//		rates = ieee80211_sta_get_rates(local, &elems, rx_status->band);
		cand = create_candidate(mgmt->sa, me, cookie);
		if (!cand) {
			sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink error: plink table full\n");
			return 0;
		}
		event = OPN_ACPT;
	} else if (matches_local) {
		switch (ftype) {
		case PLINK_OPEN:
			if (!plink_free_count() ||
			    (cand->peer.lid && cand->peer.lid != plid))
				event = OPN_IGNR;
			else
				event = OPN_ACPT;
			break;
		case PLINK_CONFIRM:
			if (!plink_free_count() ||
			    (cand->local.lid != llid || cand->peer.lid != plid))
				event = CNF_IGNR;
			else
				event = CNF_ACPT;
			break;
		case PLINK_CLOSE:
			if (cand->state == PLINK_ESTAB)
				/* Do not check for llid or plid. This does not
				 * follow the standard but since multiple plinks
				 * per cand are not supported, it is necessary in
				 * order to avoid a livelock when MP A sees an
				 * establish peer link to MP B but MP B does not
				 * see it. This can be caused by a timeout in
				 * B's peer link establishment or B beign
				 * restarted.
				 */
				event = CLS_ACPT;
			else if (cand->peer.lid != plid)
				event = CLS_IGNR;
			else if (ie_len == 7 && cand->local.lid != llid)
				event = CLS_IGNR;
			else
				event = CLS_ACPT;
			break;
		default:
			sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink: unknown frame subtype\n");
			return 0;
		}
	}

	sae_debug(AMPE_DEBUG_CANDIDATES, "Mesh plink (peer, state, llid, plid, event): %pM %s %d %d %d\n",
		mgmt->sa, mplstates[cand->state],
		le16toh(cand->local.lid), le16toh(cand->peer.lid),
		event);
	reason = 0;

    fsm_step(cand, event);

    return 0;
}

int ampe_initialize(unsigned char *mesh_id, unsigned char len)
{
        if (len > 32) {
            sae_debug(SAE_DEBUG_ERR, "AMPE: Invalid meshid len");
            return -1;
        }
        meshid_len = len;
        memcpy(meshid, mesh_id, len);
        TAILQ_INIT(&candidates);
        return 0;
}
