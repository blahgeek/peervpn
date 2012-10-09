/***************************************************************************
 *   Copyright (C) 2012 by Tobias Volk                                     *
 *   mail@tobiasvolk.de                                                    *
 *                                                                         *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/


#ifndef F_PEERMGT_C
#define F_PEERMGT_C


#include "nodedb.c"
#include "authmgt.c"
#include "packet.c"
#include "dfrag.c"


// Minimum message size supported (without fragmentation).
#define peermgt_MSGSIZE_MIN 1024


// Maximum message size supported (with or without fragmentation).
#define peermgt_MSGSIZE_MAX 8192


// Ping buffer size
#define peermgt_PINGBUF_SIZE 64


// Number of fragment buffers.
#define peermgt_FRAGBUF_COUNT 64


// Maximum packet decode recursion depth
#define peermgt_DECODE_RECURSION_MAX_DEPTH 2


// States.
#define peermgt_STATE_INVALID 0
#define peermgt_STATE_AUTHED 1
#define peermgt_STATE_COMPLETE 2


// Timeouts.
#define peermgt_RECV_TIMEOUT 100
#define peermgt_KEEPALIVE_INTERVAL 10
#define peermgt_NEWCONNECT_INTERVAL 1
#define peermgt_NEWCONNECT_MAX_AGE 604800


// Flags.
#define peermgt_FLAG_USERDATA 0x0001
#define peermgt_FLAG_RELAY 0x0002
#define peermgt_FLAG_F03 0x0004
#define peermgt_FLAG_F04 0x0008
#define peermgt_FLAG_F05 0x0010
#define peermgt_FLAG_F06 0x0020
#define peermgt_FLAG_F07 0x0040
#define peermgt_FLAG_F08 0x0080
#define peermgt_FLAG_F09 0x0100
#define peermgt_FLAG_F10 0x0200
#define peermgt_FLAG_F11 0x0400
#define peermgt_FLAG_F12 0x0800
#define peermgt_FLAG_F13 0x1000
#define peermgt_FLAG_F14 0x2000
#define peermgt_FLAG_F15 0x4000
#define peermgt_FLAG_F16 0x8000


// Constraints.
#if auth_MAXMSGSIZE > peermgt_MSGSIZE_MIN
#error auth_MAXMSGSIZE too big
#endif
#if peermgt_PINGBUF_SIZE > peermgt_MSGSIZE_MIN
#error peermgt_PINGBUF_SIZE too big
#endif


// The peer manager structure.
struct s_peermgt {
	struct s_netid netid;
	struct s_map map;
	struct s_nodedb nodedb;
	struct s_authmgt authmgt;
	struct s_dfrag dfrag;
	struct s_nodekey *nodekey;
	struct s_crypto *ctx;
	struct s_seq_state *seq;
	struct s_peeraddr *remoteaddr;
	int *remoteid;
	int64_t *remoteseq;
	int *remoteflags;
	int *state;
	int *lastrecv;
	int *lastsend;
	int *conntime;
	int localflags;
	unsigned char msgbuf[peermgt_MSGSIZE_MAX];
	unsigned char relaymsgbuf[peermgt_MSGSIZE_MAX];
	unsigned char rrmsgbuf[peermgt_MSGSIZE_MAX];
	int msgsize;
	int msgpeerid;
	struct s_msg outmsg;
	int outmsgpeerid;
	int outmsgbroadcast;
	int outmsgbroadcastcount;
	struct s_msg rrmsg;
	int rrmsgpeerid;
	int rrmsgtype;
	int loopback;
	int fragmentation;
	int fragoutpeerid;
	int fragoutcount;
	int fragoutsize;
	int fragoutpos;
	int lastconnect;
};


// Check if PeerID is valid.
static int peermgtIsValidID(struct s_peermgt *mgt, const int peerid) {
	if(!(peerid < 0)) {
		if(peerid < mapGetMapSize(&mgt->map)) {
			if(mapIsValidID(&mgt->map, peerid)) {
				return 1;
			}
		}
	}
	return 0;
}


// Check if PeerID is active (ready to send/recv data).
static int peermgtIsActiveID(struct s_peermgt *mgt, const int peerid) {
	if(peermgtIsValidID(mgt, peerid)) {
		if(mgt->state[peerid] == peermgt_STATE_COMPLETE) {
			return 1;
		}
	}
	return 0;
}


// Check if PeerID is active and remote (> 0).
static int peermgtIsActiveRemoteID(struct s_peermgt *mgt, const int peerid) {
	return ((peerid > 0) && (peermgtIsActiveID(mgt, peerid)));
}


// Check if PeerID is active, remote (> 0) and matches the specified connection time.
static int peermgtIsActiveRemoteIDCT(struct s_peermgt *mgt, const int peerid, const int conntime) {
	if(peermgtIsActiveRemoteID(mgt, peerid)) {
		if(mgt->conntime[peerid] == conntime) {
			return 1;
		}
		else {
			return 0;
		}
	}
	else {
		return 0;
	}
}


// Return the next valid PeerID.
static int peermgtGetNextID(struct s_peermgt *mgt) {
	return mapGetNextKeyID(&mgt->map);
}


// Get PeerID of NodeID. Returns -1 if it is not found.
static int peermgtGetID(struct s_peermgt *mgt, const struct s_nodeid *nodeid) {
	return mapGetKeyID(&mgt->map, nodeid->id);
}


// Returns PeerID if active PeerID or NodeID is specified. Returns -1 if it is not found or both IDs are specified and don't match the same node.
static int peermgtGetActiveID(struct s_peermgt *mgt, const struct s_nodeid *nodeid, const int peerid) {
	int outpeerid = -1;
	
	if(nodeid != NULL) {
		outpeerid = peermgtGetID(mgt, nodeid);
		if(outpeerid < 0) return -1;
	}
	if(!(peerid < 0)) {
		if(outpeerid < 0) {
			outpeerid = peerid;
		}
		else {
			if(peerid != outpeerid) return -1;
		}
	}
	if(!(outpeerid < 0)) {
		if(peermgtIsActiveID(mgt, outpeerid)) {
			return outpeerid;
		}
	}

	return -1;
}


// Get NodeID of PeerID. Returns 1 on success.
static int peermgtGetNodeID(struct s_peermgt *mgt, struct s_nodeid *nodeid, const int peerid) {
	unsigned char *ret;
	if(peermgtIsValidID(mgt, peerid)) {
		ret = mapGetKeyByID(&mgt->map, peerid);
		memcpy(nodeid->id, ret, nodeid_SIZE);
		return 1;
	}
	else {
		return 0;
	}
}


// Reset the data for an ID.
static void peermgtResetID(struct s_peermgt *mgt, const int peerid) {
	mgt->state[peerid] = peermgt_STATE_INVALID;
	memset(mgt->remoteaddr[peerid].addr, 0, peeraddr_SIZE);
	cryptoSetKeysRandom(&mgt->ctx[peerid], 1);
}


// Register new peer.
static int peermgtNew(struct s_peermgt *mgt, const struct s_nodeid *nodeid, const struct s_peeraddr *addr) {
	int tnow = utilGetTime();
	int peerid = mapAddReturnID(&mgt->map, nodeid->id, &tnow);
	if(!(peerid < 0)) {
		mgt->state[peerid] = peermgt_STATE_AUTHED;
		mgt->remoteaddr[peerid] = *addr;
		mgt->conntime[peerid] = tnow;
		mgt->lastrecv[peerid] = tnow;
		mgt->lastsend[peerid] = tnow;
		seqInit(&mgt->seq[peerid], cryptoRand64());
		mgt->remoteflags[peerid] = 0;
		return peerid;
	}
	return -1;
}


// Unregister a peer using its NodeID.
static void peermgtDelete(struct s_peermgt *mgt, const struct s_nodeid *nodeid) {
	int peerid = peermgtGetID(mgt, nodeid);
	if(peerid > 0) { // don't allow special ID 0 to be deleted.
		mapRemove(&mgt->map, nodeid->id);
		peermgtResetID(mgt, peerid);
	}
}


// Unregister a peer using its ID.
static void peermgtDeleteID(struct s_peermgt *mgt, const int peerid) {
	struct s_nodeid nodeid;
	if(peerid > 0 && peermgtGetNodeID(mgt, &nodeid, peerid)) {
		peermgtDelete(mgt, &nodeid);
	}
}


// Connect to a new peer.
static int peermgtConnect(struct s_peermgt *mgt, const struct s_peeraddr *remote_addr) {
	return authmgtStart(&mgt->authmgt, remote_addr);
}


// Enable/Disable loopback messages.
static void peermgtSetLoopback(struct s_peermgt *mgt, const int enable) {
	if(enable) {
		mgt->loopback = 1;
	}
	else {
		mgt->loopback = 0;
	}
}


// Enable/disable fastauth (ignore send delay after auth status change).
static void peermgtSetFastauth(struct s_peermgt *mgt, const int enable) {
	authmgtSetFastauth(&mgt->authmgt, enable);
}


// Enable/disable packet fragmentation.
static void peermgtSetFragmentation(struct s_peermgt *mgt, const int enable) {
	if(enable) {
		mgt->fragmentation = 1;
	}
	else {
		mgt->fragmentation = 0;
	}
}


// Set flags.
static void peermgtSetFlags(struct s_peermgt *mgt, const int flags) {
	mgt->localflags = flags;
}


// Get single flag.
static int peermgtGetFlag(struct s_peermgt *mgt, const int flag) {
	int f;
	f = (mgt->localflags & flag);
	return (f != 0);
}


// Get single remote flag.
static int peermgtGetRemoteFlag(struct s_peermgt *mgt, const int peerid, const int flag) {
	int f;
	f = (mgt->remoteflags[peerid] & flag);
	return (f != 0);
}


// Generate peerinfo packet.
static void peermgtGenPacketPeerinfo(struct s_packet_data *data, struct s_peermgt *mgt) {
	const int peerinfo_size = (packet_PEERID_SIZE + nodeid_SIZE + peeraddr_SIZE);
	int peerinfo_max = mapGetKeyCount(&mgt->map);
	int peerinfo_count = 0;
	int pos = 4;
	int i = 0;
	int infoid;
	unsigned char infocid[packet_PEERID_SIZE];
	struct s_nodeid infonid;

	while((i < peerinfo_max) && (pos + peerinfo_size < data->pl_buf_size)) {
		infoid = peermgtGetNextID(mgt);
		if((infoid > 0) && (mgt->state[infoid] == peermgt_STATE_COMPLETE) && (!peeraddrIsInternal(&mgt->remoteaddr[infoid]))) {
			utilWriteInt32(infocid, infoid);
			memcpy(&data->pl_buf[pos], infocid, packet_PEERID_SIZE);
			peermgtGetNodeID(mgt, &infonid, infoid);
			memcpy(&data->pl_buf[(pos + packet_PEERID_SIZE)], infonid.id, nodeid_SIZE);
			memcpy(&data->pl_buf[(pos + packet_PEERID_SIZE + nodeid_SIZE)], &mgt->remoteaddr[infoid].addr, peeraddr_SIZE);
			pos = pos + peerinfo_size;
			peerinfo_count++;
		}
		i++;
	}
	utilWriteInt32(data->pl_buf, peerinfo_count);

	data->pl_length = (4 + (peerinfo_count * peerinfo_size));
	data->pl_type = packet_PLTYPE_PEERINFO;
	data->pl_options = 0;
}


// Generate next peer manager packet. Returns length if successful.
static int peermgtGetNextPacketGen(struct s_peermgt *mgt, unsigned char *pbuf, const int pbuf_size, const int tnow, struct s_peeraddr *target) {
	int used = mapGetKeyCount(&mgt->map);
	int len;
	int outlen;
	int fragoutlen;
	int peerid;
	int i;
	int fragcount;
	int fragpos;
	const int plbuf_size = peermgt_MSGSIZE_MIN;
	unsigned char plbuf[plbuf_size];
	struct s_msg authmsg;
	struct s_packet_data data;
	
	// send out user data
	outlen = mgt->outmsg.len;
	fragoutlen = mgt->fragoutsize;
	if(outlen > 0 && (!(fragoutlen > 0))) {
		if(mgt->outmsgbroadcast) { // get PeerID for broadcast message
			do {
				peerid = peermgtGetNextID(mgt);
				mgt->outmsgbroadcastcount++;
			}
			while((!peermgtIsActiveRemoteID(mgt, peerid)) && (mgt->outmsgbroadcastcount < used));
			if(mgt->outmsgbroadcastcount >= used) {
				mgt->outmsgbroadcast = 0;
				mgt->outmsg.len = 0;
			}
		}
		else { // get PeerID for unicast message
			peerid = mgt->outmsgpeerid;
			mgt->outmsg.len = 0;
		}
		if(peermgtIsActiveRemoteID(mgt, peerid)) {  // check if session is active
			if(peermgtGetRemoteFlag(mgt, peerid, peermgt_FLAG_USERDATA)) {
				if((mgt->fragmentation > 0) && (outlen > peermgt_MSGSIZE_MIN)) {
					// start generating fragmented userdata packets
					mgt->fragoutpeerid = peerid;
					mgt->fragoutcount = (((outlen - 1) / peermgt_MSGSIZE_MIN) + 1); // calculate number of fragments
					mgt->fragoutsize = outlen;
					fragoutlen = outlen;
					mgt->fragoutpos = 0;
				}
				else {
					// generate userdata packet
					data.pl_buf = mgt->outmsg.msg;
					data.pl_buf_size = outlen;
					data.peerid = mgt->remoteid[peerid];
					data.seq = ++mgt->remoteseq[peerid];
					data.pl_length = outlen;
					data.pl_type = packet_PLTYPE_USERDATA;
					data.pl_options = 0;
					len = packetEncode(pbuf, pbuf_size, &data, &mgt->ctx[peerid]);
					if(len > 0) {
						mgt->lastsend[peerid] = tnow;
						*target = mgt->remoteaddr[peerid];
						return len;
					}
				}
			}
		}
	}

	// send out fragments
	if(fragoutlen > 0) {
		fragcount = mgt->fragoutcount;
		fragpos = mgt->fragoutpos;
		peerid = mgt->fragoutpeerid;
		if(peermgtIsActiveRemoteID(mgt, peerid)) {  // check if session is active
			// generate fragmented packet
			data.pl_buf = &mgt->outmsg.msg[(fragpos * peermgt_MSGSIZE_MIN)];
			if(fragoutlen > peermgt_MSGSIZE_MIN) {
				// start or middle fragment
				data.pl_buf_size = peermgt_MSGSIZE_MIN;
				data.pl_length = peermgt_MSGSIZE_MIN;
				mgt->fragoutsize = (fragoutlen - peermgt_MSGSIZE_MIN);
			}
			else {
				// end fragment
				data.pl_buf_size = fragoutlen;
				data.pl_length = fragoutlen;
				mgt->fragoutsize = 0;
			}
			data.peerid = mgt->remoteid[peerid];
			data.seq = ++mgt->remoteseq[peerid];
			data.pl_type = packet_PLTYPE_USERDATA_FRAGMENT;
			data.pl_options = (fragcount << 4) | (fragpos);
			len = packetEncode(pbuf, pbuf_size, &data, &mgt->ctx[peerid]);
			mgt->fragoutpos = (fragpos + 1);
			if(len > 0) {
				mgt->lastsend[peerid] = tnow;
				*target = mgt->remoteaddr[peerid];
				return len;
			}
		}
		else {
			// session not active anymore, abort sending fragments
			mgt->fragoutsize = 0;
		}
	}

	// send out request-response packet
	outlen = mgt->rrmsg.len;
	if(outlen > 0) {
		peerid = mgt->rrmsgpeerid;
		mgt->rrmsg.len = 0;
		if((outlen < peermgt_MSGSIZE_MAX) && (peermgtIsActiveRemoteID(mgt, peerid))) {  // check if session is active
			data.pl_buf = mgt->rrmsg.msg;
			data.pl_buf_size = peermgt_MSGSIZE_MAX;
			data.pl_length = outlen;
			data.pl_type = mgt->rrmsgtype;
			data.pl_options = 0;
			data.peerid = mgt->remoteid[peerid];
			data.seq = ++mgt->remoteseq[peerid];

			len = packetEncode(pbuf, pbuf_size, &data, &mgt->ctx[peerid]);
			if(len > 0) {
				mgt->lastsend[peerid] = tnow;
				*target = mgt->remoteaddr[peerid];
				return len;
			}
		}
	}

	// send keepalive to peers
	for(i=0; i<used; i++) {
		peerid = peermgtGetNextID(mgt);
		if(peerid > 0) {
			if((tnow - mgt->lastrecv[peerid]) < peermgt_RECV_TIMEOUT) { // check if session has expired
				if(mgt->state[peerid] == peermgt_STATE_COMPLETE) {  // check if session is active
					if((tnow - mgt->lastsend[peerid]) > peermgt_KEEPALIVE_INTERVAL) { // check if session needs keepalive packet
						data.pl_buf = plbuf;
						data.pl_buf_size = plbuf_size;
						data.peerid = mgt->remoteid[peerid];
						data.seq = ++mgt->remoteseq[peerid];
						peermgtGenPacketPeerinfo(&data, mgt);
						len = packetEncode(pbuf, pbuf_size, &data, &mgt->ctx[peerid]);
						if(len > 0) {
							mgt->lastsend[peerid] = tnow;
							*target = mgt->remoteaddr[peerid];
							return len;
						}
					}
				}
			}
			else {
				peermgtDeleteID(mgt, peerid);
			}
		}
	}
	
	// send auth manager message
	if(authmgtGetNextMsg(&mgt->authmgt, &authmsg, target)) {
		data.pl_buf = authmsg.msg;
		data.pl_buf_size = authmsg.len;
		data.peerid = 0;
		data.seq = 0;
		data.pl_length = authmsg.len;
		if(data.pl_length > 0) {
			data.pl_type = packet_PLTYPE_AUTH;
			data.pl_options = 0;
			len = packetEncode(pbuf, pbuf_size, &data, &mgt->ctx[0]);
			if(len > 0) {
				mgt->lastsend[0] = tnow;
				return len;
			}
		}
	}
	
	// connect new peer
	if((authmgtUsedSlotCount(&mgt->authmgt) < (authmgtSlotCount(&mgt->authmgt) / 2)) && ((tnow - mgt->lastconnect) > peermgt_NEWCONNECT_INTERVAL)) {
		i = nodedbNextID(&mgt->nodedb, peermgt_NEWCONNECT_MAX_AGE, 0, 1);
		if(!(i < 0)) {
			if(peermgtGetID(mgt, nodedbGetNodeID(&mgt->nodedb, i)) < 0) { // check if node is already connected
				if(peermgtConnect(mgt, nodedbGetNodeAddress(&mgt->nodedb, i))) { // try to connect
					nodedbUpdate(&mgt->nodedb, nodedbGetNodeID(&mgt->nodedb, i), NULL, 0, 0, 1);
					mgt->lastconnect = tnow;
				}
			}
		}
	}
	
	return 0;
}


// Get next peer manager packet. Also encapsulates packets for relaying if necessary. Returns length if successful.
static int peermgtGetNextPacket(struct s_peermgt *mgt, unsigned char *pbuf, const int pbuf_size, struct s_peeraddr *target) {
	int tnow;
	int ready;
	int len;
	int outlen;
	int relayid;
	int relayct;
	int relaypeerid;
	struct s_packet_data data;
	tnow = utilGetTime();
	ready = 0;
	outlen = peermgtGetNextPacketGen(mgt, pbuf, pbuf_size, tnow, target);
	while((outlen > 0) && (ready == 0)) {
		if(!peeraddrIsInternal(target)) {
			// address is external, packet is ready for sending
			ready = 1;
		}
		else {
			if(((packet_PEERID_SIZE + outlen) < peermgt_MSGSIZE_MAX) && (peeraddrGetIndirect(target, &relayid, &relayct, &relaypeerid))) {
				// address is indirect, encapsulate packet for relaying
				if(peermgtIsActiveRemoteIDCT(mgt, relayid, relayct)) {
					// generate relay-in packet
					utilWriteInt32(&mgt->relaymsgbuf[0], relaypeerid);
					memcpy(&mgt->relaymsgbuf[packet_PEERID_SIZE], pbuf, outlen);
					data.pl_buf = mgt->relaymsgbuf;
					data.pl_buf_size = packet_PEERID_SIZE + outlen;
					data.peerid = mgt->remoteid[relayid];
					data.seq = ++mgt->remoteseq[relayid];
					data.pl_length = packet_PEERID_SIZE + outlen;
					data.pl_type = packet_PLTYPE_RELAY_IN;
					data.pl_options = 0;
					
					// encode relay-in packet
					len = packetEncode(pbuf, pbuf_size, &data, &mgt->ctx[relayid]);
					if(len > 0) {
						mgt->lastsend[relayid] = tnow;
						*target = mgt->remoteaddr[relayid];
						outlen = len;
					}
					else {
						outlen = 0;
					}
				}
				else {
					outlen = 0;
				}
			}
			else {
				outlen = 0;
			}
		}
	}
	return outlen;
}


// Decode auth packet
static int peermgtDecodePacketAuth(struct s_peermgt *mgt, const struct s_packet_data *data, const struct s_peeraddr *source_addr) {
	int tnow = utilGetTime();
	struct s_authmgt *authmgt = &mgt->authmgt;
	struct s_nodeid peer_nodeid;
	int peerid;
	int dupid;
	int64_t remoteflags = 0;
	
	if(authmgtDecodeMsg(authmgt, data->pl_buf, data->pl_length, source_addr)) {
		if(authmgtGetAuthedPeerNodeID(authmgt, &peer_nodeid)) {
			dupid = peermgtGetID(mgt, &peer_nodeid);
			if(dupid < 0) {
				// Create new PeerID.
				peerid = peermgtNew(mgt, &peer_nodeid, source_addr);
			}
			else {
				// Don't replace active existing session.
				peerid = -1;
			}
			if(peerid > 0) {
				// NodeID gets accepted here.
				authmgtAcceptAuthedPeer(authmgt, peerid, seqGet(&mgt->seq[peerid]), mgt->localflags);
			}
			else {
				// Reject authentication attempt because local PeerID could not be generated.
				authmgtRejectAuthedPeer(authmgt);
			}
		}
		if(authmgtGetCompletedPeerNodeID(authmgt, &peer_nodeid)) {
			peerid = peermgtGetID(mgt, &peer_nodeid);
			if((peerid > 0) && (mgt->state[peerid] >= peermgt_STATE_AUTHED) && (authmgtGetCompletedPeerLocalID(authmgt)) == peerid) {
				// Node data gets completed here.
				authmgtGetCompletedPeerAddress(authmgt, &mgt->remoteid[peerid], &mgt->remoteaddr[peerid]);
				authmgtGetCompletedPeerSessionKeys(authmgt, &mgt->ctx[peerid]);
				authmgtGetCompletedPeerConnectionParams(authmgt, &mgt->remoteseq[peerid], &remoteflags);
				mgt->remoteflags[peerid] = remoteflags;
				mgt->state[peerid] = peermgt_STATE_COMPLETE;
				mgt->lastrecv[peerid] = tnow;
				nodedbUpdate(&mgt->nodedb, &peer_nodeid, &mgt->remoteaddr[peerid], 1, 1, 0);
			}
			authmgtFinishCompletedPeer(authmgt);
		}
		return 1;
	}
	else {
		return 0;
	}
}


// Decode peerinfo packet
static int peermgtDecodePacketPeerinfo(struct s_peermgt *mgt, const struct s_packet_data *data) {
	const int peerinfo_size = (packet_PEERID_SIZE + nodeid_SIZE + peeraddr_SIZE);
	struct s_nodeid nodeid;
	struct s_peeraddr addr;
	int peerinfo_count;
	int localid;
	int pos;
	int i;
	int64_t r;
	if(data->pl_length > 4) {
		peerinfo_count = utilReadInt32(data->pl_buf);
		if(peerinfo_count > 0 && (((peerinfo_count * peerinfo_size) + 4) <= data->pl_length)) {
			r = (abs(cryptoRand64()) % peerinfo_count); // randomly select a peer
			for(i=0; i<peerinfo_count; i++) {
				pos = (4 + (r * peerinfo_size));
				memcpy(nodeid.id, &data->pl_buf[(pos + (packet_PEERID_SIZE))], nodeid_SIZE);
				memcpy(addr.addr, &data->pl_buf[(pos + (packet_PEERID_SIZE + nodeid_SIZE))], peeraddr_SIZE);
				localid = peermgtGetID(mgt, &nodeid);
				if(localid < 0) { // check if we are already connected to this NodeID
					nodedbUpdate(&mgt->nodedb, &nodeid, &addr, 1, 0, 0);
				}
				else {
					if(localid > 0) {
						nodedbUpdate(&mgt->nodedb, &nodeid, &mgt->remoteaddr[localid], 1, 1, 0);
					}
				}
				r = ((r + 1) % peerinfo_count);
			}
			return 1;
		}
	}
	return 0;
}


// Decode ping packet
static int peermgtDecodePacketPing(struct s_peermgt *mgt, const struct s_packet_data *data) {
	int len = data->pl_length;
	if(len == peermgt_PINGBUF_SIZE) {
		memcpy(mgt->rrmsg.msg, data->pl_buf, peermgt_PINGBUF_SIZE);
		mgt->rrmsgpeerid = data->peerid;
		mgt->rrmsgtype = packet_PLTYPE_PONG;
		mgt->rrmsg.len = peermgt_PINGBUF_SIZE;
		return 1;
	}
	return 0;
}


// Decode pong packet
static int peermgtDecodePacketPong(struct s_peermgt *mgt, const struct s_packet_data *data) {
	int len = data->pl_length;
	unsigned char pingbuf[peermgt_PINGBUF_SIZE];
	if(len == peermgt_PINGBUF_SIZE) {
		// not implemented yet
		memcpy(pingbuf, data->pl_buf, peermgt_PINGBUF_SIZE);
	}
	return 0;
}


// Decode relay-in packet
static int peermgtDecodePacketRelayIn(struct s_peermgt *mgt, const struct s_packet_data *data) {
	int targetpeerid;
	int len = data->pl_length;
	
	if((len > 4) && (len < (peermgt_MSGSIZE_MAX - 4))) {
		targetpeerid = utilReadInt32(data->pl_buf);
		if(peermgtIsActiveRemoteID(mgt, targetpeerid)) {
			utilWriteInt32(&mgt->rrmsg.msg[0], data->peerid);
			memcpy(&mgt->rrmsg.msg[4], &data->pl_buf[4], (len - 4));
			mgt->rrmsgpeerid = targetpeerid;
			mgt->rrmsgtype = packet_PLTYPE_RELAY_OUT;
			mgt->rrmsg.len = len;
			return 1;
		}
	}
	
	return 0;
}


// Decode fragmented packet
static int peermgtDecodeUserdataFragment(struct s_peermgt *mgt, struct s_packet_data *data) {
	int fragcount = (data->pl_options >> 4);
	int fragpos = (data->pl_options & 0x0F);
	int64_t fragseq = (data->seq - (int64_t)fragpos);
	int peerid = data->peerid;
	int id = dfragAssemble(&mgt->dfrag, mgt->conntime[peerid], peerid, fragseq, data->pl_buf, data->pl_length, fragpos, fragcount);
	int len;
	if(!(id < 0)) {
		len = dfragLength(&mgt->dfrag, id);
		if(len > 0 && len <= data->pl_buf_size) {
			memcpy(data->pl_buf, dfragGet(&mgt->dfrag, id), len); // temporary solution, should be replaced by a zero-copy method later
			dfragClear(&mgt->dfrag, id);
			data->pl_length = len;
			return 1;
		}
		else {
			dfragClear(&mgt->dfrag, id);
			data->pl_length = 0;
			return 0;
		}
	}
	else {
		return 0;
	}
}


// Decode input packet recursively. Decapsulates relayed packets if necessary.
static int peermgtDecodePacketRecursive(struct s_peermgt *mgt, const unsigned char *packet, const int packet_len, const struct s_peeraddr *source_addr, const int tnow, const int depth) {
	int ret;
	int peerid;
	struct s_packet_data data = { .pl_buf_size = peermgt_MSGSIZE_MAX, .pl_buf = mgt->msgbuf };
	struct s_peeraddr indirect_addr;
	ret = 0;
	if(packet_len > (packet_PEERID_SIZE + packet_HMAC_SIZE) && (depth < peermgt_DECODE_RECURSION_MAX_DEPTH)) {
		peerid = packetGetPeerID(packet);
		if(peermgtIsActiveID(mgt, peerid)) {
			if(peerid > 0) {
				// packet has an active PeerID
				mgt->msgsize = 0;
				if(packetDecode(&data, packet, packet_len, &mgt->ctx[peerid], &mgt->seq[peerid]) > 0) {
					if((data.pl_length > 0) && (data.pl_length < peermgt_MSGSIZE_MAX)) {
						switch(data.pl_type) {
							case packet_PLTYPE_USERDATA:
								if(peermgtGetFlag(mgt, peermgt_FLAG_USERDATA)) {
									ret = 1;
									mgt->msgsize = data.pl_length;
									mgt->msgpeerid = data.peerid;
								}
								else {
									ret = 0;
								}
								break;
							case packet_PLTYPE_USERDATA_FRAGMENT:
								if(peermgtGetFlag(mgt, peermgt_FLAG_USERDATA)) {
									ret = peermgtDecodeUserdataFragment(mgt, &data);
									if(ret > 0) {
										mgt->msgsize = data.pl_length;
										mgt->msgpeerid = data.peerid;
									}
								}
								else {
									ret = 0;
								}
								break;
							case packet_PLTYPE_PEERINFO:
								ret = peermgtDecodePacketPeerinfo(mgt, &data);
								break;
							case packet_PLTYPE_PING:
								ret = peermgtDecodePacketPing(mgt, &data);
								break;
							case packet_PLTYPE_PONG:
								ret = peermgtDecodePacketPong(mgt, &data);
								break;
							case packet_PLTYPE_RELAY_IN:
								if(peermgtGetFlag(mgt, peermgt_FLAG_RELAY)) {
									ret = peermgtDecodePacketRelayIn(mgt, &data);
								}
								else {
									ret = 0;
								}
								break;
							case packet_PLTYPE_RELAY_OUT:
								if(data.pl_length > packet_PEERID_SIZE) {
									memcpy(mgt->relaymsgbuf, &data.pl_buf[4], (data.pl_length - packet_PEERID_SIZE)); 
									peeraddrSetIndirect(&indirect_addr, peerid, mgt->conntime[peerid], utilReadInt32(&data.pl_buf[0])); // generate indirect PeerAddr
									ret = peermgtDecodePacketRecursive(mgt, mgt->relaymsgbuf, (data.pl_length - packet_PEERID_SIZE), &indirect_addr, tnow, (depth + 1)); // decode decapsulated packet
								}
								break;
							default:
								ret = 0;
								break;
						}
						if(ret > 0) {
							mgt->lastrecv[peerid] = tnow;
							mgt->remoteaddr[peerid] = *source_addr;
							return 1;
						}
					}
				}
			}
			else if(peerid == 0) {
				// packet has an anonymous PeerID
				if(packetDecode(&data, packet, packet_len, &mgt->ctx[0], NULL)) {
					switch(data.pl_type) {
						case packet_PLTYPE_AUTH:
							return peermgtDecodePacketAuth(mgt, &data, source_addr);
						default:
							return 0;
					}
				}
			}
		}
	}
	return 0;
}


// Decode input packet. Returns 1 on success.
static int peermgtDecodePacket(struct s_peermgt *mgt, const unsigned char *packet, const int packet_len, const struct s_peeraddr *source_addr) {
	int tnow;
	tnow = utilGetTime();
	return peermgtDecodePacketRecursive(mgt, packet, packet_len, source_addr, tnow, 0);
}


// Return received user data. Return 1 if successful.
static int peermgtRecvUserdata(struct s_peermgt *mgt, struct s_msg *recvmsg, struct s_nodeid *fromnodeid, int *frompeerid) {
	if((mgt->msgsize > 0) && (recvmsg != NULL)) {
		recvmsg->msg = mgt->msgbuf;
		recvmsg->len = mgt->msgsize;
		if(fromnodeid != NULL) peermgtGetNodeID(mgt, fromnodeid, mgt->msgpeerid);
		if(frompeerid != NULL) *frompeerid = mgt->msgpeerid;
		mgt->msgsize = 0;
		return 1;
	}
	else {
		return 0;
	}
}


// Send user data. Return 1 if successful.
static int peermgtSendUserdata(struct s_peermgt *mgt, const struct s_msg *sendmsg, const struct s_nodeid *tonodeid, const int topeerid) {
	int outpeerid;

	if(sendmsg != NULL) {
		if((sendmsg->len > 0) && (sendmsg->len <= peermgt_MSGSIZE_MAX)) {
			outpeerid = peermgtGetActiveID(mgt, tonodeid, topeerid);
			if(outpeerid >= 0) {
				if(outpeerid > 0) {
					// message goes out
					mgt->outmsg.msg = sendmsg->msg;
					mgt->outmsg.len = sendmsg->len;
					mgt->outmsgpeerid = outpeerid;
					return 1;
				}
				else {
					// message goes to loopback
					if(mgt->loopback) {
						memcpy(mgt->msgbuf, sendmsg->msg, sendmsg->len);
						mgt->msgsize = sendmsg->len;
						mgt->msgpeerid = outpeerid;
						return 1;
					}
				}
			}
		}
	}
	return 0;
}


// Send user data to all connected peers. Return 1 if successful.
static int peermgtSendBroadcastUserdata(struct s_peermgt *mgt, const struct s_msg *sendmsg) {
	if(sendmsg != NULL) {
		if((sendmsg->len > 0) && (sendmsg->len <= peermgt_MSGSIZE_MAX)) {
			mgt->outmsg.msg = sendmsg->msg;
			mgt->outmsg.len = sendmsg->len;
			mgt->outmsgpeerid = -1;
			mgt->outmsgbroadcast = 1;
			mgt->outmsgbroadcastcount = 0;
			return 1;
		}
	}
	return 0;
}


// Send ping. Return 1 if successful.
static int peermgtSendPing(struct s_peermgt *mgt, const struct s_nodeid *tonodeid, const int topeerid) {
	int outpeerid;
	unsigned char pingbuf[peermgt_PINGBUF_SIZE];

	outpeerid = peermgtGetActiveID(mgt, tonodeid, topeerid);

	if(outpeerid > 0) {
		cryptoRand(pingbuf, 64); // generate ping message
		memcpy(mgt->rrmsg.msg, pingbuf, peermgt_PINGBUF_SIZE);
		mgt->rrmsgpeerid = outpeerid;
		mgt->rrmsgtype = packet_PLTYPE_PING;
		mgt->rrmsg.len = peermgt_PINGBUF_SIZE;
		return 1;
	}

	return 0;
}


// Set NetID from network name.
static int peermgtSetNetID(struct s_peermgt *mgt, const char *netname, const int netname_len) {
	return netidSet(&mgt->netid, netname, netname_len);
}


// Set shared group password.
static int peermgtSetPassword(struct s_peermgt *mgt, const char *password, const int password_len) {
	return cryptoSetSessionKeysFromPassword(&mgt->ctx[0], (const unsigned char *)password, password_len, crypto_AES256, crypto_SHA256);
}


// Initialize peer manager object.
static int peermgtInit(struct s_peermgt *mgt) {
	const char *defaultpw = "default";
	int i;
	int s = mapGetMapSize(&mgt->map);
	struct s_peeraddr empty_addr;
	struct s_nodeid *local_nodeid = &mgt->nodekey->nodeid;
	
	mgt->msgsize = 0;
	mgt->loopback = 0;
	mgt->outmsg.len = 0;
	mgt->outmsgbroadcast = 0;
	mgt->outmsgbroadcastcount = 0;
	mgt->rrmsg.len = 0;
	mgt->rrmsgpeerid = 0;
	mgt->fragoutpeerid = 0;
	mgt->fragoutcount = 0;
	mgt->fragoutsize = 0;
	mgt->fragoutpos = 0;
	mgt->localflags = 0;
	
	for(i=0; i<s; i++) {
		mgt->state[i] = peermgt_STATE_INVALID;
	}

	memset(empty_addr.addr, 0, peeraddr_SIZE);
	mapInit(&mgt->map);
	authmgtReset(&mgt->authmgt);
	nodedbInit(&mgt->nodedb);
	nodedbSetMaxAge(&mgt->nodedb, peermgt_NEWCONNECT_MAX_AGE);

	if(peermgtNew(mgt, local_nodeid, &empty_addr) == 0) { // ID 0 should always represent local NodeID
		if(peermgtGetID(mgt, local_nodeid) == 0) {
			if(peermgtSetNetID(mgt, defaultpw, 7) && peermgtSetPassword(mgt, defaultpw, 7)) {
				mgt->state[0] = peermgt_STATE_COMPLETE;
				return 1;
			}
		}
	}
	
	return 0;
}


// Generate peer manager status report.
static void peermgtStatus(struct s_peermgt *mgt, char *report, const int report_len) {
	int tnow = utilGetTime();
	int pos = 0;
	int size = mapGetMapSize(&mgt->map);
	int maxpos = (((size + 2) * (156)) + 1);
	unsigned char infoid[packet_PEERID_SIZE];
	unsigned char infostate[1];
	unsigned char infoflags[2];
	unsigned char timediff[4];
	struct s_nodeid nodeid;
	int i = 0;
	
	if(maxpos > report_len) { maxpos = report_len; }
	
	memcpy(&report[pos], "PeerID    NodeID                                                            Address                                       Status  LastPkt   SessAge   Flag", 154);
	pos = pos + 154;
	report[pos++] = '\n';
	
	while(i < size && pos < maxpos) {
		if(peermgtGetNodeID(mgt, &nodeid, i)) {
			utilWriteInt32(infoid, i);
			utilByteArrayToHexstring(&report[pos], ((packet_PEERID_SIZE * 2) + 2), infoid, packet_PEERID_SIZE);
			pos = pos + (packet_PEERID_SIZE * 2);
			report[pos++] = ' ';
			report[pos++] = ' ';
			utilByteArrayToHexstring(&report[pos], ((nodeid_SIZE * 2) + 2), nodeid.id, nodeid_SIZE);
			pos = pos + (nodeid_SIZE * 2);
			report[pos++] = ' ';
			report[pos++] = ' ';
			utilByteArrayToHexstring(&report[pos], ((peeraddr_SIZE * 2) + 2), mgt->remoteaddr[i].addr, peeraddr_SIZE);
			pos = pos + (peeraddr_SIZE * 2);
			report[pos++] = ' ';
			report[pos++] = ' ';
			infostate[0] = mgt->state[i];
			utilByteArrayToHexstring(&report[pos], 4, infostate, 1);
			pos = pos + 2;
			report[pos++] = ' ';
			report[pos++] = ' ';
			utilWriteInt32(timediff, (tnow - mgt->lastrecv[i]));
			utilByteArrayToHexstring(&report[pos], 10, timediff, 4);
			pos = pos + 8;
			report[pos++] = ' ';
			report[pos++] = ' ';
			utilWriteInt32(timediff, (tnow - mgt->conntime[i]));
			utilByteArrayToHexstring(&report[pos], 10, timediff, 4);
			pos = pos + 8;
			report[pos++] = ' ';
			report[pos++] = ' ';
			utilWriteInt16(infoflags, mgt->remoteflags[i]);
			utilByteArrayToHexstring(&report[pos], 6, infoflags, 2);
			pos = pos + 4;
			report[pos++] = '\n';
		}
		i++;
	}
	report[pos++] = '\0';
}


// Create peer manager object.
static int peermgtCreate(struct s_peermgt *mgt, const int peer_slots, const int auth_slots, struct s_nodekey *local_nodekey, struct s_dh_state *dhstate) {
	int tnow = utilGetTime();
	const char *defaultid = "default";
	int *conntime_mem;
	int *state_mem;
	int *lastsend_mem;
	int *lastrecv_mem;
	struct s_peeraddr *remoteaddr_mem;
	int *remoteid_mem;
	struct s_crypto *ctx_mem;
	int64_t *remoteseq_mem;
	int *remoteflags_mem;
	struct s_seq_state *seq_mem;

	if((peer_slots > 0) && (auth_slots > 0) && (peermgtSetNetID(mgt, defaultid, 7))) {
		conntime_mem = malloc(sizeof(int) * (peer_slots + 1));
		if(conntime_mem != NULL) {
			state_mem = malloc(sizeof(int) * (peer_slots + 1));
			if(state_mem != NULL) {
				lastsend_mem = malloc(sizeof(int) * (peer_slots + 1));
				if(lastsend_mem != NULL) {
					lastrecv_mem = malloc(sizeof(int) * (peer_slots + 1));
					if(lastrecv_mem != NULL) {
						remoteaddr_mem = malloc(sizeof(struct s_peeraddr) * (peer_slots + 1));
						if(remoteaddr_mem != NULL) {
							remoteid_mem = malloc(sizeof(int) * (peer_slots + 1));
							if(remoteid_mem != NULL) {
								ctx_mem = malloc(sizeof(struct s_crypto) * (peer_slots + 1));
								if(ctx_mem != NULL) {
									if(cryptoCreate(ctx_mem, (peer_slots + 1))) {
										remoteflags_mem = malloc(sizeof(int) * (peer_slots + 1));
										if(remoteflags_mem) {
											remoteseq_mem = malloc(sizeof(int64_t) * (peer_slots + 1));
											if(remoteseq_mem) {
												seq_mem = malloc(sizeof(struct s_seq_state) * (peer_slots + 1));
												if(seq_mem != NULL) {
													if(dfragCreate(&mgt->dfrag, peermgt_MSGSIZE_MIN, peermgt_FRAGBUF_COUNT)) {
														if(authmgtCreate(&mgt->authmgt, &mgt->netid, auth_slots, local_nodekey, dhstate)) {
															if(nodedbCreate(&mgt->nodedb, ((peer_slots * 8) + 1))) {
																if(mapCreate(&mgt->map, (peer_slots + 1), nodeid_SIZE, 1)) {
																	mgt->nodekey = local_nodekey;
																	mgt->conntime = conntime_mem;
																	mgt->state = state_mem;
																	mgt->lastsend = lastsend_mem;
																	mgt->lastrecv = lastrecv_mem;
																	mgt->remoteaddr = remoteaddr_mem;
																	mgt->remoteid = remoteid_mem;
																	mgt->ctx = ctx_mem;
																	mgt->remoteseq = remoteseq_mem;
																	mgt->remoteflags = remoteflags_mem;
																	mgt->seq = seq_mem;
																	mgt->lastconnect = tnow;
																	mgt->rrmsg.msg = mgt->rrmsgbuf;
																	if(peermgtInit(mgt)) {
																		return 1;
																	}
																	mgt->nodekey = NULL;
																	mgt->state = NULL;
																	mgt->lastsend = NULL;
																	mgt->lastrecv = NULL;
																	mgt->remoteaddr = NULL;
																	mgt->remoteid = NULL;
																	mgt->ctx = NULL;
																	mgt->remoteseq = NULL;
																	mgt->seq = NULL;
																	mapDestroy(&mgt->map);
																}
																nodedbDestroy(&mgt->nodedb);
															}
															authmgtDestroy(&mgt->authmgt);
														}
														dfragDestroy(&mgt->dfrag);
													}
													free(seq_mem);
												}
												free(remoteseq_mem);
											}
											free(remoteflags_mem);
										}
										cryptoDestroy(ctx_mem, (peer_slots + 1));
									}
									free(ctx_mem);
								}
								free(remoteid_mem);
							}
							free(remoteaddr_mem);
						}
						free(lastrecv_mem);
					}
					free(lastsend_mem);
				}
				free(state_mem);
			}
			free(conntime_mem);
		}
	}
	return 0;
}


// Destroy peer manager object.
static void peermgtDestroy(struct s_peermgt *mgt) {
	int size = mapGetMapSize(&mgt->map);
	mapDestroy(&mgt->map);
	nodedbDestroy(&mgt->nodedb);
	authmgtDestroy(&mgt->authmgt);
	dfragDestroy(&mgt->dfrag);
	free(mgt->seq);
	free(mgt->remoteseq);
	free(mgt->remoteflags);
	cryptoDestroy(mgt->ctx, size);
	free(mgt->ctx);
	free(mgt->remoteid);
	free(mgt->remoteaddr);
	free(mgt->lastrecv);
	free(mgt->lastsend);
	free(mgt->state);
	free(mgt->conntime);
}


#endif // F_PEERMGT_C
