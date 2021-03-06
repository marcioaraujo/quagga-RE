/* RIP version 1 and 2.
 * Copyright (C) 2005 6WIND <alain.ritoux@6wind.com>
 * Copyright (C) 1997, 98, 99 Kunihiro Ishiguro <kunihiro@zebra.org>
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.  
 */

#include <zebra.h>

#include "memory.h"
#include "log.h"
#include "stream.h"
#include "sockunion.h"
#include "sockopt.h"
#include "routemap.h"
#include "if_rmap.h"
#include "distribute.h"
#include "privs.h"
#include "cryptohash.h"

#include "ripd/ripd.h"
#include "ripd/rip_debug.h"
#include "ripd/rip_interface.h"
#include "ripd/rip_main.h"
#include "ripd/rip_peer.h"
#include "ripd/rip_zebra.h"
#include "ripd/rip_routemap.h"
#include "ripd/rip_offset.h"
#include "ripd/rip_snmp.h"
#include "ripd/rip_auth.h"

/* RIP Structure. */
struct rip *rip = NULL;

/* RIP route changes. */
long rip_global_route_changes = 0;

/* RIP queries. */
long rip_global_queries = 0;

/* Prototypes. */
static void rip_event (enum rip_event, int);
static void rip_output_process (struct connected *, struct sockaddr_in *, int, u_char);
static int rip_triggered_update (struct thread *);
static u_char rip_distance_apply (struct rip_info *);

/* RIP output routes type. */
enum
{
  rip_all_route,
  rip_changed_route
};

/* RIP command strings. */
static const struct message rip_msg[] =
{
  {RIP_REQUEST,    "REQUEST"},
  {RIP_RESPONSE,   "RESPONSE"},
};
static const size_t rip_msg_max = sizeof (rip_msg) / sizeof (rip_msg[0]);

static int
rip_route_rte (struct rip_info *rinfo)
{
  return (rinfo->type == ZEBRA_ROUTE_RIP && rinfo->sub_type == RIP_ROUTE_RTE);
}

static struct rip_info *
rip_info_new (void)
{
  return XCALLOC (MTYPE_RIP_INFO, sizeof (struct rip_info));
}

static void
rip_info_free (struct rip_info *rinfo)
{
  XFREE (MTYPE_RIP_INFO, rinfo);
}

/* RIP route garbage collect timer. */
static int
rip_garbage_collect (struct thread *t)
{
  struct rip_info *rinfo;
  struct route_node *rp;

  rinfo = THREAD_ARG (t);
  rinfo->t_garbage_collect = NULL;

  /* Off timeout timer. */
  RIP_TIMER_OFF (rinfo->t_timeout);
  
  /* Get route_node pointer. */
  rp = rinfo->rp;

  /* Unlock route_node. */
  rp->info = NULL;
  route_unlock_node (rp);

  /* Free RIP routing information. */
  rip_info_free (rinfo);

  return 0;
}

/* Timeout RIP routes. */
static int
rip_timeout (struct thread *t)
{
  struct rip_info *rinfo;
  struct route_node *rn;

  rinfo = THREAD_ARG (t);
  rinfo->t_timeout = NULL;

  rn = rinfo->rp;

  /* - The garbage-collection timer is set for 120 seconds. */
  RIP_TIMER_ON (rinfo->t_garbage_collect, rip_garbage_collect, 
		rip->garbage_time);

  rip_zebra_ipv4_delete ((struct prefix_ipv4 *)&rn->p, &rinfo->nexthop,
			 rinfo->metric);
  /* - The metric for the route is set to 16 (infinity).  This causes
     the route to be removed from service. */
  rinfo->metric = RIP_METRIC_INFINITY;
  rinfo->flags &= ~RIP_RTF_FIB;

  /* - The route change flag is to indicate that this entry has been
     changed. */
  rinfo->flags |= RIP_RTF_CHANGED;

  /* - The output process is signalled to trigger a response. */
  rip_event (RIP_TRIGGERED_UPDATE, 0);

  return 0;
}

static void
rip_timeout_update (struct rip_info *rinfo)
{
  if (rinfo->metric != RIP_METRIC_INFINITY)
    {
      RIP_TIMER_OFF (rinfo->t_timeout);
      RIP_TIMER_ON (rinfo->t_timeout, rip_timeout, rip->timeout_time);
    }
}

static int
rip_filter (int rip_distribute, struct prefix_ipv4 *p, struct rip_interface *ri)
{
  struct distribute *dist;
  struct access_list *alist;
  struct prefix_list *plist;
  int distribute = rip_distribute == RIP_FILTER_OUT ?
      DISTRIBUTE_V4_OUT : DISTRIBUTE_V4_IN;
  const char *inout = rip_distribute == RIP_FILTER_OUT ? "out" : "in";

  /* Input distribute-list filtering. */
  if (ri->list[rip_distribute])
    {
      if (access_list_apply (ri->list[rip_distribute],
			     (struct prefix *) p) == FILTER_DENY)
	{
	  if (IS_RIP_DEBUG_PACKET)
	    zlog_debug ("%s/%d filtered by distribute %s",
                        inet_ntoa (p->prefix), p->prefixlen, inout);
	  return -1;
	}
    }
  if (ri->prefix[rip_distribute])
    {
      if (prefix_list_apply (ri->prefix[rip_distribute],
			     (struct prefix *) p) == PREFIX_DENY)
	{
	  if (IS_RIP_DEBUG_PACKET)
	    zlog_debug ("%s/%d filtered by prefix-list %s",
                        inet_ntoa (p->prefix), p->prefixlen, inout);
	  return -1;
	}
    }

  /* All interface filter check. */
  dist = distribute_lookup (NULL);
  if (dist)
    {
      if (dist->list[distribute])
	{
	  alist = access_list_lookup (AFI_IP, dist->list[distribute]);

	  if (alist)
	    {
	      if (access_list_apply (alist, (struct prefix *) p) == FILTER_DENY)
		{
		  if (IS_RIP_DEBUG_PACKET)
		    zlog_debug ("%s/%d filtered by distribute %s",
                                inet_ntoa (p->prefix), p->prefixlen, inout);
		  return -1;
		}
	    }
	}
      if (dist->prefix[distribute])
	{
	  plist = prefix_list_lookup (AFI_IP, dist->prefix[distribute]);

	  if (plist)
	    {
	      if (prefix_list_apply (plist,
				     (struct prefix *) p) == PREFIX_DENY)
		{
		  if (IS_RIP_DEBUG_PACKET)
		    zlog_debug ("%s/%d filtered by prefix-list %s",
                                inet_ntoa (p->prefix), p->prefixlen, inout);
		  return -1;
		}
	    }
	}
    }
  return 0;
}

/* Check nexthop address validity. */
static int
rip_nexthop_check (struct in_addr *addr)
{
  struct listnode *node;
  struct listnode *cnode;
  struct interface *ifp;
  struct connected *ifc;
  struct prefix *p;

  /* If nexthop address matches local configured address then it is
     invalid nexthop. */
  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    {
      for (ALL_LIST_ELEMENTS_RO (ifp->connected, cnode, ifc))
	{	    
	  p = ifc->address;

	  if (p->family == AF_INET
	      && IPV4_ADDR_SAME (&p->u.prefix4, addr))
	    return -1;
	}
    }
  return 0;
}

/* RIP add route to routing table. */
static void
rip_rte_process (struct rte *rte, struct sockaddr_in *from,
                 struct interface *ifp)
{
  int ret;
  struct prefix_ipv4 p;
  struct route_node *rp;
  struct rip_info *rinfo, rinfotmp;
  struct rip_interface *ri;
  struct in_addr *nexthop;
  u_char oldmetric;
  int same = 0;
  int route_reuse = 0;
  unsigned char old_dist, new_dist;

  /* Make prefix structure. */
  memset (&p, 0, sizeof (struct prefix_ipv4));
  p.family = AF_INET;
  p.prefix = rte->prefix;
  p.prefixlen = ip_masklen (rte->mask);

  /* Make sure mask is applied. */
  apply_mask_ipv4 (&p);

  /* Apply input filters. */
  ri = ifp->info;

  ret = rip_filter (RIP_FILTER_IN, &p, ri);
  if (ret < 0)
    return;

  /* Modify entry according to the interface routemap. */
  if (ri->routemap[RIP_FILTER_IN])
    {
      int ret;
      struct rip_info newinfo;

      memset (&newinfo, 0, sizeof (newinfo));
      newinfo.type = ZEBRA_ROUTE_RIP;
      newinfo.sub_type = RIP_ROUTE_RTE;
      newinfo.nexthop = rte->nexthop;
      newinfo.from = from->sin_addr;
      newinfo.ifindex = ifp->ifindex;
      newinfo.metric = rte->metric;
      newinfo.metric_out = rte->metric; /* XXX */
      newinfo.tag = ntohs (rte->tag);   /* XXX */

      /* The object should be of the type of rip_info */
      ret = route_map_apply (ri->routemap[RIP_FILTER_IN],
                             (struct prefix *) &p, RMAP_RIP, &newinfo);

      if (ret == RMAP_DENYMATCH)
        {
          if (IS_RIP_DEBUG_PACKET)
            zlog_debug ("RIP %s/%d is filtered by route-map in",
                       inet_ntoa (p.prefix), p.prefixlen);
          return;
        }

      /* Get back the object */
      rte->nexthop = newinfo.nexthop_out;
      rte->tag = htons (newinfo.tag_out);       /* XXX */
      rte->metric = newinfo.metric_out; /* XXX: the routemap uses the metric_out field */
    }

  /* Once the entry has been validated, update the metric by
     adding the cost of the network on wich the message
     arrived. If the result is greater than infinity, use infinity
     (RFC2453 Sec. 3.9.2) */
  /* Zebra ripd can handle offset-list in. */
  ret = rip_offset_list_apply_in (&p, ifp, &rte->metric);

  /* If offset-list does not modify the metric use interface's
     metric. */
  if (!ret)
    rte->metric += ifp->metric;

  if (rte->metric > RIP_METRIC_INFINITY)
    rte->metric = RIP_METRIC_INFINITY;

  /* Set nexthop pointer. */
  if (rte->nexthop.s_addr == 0)
    nexthop = &from->sin_addr;
  else
    nexthop = &rte->nexthop;

  /* Check if nexthop address is myself, then do nothing. */
  if (rip_nexthop_check (nexthop) < 0)
    {
      if (IS_RIP_DEBUG_PACKET)
        zlog_debug ("Nexthop address %s is myself", inet_ntoa (*nexthop));
      return;
    }

  /* Get index for the prefix. */
  rp = route_node_get (rip->table, (struct prefix *) &p);

  /* Check to see whether there is already RIP route on the table. */
  rinfo = rp->info;

  if (rinfo)
    {
      /* Local static route. */
      if (rinfo->type == ZEBRA_ROUTE_RIP
          && ((rinfo->sub_type == RIP_ROUTE_STATIC) ||
              (rinfo->sub_type == RIP_ROUTE_DEFAULT))
          && rinfo->metric != RIP_METRIC_INFINITY)
        {
          route_unlock_node (rp);
          return;
        }

      /* Redistributed route check. */
      if (rinfo->type != ZEBRA_ROUTE_RIP
          && rinfo->metric != RIP_METRIC_INFINITY)
        {
          /* Fill in a minimaly temporary rip_info structure, for a future
             rip_distance_apply() use) */
          memset (&rinfotmp, 0, sizeof (rinfotmp));
          IPV4_ADDR_COPY (&rinfotmp.from, &from->sin_addr);
          rinfotmp.rp = rinfo->rp;
          new_dist = rip_distance_apply (&rinfotmp);
          new_dist = new_dist ? new_dist : ZEBRA_RIP_DISTANCE_DEFAULT;
          old_dist = rinfo->distance;
          /* Only connected routes may have a valid NULL distance */
          if (rinfo->type != ZEBRA_ROUTE_CONNECT)
            old_dist = old_dist ? old_dist : ZEBRA_RIP_DISTANCE_DEFAULT;
          /* If imported route does not have STRICT precedence, 
             mark it as a ghost */
          if (new_dist > old_dist 
              || rte->metric == RIP_METRIC_INFINITY)
            {
              route_unlock_node (rp);
              return;
            }
          else
            {
              RIP_TIMER_OFF (rinfo->t_timeout);
              RIP_TIMER_OFF (rinfo->t_garbage_collect);
                                                                                
              rp->info = NULL;
              if (rip_route_rte (rinfo))
                rip_zebra_ipv4_delete ((struct prefix_ipv4 *)&rp->p, 
                                        &rinfo->nexthop, rinfo->metric);
              rip_info_free (rinfo);
              rinfo = NULL;
              route_reuse = 1;
            }
        }
    }

  if (!rinfo)
    {
      /* Now, check to see whether there is already an explicit route
         for the destination prefix.  If there is no such route, add
         this route to the routing table, unless the metric is
         infinity (there is no point in adding a route which
         unusable). */
      if (rte->metric != RIP_METRIC_INFINITY)
        {
          rinfo = rip_info_new ();

          /* - Setting the destination prefix and length to those in
             the RTE. */
          rinfo->rp = rp;

          /* - Setting the metric to the newly calculated metric (as
             described above). */
          rinfo->metric = rte->metric;
          rinfo->tag = ntohs (rte->tag);

          /* - Set the next hop address to be the address of the router
             from which the datagram came or the next hop address
             specified by a next hop RTE. */
          IPV4_ADDR_COPY (&rinfo->nexthop, nexthop);
          IPV4_ADDR_COPY (&rinfo->from, &from->sin_addr);
          rinfo->ifindex = ifp->ifindex;

          /* - Initialize the timeout for the route.  If the
             garbage-collection timer is running for this route, stop it
             (see section 2.3 for a discussion of the timers). */
          rip_timeout_update (rinfo);

          /* - Set the route change flag. */
          rinfo->flags |= RIP_RTF_CHANGED;

          /* - Signal the output process to trigger an update (see section
             2.5). */
          rip_event (RIP_TRIGGERED_UPDATE, 0);

          /* Finally, route goes into the kernel. */
          rinfo->type = ZEBRA_ROUTE_RIP;
          rinfo->sub_type = RIP_ROUTE_RTE;

          /* Set distance value. */
          rinfo->distance = rip_distance_apply (rinfo);

          rp->info = rinfo;
          rip_zebra_ipv4_add (&p, &rinfo->nexthop, rinfo->metric,
                              rinfo->distance);
          rinfo->flags |= RIP_RTF_FIB;
        }

      /* Unlock temporary lock, i.e. same behaviour */
      if (route_reuse)
        route_unlock_node (rp);
    }
  else
    {
      /* Route is there but we are not sure the route is RIP or not. */
      rinfo = rp->info;

      /* If there is an existing route, compare the next hop address
         to the address of the router from which the datagram came.
         If this datagram is from the same router as the existing
         route, reinitialize the timeout.  */
      same = (IPV4_ADDR_SAME (&rinfo->from, &from->sin_addr)
              && (rinfo->ifindex == ifp->ifindex));

      if (same)
        rip_timeout_update (rinfo);


      /* Fill in a minimaly temporary rip_info structure, for a future
         rip_distance_apply() use) */
      memset (&rinfotmp, 0, sizeof (rinfotmp));
      IPV4_ADDR_COPY (&rinfotmp.from, &from->sin_addr);
      rinfotmp.rp = rinfo->rp;


      /* Next, compare the metrics.  If the datagram is from the same
         router as the existing route, and the new metric is different
         than the old one; or, if the new metric is lower than the old
         one, or if the tag has been changed; or if there is a route
         with a lower administrave distance; or an update of the
         distance on the actual route; do the following actions: */
      if ((same && rinfo->metric != rte->metric)
          || (rte->metric < rinfo->metric)
          || ((same)
              && (rinfo->metric == rte->metric)
              && ntohs (rte->tag) != rinfo->tag)
          || (rinfo->distance > rip_distance_apply (&rinfotmp))
          || ((rinfo->distance != rip_distance_apply (rinfo)) && same))
        {
          /* - Adopt the route from the datagram.  That is, put the
             new metric in, and adjust the next hop address (if
             necessary). */
          oldmetric = rinfo->metric;
          rinfo->metric = rte->metric;
          rinfo->tag = ntohs (rte->tag);
          IPV4_ADDR_COPY (&rinfo->from, &from->sin_addr);
          rinfo->ifindex = ifp->ifindex;
          rinfo->distance = rip_distance_apply (rinfo);

          /* Should a new route to this network be established
             while the garbage-collection timer is running, the
             new route will replace the one that is about to be
             deleted.  In this case the garbage-collection timer
             must be cleared. */

          if (oldmetric == RIP_METRIC_INFINITY &&
              rinfo->metric < RIP_METRIC_INFINITY)
            {
              rinfo->type = ZEBRA_ROUTE_RIP;
              rinfo->sub_type = RIP_ROUTE_RTE;

              RIP_TIMER_OFF (rinfo->t_garbage_collect);

              if (!IPV4_ADDR_SAME (&rinfo->nexthop, nexthop))
                IPV4_ADDR_COPY (&rinfo->nexthop, nexthop);

              rip_zebra_ipv4_add (&p, nexthop, rinfo->metric,
                                  rinfo->distance);
              rinfo->flags |= RIP_RTF_FIB;
            }

          /* Update nexthop and/or metric value.  */
          if (oldmetric != RIP_METRIC_INFINITY)
            {
              rip_zebra_ipv4_delete (&p, &rinfo->nexthop, oldmetric);
              rip_zebra_ipv4_add (&p, nexthop, rinfo->metric,
                                  rinfo->distance);
              rinfo->flags |= RIP_RTF_FIB;

              if (!IPV4_ADDR_SAME (&rinfo->nexthop, nexthop))
                IPV4_ADDR_COPY (&rinfo->nexthop, nexthop);
            }

          /* - Set the route change flag and signal the output process
             to trigger an update. */
          rinfo->flags |= RIP_RTF_CHANGED;
          rip_event (RIP_TRIGGERED_UPDATE, 0);

          /* - If the new metric is infinity, start the deletion
             process (described above); */
          if (rinfo->metric == RIP_METRIC_INFINITY)
            {
              /* If the new metric is infinity, the deletion process
                 begins for the route, which is no longer used for
                 routing packets.  Note that the deletion process is
                 started only when the metric is first set to
                 infinity.  If the metric was already infinity, then a
                 new deletion process is not started. */
              if (oldmetric != RIP_METRIC_INFINITY)
                {
                  /* - The garbage-collection timer is set for 120 seconds. */
                  RIP_TIMER_ON (rinfo->t_garbage_collect,
                                rip_garbage_collect, rip->garbage_time);
                  RIP_TIMER_OFF (rinfo->t_timeout);

                  /* - The metric for the route is set to 16
                     (infinity).  This causes the route to be removed
                     from service. */
                  rip_zebra_ipv4_delete (&p, &rinfo->nexthop, oldmetric);
                  rinfo->flags &= ~RIP_RTF_FIB;

                  /* - The route change flag is to indicate that this
                     entry has been changed. */
                  /* - The output process is signalled to trigger a
                     response. */
                  ;             /* Above processes are already done previously. */
                }
            }
          else
            {
              /* otherwise, re-initialize the timeout. */
              rip_timeout_update (rinfo);
            }
        }
      /* Unlock tempolary lock of the route. */
      route_unlock_node (rp);
    }
}

/* Dump RIP packet */
static void
rip_packet_dump (struct rip_packet *packet, int size, const char *sndrcv)
{
  caddr_t lim;
  struct rte *rte;
  char pbuf[BUFSIZ], nbuf[BUFSIZ];

  /* Dump packet header. */
  zlog_debug ("%s %s version %d packet size %d",
	     sndrcv, LOOKUP (rip_msg, packet->command), packet->version, size);

  /* Dump each routing table entry. */
  rte = packet->rte;
  
  for (lim = (caddr_t) packet + size; (caddr_t) rte < lim; rte++)
    {
      if (packet->version == RIPv2)
	{
          if (rte->family == htons (RIP_FAMILY_AUTH))
          {
            if (rip_auth_dump_ffff_rte ((struct rip_auth_rte *) rte, lim - (caddr_t)rte) < 1)
              break;
          }
	  else
	    zlog_debug ("  %s/%d -> %s family %d tag %d metric %ld",
                       inet_ntop (AF_INET, &rte->prefix, pbuf, BUFSIZ),
                       ip_masklen_safe (rte->mask),
                       inet_ntop (AF_INET, &rte->nexthop, nbuf, BUFSIZ),
                       ntohs (rte->family),
                       ntohs (rte->tag), (u_long) ntohl (rte->metric));
	}
      else
	{
	  zlog_debug ("  %s family %d tag %d metric %ld", 
		     inet_ntop (AF_INET, &rte->prefix, pbuf, BUFSIZ),
		     ntohs (rte->family), ntohs (rte->tag),
		     (u_long)ntohl (rte->metric));
	}
    }
}

/* Check if the destination address is valid (unicast; not net 0
   or 127) (RFC2453 Section 3.9.2 - Page 26).  But we don't
   check net 0 because we accept default route. */
static int
rip_destination_check (struct in_addr addr)
{
  u_int32_t destination;

  /* Convert to host byte order. */
  destination = ntohl (addr.s_addr);

  if (IPV4_NET127 (destination))
    return 0;

  /* Net 0 may match to the default route. */
  if (IPV4_NET0 (destination) && destination != 0)
    return 0;

  /* Unicast address must belong to class A, B, C. */
  if (IN_CLASSA (destination))
    return 1;
  if (IN_CLASSB (destination))
    return 1;
  if (IN_CLASSC (destination))
    return 1;

  return 0;
}

/* RIP routing information. */
static void
rip_response_process (struct rip_packet *packet, int size, 
		      struct sockaddr_in *from, struct connected *ifc)
{
  caddr_t lim;
  struct rte *rte;
  struct prefix_ipv4 ifaddr;
  struct prefix_ipv4 ifaddrclass;
  int subnetted;
  struct rip_interface *ri;
      
  ri = ifc->ifp->info;
  /* We don't know yet. */
  subnetted = -1;

  /* The Response must be ignored if it is not from the RIP
     port. (RFC2453 - Sec. 3.9.2)*/
  if (from->sin_port != htons(RIP_PORT_DEFAULT))
    {
      zlog_info ("response doesn't come from RIP port: %d",
		 from->sin_port);
      rip_peer_bad_packet (from);
      ri->recv_badpackets++;
      return;
    }

  /* The datagram's IPv4 source address should be checked to see
     whether the datagram is from a valid neighbor; the source of the
     datagram must be on a directly connected network (RFC2453 - Sec. 3.9.2) */
  if (if_lookup_address(from->sin_addr) == NULL) 
    {
      zlog_info ("This datagram doesn't came from a valid neighbor: %s",
		 inet_ntoa (from->sin_addr));
      rip_peer_bad_packet (from);
      ri->recv_badpackets++;
      return;
    }

  /* Update RIP peer. */
  rip_peer_update (from, packet->version);

  /* Set RTE pointer. */
  rte = packet->rte;

  for (lim = (caddr_t) packet + size; (caddr_t) rte < lim; rte++)
    {
      /* Assume rip_packet_examin() to do the job properly. */
      if (rte->family == htons (RIP_FAMILY_AUTH))
	continue;
      assert (rte->family == htons (AF_INET));

      /* - is the destination address valid (e.g., unicast; not net 0
         or 127) */
      if (! rip_destination_check (rte->prefix))
        {
	  zlog_info ("Network is net 0 or net 127 or it is not unicast network");
	  rip_peer_bad_route (from);
	  ri->recv_badroutes++;
	  continue;
	} 

      /* Convert metric value to host byte order. */
      rte->metric = ntohl (rte->metric);

      /* - is the metric valid (i.e., between 1 and 16, inclusive) */
      if (! (rte->metric >= 1 && rte->metric <= 16))
	{
	  zlog_info ("Route's metric is not in the 1-16 range.");
	  rip_peer_bad_route (from);
	  ri->recv_badroutes++;
	  continue;
	}

      /* RIPv1 does not have nexthop value. */
      if (packet->version == RIPv1 && rte->nexthop.s_addr != 0)
	{
	  zlog_info ("RIPv1 packet with nexthop value %s",
		     inet_ntoa (rte->nexthop));
	  rip_peer_bad_route (from);
	  ri->recv_badroutes++;
	  continue;
	}

      /* That is, if the provided information is ignored, a possibly
	 sub-optimal, but absolutely valid, route may be taken.  If
	 the received Next Hop is not directly reachable, it should be
	 treated as 0.0.0.0. */
      if (packet->version == RIPv2 && rte->nexthop.s_addr != 0)
	{
	  u_int32_t addrval;

	  /* Multicast address check. */
	  addrval = ntohl (rte->nexthop.s_addr);
	  if (IN_CLASSD (addrval))
	    {
	      zlog_info ("Nexthop %s is multicast address, skip this rte",
			 inet_ntoa (rte->nexthop));
	      continue;
	    }

	  if (! if_lookup_address (rte->nexthop))
	    {
	      struct route_node *rn;
	      struct rip_info *rinfo;

	      rn = route_node_match_ipv4 (rip->table, &rte->nexthop);

	      if (rn)
		{
		  rinfo = rn->info;

		  if (rinfo->type == ZEBRA_ROUTE_RIP
		      && rinfo->sub_type == RIP_ROUTE_RTE)
		    {
		      if (IS_RIP_DEBUG_EVENT)
			zlog_debug ("Next hop %s is on RIP network.  Set nexthop to the packet's originator", inet_ntoa (rte->nexthop));
		      rte->nexthop = rinfo->from;
		    }
		  else
		    {
		      if (IS_RIP_DEBUG_EVENT)
			zlog_debug ("Next hop %s is not directly reachable. Treat it as 0.0.0.0", inet_ntoa (rte->nexthop));
		      rte->nexthop.s_addr = 0;
		    }

		  route_unlock_node (rn);
		}
	      else
		{
		  if (IS_RIP_DEBUG_EVENT)
		    zlog_debug ("Next hop %s is not directly reachable. Treat it as 0.0.0.0", inet_ntoa (rte->nexthop));
		  rte->nexthop.s_addr = 0;
		}

	    }
	}

     /* For RIPv1, there won't be a valid netmask.  

	This is a best guess at the masks.  If everyone was using old
	Ciscos before the 'ip subnet zero' option, it would be almost
	right too :-)
      
	Cisco summarize ripv1 advertisments to the classful boundary
	(/16 for class B's) except when the RIP packet does to inside
	the classful network in question.  */

      if ((packet->version == RIPv1 && rte->prefix.s_addr != 0) 
	  || (packet->version == RIPv2 
	      && (rte->prefix.s_addr != 0 && rte->mask.s_addr == 0)))
	{
	  u_int32_t destination;

	  if (subnetted == -1)
            {
              memcpy (&ifaddr, ifc->address, sizeof (struct prefix_ipv4));
              memcpy (&ifaddrclass, &ifaddr, sizeof (struct prefix_ipv4));
              apply_classful_mask_ipv4 (&ifaddrclass);
              subnetted = 0;
              if (ifaddr.prefixlen > ifaddrclass.prefixlen)
                subnetted = 1;
            }

	  destination = ntohl (rte->prefix.s_addr);

	  if (IN_CLASSA (destination))
	      masklen2ip (8, &rte->mask);
	  else if (IN_CLASSB (destination))
	      masklen2ip (16, &rte->mask);
	  else if (IN_CLASSC (destination))
	      masklen2ip (24, &rte->mask);

	  if (subnetted == 1)
	    masklen2ip (ifaddrclass.prefixlen,
			(struct in_addr *) &destination);
	  if ((subnetted == 1) && ((rte->prefix.s_addr & destination) ==
	      ifaddrclass.prefix.s_addr))
	    {
	      masklen2ip (ifaddr.prefixlen, &rte->mask);
	      if ((rte->prefix.s_addr & rte->mask.s_addr) != rte->prefix.s_addr)
		masklen2ip (32, &rte->mask);
	      if (IS_RIP_DEBUG_EVENT)
		zlog_debug ("Subnetted route %s", inet_ntoa (rte->prefix));
	    }
	  else
	    {
	      if ((rte->prefix.s_addr & rte->mask.s_addr) != rte->prefix.s_addr)
		continue;
	    }

	  if (IS_RIP_DEBUG_EVENT)
	    {
	      zlog_debug ("Resultant route %s", inet_ntoa (rte->prefix));
	      zlog_debug ("Resultant mask %s", inet_ntoa (rte->mask));
	    }
	}

      /* The "Subnet Mask" RTE field must contain a valid netmask. */
      if (packet->version == RIPv2 && ip_masklen_safe (rte->mask) < 0)
	{
	  if (IS_RIP_DEBUG_RECV)
	    zlog_warn ("%s: malformed RIPv2 RTE netmask", __func__);
	  rip_peer_bad_route (from);
	  ri->recv_badroutes++;
	  continue;
	}

      /* In case of RIPv2, if prefix in RTE is not netmask applied one
         ignore the entry.  */
      if ((packet->version == RIPv2) 
	  && (rte->mask.s_addr != 0) 
	  && ((rte->prefix.s_addr & rte->mask.s_addr) != rte->prefix.s_addr))
	{
	  zlog_warn ("RIPv2 address %s is not mask /%d applied one",
		     inet_ntoa (rte->prefix), ip_masklen (rte->mask));
	  rip_peer_bad_route (from);
	  ri->recv_badroutes++;
	  continue;
	}

      /* Default route's netmask is ignored. */
      if (packet->version == RIPv2
	  && (rte->prefix.s_addr == 0)
	  && (rte->mask.s_addr != 0))
	{
	  if (IS_RIP_DEBUG_EVENT)
	    zlog_debug ("Default route with non-zero netmask.  Set zero to netmask");
	  rte->mask.s_addr = 0;
	}
	  
      /* Routing table updates. */
      rip_rte_process (rte, from, ifc->ifp);
    }
}

/* Make socket for RIP protocol. */
static int 
rip_create_socket (struct sockaddr_in *from)
{
  int ret;
  int sock;
  struct sockaddr_in addr;
  
  memset (&addr, 0, sizeof (struct sockaddr_in));
  
  if (!from)
    {
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
      addr.sin_len = sizeof (struct sockaddr_in);
#endif /* HAVE_STRUCT_SOCKADDR_IN_SIN_LEN */
    } else {
      memcpy(&addr, from, sizeof(addr));
    }
  
  /* sending port must always be the RIP port */
  addr.sin_port = htons (RIP_PORT_DEFAULT);
  
  /* Make datagram socket. */
  sock = socket (AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) 
    {
      zlog_err("Cannot create UDP socket: %s", safe_strerror(errno));
      exit (1);
    }

  setsockopt_so_broadcast (sock, 1);
  setsockopt_so_reuseaddr (sock, 1);
  setsockopt_so_reuseport (sock, 1);
  setsockopt_ipv4_tos (sock, IPTOS_PREC_INTERNETCONTROL);

  if (ripd_privs.change (ZPRIVS_RAISE))
      zlog_err ("rip_create_socket: could not raise privs");
  setsockopt_so_recvbuf (sock, RIP_UDP_RCV_BUF);
  if ( (ret = bind (sock, (struct sockaddr *) & addr, sizeof (addr))) < 0)
  
    {
      int save_errno = errno;
      if (ripd_privs.change (ZPRIVS_LOWER))
        zlog_err ("rip_create_socket: could not lower privs");
      
      zlog_err("%s: Can't bind socket %d to %s port %d: %s", __func__,
	       sock, inet_ntoa(addr.sin_addr), 
	       (int) ntohs(addr.sin_port), 
	       safe_strerror(save_errno));
      
      close (sock);
      return ret;
    }
  
  if (ripd_privs.change (ZPRIVS_LOWER))
      zlog_err ("rip_create_socket: could not lower privs");
      
  return sock;
}

/* RIP packet send to destination address, on interface denoted by
 * by connected argument. NULL to argument denotes destination should be
 * should be RIP multicast group
 */
static int
rip_send_packet (u_char * buf, int size, struct sockaddr_in *to,
                 struct connected *ifc)
{
  int ret, send_sock;
  struct sockaddr_in sin;
  
  assert (ifc != NULL);
  
  if (IS_RIP_DEBUG_PACKET)
    {
#define ADDRESS_SIZE 20
      char dst[ADDRESS_SIZE];
      dst[ADDRESS_SIZE - 1] = '\0';
      
      if (to)
        {
          strncpy (dst, inet_ntoa(to->sin_addr), ADDRESS_SIZE - 1);
        }
      else
        {
          sin.sin_addr.s_addr = htonl (INADDR_RIP_GROUP);
          strncpy (dst, inet_ntoa(sin.sin_addr), ADDRESS_SIZE - 1);
        }
#undef ADDRESS_SIZE
      zlog_debug("rip_send_packet %s > %s (%s)",
                inet_ntoa(ifc->address->u.prefix4),
                dst, ifc->ifp->name);
    }
  
  if ( CHECK_FLAG (ifc->flags, ZEBRA_IFA_SECONDARY) )
    {
      /*
       * ZEBRA_IFA_SECONDARY is set on linux when an interface is configured
       * with multiple addresses on the same subnet: the first address
       * on the subnet is configured "primary", and all subsequent addresses
       * on that subnet are treated as "secondary" addresses. 
       * In order to avoid routing-table bloat on other rip listeners, 
       * we do not send out RIP packets with ZEBRA_IFA_SECONDARY source addrs.
       * XXX Since Linux is the only system for which the ZEBRA_IFA_SECONDARY
       * flag is set, we would end up sending a packet for a "secondary"
       * source address on non-linux systems.  
       */
      if (IS_RIP_DEBUG_PACKET)
        zlog_debug("duplicate dropped");
      return 0;
    }

  /* Make destination address. */
  memset (&sin, 0, sizeof (struct sockaddr_in));
  sin.sin_family = AF_INET;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
  sin.sin_len = sizeof (struct sockaddr_in);
#endif /* HAVE_STRUCT_SOCKADDR_IN_SIN_LEN */

  /* When destination is specified, use it's port and address. */
  if (to)
    {
      sin.sin_port = to->sin_port;
      sin.sin_addr = to->sin_addr;
      send_sock = rip->sock;
    }
  else
    {
      struct sockaddr_in from;
      
      sin.sin_port = htons (RIP_PORT_DEFAULT);
      sin.sin_addr.s_addr = htonl (INADDR_RIP_GROUP);
      
      /* multicast send should bind to local interface address */
      from.sin_family = AF_INET;
      from.sin_port = htons (RIP_PORT_DEFAULT);
      from.sin_addr = ifc->address->u.prefix4;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
      from.sin_len = sizeof (struct sockaddr_in);
#endif /* HAVE_STRUCT_SOCKADDR_IN_SIN_LEN */
      
      /*
       * we have to open a new socket for each packet because this
       * is the most portable way to bind to a different source
       * ipv4 address for each packet. 
       */
      if ( (send_sock = rip_create_socket (&from)) < 0)
        {
          zlog_warn("rip_send_packet could not create socket.");
          return -1;
        }
      rip_interface_multicast_set (send_sock, ifc);
    }

  ret = sendto (send_sock, buf, size, 0, (struct sockaddr *)&sin,
		sizeof (struct sockaddr_in));

  if (IS_RIP_DEBUG_EVENT)
      zlog_debug ("SEND to  %s.%d", inet_ntoa(sin.sin_addr), 
                  ntohs (sin.sin_port));

  if (ret < 0)
    zlog_warn ("can't send packet : %s", safe_strerror (errno));

  if (!to)
    close(send_sock);

  return ret;
}

/* Add redistributed route to RIP table. */
void
rip_redistribute_add (int type, int sub_type, struct prefix_ipv4 *p, 
		      unsigned int ifindex, struct in_addr *nexthop,
                      unsigned int metric, unsigned char distance)
{
  int ret;
  struct route_node *rp;
  struct rip_info *rinfo;

  /* Redistribute route  */
  ret = rip_destination_check (p->prefix);
  if (! ret)
    return;

  rp = route_node_get (rip->table, (struct prefix *) p);

  rinfo = rp->info;

  if (rinfo)
    {
      if (rinfo->type == ZEBRA_ROUTE_CONNECT 
	  && rinfo->sub_type == RIP_ROUTE_INTERFACE
	  && rinfo->metric != RIP_METRIC_INFINITY)
	{
	  route_unlock_node (rp);
	  return;
	}

      /* Manually configured RIP route check. */
      if (rinfo->type == ZEBRA_ROUTE_RIP 
	  && ((rinfo->sub_type == RIP_ROUTE_STATIC) ||
	      (rinfo->sub_type == RIP_ROUTE_DEFAULT)) )
	{
	  if (type != ZEBRA_ROUTE_RIP || ((sub_type != RIP_ROUTE_STATIC) &&
	                                  (sub_type != RIP_ROUTE_DEFAULT)))
	    {
	      route_unlock_node (rp);
	      return;
	    }
	}

      RIP_TIMER_OFF (rinfo->t_timeout);
      RIP_TIMER_OFF (rinfo->t_garbage_collect);

      if (rip_route_rte (rinfo))
	rip_zebra_ipv4_delete ((struct prefix_ipv4 *)&rp->p, &rinfo->nexthop,
			       rinfo->metric);
      rp->info = NULL;
      rip_info_free (rinfo);
      
      route_unlock_node (rp);      
    }

  rinfo = rip_info_new ();
    
  rinfo->type = type;
  rinfo->sub_type = sub_type;
  rinfo->ifindex = ifindex;
  rinfo->metric = 1;
  rinfo->external_metric = metric;
  rinfo->distance = distance;
  rinfo->rp = rp;

  if (nexthop)
    rinfo->nexthop = *nexthop;

  rinfo->flags |= RIP_RTF_FIB;
  rp->info = rinfo;

  rinfo->flags |= RIP_RTF_CHANGED;

  if (IS_RIP_DEBUG_EVENT) {
    if (!nexthop)
      zlog_debug ("Redistribute new prefix %s/%d on the interface %s",
                 inet_ntoa(p->prefix), p->prefixlen,
                 ifindex2ifname(ifindex));
    else
      zlog_debug ("Redistribute new prefix %s/%d with nexthop %s on the interface %s",
                 inet_ntoa(p->prefix), p->prefixlen, inet_ntoa(rinfo->nexthop),
                 ifindex2ifname(ifindex));
  }


  rip_event (RIP_TRIGGERED_UPDATE, 0);
}

/* Delete redistributed route from RIP table. */
void
rip_redistribute_delete (int type, int sub_type, struct prefix_ipv4 *p, 
			   unsigned int ifindex)
{
  int ret;
  struct route_node *rp;
  struct rip_info *rinfo;

  ret = rip_destination_check (p->prefix);
  if (! ret)
    return;

  rp = route_node_lookup (rip->table, (struct prefix *) p);
  if (rp)
    {
      rinfo = rp->info;

      if (rinfo != NULL
	  && rinfo->type == type 
	  && rinfo->sub_type == sub_type 
	  && rinfo->ifindex == ifindex)
	{
	  /* Perform poisoned reverse. */
	  rinfo->metric = RIP_METRIC_INFINITY;
	  RIP_TIMER_ON (rinfo->t_garbage_collect, 
			rip_garbage_collect, rip->garbage_time);
	  RIP_TIMER_OFF (rinfo->t_timeout);
	  rinfo->flags |= RIP_RTF_CHANGED;

          if (IS_RIP_DEBUG_EVENT)
            zlog_debug ("Poisone %s/%d on the interface %s with an infinity metric [delete]",
                       inet_ntoa(p->prefix), p->prefixlen,
                       ifindex2ifname(ifindex));

	  rip_event (RIP_TRIGGERED_UPDATE, 0);
	}
    }
}

/* Response to request called from rip_read ().*/
static void
rip_request_process (struct rip_packet *packet, int size, 
		     struct sockaddr_in *from, struct connected *ifc)
{
  caddr_t lim;
  struct rte *rte;
  struct prefix_ipv4 p;
  struct route_node *rp;
  struct rip_info *rinfo;
  struct rip_interface *ri;
  struct stream *rtebuf, *response;
  unsigned allowed, buffered = 0;

  /* Does not reponse to the requests on the loopback interfaces */
  if (if_is_loopback (ifc->ifp))
    return;

  /* Check RIP process is enabled on this interface. */
  ri = ifc->ifp->info;
  if (! ri->running)
    return;

  /* When passive interface is specified, suppress responses */
  if (ri->passive)
    return;
  
  /* RIP peer update. */
  rip_peer_update (from, packet->version);

  lim = ((caddr_t) packet) + size;
  rte = packet->rte;
  allowed = rip_auth_allowed_inet_rtes (ri, packet->version);
  rtebuf = stream_new (allowed * RIP_RTE_SIZE);
  response = stream_new (RIP_PACKET_MAXSIZ);

  /* The Request is processed entry by entry.  If there are no
     entries, no response is given. */

  /* There is one special case.  If there is exactly one entry in the
     request, and it has an address family identifier of zero and a
     metric of infinity (i.e., 16), then this is a request to send the
     entire routing table. */

      /* Examine the list of RTEs in the Request one by one.  For each
	 entry, look up the destination in the router's routing
	 database and, if there is a route, put that route's metric in
	 the metric field of the RTE.  If there is no explicit route
	 to the specified destination, put infinity in the metric
	 field.  Once all the entries have been filled in, change the
	 command from Request to Response and send the datagram back
	 to the requestor. */
      p.family = AF_INET;

      for (; ((caddr_t) rte) < lim; rte++)
	{
	  char masklen = ip_masklen_safe (rte->mask);
	  if (ntohs (rte->family) == RIP_FAMILY_AUTH)
	    continue;
	  if (ntohs (rte->family) == 0 && ntohl (rte->metric) == RIP_METRIC_INFINITY)
	    {
	      rip_output_process (ifc, from, rip_all_route, packet->version);
	      break;
	    }
	  if (masklen < 0)
	    {
	      if (IS_RIP_DEBUG_RECV)
	        zlog_warn ("%s: malformed RIPv2 RTE netmask", __func__);
	      rte->metric = htonl (RIP_METRIC_INFINITY);
	      rip_peer_bad_route (from);
	      ri->recv_badroutes++;
	    }
	  else
	  {
	  p.prefix = rte->prefix;
	  p.prefixlen = masklen;
	  apply_mask_ipv4 (&p);
	  
	  rp = route_node_lookup (rip->table, (struct prefix *) &p);
	  if (rp)
	    {
	      rinfo = rp->info;
	      rte->metric = htonl (rinfo->metric);
	      route_unlock_node (rp);
	    }
	  else
	    rte->metric = htonl (RIP_METRIC_INFINITY);
	  }
	  stream_put (rtebuf, rte, RIP_RTE_SIZE);
	  /* send on buffer full or last RTE */
	  if (++buffered == allowed || rte + 1 == (struct rte *)lim)
	    {
	      if (rip_auth_make_packet (ri, response, rtebuf, packet->version, RIP_RESPONSE) < 0)
	        zlog_err ("%s: rip_auth_make_packet() failed", __func__);
	      else
	        {
	          int sent = rip_send_packet (stream_get_data (response), stream_get_endp (response), from, ifc);
	          if (sent > 0 && IS_RIP_DEBUG_SEND)
	            rip_packet_dump ((struct rip_packet *) stream_get_data (response), sent, "SEND");
	        }
	      buffered = 0;
	      stream_reset (rtebuf);
	    }
	}
  stream_free (rtebuf);
  stream_free (response);
  rip_global_queries++;
}

static int
rip_packet_examin
(
  struct rip_interface *ri,
  struct rip_packet *packet,
  size_t bytesonwire,            /* from start of RIP header to end of frame */
  const size_t bending_bytes,    /* for IOS proprietary MD5 authentication   */
  const u_char relaxed_rx
)
{
  u_int8_t declared_auth_len = 0;
  u_int16_t declared_packet_len = 0;
  unsigned header_rte = 0, af0_rte = 0, inet_rtes = 0;
  u_char auth_trailer_missing = 0;
  struct rte *rte = packet->rte;
  struct rip_auth_rte *auth;

  /* header checks */
  if (bytesonwire < RIP_HEADER_SIZE)
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: undersized (%zuB) packet", __func__, bytesonwire);
    return MSG_NG;
  }
  if (packet->version != RIPv1 && packet->version != RIPv2)
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: unsupported version %u", __func__, packet->version);
    return MSG_NG;
  }
  if (packet->command != RIP_REQUEST && packet->command != RIP_RESPONSE)
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: unsupported command %u", __func__, packet->command);
    return MSG_NG;
  }
  if (! relaxed_rx && bytesonwire > RIP_PACKET_MAXSIZ)
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: oversized (%zuB) packet failed strict size check", __func__, bytesonwire);
    return MSG_NG;
  }
  bytesonwire -= RIP_HEADER_SIZE;

  /* RTEs/trailer checks */
  while (bytesonwire >= RIP_RTE_SIZE)
  {
    switch (ntohs (rte->family))
    {
    case 0: /* full table request */
      if (packet->command != RIP_REQUEST)
      {
        if (IS_RIP_DEBUG_RECV)
          zlog_warn ("%s: AF 0 RTE #%u in a response packet", __func__, header_rte + inet_rtes);
        return MSG_NG;
      }
      if (af0_rte)
      {
        if (IS_RIP_DEBUG_RECV)
          zlog_warn ("%s: duplicate AF 0 RTE #%u", __func__, 1 + header_rte + inet_rtes);
        return MSG_NG;
      }
      af0_rte = 1;
      break;
    case AF_INET:
      inet_rtes++;
      break;
    case RIP_FAMILY_AUTH:
      if (packet->version == RIPv1)
      {
        if (IS_RIP_DEBUG_RECV)
          zlog_warn ("%s: authentication family RTE in a RIP-1 packet", __func__);
        return MSG_NG;
      }
      auth = (struct rip_auth_rte *) rte;
      switch (ntohs (auth->type))
      {
      case RIP_AUTH_SIMPLE_PASSWORD:
        if (header_rte + af0_rte + inet_rtes)
        {
          if (IS_RIP_DEBUG_RECV)
            zlog_warn ("%s: simple authentication header does not come first (%u)",
              __func__, header_rte + af0_rte + inet_rtes);
          return MSG_NG;
        }
        header_rte = 1;
        break;
      case RIP_AUTH_HASH:
        if (header_rte + af0_rte + inet_rtes)
        {
          if (IS_RIP_DEBUG_RECV)
            zlog_warn ("%s: hash authentication header does not come first (%u)",
              __func__, header_rte + af0_rte + inet_rtes);
          return MSG_NG;
        }
        auth_trailer_missing = 1;
        declared_packet_len = ntohs (auth->u.hash_info.packet_len);
        declared_auth_len = auth->u.hash_info.auth_len;
        header_rte = 1;
        break;
      case RIP_AUTH_DATA:
        if (! auth_trailer_missing)
        {
          if (IS_RIP_DEBUG_RECV)
            zlog_warn ("%s: unexpected authentication trailer after %u fixed RTEs",
              __func__, header_rte + af0_rte + inet_rtes);
          return MSG_NG;
        }
        /* header_rte == 1 */
        if (declared_packet_len != RIP_HEADER_SIZE + (1 + af0_rte + inet_rtes) * RIP_RTE_SIZE)
        {
          if (IS_RIP_DEBUG_RECV)
            zlog_warn ("%s: packet length declared %u in auth header despite %u fixed RTEs",
                       __func__, declared_packet_len, 1 + af0_rte + inet_rtes);
          return MSG_NG;
        }
        /* The trailer must be within the remaining buffer, unused bytes don't matter. */
        if (declared_auth_len + 4U > bytesonwire + bending_bytes)
        {
          if (IS_RIP_DEBUG_RECV)
            zlog_warn ("%s: authentication trailer does not fit the packet", __func__);
          return MSG_NG;
        }
        /* been there, no next pass */
        auth_trailer_missing = 0;
        bytesonwire = RIP_RTE_SIZE;
        break;
      default:
        if (IS_RIP_DEBUG_RECV)
          zlog_warn ("%s: unknown authentication type %u", __func__, ntohs (auth->type));
        return MSG_NG;
      } /* switch (type) */
      break;
    default:
      if (IS_RIP_DEBUG_RECV)
        zlog_warn ("%s: unknown RTE family %u", __func__, ntohs (rte->family));
      return MSG_NG;
    } /* switch (family) */
    rte = (struct rte *) (((caddr_t) rte) + RIP_RTE_SIZE);
    bytesonwire -= RIP_RTE_SIZE;
  } /* while() */
  if (bytesonwire)
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: unknown trailing data (%zuB)", __func__, bytesonwire);
    return MSG_NG;
  }
  /* "There may be between 1 and 25 (inclusive) RIP entries." -- RFC2453 3.6
   * In the scope of packet size measurement the following is true:
   * 1. Authentication header and trailer count for the upper limit only.
   * 2. The ultimate upper limit for number of non-trailer RTEs is 25.
   * 3. Different treatments may count an authentication trailer for 0, 1 or
   *    more RTEs. This means, that number of inet AF RTEs should be lowered
   *    accordingly, "relaxed size check" runtime mode addresses this concern.
   */
  if (! (af0_rte + inet_rtes) || (header_rte + af0_rte + inet_rtes) > RIP_MAX_RTE)
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: malformed packet: %u auth header RTE(s), %u AF 0 RTE(s), %u inet RTE(s)",
        __func__, header_rte, af0_rte, inet_rtes);
    return MSG_NG;
  }
  if (! relaxed_rx && inet_rtes > rip_auth_allowed_inet_rtes (ri, packet->version))
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: too many (%u) inet RTEs for strict size check", __func__, inet_rtes);
    return MSG_NG;
  }
  /* There may be at most 1 AFI 0 RTE, in which case it must be the only RTE. */
  if (af0_rte && inet_rtes)
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: both AF 0 and %u inet RTE(s) in the packet", __func__, inet_rtes);
    return MSG_NG;
  }
  if (auth_trailer_missing)
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_warn ("%s: hash authentication header is present, but trailer is not", __func__);
    return MSG_NG;
  }
  return MSG_OK;
}

/* First entry point of RIP packet. */
static int
rip_read (struct thread *t)
{
  int sock;
  union rip_buf rip_buf;
  struct rip_packet *packet;
  struct sockaddr_in from;
  int len;
  int vrecv;
  socklen_t fromlen;
  struct interface *ifp;
  struct connected *ifc;
  struct rip_interface *ri;
  size_t bending_bytes = 0;

  /* Fetch socket then register myself. */
  sock = THREAD_FD (t);
  rip->t_read = NULL;

  /* Add myself to tne next event */
  rip_event (RIP_READ, sock);

  /* RIPd manages only IPv4. */
  memset (&from, 0, sizeof (struct sockaddr_in));
  fromlen = sizeof (struct sockaddr_in);

  len = recvfrom (sock, rip_buf.buf, sizeof (rip_buf.buf), 0,
		  (struct sockaddr *) &from, &fromlen);
  if (len < 0) 
    {
      zlog_info ("recvfrom failed: %s", safe_strerror (errno));
      return len;
    }

  /* Check is this packet comming from myself? */
  if (if_check_address (from.sin_addr)) 
    {
      if (IS_RIP_DEBUG_PACKET)
	zlog_debug ("ignore packet comes from myself");
      return -1;
    }

  /* Which interface is this packet comes from. */
  ifp = if_lookup_address (from.sin_addr);
  
  /* RIP packet received */
  if (IS_RIP_DEBUG_EVENT)
    zlog_debug ("RECV packet from %s port %d on %s",
	       inet_ntoa (from.sin_addr), ntohs (from.sin_port),
	       ifp ? ifp->name : "unknown");

  /* If this packet come from unknown interface, ignore it. */
  if (ifp == NULL)
    {
      zlog_info ("rip_read: cannot find interface for packet from %s port %d",
		 inet_ntoa(from.sin_addr), ntohs (from.sin_port));
      return -1;
    }
  
  ifc = connected_lookup_address (ifp, from.sin_addr);
  
  if (ifc == NULL)
    {
      zlog_info ("rip_read: cannot find connected address for packet from %s "
		 "port %d on interface %s",
		 inet_ntoa(from.sin_addr), ntohs (from.sin_port), ifp->name);
      return -1;
    }

  /* For easy to handle. */
  packet = &rip_buf.rip_packet;
  ri = ifp->info;

  /* In MD5 authentication mode declared length may require a non-RFC offset. */
  if (ri->auth_type == RIP_AUTH_HASH)
    bending_bytes = ri->md5_auth_len - HASH_SIZE_MD5;

  if (rip_packet_examin (ri, packet, len, bending_bytes, rip->relaxed_recv_size_checks) != MSG_OK)
    {
      rip_peer_bad_packet (&from);
      ri->recv_badpackets++;
      return -1;
    }

  /* Dump RIP packet. */
  if (IS_RIP_DEBUG_RECV)
    rip_packet_dump (packet, len, "RECV");

  /* Is RIP running or is this RIP neighbor ?*/
  if (! ri->running && ! rip_neighbor_lookup (&from))
    {
      if (IS_RIP_DEBUG_EVENT)
	zlog_debug ("RIP is not enabled on interface %s.", ifp->name);
      rip_peer_bad_packet (&from);
      ri->recv_badpackets++;
      return -1;
    }

  /* RIP Version check. RFC2453, 4.6 and 5.1 */
  vrecv = ((ri->ri_receive == RI_RIP_UNSPEC) ?
           rip->version_recv : ri->ri_receive);
  if ((packet->version == RIPv1) && !(vrecv & RIPv1))
    {
      if (IS_RIP_DEBUG_PACKET)
        zlog_debug ("  packet's v%d doesn't fit to if version spec", 
                   packet->version);
      rip_peer_bad_packet (&from);
      ri->recv_badpackets++;
      return -1;
    }
  if ((packet->version == RIPv2) && !(vrecv & RIPv2))
    {
      if (IS_RIP_DEBUG_PACKET)
        zlog_debug ("  packet's v%d doesn't fit to if version spec", 
                   packet->version);
      rip_peer_bad_packet (&from);
      ri->recv_badpackets++;
      return -1;
    }

  /* rip_auth_check_packet() will handle logging */
  if (! (len = rip_auth_check_packet (ri, &from, packet, len)))
  {
    if (IS_RIP_DEBUG_RECV)
      zlog_debug ("authentication check failed, packet discarded");
    return -1;
  }
  
  /* Process each command. */
  switch (packet->command)
    {
    case RIP_RESPONSE:
      rip_response_process (packet, len, &from, ifc);
      break;
    case RIP_REQUEST:
      rip_request_process (packet, len, &from, ifc);
      break;
    default:
      assert (0);
    }

  return len;
}

/* Write routing table entry to the stream and return next index of
   the routing table entry in the stream. */
static int
rip_write_rte (int num, struct stream *s, struct prefix_ipv4 *p,
               u_char version, struct rip_info *rinfo)
{
  struct in_addr mask;

  /* Write routing table entry. */
  if (version == RIPv1)
    {
      stream_putw (s, AF_INET);
      stream_putw (s, 0);
      stream_put_ipv4 (s, p->prefix.s_addr);
      stream_put_ipv4 (s, 0);
      stream_put_ipv4 (s, 0);
      stream_putl (s, rinfo->metric_out);
    }
  else
    {
      masklen2ip (p->prefixlen, &mask);

      stream_putw (s, AF_INET);
      stream_putw (s, rinfo->tag_out);
      stream_put_ipv4 (s, p->prefix.s_addr);
      stream_put_ipv4 (s, mask.s_addr);
      stream_put_ipv4 (s, rinfo->nexthop_out.s_addr);
      stream_putl (s, rinfo->metric_out);
    }

  return ++num;
}

/* Send update to the ifp or spcified neighbor. */
void
rip_output_process (struct connected *ifc, struct sockaddr_in *to, 
                    int route_type, u_char version)
{
  int ret;
  struct stream *s;
  struct stream *rtebuf;
  struct route_node *rp;
  struct rip_info *rinfo;
  struct rip_interface *ri;
  struct prefix_ipv4 *p;
  struct prefix_ipv4 classfull;
  struct prefix_ipv4 ifaddrclass;
  /* this might need to made dynamic if RIP ever supported auth methods
     with larger key string sizes */
  int num = 0;
  int rtemax;
  int subnetted = 0;

  /* Logging output event. */
  if (IS_RIP_DEBUG_EVENT)
    {
      if (to)
	zlog_debug ("update routes to neighbor %s", inet_ntoa (to->sin_addr));
      else
	zlog_debug ("update routes on interface %s ifindex %d",
		   ifc->ifp->name, ifc->ifp->ifindex);
    }

  /* Set output stream. */
  s = rip->obuf;

  /* Detect packet layout and setup RTE buffer appropriately. */
  rtemax = rip_auth_allowed_inet_rtes (ifc->ifp->info, version);
  rtebuf = stream_new (rtemax * RIP_RTE_SIZE);

  /* Get RIP interface. */
  ri = ifc->ifp->info;
    
  if (version == RIPv1)
    {
      memcpy (&ifaddrclass, ifc->address, sizeof (struct prefix_ipv4));
      apply_classful_mask_ipv4 (&ifaddrclass);
      subnetted = 0;
      if (ifc->address->prefixlen > ifaddrclass.prefixlen)
        subnetted = 1;
    }

  for (rp = route_top (rip->table); rp; rp = route_next (rp))
    if ((rinfo = rp->info) != NULL)
      {
	/* For RIPv1, if we are subnetted, output subnets in our network    */
	/* that have the same mask as the output "interface". For other     */
	/* networks, only the classfull version is output.                  */
	
	if (version == RIPv1)
	  {
	    p = (struct prefix_ipv4 *) &rp->p;

	    if (IS_RIP_DEBUG_PACKET)
	      zlog_debug("RIPv1 mask check, %s/%d considered for output",
			inet_ntoa (rp->p.u.prefix4), rp->p.prefixlen);

	    if (subnetted &&
		prefix_match ((struct prefix *) &ifaddrclass, &rp->p))
	      {
		if ((ifc->address->prefixlen != rp->p.prefixlen) &&
		    (rp->p.prefixlen != 32))
		  continue;
	      }
	    else
	      {
		memcpy (&classfull, &rp->p, sizeof(struct prefix_ipv4));
		apply_classful_mask_ipv4(&classfull);
		if (rp->p.u.prefix4.s_addr != 0 &&
		    classfull.prefixlen != rp->p.prefixlen)
		  continue;
	      }
	    if (IS_RIP_DEBUG_PACKET)
	      zlog_debug("RIPv1 mask check, %s/%d made it through",
			inet_ntoa (rp->p.u.prefix4), rp->p.prefixlen);
	  }
	else 
	  p = (struct prefix_ipv4 *) &rp->p;

	/* Apply output filters. */
	ret = rip_filter (RIP_FILTER_OUT, p, ri);
	if (ret < 0)
	  continue;

	/* Changed route only output. */
	if (route_type == rip_changed_route &&
	    (! (rinfo->flags & RIP_RTF_CHANGED)))
	  continue;

	/* Split horizon. */
	/* if (split_horizon == rip_split_horizon) */
	if (ri->split_horizon == RIP_SPLIT_HORIZON)
	  {
	    /* 
	     * We perform split horizon for RIP and connected route. 
	     * For rip routes, we want to suppress the route if we would
             * end up sending the route back on the interface that we
             * learned it from, with a higher metric. For connected routes,
             * we suppress the route if the prefix is a subset of the
             * source address that we are going to use for the packet 
             * (in order to handle the case when multiple subnets are
             * configured on the same interface).
             */
	    if (rinfo->type == ZEBRA_ROUTE_RIP  &&
                 rinfo->ifindex == ifc->ifp->ifindex) 
	      continue;
	    if (rinfo->type == ZEBRA_ROUTE_CONNECT &&
                 prefix_match((struct prefix *)p, ifc->address))
	      continue;
	  }

	/* Preparation for route-map. */
	rinfo->metric_set = 0;
	rinfo->nexthop_out.s_addr = 0;
	rinfo->metric_out = rinfo->metric;
	rinfo->tag_out = rinfo->tag;
	rinfo->ifindex_out = ifc->ifp->ifindex;

	/* In order to avoid some local loops,
	 * if the RIP route has a nexthop via this interface, keep the nexthop,
	 * otherwise set it to 0. The nexthop should not be propagated
	 * beyond the local broadcast/multicast area in order
	 * to avoid an IGP multi-level recursive look-up.
	 * see (4.4)
	 */
	if (rinfo->ifindex == ifc->ifp->ifindex)
	  rinfo->nexthop_out = rinfo->nexthop;

	/* Interface route-map */
	if (ri->routemap[RIP_FILTER_OUT])
	  {
	    ret = route_map_apply (ri->routemap[RIP_FILTER_OUT], 
				     (struct prefix *) p, RMAP_RIP, 
				     rinfo);

	    if (ret == RMAP_DENYMATCH)
	      {
	        if (IS_RIP_DEBUG_PACKET)
	          zlog_debug ("RIP %s/%d is filtered by route-map out",
			     inet_ntoa (p->prefix), p->prefixlen);
		  continue;
	      }
	  }
           
	/* Apply redistribute route map - continue, if deny */
	if (rip->route_map[rinfo->type].name
	    && rinfo->sub_type != RIP_ROUTE_INTERFACE)
	  {
	    ret = route_map_apply (rip->route_map[rinfo->type].map,
				   (struct prefix *)p, RMAP_RIP, rinfo);

	    if (ret == RMAP_DENYMATCH) 
	      {
		if (IS_RIP_DEBUG_PACKET)
		  zlog_debug ("%s/%d is filtered by route-map",
			     inet_ntoa (p->prefix), p->prefixlen);
		continue;
	      }
	  }

	/* When route-map does not set metric. */
	if (! rinfo->metric_set)
	  {
	    /* If redistribute metric is set. */
	    if (rip->route_map[rinfo->type].metric_config
		&& rinfo->metric != RIP_METRIC_INFINITY)
	      {
		rinfo->metric_out = rip->route_map[rinfo->type].metric;
	      }
	    else
	      {
		/* If the route is not connected or localy generated
		   one, use default-metric value*/
		if (rinfo->type != ZEBRA_ROUTE_RIP 
		    && rinfo->type != ZEBRA_ROUTE_CONNECT
		    && rinfo->metric != RIP_METRIC_INFINITY)
		  rinfo->metric_out = rip->default_metric;
	      }
	  }

	/* Apply offset-list */
	if (rinfo->metric != RIP_METRIC_INFINITY)
	  rip_offset_list_apply_out (p, ifc->ifp, &rinfo->metric_out);

	if (rinfo->metric_out > RIP_METRIC_INFINITY)
	  rinfo->metric_out = RIP_METRIC_INFINITY;

	/* Perform split-horizon with poisoned reverse 
	 * for RIP and connected routes.
	 **/
	if (ri->split_horizon == RIP_SPLIT_HORIZON_POISONED_REVERSE) {
	    /* 
	     * We perform split horizon for RIP and connected route. 
	     * For rip routes, we want to suppress the route if we would
             * end up sending the route back on the interface that we
             * learned it from, with a higher metric. For connected routes,
             * we suppress the route if the prefix is a subset of the
             * source address that we are going to use for the packet 
             * (in order to handle the case when multiple subnets are
             * configured on the same interface).
             */
	  if (rinfo->type == ZEBRA_ROUTE_RIP  &&
	       rinfo->ifindex == ifc->ifp->ifindex)
	       rinfo->metric_out = RIP_METRIC_INFINITY;
	  if (rinfo->type == ZEBRA_ROUTE_CONNECT &&
              prefix_match((struct prefix *)p, ifc->address))
	       rinfo->metric_out = RIP_METRIC_INFINITY;
	}
	
	/* Write RTE to the stream. */
	num = rip_write_rte (num, rtebuf, p, version, rinfo);
	if (num == rtemax)
	  {
	    if (rip_auth_make_packet (ifc->ifp->info, s, rtebuf, version, RIP_RESPONSE) < 0)
	      {
	        stream_free (rtebuf);
	        return;
	      }
	    ret = rip_send_packet (STREAM_DATA (s), stream_get_endp (s),
				   to, ifc);

	    if (ret >= 0 && IS_RIP_DEBUG_SEND)
	      rip_packet_dump ((struct rip_packet *)STREAM_DATA (s),
			       stream_get_endp(s), "SEND");
	    num = 0;
	  }
      }

  /* Flush unwritten RTE. */
  if (num != 0)
    {
      if (rip_auth_make_packet (ifc->ifp->info, s, rtebuf, version, RIP_RESPONSE) < 0)
        {
          stream_free (rtebuf);
          return;
        }

      ret = rip_send_packet (STREAM_DATA (s), stream_get_endp (s), to, ifc);

      if (ret >= 0 && IS_RIP_DEBUG_SEND)
	rip_packet_dump ((struct rip_packet *)STREAM_DATA (s),
			 stream_get_endp (s), "SEND");
    }

  stream_free (rtebuf);
  /* Statistics updates. */
  ri->sent_updates++;
}

/* Send RIP packet to the interface. */
static void
rip_update_interface (struct connected *ifc, u_char version, int route_type)
{
  struct sockaddr_in to;

  /* When RIP version is 2 and multicast enable interface. */
  if (version == RIPv2 && if_is_multicast (ifc->ifp)) 
    {
      if (IS_RIP_DEBUG_EVENT)
	zlog_debug ("multicast announce on %s ", ifc->ifp->name);

      rip_output_process (ifc, NULL, route_type, version);
      return;
    }
  
  /* If we can't send multicast packet, send it with unicast. */
  if (if_is_broadcast (ifc->ifp) || if_is_pointopoint (ifc->ifp))
    {
      if (ifc->address->family == AF_INET)
        {
          /* Destination address and port setting. */
          memset (&to, 0, sizeof (struct sockaddr_in));
          if (ifc->destination)
            /* use specified broadcast or peer destination addr */
            to.sin_addr = ifc->destination->u.prefix4;
          else if (ifc->address->prefixlen < IPV4_MAX_PREFIXLEN)
            /* calculate the appropriate broadcast address */
            to.sin_addr.s_addr =
              ipv4_broadcast_addr(ifc->address->u.prefix4.s_addr,
                                  ifc->address->prefixlen);
	  else
	    /* do not know where to send the packet */
	    return;
          to.sin_port = htons (RIP_PORT_DEFAULT);

          if (IS_RIP_DEBUG_EVENT)
            zlog_debug("%s announce to %s on %s",
		       CONNECTED_PEER(ifc) ? "unicast" : "broadcast",
		       inet_ntoa (to.sin_addr), ifc->ifp->name);

          rip_output_process (ifc, &to, route_type, version);
        }
    }
}

/* Update send to all interface and neighbor. */
static void
rip_update_process (int route_type)
{
  struct listnode *node;
  struct listnode *ifnode, *ifnnode;
  struct connected *connected;
  struct interface *ifp;
  struct rip_interface *ri;
  struct route_node *rp;
  struct sockaddr_in to;
  struct prefix_ipv4 *p;

  /* Send RIP update to each interface. */
  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    {
      if (if_is_loopback (ifp))
	continue;

      if (! if_is_operative (ifp))
	continue;

      /* Fetch RIP interface information. */
      ri = ifp->info;

      /* When passive interface is specified, suppress announce to the
         interface. */
      if (ri->passive)
	continue;

      if (ri->running)
	{
	  /* 
	   * If there is no version configuration in the interface,
	   * use rip's version setting. 
	   */
	  int vsend = ((ri->ri_send == RI_RIP_UNSPEC) ?
		       rip->version_send : ri->ri_send);

	  if (IS_RIP_DEBUG_EVENT) 
	    zlog_debug("SEND UPDATE to %s ifindex %d",
		       (ifp->name ? ifp->name : "_unknown_"), ifp->ifindex);

          /* send update on each connected network */
	  for (ALL_LIST_ELEMENTS (ifp->connected, ifnode, ifnnode, connected))
	    {
	      if (connected->address->family == AF_INET)
	        {
		  if (vsend & RIPv1)
		    rip_update_interface (connected, RIPv1, route_type);
		  if ((vsend & RIPv2) && if_is_multicast(ifp))
		    rip_update_interface (connected, RIPv2, route_type);
		}
	    }
	}
    }

  /* RIP send updates to each neighbor. */
  for (rp = route_top (rip->neighbor); rp; rp = route_next (rp))
    if (rp->info != NULL)
      {
	p = (struct prefix_ipv4 *) &rp->p;

	ifp = if_lookup_address (p->prefix);
	if (! ifp)
	  {
	    zlog_warn ("Neighbor %s doesnt have connected interface!",
		       inet_ntoa (p->prefix));
	    continue;
	  }
        
        if ( (connected = connected_lookup_address (ifp, p->prefix)) == NULL)
          {
            zlog_warn ("Neighbor %s doesnt have connected network",
                       inet_ntoa (p->prefix));
            continue;
          }
        
	/* Set destination address and port */
	memset (&to, 0, sizeof (struct sockaddr_in));
	to.sin_addr = p->prefix;
	to.sin_port = htons (RIP_PORT_DEFAULT);

	/* RIP version is rip's configuration. */
	rip_output_process (connected, &to, route_type, rip->version_send);
      }
}

/* RIP's periodical timer. */
static int
rip_update (struct thread *t)
{
  /* Clear timer pointer. */
  rip->t_update = NULL;

  if (IS_RIP_DEBUG_EVENT)
    zlog_debug ("update timer fire!");

  /* Process update output. */
  rip_update_process (rip_all_route);

  /* Triggered updates may be suppressed if a regular update is due by
     the time the triggered update would be sent. */
  if (rip->t_triggered_interval)
    {
      thread_cancel (rip->t_triggered_interval);
      rip->t_triggered_interval = NULL;
    }
  rip->trigger = 0;

  /* Register myself. */
  rip_event (RIP_UPDATE_EVENT, 0);

  return 0;
}

/* Walk down the RIP routing table then clear changed flag. */
static void
rip_clear_changed_flag (void)
{
  struct route_node *rp;
  struct rip_info *rinfo;

  for (rp = route_top (rip->table); rp; rp = route_next (rp))
    if ((rinfo = rp->info) != NULL)
      if (rinfo->flags & RIP_RTF_CHANGED)
	rinfo->flags &= ~RIP_RTF_CHANGED;
}

/* Triggered update interval timer. */
static int
rip_triggered_interval (struct thread *t)
{
  int rip_triggered_update (struct thread *);

  rip->t_triggered_interval = NULL;

  if (rip->trigger)
    {
      rip->trigger = 0;
      rip_triggered_update (t);
    }
  return 0;
}     

/* Execute triggered update. */
static int
rip_triggered_update (struct thread *t)
{
  int interval;

  /* Clear thred pointer. */
  rip->t_triggered_update = NULL;

  /* Cancel interval timer. */
  if (rip->t_triggered_interval)
    {
      thread_cancel (rip->t_triggered_interval);
      rip->t_triggered_interval = NULL;
    }
  rip->trigger = 0;

  /* Logging triggered update. */
  if (IS_RIP_DEBUG_EVENT)
    zlog_debug ("triggered update!");

  /* Split Horizon processing is done when generating triggered
     updates as well as normal updates (see section 2.6). */
  rip_update_process (rip_changed_route);

  /* Once all of the triggered updates have been generated, the route
     change flags should be cleared. */
  rip_clear_changed_flag ();

  /* After a triggered update is sent, a timer should be set for a
   random interval between 1 and 5 seconds.  If other changes that
   would trigger updates occur before the timer expires, a single
   update is triggered when the timer expires. */
  interval = (random () % 5) + 1;

  rip->t_triggered_interval = 
    thread_add_timer (master, rip_triggered_interval, NULL, interval);

  return 0;
}

/* Withdraw redistributed route. */
void
rip_redistribute_withdraw (int type)
{
  struct route_node *rp;
  struct rip_info *rinfo;

  if (!rip)
    return;

  for (rp = route_top (rip->table); rp; rp = route_next (rp))
    if ((rinfo = rp->info) != NULL)
      {
	if (rinfo->type == type
	    && rinfo->sub_type != RIP_ROUTE_INTERFACE)
	  {
	    /* Perform poisoned reverse. */
	    rinfo->metric = RIP_METRIC_INFINITY;
	    RIP_TIMER_ON (rinfo->t_garbage_collect, 
			  rip_garbage_collect, rip->garbage_time);
	    RIP_TIMER_OFF (rinfo->t_timeout);
	    rinfo->flags |= RIP_RTF_CHANGED;

	    if (IS_RIP_DEBUG_EVENT) {
              struct prefix_ipv4 *p = (struct prefix_ipv4 *) &rp->p;

              zlog_debug ("Poisone %s/%d on the interface %s with an infinity metric [withdraw]",
                         inet_ntoa(p->prefix), p->prefixlen,
                         ifindex2ifname(rinfo->ifindex));
	    }

	    rip_event (RIP_TRIGGERED_UPDATE, 0);
	  }
      }
}

/* Create new RIP instance and set it to global variable. */
static int
rip_create (void)
{
  rip = XCALLOC (MTYPE_RIP, sizeof (struct rip));

  /* Set initial value. */
  rip->version_send = RI_RIP_VERSION_2;
  rip->version_recv = RI_RIP_VERSION_1_AND_2;
  rip->update_time = RIP_UPDATE_TIMER_DEFAULT;
  rip->timeout_time = RIP_TIMEOUT_TIMER_DEFAULT;
  rip->garbage_time = RIP_GARBAGE_TIMER_DEFAULT;
  rip->default_metric = RIP_DEFAULT_METRIC_DEFAULT;

  /* Initialize RIP routig table. */
  rip->table = route_table_init ();
  rip->route = route_table_init ();
  rip->neighbor = route_table_init ();

  /* Make output stream. */
  rip->obuf = stream_new (1500);

  /* Make socket. */
  rip->sock = rip_create_socket (NULL);
  if (rip->sock < 0)
    return rip->sock;

  /* Create read and timer thread. */
  rip_event (RIP_READ, rip->sock);
  rip_event (RIP_UPDATE_EVENT, 1);

  return 0;
}

/* Send RIP Request to the destination. */
int
rip_request_send (struct sockaddr_in *to, struct interface *ifp,
		  u_char version, struct connected *connected)
{
  struct listnode *node, *nnode;
  struct stream *packet = stream_new (RIP_PACKET_MAXSIZ);
  struct stream *rtebuf = stream_new (RIP_RTE_SIZE);
  struct rip_interface *ri = ifp->info;
  int tosend, sent = -1;

  /* build packet */
  stream_put (rtebuf, NULL, 16); /* zero-fill up to metric */
  stream_putl (rtebuf, RIP_METRIC_INFINITY);
  if (rip_auth_make_packet (ri, packet, rtebuf, version, RIP_REQUEST) < 0)
    {
      stream_free (packet);
      stream_free (rtebuf);
      zlog_err ("%s: rip_auth_make_packet() failed", __func__);
      return -1;
    }
  stream_free (rtebuf);

  /* send packet */
  tosend = stream_get_endp (packet);
  if (connected) 
    {
      /* 
       * connected is only sent for ripv1 case, or when
       * interface does not support multicast.  Caller loops
       * over each connected address for this case.
       */
      sent = rip_send_packet (stream_get_data (packet), tosend, to, connected);
      if (sent >= 0 && IS_RIP_DEBUG_SEND)
        rip_packet_dump ((struct rip_packet *) stream_get_data (packet), sent, "SEND");
      stream_free (packet);
      return sent == tosend ? sent : -1;
    }
	
  /* send request on each connected network */
  for (ALL_LIST_ELEMENTS (ifp->connected, node, nnode, connected))
    {
      struct prefix_ipv4 *p = (struct prefix_ipv4 *) connected->address;

      if (p->family != AF_INET)
        continue;
      sent = rip_send_packet (stream_get_data (packet), tosend, to, connected);
      if (sent >= 0 && IS_RIP_DEBUG_SEND)
        rip_packet_dump ((struct rip_packet *) stream_get_data (packet), sent, "SEND");
      if (sent != tosend)
        break;
    }
  stream_free (packet);
  return sent == tosend ? sent : -1;
}

static int
rip_update_jitter (unsigned long time)
{
#define JITTER_BOUND 4
  /* We want to get the jitter to +/- 1/JITTER_BOUND the interval.
     Given that, we cannot let time be less than JITTER_BOUND seconds.
     The RIPv2 RFC says jitter should be small compared to
     update_time.  We consider 1/JITTER_BOUND to be small.
  */
  
  int jitter_input = time;
  int jitter;
  
  if (jitter_input < JITTER_BOUND)
    jitter_input = JITTER_BOUND;
  
  jitter = (((rand () % ((jitter_input * 2) + 1)) - jitter_input));  

  return jitter/JITTER_BOUND;
}

void
rip_event (enum rip_event event, int sock)
{
  int jitter = 0;

  switch (event)
    {
    case RIP_READ:
      rip->t_read = thread_add_read (master, rip_read, NULL, sock);
      break;
    case RIP_UPDATE_EVENT:
      if (rip->t_update)
	{
	  thread_cancel (rip->t_update);
	  rip->t_update = NULL;
	}
      jitter = rip_update_jitter (rip->update_time);
      rip->t_update = 
	thread_add_timer (master, rip_update, NULL, 
			  sock ? 2 : rip->update_time + jitter);
      break;
    case RIP_TRIGGERED_UPDATE:
      if (rip->t_triggered_interval)
	rip->trigger = 1;
      else if (! rip->t_triggered_update)
	rip->t_triggered_update = 
	  thread_add_event (master, rip_triggered_update, NULL, 0);
      break;
    default:
      break;
    }
}

DEFUN (router_rip,
       router_rip_cmd,
       "router rip",
       "Enable a routing process\n"
       "Routing Information Protocol (RIP)\n")
{
  int ret;

  /* If rip is not enabled before. */
  if (! rip)
    {
      ret = rip_create ();
      if (ret < 0)
	{
	  zlog_info ("Can't create RIP");
	  return CMD_WARNING;
	}
    }
  vty->node = RIP_NODE;
  vty->index = rip;

  return CMD_SUCCESS;
}

DEFUN (no_router_rip,
       no_router_rip_cmd,
       "no router rip",
       NO_STR
       "Enable a routing process\n"
       "Routing Information Protocol (RIP)\n")
{
  if (rip)
    rip_clean ();
  return CMD_SUCCESS;
}

DEFUN (rip_version,
       rip_version_cmd,
       "version <1-2>",
       "Set routing protocol version\n"
       "version\n")
{
  int version;

  version = atoi (argv[0]);
  if (version != RIPv1 && version != RIPv2)
    {
      vty_out (vty, "invalid rip version %d%s", version,
	       VTY_NEWLINE);
      return CMD_WARNING;
    }
  rip->version_send = version;
  rip->version_recv = version;

  return CMD_SUCCESS;
} 

DEFUN (no_rip_version,
       no_rip_version_cmd,
       "no version",
       NO_STR
       "Set routing protocol version\n")
{
  /* Set RIP version to the default. */
  rip->version_send = RI_RIP_VERSION_2;
  rip->version_recv = RI_RIP_VERSION_1_AND_2;

  return CMD_SUCCESS;
} 

ALIAS (no_rip_version,
       no_rip_version_val_cmd,
       "no version <1-2>",
       NO_STR
       "Set routing protocol version\n"
       "version\n")

DEFUN (rip_route,
       rip_route_cmd,
       "route A.B.C.D/M",
       "RIP static route configuration\n"
       "IP prefix <network>/<length>\n")
{
  int ret;
  struct prefix_ipv4 p;
  struct route_node *node;

  ret = str2prefix_ipv4 (argv[0], &p);
  if (ret == 0)
    {
      vty_out (vty, "Malformed address%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  apply_mask_ipv4 (&p);

  /* For router rip configuration. */
  node = route_node_get (rip->route, (struct prefix *) &p);

  if (node->info)
    {
      vty_out (vty, "There is already same static route.%s", VTY_NEWLINE);
      route_unlock_node (node);
      return CMD_WARNING;
    }

  node->info = (char *)"static";

  rip_redistribute_add (ZEBRA_ROUTE_RIP, RIP_ROUTE_STATIC, &p, 0, NULL, 0, 0);

  return CMD_SUCCESS;
}

DEFUN (no_rip_route,
       no_rip_route_cmd,
       "no route A.B.C.D/M",
       NO_STR
       "RIP static route configuration\n"
       "IP prefix <network>/<length>\n")
{
  int ret;
  struct prefix_ipv4 p;
  struct route_node *node;

  ret = str2prefix_ipv4 (argv[0], &p);
  if (ret == 0)
    {
      vty_out (vty, "Malformed address%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  apply_mask_ipv4 (&p);

  /* For router rip configuration. */
  node = route_node_lookup (rip->route, (struct prefix *) &p);
  if (! node)
    {
      vty_out (vty, "Can't find route %s.%s", argv[0],
	       VTY_NEWLINE);
      return CMD_WARNING;
    }

  rip_redistribute_delete (ZEBRA_ROUTE_RIP, RIP_ROUTE_STATIC, &p, 0);
  route_unlock_node (node);

  node->info = NULL;
  route_unlock_node (node);

  return CMD_SUCCESS;
}

#if 0
static void
rip_update_default_metric (void)
{
  struct route_node *np;
  struct rip_info *rinfo;

  for (np = route_top (rip->table); np; np = route_next (np))
    if ((rinfo = np->info) != NULL)
      if (rinfo->type != ZEBRA_ROUTE_RIP && rinfo->type != ZEBRA_ROUTE_CONNECT)
        rinfo->metric = rip->default_metric;
}
#endif

DEFUN (rip_default_metric,
       rip_default_metric_cmd,
       "default-metric <1-16>",
       "Set a metric of redistribute routes\n"
       "Default metric\n")
{
  if (rip)
    {
      rip->default_metric = atoi (argv[0]);
      /* rip_update_default_metric (); */
    }
  return CMD_SUCCESS;
}

DEFUN (no_rip_default_metric,
       no_rip_default_metric_cmd,
       "no default-metric",
       NO_STR
       "Set a metric of redistribute routes\n"
       "Default metric\n")
{
  if (rip)
    {
      rip->default_metric = RIP_DEFAULT_METRIC_DEFAULT;
      /* rip_update_default_metric (); */
    }
  return CMD_SUCCESS;
}

ALIAS (no_rip_default_metric,
       no_rip_default_metric_val_cmd,
       "no default-metric <1-16>",
       NO_STR
       "Set a metric of redistribute routes\n"
       "Default metric\n")

DEFUN (rip_timers,
       rip_timers_cmd,
       "timers basic <5-2147483647> <5-2147483647> <5-2147483647>",
       "Adjust routing timers\n"
       "Basic routing protocol update timers\n"
       "Routing table update timer value in second. Default is 30.\n"
       "Routing information timeout timer. Default is 180.\n"
       "Garbage collection timer. Default is 120.\n")
{
  unsigned long update;
  unsigned long timeout;
  unsigned long garbage;
  char *endptr = NULL;
  unsigned long RIP_TIMER_MAX = 2147483647;
  unsigned long RIP_TIMER_MIN = 5;

  update = strtoul (argv[0], &endptr, 10);
  if (update > RIP_TIMER_MAX || update < RIP_TIMER_MIN || *endptr != '\0')  
    {
      vty_out (vty, "update timer value error%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  
  timeout = strtoul (argv[1], &endptr, 10);
  if (timeout > RIP_TIMER_MAX || timeout < RIP_TIMER_MIN || *endptr != '\0') 
    {
      vty_out (vty, "timeout timer value error%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  
  garbage = strtoul (argv[2], &endptr, 10);
  if (garbage > RIP_TIMER_MAX || garbage < RIP_TIMER_MIN || *endptr != '\0') 
    {
      vty_out (vty, "garbage timer value error%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* Set each timer value. */
  rip->update_time = update;
  rip->timeout_time = timeout;
  rip->garbage_time = garbage;

  /* Reset update timer thread. */
  rip_event (RIP_UPDATE_EVENT, 0);

  return CMD_SUCCESS;
}

DEFUN (no_rip_timers,
       no_rip_timers_cmd,
       "no timers basic",
       NO_STR
       "Adjust routing timers\n"
       "Basic routing protocol update timers\n")
{
  /* Set each timer value to the default. */
  rip->update_time = RIP_UPDATE_TIMER_DEFAULT;
  rip->timeout_time = RIP_TIMEOUT_TIMER_DEFAULT;
  rip->garbage_time = RIP_GARBAGE_TIMER_DEFAULT;

  /* Reset update timer thread. */
  rip_event (RIP_UPDATE_EVENT, 0);

  return CMD_SUCCESS;
}

ALIAS (no_rip_timers,
       no_rip_timers_val_cmd,
       "no timers basic <0-65535> <0-65535> <0-65535>",
       NO_STR
       "Adjust routing timers\n"
       "Basic routing protocol update timers\n"
       "Routing table update timer value in second. Default is 30.\n"
       "Routing information timeout timer. Default is 180.\n"
       "Garbage collection timer. Default is 120.\n")


struct route_table *rip_distance_table;

struct rip_distance
{
  /* Distance value for the IP source prefix. */
  u_char distance;

  /* Name of the access-list to be matched. */
  char *access_list;
};

static struct rip_distance *
rip_distance_new (void)
{
  return XCALLOC (MTYPE_RIP_DISTANCE, sizeof (struct rip_distance));
}

static void
rip_distance_free (struct rip_distance *rdistance)
{
  XFREE (MTYPE_RIP_DISTANCE, rdistance);
}

static int
rip_distance_set (struct vty *vty, const char *distance_str, const char *ip_str,
		  const char *access_list_str)
{
  int ret;
  struct prefix_ipv4 p;
  u_char distance;
  struct route_node *rn;
  struct rip_distance *rdistance;

  ret = str2prefix_ipv4 (ip_str, &p);
  if (ret == 0)
    {
      vty_out (vty, "Malformed prefix%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  distance = atoi (distance_str);

  /* Get RIP distance node. */
  rn = route_node_get (rip_distance_table, (struct prefix *) &p);
  if (rn->info)
    {
      rdistance = rn->info;
      route_unlock_node (rn);
    }
  else
    {
      rdistance = rip_distance_new ();
      rn->info = rdistance;
    }

  /* Set distance value. */
  rdistance->distance = distance;

  /* Reset access-list configuration. */
  if (rdistance->access_list)
    {
      free (rdistance->access_list);
      rdistance->access_list = NULL;
    }
  if (access_list_str)
    rdistance->access_list = strdup (access_list_str);

  return CMD_SUCCESS;
}

static int
rip_distance_unset (struct vty *vty, const char *distance_str,
		    const char *ip_str, const char *access_list_str)
{
  int ret;
  struct prefix_ipv4 p;
  struct route_node *rn;
  struct rip_distance *rdistance;

  ret = str2prefix_ipv4 (ip_str, &p);
  if (ret == 0)
    {
      vty_out (vty, "Malformed prefix%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  rn = route_node_lookup (rip_distance_table, (struct prefix *)&p);
  if (! rn)
    {
      vty_out (vty, "Can't find specified prefix%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  rdistance = rn->info;

  if (rdistance->access_list)
    free (rdistance->access_list);
  rip_distance_free (rdistance);

  rn->info = NULL;
  route_unlock_node (rn);
  route_unlock_node (rn);

  return CMD_SUCCESS;
}

static void
rip_distance_reset (void)
{
  struct route_node *rn;
  struct rip_distance *rdistance;

  for (rn = route_top (rip_distance_table); rn; rn = route_next (rn))
    if ((rdistance = rn->info) != NULL)
      {
	if (rdistance->access_list)
	  free (rdistance->access_list);
	rip_distance_free (rdistance);
	rn->info = NULL;
	route_unlock_node (rn);
      }
}

/* Apply RIP information to distance method. */
static u_char
rip_distance_apply (struct rip_info *rinfo)
{
  struct route_node *rn;
  struct prefix_ipv4 p;
  struct rip_distance *rdistance;
  struct access_list *alist;

  if (! rip)
    return 0;

  memset (&p, 0, sizeof (struct prefix_ipv4));
  p.family = AF_INET;
  p.prefix = rinfo->from;
  p.prefixlen = IPV4_MAX_BITLEN;

  /* Check source address. */
  rn = route_node_match (rip_distance_table, (struct prefix *) &p);
  if (rn)
    {
      rdistance = rn->info;
      route_unlock_node (rn);

      if (rdistance->access_list)
	{
	  alist = access_list_lookup (AFI_IP, rdistance->access_list);
	  if (alist == NULL)
	    return 0;
	  if (access_list_apply (alist, &rinfo->rp->p) == FILTER_DENY)
	    return 0;

	  return rdistance->distance;
	}
      else
	return rdistance->distance;
    }

  if (rip->distance)
    return rip->distance;

  return 0;
}

static void
rip_distance_show (struct vty *vty)
{
  struct route_node *rn;
  struct rip_distance *rdistance;
  int header = 1;
  char buf[BUFSIZ];
  
  vty_out (vty, "  Distance: (default is %d)%s",
	   rip->distance ? rip->distance :ZEBRA_RIP_DISTANCE_DEFAULT,
	   VTY_NEWLINE);

  for (rn = route_top (rip_distance_table); rn; rn = route_next (rn))
    if ((rdistance = rn->info) != NULL)
      {
	if (header)
	  {
	    vty_out (vty, "    Address           Distance  List%s",
		     VTY_NEWLINE);
	    header = 0;
	  }
	sprintf (buf, "%s/%d", inet_ntoa (rn->p.u.prefix4), rn->p.prefixlen);
	vty_out (vty, "    %-20s  %4d  %s%s",
		 buf, rdistance->distance,
		 rdistance->access_list ? rdistance->access_list : "",
		 VTY_NEWLINE);
      }
}

DEFUN (rip_distance,
       rip_distance_cmd,
       "distance <1-255>",
       "Administrative distance\n"
       "Distance value\n")
{
  rip->distance = atoi (argv[0]);
  return CMD_SUCCESS;
}

DEFUN (no_rip_distance,
       no_rip_distance_cmd,
       "no distance <1-255>",
       NO_STR
       "Administrative distance\n"
       "Distance value\n")
{
  rip->distance = 0;
  return CMD_SUCCESS;
}

DEFUN (rip_distance_source,
       rip_distance_source_cmd,
       "distance <1-255> A.B.C.D/M",
       "Administrative distance\n"
       "Distance value\n"
       "IP source prefix\n")
{
  rip_distance_set (vty, argv[0], argv[1], NULL);
  return CMD_SUCCESS;
}

DEFUN (no_rip_distance_source,
       no_rip_distance_source_cmd,
       "no distance <1-255> A.B.C.D/M",
       NO_STR
       "Administrative distance\n"
       "Distance value\n"
       "IP source prefix\n")
{
  rip_distance_unset (vty, argv[0], argv[1], NULL);
  return CMD_SUCCESS;
}

DEFUN (rip_distance_source_access_list,
       rip_distance_source_access_list_cmd,
       "distance <1-255> A.B.C.D/M WORD",
       "Administrative distance\n"
       "Distance value\n"
       "IP source prefix\n"
       "Access list name\n")
{
  rip_distance_set (vty, argv[0], argv[1], argv[2]);
  return CMD_SUCCESS;
}

DEFUN (no_rip_distance_source_access_list,
       no_rip_distance_source_access_list_cmd,
       "no distance <1-255> A.B.C.D/M WORD",
       NO_STR
       "Administrative distance\n"
       "Distance value\n"
       "IP source prefix\n"
       "Access list name\n")
{
  rip_distance_unset (vty, argv[0], argv[1], argv[2]);
  return CMD_SUCCESS;
}

/* Print out routes update time. */
static void
rip_vty_out_uptime (struct vty *vty, struct rip_info *rinfo)
{
  time_t clock;
  struct tm *tm;
#define TIME_BUF 25
  char timebuf [TIME_BUF];
  struct thread *thread;

  if ((thread = rinfo->t_timeout) != NULL)
    {
      clock = thread_timer_remain_second (thread);
      tm = gmtime (&clock);
      strftime (timebuf, TIME_BUF, "%M:%S", tm);
      vty_out (vty, "%5s", timebuf);
    }
  else if ((thread = rinfo->t_garbage_collect) != NULL)
    {
      clock = thread_timer_remain_second (thread);
      tm = gmtime (&clock);
      strftime (timebuf, TIME_BUF, "%M:%S", tm);
      vty_out (vty, "%5s", timebuf);
    }
}

static const char *
rip_route_type_print (int sub_type)
{
  switch (sub_type)
    {
      case RIP_ROUTE_RTE:
	return "n";
      case RIP_ROUTE_STATIC:
	return "s";
      case RIP_ROUTE_DEFAULT:
	return "d";
      case RIP_ROUTE_REDISTRIBUTE:
	return "r";
      case RIP_ROUTE_INTERFACE:
	return "i";
      default:
	return "?";
    }
}

DEFUN (show_ip_rip,
       show_ip_rip_cmd,
       "show ip rip",
       SHOW_STR
       IP_STR
       "Show RIP routes\n")
{
  struct route_node *np;
  struct rip_info *rinfo;

  if (! rip)
    return CMD_SUCCESS;

  vty_out (vty, "Codes: R - RIP, C - connected, S - Static, O - OSPF, B - BGP%s"
	   "Sub-codes:%s"
           "      (n) - normal, (s) - static, (d) - default, (r) - redistribute,%s"
	   "      (i) - interface%s%s"
	   "     Network            Next Hop         Metric From            Tag Time%s",
	   VTY_NEWLINE, VTY_NEWLINE,  VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE);
  
  for (np = route_top (rip->table); np; np = route_next (np))
    if ((rinfo = np->info) != NULL)
      {
	int len;

	len = vty_out (vty, "%c(%s) %s/%d",
		       /* np->lock, For debugging. */
		       zebra_route_char(rinfo->type),
		       rip_route_type_print (rinfo->sub_type),
		       inet_ntoa (np->p.u.prefix4), np->p.prefixlen);
	
	len = 24 - len;

	if (len > 0)
	  vty_out (vty, "%*s", len, " ");

        if (rinfo->nexthop.s_addr) 
	  vty_out (vty, "%-20s %2d ", inet_ntoa (rinfo->nexthop),
		   rinfo->metric);
        else
	  vty_out (vty, "0.0.0.0              %2d ", rinfo->metric);

	/* Route which exist in kernel routing table. */
	if ((rinfo->type == ZEBRA_ROUTE_RIP) && 
	    (rinfo->sub_type == RIP_ROUTE_RTE))
	  {
	    vty_out (vty, "%-15s ", inet_ntoa (rinfo->from));
	    vty_out (vty, "%3d ", rinfo->tag);
	    rip_vty_out_uptime (vty, rinfo);
	  }
	else if (rinfo->metric == RIP_METRIC_INFINITY)
	  {
	    vty_out (vty, "self            ");
	    vty_out (vty, "%3d ", rinfo->tag);
	    rip_vty_out_uptime (vty, rinfo);
	  }
	else
	  {
	    if (rinfo->external_metric)
	      {
	        len = vty_out (vty, "self (%s:%d)", 
			       zebra_route_string(rinfo->type),
	                       rinfo->external_metric);
	        len = 16 - len;
	        if (len > 0)
	          vty_out (vty, "%*s", len, " ");
	      }
	    else
	      vty_out (vty, "self            ");
	    vty_out (vty, "%3d", rinfo->tag);
	  }

	vty_out (vty, "%s", VTY_NEWLINE);
      }
  return CMD_SUCCESS;
}

/* Vincent: formerly, it was show_ip_protocols_rip: "show ip protocols" */
DEFUN (show_ip_rip_status,
       show_ip_rip_status_cmd,
       "show ip rip status",
       SHOW_STR
       IP_STR
       "Show RIP routes\n"
       "IP routing protocol process parameters and statistics\n")
{
  struct listnode *node;
  struct interface *ifp;
  struct rip_interface *ri;
  const char *send_version;
  const char *receive_version;

  if (! rip)
    return CMD_SUCCESS;

  vty_out (vty, "Routing Protocol is \"rip\"%s", VTY_NEWLINE);
  vty_out (vty, "  Sending updates every %ld seconds with +/-50%%,",
	   rip->update_time);
  vty_out (vty, " next due in %lu seconds%s", 
	   thread_timer_remain_second(rip->t_update),
	   VTY_NEWLINE);
  vty_out (vty, "  Timeout after %ld seconds,", rip->timeout_time);
  vty_out (vty, " garbage collect after %ld seconds%s", rip->garbage_time,
	   VTY_NEWLINE);

  /* Filtering status show. */
  config_show_distribute (vty);
		 
  /* Default metric information. */
  vty_out (vty, "  Default redistribution metric is %d%s",
	   rip->default_metric, VTY_NEWLINE);

  vty_out (vty, "  Relaxed receiving size checks are %s%s",
	   rip->relaxed_recv_size_checks ? "on" : "off", VTY_NEWLINE);

  /* Redistribute information. */
  vty_out (vty, "  Redistributing:");
  config_write_rip_redistribute (vty, 0);
  vty_out (vty, "%s", VTY_NEWLINE);

  vty_out (vty, "  Default version control: send version %s,",
	   LOOKUP (ri_version_msg,rip->version_send));
  if (rip->version_recv == RI_RIP_VERSION_1_AND_2)
    vty_out (vty, " receive any version %s", VTY_NEWLINE);
  else
    vty_out (vty, " receive version %s %s",
	     LOOKUP (ri_version_msg,rip->version_recv), VTY_NEWLINE);

  vty_out (vty, "    Interface        Send  Recv   Key-chain%s", VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    {
      ri = ifp->info;

      if (!ri->running)
	continue;

      if (ri->enable_network || ri->enable_interface)
	{
	  if (ri->ri_send == RI_RIP_UNSPEC)
	    send_version = LOOKUP (ri_version_msg, rip->version_send);
	  else
	    send_version = LOOKUP (ri_version_msg, ri->ri_send);

	  if (ri->ri_receive == RI_RIP_UNSPEC)
	    receive_version = LOOKUP (ri_version_msg, rip->version_recv);
	  else
	    receive_version = LOOKUP (ri_version_msg, ri->ri_receive);
	
	  vty_out (vty, "    %-17s%-3s   %-3s    %s%s", ifp->name,
		   send_version,
		   receive_version,
		   ri->key_chain ? ri->key_chain : "",
		   VTY_NEWLINE);
	}
    }

  vty_out (vty, "  Routing for Networks:%s", VTY_NEWLINE);
  config_write_rip_network (vty, 0);  

  {
    int found_passive = 0;
    for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
      {
	ri = ifp->info;

	if ((ri->enable_network || ri->enable_interface) && ri->passive)
	  {
	    if (!found_passive)
	      {
		vty_out (vty, "  Passive Interface(s):%s", VTY_NEWLINE);
		found_passive = 1;
	      }
	    vty_out (vty, "    %s%s", ifp->name, VTY_NEWLINE);
	  }
      }
  }

  vty_out (vty, "  Routing Information Sources:%s", VTY_NEWLINE);
  vty_out (vty, "    Gateway          BadPackets BadRoutes  Distance Last Update%s", VTY_NEWLINE);
  rip_peer_display (vty);

  rip_distance_show (vty);

  return CMD_SUCCESS;
}

DEFUN (rip_relaxed_recv_size_checks,
       rip_relaxed_recv_size_checks_cmd,
       "relaxed-recv-size-checks",
       "Abide other treatments of RFC for received packets\n")
{
  if (rip)
    rip->relaxed_recv_size_checks = 1;
  return CMD_SUCCESS;
}

DEFUN (no_rip_relaxed_recv_size_checks,
       no_rip_relaxed_recv_size_checks_cmd,
       "no relaxed-recv-size-checks",
       NO_STR
       "Abide other treatments of RFC for received packets\n")
{
  if (rip)
    rip->relaxed_recv_size_checks = 0;
  return CMD_SUCCESS;
}

/* RIP configuration write function. */
static int
config_write_rip (struct vty *vty)
{
  int write = 0;
  struct route_node *rn;
  struct rip_distance *rdistance;

  if (rip)
    {
      /* Router RIP statement. */
      vty_out (vty, "router rip%s", VTY_NEWLINE);
      write++;
  
      /* RIP version statement.  Default is RIP version 2. */
      if (rip->version_send != RI_RIP_VERSION_2
	  || rip->version_recv != RI_RIP_VERSION_1_AND_2)
	vty_out (vty, " version %d%s", rip->version_send,
		 VTY_NEWLINE);
 
      /* RIP timer configuration. */
      if (rip->update_time != RIP_UPDATE_TIMER_DEFAULT 
	  || rip->timeout_time != RIP_TIMEOUT_TIMER_DEFAULT 
	  || rip->garbage_time != RIP_GARBAGE_TIMER_DEFAULT)
	vty_out (vty, " timers basic %lu %lu %lu%s",
		 rip->update_time,
		 rip->timeout_time,
		 rip->garbage_time,
		 VTY_NEWLINE);

      /* Default information configuration. */
      if (rip->default_information)
	{
	  if (rip->default_information_route_map)
	    vty_out (vty, " default-information originate route-map %s%s",
		     rip->default_information_route_map, VTY_NEWLINE);
	  else
	    vty_out (vty, " default-information originate%s",
		     VTY_NEWLINE);
	}

      /* Redistribute configuration. */
      config_write_rip_redistribute (vty, 1);

      /* RIP offset-list configuration. */
      config_write_rip_offset_list (vty);

      /* RIP enabled network and interface configuration. */
      config_write_rip_network (vty, 1);
			
      /* RIP default metric configuration */
      if (rip->default_metric != RIP_DEFAULT_METRIC_DEFAULT)
        vty_out (vty, " default-metric %d%s",
		 rip->default_metric, VTY_NEWLINE);

      /* Relaxed Rx, default is off. */
      if (rip->relaxed_recv_size_checks)
        vty_out (vty, " relaxed-recv-size-checks%s", VTY_NEWLINE);

      /* Distribute configuration. */
      write += config_write_distribute (vty);

      /* Interface routemap configuration */
      write += config_write_if_rmap (vty);

      /* Distance configuration. */
      if (rip->distance)
	vty_out (vty, " distance %d%s", rip->distance, VTY_NEWLINE);

      /* RIP source IP prefix distance configuration. */
      for (rn = route_top (rip_distance_table); rn; rn = route_next (rn))
	if ((rdistance = rn->info) != NULL)
	  vty_out (vty, " distance %d %s/%d %s%s", rdistance->distance,
		   inet_ntoa (rn->p.u.prefix4), rn->p.prefixlen,
		   rdistance->access_list ? rdistance->access_list : "",
		   VTY_NEWLINE);

      /* RIP static route configuration. */
      for (rn = route_top (rip->route); rn; rn = route_next (rn))
	if (rn->info)
	  vty_out (vty, " route %s/%d%s", 
		   inet_ntoa (rn->p.u.prefix4),
		   rn->p.prefixlen,
		   VTY_NEWLINE);

    }
  return write;
}

/* RIP node structure. */
static struct cmd_node rip_node =
{
  RIP_NODE,
  "%s(config-router)# ",
  1
};

/* Distribute-list update functions. */
static void
rip_distribute_update (struct distribute *dist)
{
  struct interface *ifp;
  struct rip_interface *ri;
  struct access_list *alist;
  struct prefix_list *plist;

  if (! dist->ifname)
    return;

  ifp = if_lookup_by_name (dist->ifname);
  if (ifp == NULL)
    return;

  ri = ifp->info;

  if (dist->list[DISTRIBUTE_V4_IN])
    {
      alist = access_list_lookup (AFI_IP, dist->list[DISTRIBUTE_V4_IN]);
      if (alist)
	ri->list[RIP_FILTER_IN] = alist;
      else
	ri->list[RIP_FILTER_IN] = NULL;
    }
  else
    ri->list[RIP_FILTER_IN] = NULL;

  if (dist->list[DISTRIBUTE_V4_OUT])
    {
      alist = access_list_lookup (AFI_IP, dist->list[DISTRIBUTE_V4_OUT]);
      if (alist)
	ri->list[RIP_FILTER_OUT] = alist;
      else
	ri->list[RIP_FILTER_OUT] = NULL;
    }
  else
    ri->list[RIP_FILTER_OUT] = NULL;

  if (dist->prefix[DISTRIBUTE_V4_IN])
    {
      plist = prefix_list_lookup (AFI_IP, dist->prefix[DISTRIBUTE_V4_IN]);
      if (plist)
	ri->prefix[RIP_FILTER_IN] = plist;
      else
	ri->prefix[RIP_FILTER_IN] = NULL;
    }
  else
    ri->prefix[RIP_FILTER_IN] = NULL;

  if (dist->prefix[DISTRIBUTE_V4_OUT])
    {
      plist = prefix_list_lookup (AFI_IP, dist->prefix[DISTRIBUTE_V4_OUT]);
      if (plist)
	ri->prefix[RIP_FILTER_OUT] = plist;
      else
	ri->prefix[RIP_FILTER_OUT] = NULL;
    }
  else
    ri->prefix[RIP_FILTER_OUT] = NULL;
}

void
rip_distribute_update_interface (struct interface *ifp)
{
  struct distribute *dist;

  dist = distribute_lookup (ifp->name);
  if (dist)
    rip_distribute_update (dist);
}

/* Update all interface's distribute list. */
/* ARGSUSED */
static void
rip_distribute_update_all (struct prefix_list *notused)
{
  struct interface *ifp;
  struct listnode *node, *nnode;

  for (ALL_LIST_ELEMENTS (iflist, node, nnode, ifp))
    rip_distribute_update_interface (ifp);
}
/* ARGSUSED */
static void
rip_distribute_update_all_wrapper(struct access_list *notused)
{
        rip_distribute_update_all(NULL);
}

/* Delete all added rip route. */
void
rip_clean (void)
{
  int i;
  struct route_node *rp;
  struct rip_info *rinfo;

  if (rip)
    {
      /* Clear RIP routes */
      for (rp = route_top (rip->table); rp; rp = route_next (rp))
	if ((rinfo = rp->info) != NULL)
	  {
	    if (rinfo->type == ZEBRA_ROUTE_RIP &&
		rinfo->sub_type == RIP_ROUTE_RTE)
	      rip_zebra_ipv4_delete ((struct prefix_ipv4 *)&rp->p,
				     &rinfo->nexthop, rinfo->metric);
	
	    RIP_TIMER_OFF (rinfo->t_timeout);
	    RIP_TIMER_OFF (rinfo->t_garbage_collect);

	    rp->info = NULL;
	    route_unlock_node (rp);

	    rip_info_free (rinfo);
	  }

      /* Cancel RIP related timers. */
      RIP_TIMER_OFF (rip->t_update);
      RIP_TIMER_OFF (rip->t_triggered_update);
      RIP_TIMER_OFF (rip->t_triggered_interval);

      /* Cancel read thread. */
      if (rip->t_read)
	{
	  thread_cancel (rip->t_read);
	  rip->t_read = NULL;
	}

      /* Close RIP socket. */
      if (rip->sock >= 0)
	{
	  close (rip->sock);
	  rip->sock = -1;
	}

      /* Static RIP route configuration. */
      for (rp = route_top (rip->route); rp; rp = route_next (rp))
	if (rp->info)
	  {
	    rp->info = NULL;
	    route_unlock_node (rp);
	  }

      /* RIP neighbor configuration. */
      for (rp = route_top (rip->neighbor); rp; rp = route_next (rp))
	if (rp->info)
	  {
	    rp->info = NULL;
	    route_unlock_node (rp);
	  }

      /* Redistribute related clear. */
      if (rip->default_information_route_map)
	free (rip->default_information_route_map);

      for (i = 0; i < ZEBRA_ROUTE_MAX; i++)
	if (rip->route_map[i].name)
	  free (rip->route_map[i].name);

      XFREE (MTYPE_ROUTE_TABLE, rip->table);
      XFREE (MTYPE_ROUTE_TABLE, rip->route);
      XFREE (MTYPE_ROUTE_TABLE, rip->neighbor);
      
      XFREE (MTYPE_RIP, rip);
      rip = NULL;
    }

  rip_clean_network ();
  rip_passive_nondefault_clean ();
  rip_offset_clean ();
  rip_interface_clean ();
  rip_distance_reset ();
  rip_redistribute_clean ();
}

/* Reset all values to the default settings. */
void
rip_reset (void)
{
  /* Reset global counters. */
  rip_global_route_changes = 0;
  rip_global_queries = 0;

  /* Call ripd related reset functions. */
  rip_debug_reset ();
  rip_route_map_reset ();

  /* Call library reset functions. */
  vty_reset ();
  access_list_reset ();
  prefix_list_reset ();

  distribute_list_reset ();

  rip_interface_reset ();
  rip_distance_reset ();

  rip_zclient_reset ();
}

static void
rip_if_rmap_update (struct if_rmap *if_rmap)
{
  struct interface *ifp;
  struct rip_interface *ri;
  struct route_map *rmap;

  ifp = if_lookup_by_name (if_rmap->ifname);
  if (ifp == NULL)
    return;

  ri = ifp->info;

  if (if_rmap->routemap[IF_RMAP_IN])
    {
      rmap = route_map_lookup_by_name (if_rmap->routemap[IF_RMAP_IN]);
      if (rmap)
	ri->routemap[IF_RMAP_IN] = rmap;
      else
	ri->routemap[IF_RMAP_IN] = NULL;
    }
  else
    ri->routemap[RIP_FILTER_IN] = NULL;

  if (if_rmap->routemap[IF_RMAP_OUT])
    {
      rmap = route_map_lookup_by_name (if_rmap->routemap[IF_RMAP_OUT]);
      if (rmap)
	ri->routemap[IF_RMAP_OUT] = rmap;
      else
	ri->routemap[IF_RMAP_OUT] = NULL;
    }
  else
    ri->routemap[RIP_FILTER_OUT] = NULL;
}

void
rip_if_rmap_update_interface (struct interface *ifp)
{
  struct if_rmap *if_rmap;

  if_rmap = if_rmap_lookup (ifp->name);
  if (if_rmap)
    rip_if_rmap_update (if_rmap);
}

static void
rip_routemap_update_redistribute (void)
{
  int i;

  if (rip)
    {
      for (i = 0; i < ZEBRA_ROUTE_MAX; i++) 
	{
	  if (rip->route_map[i].name)
	    rip->route_map[i].map = 
	      route_map_lookup_by_name (rip->route_map[i].name);
	}
    }
}

/* ARGSUSED */
static void
rip_routemap_update (const char *notused)
{
  struct interface *ifp;
  struct listnode *node, *nnode;

  for (ALL_LIST_ELEMENTS (iflist, node, nnode, ifp))
    rip_if_rmap_update_interface (ifp);

  rip_routemap_update_redistribute ();
}

/* Allocate new rip structure and set default value. */
void
rip_init (void)
{
  /* Randomize for triggered update random(). */
  srand (time (NULL));

  /* Install top nodes. */
  install_node (&rip_node, config_write_rip);

  /* Install rip commands. */
  install_element (VIEW_NODE, &show_ip_rip_cmd);
  install_element (VIEW_NODE, &show_ip_rip_status_cmd);
  install_element (ENABLE_NODE, &show_ip_rip_cmd);
  install_element (ENABLE_NODE, &show_ip_rip_status_cmd);
  install_element (CONFIG_NODE, &router_rip_cmd);
  install_element (CONFIG_NODE, &no_router_rip_cmd);

  install_default (RIP_NODE);
  install_element (RIP_NODE, &rip_version_cmd);
  install_element (RIP_NODE, &no_rip_version_cmd);
  install_element (RIP_NODE, &no_rip_version_val_cmd);
  install_element (RIP_NODE, &rip_default_metric_cmd);
  install_element (RIP_NODE, &no_rip_default_metric_cmd);
  install_element (RIP_NODE, &no_rip_default_metric_val_cmd);
  install_element (RIP_NODE, &rip_relaxed_recv_size_checks_cmd);
  install_element (RIP_NODE, &no_rip_relaxed_recv_size_checks_cmd);
  install_element (RIP_NODE, &rip_timers_cmd);
  install_element (RIP_NODE, &no_rip_timers_cmd);
  install_element (RIP_NODE, &no_rip_timers_val_cmd);
  install_element (RIP_NODE, &rip_route_cmd);
  install_element (RIP_NODE, &no_rip_route_cmd);
  install_element (RIP_NODE, &rip_distance_cmd);
  install_element (RIP_NODE, &no_rip_distance_cmd);
  install_element (RIP_NODE, &rip_distance_source_cmd);
  install_element (RIP_NODE, &no_rip_distance_source_cmd);
  install_element (RIP_NODE, &rip_distance_source_access_list_cmd);
  install_element (RIP_NODE, &no_rip_distance_source_access_list_cmd);

  /* Debug related init. */
  rip_debug_init ();

  /* SNMP init. */
#ifdef HAVE_SNMP
  rip_snmp_init ();
#endif /* HAVE_SNMP */

  /* Access list install. */
  access_list_init ();
  access_list_add_hook (rip_distribute_update_all_wrapper);
  access_list_delete_hook (rip_distribute_update_all_wrapper);

  /* Prefix list initialize.*/
  prefix_list_init ();
  prefix_list_add_hook (rip_distribute_update_all);
  prefix_list_delete_hook (rip_distribute_update_all);

  /* Distribute list install. */
  distribute_list_init (RIP_NODE);
  distribute_list_add_hook (rip_distribute_update);
  distribute_list_delete_hook (rip_distribute_update);

  /* Route-map */
  rip_route_map_init ();
  rip_offset_init ();

  route_map_add_hook (rip_routemap_update);
  route_map_delete_hook (rip_routemap_update);

  if_rmap_init (RIP_NODE);
  if_rmap_hook_add (rip_if_rmap_update);
  if_rmap_hook_delete (rip_if_rmap_update);

  /* Distance control. */
  rip_distance_table = route_table_init ();
}
