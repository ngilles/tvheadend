/*
 *  Multicasted IPTV Input
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <assert.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>

#include "tvhead.h"
#include "htsmsg.h"
#include "channels.h"
#include "transports.h"
#include "iptv_input.h"
#include "tsdemux.h"
#include "psi.h"
#include "settings.h"

static int iptv_thread_running;
static int iptv_epollfd;
static pthread_mutex_t iptv_recvmutex;

struct th_transport_list iptv_all_transports; /* All IPTV transports */
static struct th_transport_list iptv_active_transports; /* Currently enabled */

/**
 * PAT parser. We only parse a single program. CRC has already been verified
 */
static void
iptv_got_pat(const uint8_t *ptr, size_t len, void *aux)
{
  th_transport_t *t = aux;
  uint16_t prognum, pmt;

  len -= 8;
  ptr += 8;

  if(len < 4)
    return;

  prognum =  ptr[0]         << 8 | ptr[1];
  pmt     = (ptr[2] & 0x1f) << 8 | ptr[3];

  t->tht_pmt_pid = pmt;
}


/**
 * PMT parser. CRC has already been verified
 */
static void
iptv_got_pmt(const uint8_t *ptr, size_t len, void *aux)
{
  th_transport_t *t = aux;

  if(len < 3 || ptr[0] != 2)
    return;

  pthread_mutex_lock(&t->tht_stream_mutex);
  psi_parse_pmt(t, ptr + 3, len - 3, 0, 1);
  pthread_mutex_unlock(&t->tht_stream_mutex);
}


/**
 * Handle a single TS packet for the given IPTV transport
 */
static void
iptv_ts_input(th_transport_t *t, const uint8_t *tsb)
{
  uint16_t pid = ((tsb[1] & 0x1f) << 8) | tsb[2];

  if(pid == 0) {

    if(t->tht_pat_section == NULL)
      t->tht_pat_section = calloc(1, sizeof(psi_section_t));
    psi_section_reassemble(t->tht_pat_section, tsb, 1, iptv_got_pat, t);

  } else if(pid == t->tht_pmt_pid) {

    if(t->tht_pmt_section == NULL)
      t->tht_pmt_section = calloc(1, sizeof(psi_section_t));
    psi_section_reassemble(t->tht_pmt_section, tsb, 1, iptv_got_pmt, t);

  } else {
    ts_recv_packet1(t, tsb, NULL);
  } 
}


/**
 * Main epoll() based input thread for IPTV
 */
static void *
iptv_thread(void *aux)
{
  int nfds, fd, r, j;
  uint8_t tsb[65536], *buf;
  struct epoll_event ev;
  th_transport_t *t;

  while(1) {
    nfds = epoll_wait(iptv_epollfd, &ev, 1, -1);
    if(nfds == -1) {
      tvhlog(LOG_ERR, "IPTV", "epoll() error -- %s, sleeping 1 second",
	     strerror(errno));
      sleep(1);
      continue;
    }

    if(nfds < 1)
      continue;

    fd = ev.data.fd;
    r = read(fd, tsb, sizeof(tsb));

    if(r > 1 && tsb[0] == 0x47 && (r % 188) == 0) {
      /* Looks like raw TS in UDP */
      buf = tsb;
    } else {
      /* Check for valid RTP packets */
      if(r < 12)
	continue;

      if((tsb[0] & 0xc0) != 0x80)
	continue;

      if((tsb[1] & 0x7f) != 33)
	continue;
      
      int hlen = (tsb[0] & 0xf) * 4 + 12;

      if(r < hlen || (r - hlen) % 188 != 0)
	continue;

      buf = tsb + hlen;
      r -= hlen;
    }

    pthread_mutex_lock(&iptv_recvmutex);
    
    LIST_FOREACH(t, &iptv_active_transports, tht_active_link) {
      if(t->tht_iptv_fd != fd)
	continue;
      
      for(j = 0; j < r; j += 188)
	iptv_ts_input(t, buf + j);
    }
    pthread_mutex_unlock(&iptv_recvmutex);
  }
  return NULL;
}


/**
 *
 */
static int
iptv_transport_start(th_transport_t *t, unsigned int weight, int force_start)
{
  pthread_t tid;
  int fd;
  char straddr[INET6_ADDRSTRLEN];
  struct ip_mreqn m;
  struct ipv6_mreq m6;
  struct sockaddr_in sin;
  struct sockaddr_in6 sin6;
  struct ifreq ifr;
  struct epoll_event ev;

  assert(t->tht_iptv_fd == -1);

  if(iptv_thread_running == 0) {
    iptv_thread_running = 1;
    iptv_epollfd = epoll_create(10);
    pthread_create(&tid, NULL, iptv_thread, NULL);
  }

  /* Now, open the real socket for UDP */
  if(t->tht_iptv_group.s_addr!=0) {
    fd = tvh_socket(AF_INET, SOCK_DGRAM, 0);
  
  }
  else {
    fd = tvh_socket(AF_INET6, SOCK_DGRAM, 0);
  }
  if(fd == -1) {
    tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot open socket", t->tht_identifier);
    return -1;
  }

  /* First, resolve interface name */
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, IFNAMSIZ, "%s", t->tht_iptv_iface);
  ifr.ifr_name[IFNAMSIZ - 1] = 0;
  if(ioctl(fd, SIOCGIFINDEX, &ifr)) {
    tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot find interface %s", 
	   t->tht_identifier, t->tht_iptv_iface);
    close(fd);
    return -1;
  }

  /* Bind to IPv4 multicast group */
  if(t->tht_iptv_group.s_addr!=0) {
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(t->tht_iptv_port);
    sin.sin_addr.s_addr = t->tht_iptv_group.s_addr;
    if(bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
      tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot bind %s:%d -- %s",
           t->tht_identifier, inet_ntoa(sin.sin_addr), t->tht_iptv_port,
           strerror(errno));
      close(fd);
      return -1;
    }
    /* Join IPv4 group */
    memset(&m, 0, sizeof(m));
    m.imr_multiaddr.s_addr = t->tht_iptv_group.s_addr;
    m.imr_address.s_addr = 0;
    m.imr_ifindex = ifr.ifr_index;

      if(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m,
                sizeof(struct ip_mreqn)) == -1) {
      tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot join %s -- %s",
           t->tht_identifier, inet_ntoa(m.imr_multiaddr), strerror(errno));
      close(fd);
      return -1;
    }
  } else {
    /* Bind to IPv6 multicast group */
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(t->tht_iptv_port);
    sin6.sin6_addr = t->tht_iptv_group6;
    if(bind(fd, (struct sockaddr *)&sin6, sizeof(sin6)) == -1) {
      inet_ntop(AF_INET6, &sin6.sin6_addr, straddr, sizeof(straddr));
      tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot bind %s:%d -- %s",
           t->tht_identifier, straddr, t->tht_iptv_port,
           strerror(errno));
      close(fd);
      return -1;
    }
    /* Join IPv6 group */
    memset(&m6, 0, sizeof(m6));
    m6.ipv6mr_multiaddr = t->tht_iptv_group6;
    m6.ipv6mr_interface = ifr.ifr_index;

    if(setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m6,
                sizeof(struct ipv6_mreq)) == -1) {
      inet_ntop(AF_INET6, m6.ipv6mr_multiaddr.s6_addr,
		straddr, sizeof(straddr));
      tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot join %s -- %s",
           t->tht_identifier, straddr, strerror(errno));
      close(fd);
      return -1;
    }
  }


  int resize = 262142;
  if(setsockopt(fd,SOL_SOCKET,SO_RCVBUF, &resize, sizeof(resize)) == -1)
    tvhlog(LOG_WARNING, "IPTV",
	   "Can not icrease UDP receive buffer size to %d -- %s",
	   resize, strerror(errno));

  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if(epoll_ctl(iptv_epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot add to epoll set -- %s", 
	   t->tht_identifier, strerror(errno));
    close(fd);
    return -1;
  }

  t->tht_iptv_fd = fd;

  pthread_mutex_lock(&iptv_recvmutex);
  LIST_INSERT_HEAD(&iptv_active_transports, t, tht_active_link);
  pthread_mutex_unlock(&iptv_recvmutex);
  return 0;
}


/**
 *
 */
static void
iptv_transport_refresh(th_transport_t *t)
{

}


/**
 *
 */
static void
iptv_transport_stop(th_transport_t *t)
{
  struct ifreq ifr;

  pthread_mutex_lock(&iptv_recvmutex);
  LIST_REMOVE(t, tht_active_link);
  pthread_mutex_unlock(&iptv_recvmutex);

  assert(t->tht_iptv_fd >= 0);

  /* First, resolve interface name */
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, IFNAMSIZ, "%s", t->tht_iptv_iface);
  ifr.ifr_name[IFNAMSIZ - 1] = 0;
  if(ioctl(t->tht_iptv_fd, SIOCGIFINDEX, &ifr)) {
    tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot find interface %s",
	   t->tht_identifier, t->tht_iptv_iface);
  }

  if(t->tht_iptv_group.s_addr != 0) {

    struct ip_mreqn m;
    memset(&m, 0, sizeof(m));
    /* Leave multicast group */
    m.imr_multiaddr.s_addr = t->tht_iptv_group.s_addr;
    m.imr_address.s_addr = 0;
    m.imr_ifindex = ifr.ifr_index;
    
    if(setsockopt(t->tht_iptv_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m,
		  sizeof(struct ip_mreqn)) == -1) {
      tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot leave %s -- %s",
	     t->tht_identifier, inet_ntoa(m.imr_multiaddr), strerror(errno));
    }
  } else {
    char straddr[INET6_ADDRSTRLEN];

    struct ipv6_mreq m6;
    memset(&m6, 0, sizeof(m6));

    m6.ipv6mr_multiaddr = t->tht_iptv_group6;
    m6.ipv6mr_interface = ifr.ifr_index;

    if(setsockopt(t->tht_iptv_fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &m6,
		  sizeof(struct ipv6_mreq)) == -1) {
      inet_ntop(AF_INET6, m6.ipv6mr_multiaddr.s6_addr,
		straddr, sizeof(straddr));

      tvhlog(LOG_ERR, "IPTV", "\"%s\" cannot leave %s -- %s",
	     t->tht_identifier, straddr, strerror(errno));
    }



  }
  close(t->tht_iptv_fd); // Automatically removes fd from epoll set

  t->tht_iptv_fd = -1;
}


/**
 *
 */
static void
iptv_transport_save(th_transport_t *t)
{
  htsmsg_t *m = htsmsg_create_map();
  char abuf[INET_ADDRSTRLEN];
  char abuf6[INET6_ADDRSTRLEN];

  lock_assert(&global_lock);

  htsmsg_add_u32(m, "pmt", t->tht_pmt_pid);

  if(t->tht_iptv_port)
    htsmsg_add_u32(m, "port", t->tht_iptv_port);

  if(t->tht_iptv_iface)
    htsmsg_add_str(m, "interface", t->tht_iptv_iface);

  if(t->tht_iptv_group.s_addr!= 0) {
    inet_ntop(AF_INET, &t->tht_iptv_group, abuf, sizeof(abuf));
    htsmsg_add_str(m, "group", abuf);
  }
  if(IN6_IS_ADDR_MULTICAST(&t->tht_iptv_group6) ) {
    inet_ntop(AF_INET6, &t->tht_iptv_group6, abuf6, sizeof(abuf6));
    htsmsg_add_str(m, "group", abuf6);
  }
  if(t->tht_ch != NULL) {
    htsmsg_add_str(m, "channelname", t->tht_ch->ch_name);
    htsmsg_add_u32(m, "mapped", 1);
  }
  
  pthread_mutex_lock(&t->tht_stream_mutex);
  psi_save_transport_settings(m, t);
  pthread_mutex_unlock(&t->tht_stream_mutex);
  
  hts_settings_save(m, "iptvtransports/%s",
		    t->tht_identifier);

  htsmsg_destroy(m);
}


/**
 *
 */
static int
iptv_transport_quality(th_transport_t *t)
{
  if(t->tht_iptv_iface == NULL || 
     (t->tht_iptv_group.s_addr == 0 && t->tht_iptv_group6.s6_addr == 0) ||
     t->tht_iptv_port == 0)
    return 0;

  return 100;
}


/**
 * Generate a descriptive name for the source
 */
static void
iptv_transport_setsourceinfo(th_transport_t *t, struct source_info *si)
{
  char straddr[INET6_ADDRSTRLEN];
  memset(si, 0, sizeof(struct source_info));

  si->si_adapter = t->tht_iptv_iface ? strdup(t->tht_iptv_iface) : NULL;
  if(t->tht_iptv_group.s_addr != 0) {
    si->si_mux = strdup(inet_ntoa(t->tht_iptv_group));
  }
  else {
    inet_ntop(AF_INET6, &t->tht_iptv_group6, straddr, sizeof(straddr));
    si->si_mux = strdup(straddr);
  }
}


/**
 *
 */
static int
iptv_grace_period(th_transport_t *t)
{
  return 3;
}


/**
 *
 */
static void
iptv_transport_dtor(th_transport_t *t)
{
  hts_settings_remove("iptvtransports/%s", t->tht_identifier); 
}


/**
 *
 */
th_transport_t *
iptv_transport_find(const char *id, int create)
{
  static int tally;
  th_transport_t *t;
  char buf[20];

  if(id != NULL) {

    if(strncmp(id, "iptv_", 5))
      return NULL;

    LIST_FOREACH(t, &iptv_all_transports, tht_group_link)
      if(!strcmp(t->tht_identifier, id))
	return t;
  }

  if(create == 0)
    return NULL;
  
  if(id == NULL) {
    tally++;
    snprintf(buf, sizeof(buf), "iptv_%d", tally);
    id = buf;
  } else {
    tally = MAX(atoi(id + 5), tally);
  }

  t = transport_create(id, TRANSPORT_IPTV, THT_MPEG_TS);

  t->tht_start_feed    = iptv_transport_start;
  t->tht_refresh_feed  = iptv_transport_refresh;
  t->tht_stop_feed     = iptv_transport_stop;
  t->tht_config_save   = iptv_transport_save;
  t->tht_setsourceinfo = iptv_transport_setsourceinfo;
  t->tht_quality_index = iptv_transport_quality;
  t->tht_grace_period  = iptv_grace_period;
  t->tht_dtor          = iptv_transport_dtor;
  t->tht_iptv_fd = -1;

  LIST_INSERT_HEAD(&iptv_all_transports, t, tht_group_link);

  pthread_mutex_lock(&t->tht_stream_mutex); 
  transport_make_nicename(t);
  pthread_mutex_unlock(&t->tht_stream_mutex); 

  return t;
}


/**
 * Load config for the given mux
 */
static void
iptv_transport_load(void)
{
  htsmsg_t *l, *c;
  htsmsg_field_t *f;
  uint32_t pmt;
  const char *s;
  unsigned int u32;
  th_transport_t *t;

  lock_assert(&global_lock);

  if((l = hts_settings_load("iptvtransports")) == NULL)
    return;
  
  HTSMSG_FOREACH(f, l) {
    if((c = htsmsg_get_map_by_field(f)) == NULL)
      continue;

    if(htsmsg_get_u32(c, "pmt", &pmt))
      continue;
    
    t = iptv_transport_find(f->hmf_name, 1);
    t->tht_pmt_pid = pmt;

    tvh_str_update(&t->tht_iptv_iface, htsmsg_get_str(c, "interface"));

    if((s = htsmsg_get_str(c, "group")) != NULL){
      if (!inet_pton(AF_INET, s, &t->tht_iptv_group.s_addr)) {
         inet_pton(AF_INET6, s, &t->tht_iptv_group6.s6_addr);
      }
    }
    
    if(!htsmsg_get_u32(c, "port", &u32))
      t->tht_iptv_port = u32;

    pthread_mutex_lock(&t->tht_stream_mutex);
    transport_make_nicename(t);
    psi_load_transport_settings(c, t);
    pthread_mutex_unlock(&t->tht_stream_mutex);
    
    s = htsmsg_get_str(c, "channelname");
    if(htsmsg_get_u32(c, "mapped", &u32))
      u32 = 0;
    
    if(s && u32)
      transport_map_channel(t, channel_find_by_name(s, 1, 0), 0);
  }
  htsmsg_destroy(l);
}


/**
 *
 */
void
iptv_input_init(void)
{
  pthread_mutex_init(&iptv_recvmutex, NULL);
  iptv_transport_load();
}
