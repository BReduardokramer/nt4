/*******************************************************************/
/*	      Copyright(c)  1993 Microsoft Corporation		   */
/*******************************************************************/

//***
//
// Filename:	ripproc.c
//
// Description: process rip packets
//
// Author:	Stefan Solomon (stefans)    October 11, 1993.
//
// Revision History:
//
//***

#include    "rtdefs.h"

VOID
RipRequest(PPACKET_TAG	    pktp);

VOID
RipResponse(PPACKET_TAG	    pktp);

VOID
SetNetworkEntry(PUCHAR		    nep,
		PIPX_ROUTE_ENTRY    rtep);

//***
//
// Function:	ProcessRipPacket
//
// Descr:
//
// Params:	Packet
//
// Returns:	none
//
//***

VOID
ProcessRipPacket(PPACKET_TAG	pktp)
{
    USHORT	    opcode;
    PUCHAR	    hdrp;    // ptr to the packet header

    // get a ptr to the packet header
    hdrp = pktp->DataBufferp;

    // check the RIP operation type
    GETSHORT2USHORT(&opcode, hdrp + RIP_OPCODE);

    switch(opcode) {

	case RIP_REQUEST:

	    RipRequest(pktp);
	    break;

	case RIP_RESPONSE:

	    RipResponse(pktp);
	    break;

	default:

	    // this is an invalid frame

	    RtPrint(DBG_NOTIFY, ("IpxRouter: ProcessRipPacket: Reject invalid frame\n"));
	    FreeRcvPkt(pktp);
	    break;
    }
}

//***
//
// Function:	RipRequest
//
// Descr:	process the RIP request
//
//***

VOID
RipRequest(PPACKET_TAG	    pktp)
{
    USHORT		reqlen;	// offset to get next request
    USHORT		resplen; // offset to put next response
    USHORT		pktlen;	// packet length
    PUCHAR		hdrp;	// ptr to the packet header
    PNICCB		niccbp;	// ptr to nic ctrl blk that received this packet
    PNICCB		rtniccbp;
    PIPX_ROUTE_ENTRY	rtep;
    KIRQL		oldirql;
    PRIP_SNDREQ		respcbp;
    UINT		segment;
    BOOLEAN		PingRouter = FALSE;

    RtPrint(DBG_RIP, ("IpxRouter: RipRequest: Entered\n"));

    // get a ptr to the packet owner Nic
    niccbp = pktp->PacketOwnerNicCbp;

    // get a ptr to the packet header
    hdrp = pktp->DataBufferp;

    // get IPX packet length
    GETSHORT2USHORT(&pktlen, hdrp + IPXH_LENGTH);

    // We may have one or more network entry requests in the packet.
    // If one network entry is 0xFFFFFFFF, then a general RIP response is
    // requested.

    // for each network entry, try to get the answer from our routing table
    for(reqlen = resplen = RIP_INFO;
	reqlen < pktlen;
	reqlen += NE_ENTRYSIZE) {

	// check if a general response is requested
	if(!memcmp(hdrp + reqlen + NE_NETNUMBER, bcastaddress, IPX_NET_LEN)) {

	    // queue a req for rip gen response	for this net and free the packet.
	    // the req will be dequeued and processed by the rip response
	    // thread.
	    if((respcbp = ExAllocatePool(NonPagedPool,
		 sizeof(RIP_SNDREQ))) != NULL) {

		// set up the send request
		respcbp->SndReqId = RIP_GEN_RESPONSE;
		respcbp->SendOnAllNics = FALSE; // send only to requesting node
		// fill in the sending node address, to be used in the response
		memcpy(respcbp->DestNode, hdrp + IPXH_SRCNODE, 6);

		// fill in the sending socket to be used in the response
		GETSHORT2USHORT(&respcbp->DestSock, hdrp + IPXH_SRCSOCK);

		respcbp->DoNotSendNicCbp = NULL;
		respcbp->SenderNicCbp = niccbp;
		respcbp->SndCompleteEventp = NULL;

		if(!RipQueueSndReqAtNic(niccbp, respcbp)) {

		    // can't queue this request
		    ExFreePool(respcbp);
		    respcbp = NULL;
		}
	    }

	    FreeRcvPkt(pktp);
	    return;
	}

	//*** a specific response is requested. ***
	// if the requested network number is 0, we replace it with
	// the network segment the packet was received on:
	if(!memcmp(hdrp + reqlen + NE_NETNUMBER, nulladdress, IPX_NET_LEN)) {

	    RtPrint(DBG_RIP, ("IpxRouter: RipRequest: request info on directly attached net\n"));
	    memcpy(hdrp + reqlen + NE_NETNUMBER, niccbp->Network, IPX_NET_LEN);
	    PingRouter = TRUE;
	}

	segment = IpxGetSegment(hdrp + reqlen + NE_NETNUMBER);

	// LOCK THE ROUTING TABLE
	KeAcquireSpinLock(&SegmentLocksTable[segment], &oldirql);

	if(rtep = IpxGetRoute(segment, hdrp + reqlen + NE_NETNUMBER)) {

	    // check if we can route the packet
	    // the route should be on a different nic id than the received
	    // packet
	    if(rtep->NicId != niccbp->NicId) {

		// if the response will be sent on a WAN link then there
		// is some filtering to do
		if(niccbp->DeviceType == NdisMediumWan) {

		    rtniccbp = NicCbPtrTab[rtep->NicId];

		    // check if the target net is visible via a WAN-Disabled LAN
		    if((rtniccbp->DeviceType != NdisMediumWan) &&
		       (rtniccbp->WanRoutingDisabled)) {

		       // skip it!
		       // UNLOCK THE ROUTING TABLE
		       KeReleaseSpinLock(&SegmentLocksTable[segment], oldirql);

		       continue;
		    }

		    // check if the nic to send on is a WAN client.
		    if(niccbp->WanConnectionClient) {

			// Check if LAN-WAN-LAN connectivity is enabled
			if(!LanWanLan)	{

			    // this node can only inform about its virtual net
			    if(rtniccbp->NicId != VirtualNicId) {

				// skip it!

				// UNLOCK THE ROUTING TABLE
				KeReleaseSpinLock(&SegmentLocksTable[segment], oldirql);

				continue;
			    }
			}
		    }
		}

		// we can route it -> answer to it
		// fill in the network entry structure in the packet with the
		// info from the route entry
		SetNetworkEntry(hdrp + resplen, rtep);

		// increment the response length to the next response entry
		resplen += NE_ENTRYSIZE;
	    }

	    if(PingRouter) {

		// answer to the ping request
		SetNetworkEntry(hdrp + resplen, rtep);

		// increment the response length to the next response entry
		resplen += NE_ENTRYSIZE;
	    }
	}

	PingRouter = FALSE;

	// UNLOCK THE ROUTING TABLE
	KeReleaseSpinLock(&SegmentLocksTable[segment], oldirql);
    }

    // We are done answering this request.
    // Check if any response has been generated
    if(resplen == RIP_INFO) {

	// no response generated for this packet
	RtPrint(DBG_RIP, ("IpxRouter: RipRequest: no response send for this request\n"));
	FreeRcvPkt(pktp);
	return;
    }

    // Turn the packet around and send it. This is done by changing the
    // packet's src and dest nodes and sockets. (src and dst net are the same)
    memcpy(hdrp + IPXH_DESTNODE, hdrp + IPXH_SRCNODE, 6);
    memcpy(hdrp + IPXH_SRCNODE, niccbp->Node, 6);

    memcpy(hdrp + IPXH_DESTSOCK, hdrp + IPXH_SRCSOCK, 2);
    PUTUSHORT2SHORT(hdrp + IPXH_SRCSOCK, IPX_RIP_SOCKET);

    // change the packet type to RIP response
    PUTUSHORT2SHORT(hdrp + RIP_OPCODE, RIP_RESPONSE);

    // set the new packet length
    PUTUSHORT2SHORT(hdrp + IPXH_LENGTH, resplen);

    // prepare remote address structure in the packet tag to send the packet
    pktp->RemoteAddress.NicId = niccbp->NicId;
    memcpy(pktp->RemoteAddress.MacAddress, hdrp + IPXH_DESTNODE, 6);

    // Send the packet. The packet will be freed when send completes.
    RtPrint(DBG_RIP, ("IpxRouter: RipRequest: send response send for this request\n"));

    SendPacket(pktp);
}

//***
//
// Function:	RipResponse
//
// Descr:	Updates the routing table with the response info
//
// Params:	Packet
//
// Returns:	none
//
//***

VOID
RipResponse(PPACKET_TAG	    pktp)
{
    USHORT	       resplen;	// offset of the next response network entry
    USHORT	       pktlen;	// IPX packet length
    PUCHAR	       hdrp;	// ptr to the packet header
    PNICCB	       niccbp;	// ptr to the nic ctrl blk the packet that
				// received the packet
    PIPX_ROUTE_ENTRY   oldrtep, newrtep; // new and old routing tab entries
    KIRQL	       oldirql;
    USHORT	       nrofhops;
    BOOLEAN	       RouteDown;
    PRIP_UPDATE_SNDREQ respcbp = NULL;	// ptr to changes response to bcast
    PRIP_SNDREQ        sndreqp;
    PUCHAR	       sndhdrp;
    USHORT	       sndpktlen;
    UINT	       segment;

#if DBG
    UCHAR	       b[6];
#endif

    RtPrint(DBG_RIP, ("IpxRouter: RipResponse: Entered\n"));

    // get a ptr to this Nic
    niccbp = pktp->PacketOwnerNicCbp;

    // get a ptr to the received response packet header
    hdrp = pktp->DataBufferp;

    // get received response packet length
    GETSHORT2USHORT(&pktlen, hdrp + IPXH_LENGTH);

    // For each network entry, check if we have it in our routing table.
    // If we do not have this entry or if
    // it is a better entry than what we have, we add it to the routing table
    // !!! for this primary version, we don't care about better entries !!!

    for(resplen = RIP_INFO;
	resplen < pktlen;
	resplen += NE_ENTRYSIZE) {

	newrtep = NULL;
	// check if the network route is up or down
	GETSHORT2USHORT(&nrofhops, hdrp + resplen + NE_NROFHOPS);

	if(nrofhops < 16) {

	    RouteDown = FALSE;
	}
	else
	{
	    RouteDown = TRUE;
	}

	segment = IpxGetSegment(hdrp + resplen + NE_NETNUMBER);

	// LOCK THE ROUTING TABLE
	KeAcquireSpinLock(&SegmentLocksTable[segment], &oldirql);

	// check if the entry exists.
	if((oldrtep = IpxGetRoute(segment, hdrp + resplen + NE_NETNUMBER)) == NULL) {

	    //*** This route does not exist ***

	    // if this is a route down bcast and we didn't have this route
	    // we skip this information
	    if(RouteDown) {

		// UNLOCK THE ROUTING TABLE
		KeReleaseSpinLock(&SegmentLocksTable[segment], oldirql);

		continue;
	    }

	    // if this is a route with 15 hops, we choose for now to ignore
	    // it.
	    if(nrofhops == 15) {

		// UNLOCK THE ROUTING TABLE
		KeReleaseSpinLock(&SegmentLocksTable[segment], oldirql);

		continue;
	    }

	    // This is a new route. We add this route to the routing table
	    if(newrtep = ExAllocatePool(NonPagedPool, sizeof(IPX_ROUTE_ENTRY))) {

		// set it up
		memcpy(newrtep->Network, hdrp + resplen + NE_NETNUMBER, IPX_NET_LEN);
		newrtep->NicId = niccbp->NicId;
		memcpy(newrtep->NextRouter, hdrp + IPXH_SRCNODE, IPX_NODE_LEN);
		newrtep->Flags = 0;
		newrtep->Timer = 0; // TTL of this route entry is 3 min
		newrtep->Segment = segment;
		GETSHORT2USHORT(&newrtep->TickCount, hdrp + resplen + NE_NROFTICKS);
		GETSHORT2USHORT(&newrtep->HopCount, hdrp + resplen + NE_NROFHOPS);

		// increment the tick count and hop count to reflect our
		// new router in the path
		newrtep->TickCount += niccbp->TickCount;
		newrtep->HopCount++;

		InitializeListHead(&newrtep->AlternateRoute);

		// add it to the table
		IpxAddRoute(segment, newrtep);
	    }
	}
	else
	{
	    //*** This route exists ***

	    // first, reset the route timer
	    oldrtep->Timer = 0;

	    // if the info tells us that the route is down, we should delete
	    // the route from the routing table and inform all other routers
	    // of the change
	    if(RouteDown) {

		// this may be a bogus packet from the wire
		if((oldrtep->Flags & IPX_ROUTER_PERMANENT_ENTRY) ||
		   (oldrtep->Flags & IPX_ROUTER_LOCAL_NET)) {

#if DBG
		    memcpy(b, hdrp + IPXH_SRCNODE, 6);
#endif
		    RtPrint(DBG_NOTIFY, ("IpxRouter: RipResponse: Bogus resp permanent route down from %x-%x-%x-%x-%x-%x !!\n",
					  b[0],b[1],b[2],b[3],b[4],b[5]));

		    RouteDown = FALSE;
		}
		else
		{
		    IpxDeleteRoute(segment, oldrtep);

		    // set the nr of hops to 16 in the route entry; used later to
		    // bcast the change
		    oldrtep->HopCount = 16;
		}
	    }

	    // if the route is not down but this is a better route, add it
	    // instead. LATER !!!
	}

	// UNLOCK THE ROUTING TABLE
	KeReleaseSpinLock(&SegmentLocksTable[segment], oldirql);

	// check if we will broadcast any change and if a bcast packet
	// request + buffer have been allocated
	if (newrtep || (oldrtep && RouteDown)) {

	    // check if a send bcast req struct has already been allocated
	    if(respcbp == NULL) {

		// allocate a send request struct to bcast changes in the routing table
		respcbp = ExAllocatePool(NonPagedPool,
				sizeof(RIP_UPDATE_SNDREQ) + RIP_SNDPKT_MAXLEN);

		if(respcbp != NULL) {

		    // get the ipx hdr ptr for the send packet
		    sndhdrp = (PUCHAR)respcbp->RipSndPktBuff.IpxPacket;

		    // set the initial length
		    sndpktlen = RIP_INFO;
		}
	    }
	}

	// check if we have added a new route or deleted an old one
	if(newrtep) {

	    // write the network entry for this new route in the packet to be
	    // sent
	    if(respcbp) {

		// fill in the network entry structure in the packet with the
		// info from the route entry
		SetNetworkEntry(sndhdrp + sndpktlen, newrtep);

		// increment the send packet length to the next network entry
		sndpktlen += NE_ENTRYSIZE;
	    }
	}
	else
	{
	    // check if an old route has been deleted
	    if(oldrtep && RouteDown) {

		// write the network entry for this old route in the packet to be
		// sent
		if(respcbp) {

		    // fill in the network entry structure in the packet with the
		    // info from the route entry
		    SetNetworkEntry(sndhdrp + sndpktlen, oldrtep);

		    // increment the send packet length to the next network entry
		    sndpktlen += NE_ENTRYSIZE;

		    ExFreePool(oldrtep);
		}
	    }
	}
    }

    // if we have added or deleted some routes, we make a request to the rip bcast
    // thread to have them bcasted over to the other nics

    if(respcbp) {

	// Send a bcast update with the changes to all the nics except this one
	sndreqp = &respcbp->RipSndReq;
	// set the new packet length
	PUTUSHORT2SHORT(sndhdrp + IPXH_LENGTH, sndpktlen);

	BroadcastRipUpdate(sndreqp, niccbp, NULL);
    }

    // free the packet
    FreeRcvPkt(pktp);
}


//***
//
// Function:	SetNetworkEntry
//
// Descr:
//
//***

VOID
SetNetworkEntry(PUCHAR		    nep, // points to the network entry in the
					 // RIP packet
		PIPX_ROUTE_ENTRY    rtep)
{
    memcpy(nep + NE_NETNUMBER, rtep->Network, IPX_NET_LEN);
    PUTUSHORT2SHORT(nep + NE_NROFHOPS, rtep->HopCount);
    PUTUSHORT2SHORT(nep + NE_NROFTICKS, rtep->TickCount);
}