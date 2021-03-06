
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

/* CLIENT STATES */
#define CLIENT_WAITING_DATA 0
#define CLIENT_WAITING_ACK 1
#define CLIENT_WAITING_EOF_ACK 2
#define CLIENT_DONE 3

/* SERVER STATES */
#define SERVER_WAITING_DATA 0
#define SERVER_WAITING_FLUSH 1
#define SERVER_DONE 2

#define MAX_PAYLOAD_SIZE 500
#define HEADER_SIZE 12
#define ACK_PACKET_SIZE 8

typedef struct packetWrapper {
  packet_t *packet;
  uint32_t sentTime;
  int acked;
} wrapper;

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */

  wrapper **sentPackets, **recvPackets;
  int sentListSize, recvListSize;

  int windowSize;
  // Save retransmission timeout from config_common
  int timeout;

  // Sending side
  int LAST_PACKET_ACKED;
  int LAST_PACKET_SENT;

  // Receiving side
  int NEXT_PACKET_EXPECTED;

  int eofSent, eofRecv;
  /* Client */
  int client_state;

  /* Server */
  int server_state;

};
rel_t *rel_list;

int
verifyChecksum (rel_t *r, packet_t *pkt, size_t n) {
  uint16_t checksum = pkt->cksum;
  uint16_t len = ntohs(pkt->len);

  pkt->cksum = 0;
  if ((len > HEADER_SIZE + MAX_PAYLOAD_SIZE) || (cksum(pkt, len) != checksum)) {
    pkt->cksum = checksum;
    return 0;
  }
  pkt->cksum = checksum;
  return 1;
}

struct ack_packet *
createAckPacket (rel_t *r, int ackno) {
  struct ack_packet *ack;
  ack = malloc(sizeof(*ack));

  ack->cksum = 0;
  ack->len = htons(ACK_PACKET_SIZE);
  ack->ackno = htonl(ackno);
  ack->cksum = cksum(ack, ACK_PACKET_SIZE);
  return ack;
}

uint32_t
getCurrentTime () { // Returns time in ms since epoch
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint32_t ms = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
  return ms;
}

packet_t *
createDataPacket (rel_t *r, char *payload, int bytesReceived) {
  packet_t *packet;
  packet = malloc(sizeof(*packet));

  memset(packet->data, 0, MAX_PAYLOAD_SIZE);
  memcpy(packet->data, payload, bytesReceived);
  packet->cksum = 0;
  packet->len = htons(HEADER_SIZE + bytesReceived);
  packet->ackno = htonl(r->NEXT_PACKET_EXPECTED);
  packet->seqno = htonl(r->LAST_PACKET_SENT + 1);
  packet->cksum = cksum(packet, HEADER_SIZE + bytesReceived);

  return packet;
}

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  /* Do any other initialization you need here */
  r->windowSize = cc->window;
  r->timeout = cc->timeout;

  r->sentListSize = 0;
  r->recvListSize = 0;

  r->sentPackets = malloc(sizeof(wrapper *) * r->windowSize);
  r->recvPackets = malloc(sizeof(wrapper *) * r->windowSize);

  int i;
  for (i = 0; i < r->windowSize; i++) {
    r->sentPackets[i] = malloc(sizeof(wrapper));
    r->sentPackets[i]->packet = malloc(sizeof(packet_t));
    r->sentPackets[i]->acked = 0;
    r->recvPackets[i] = malloc(sizeof(wrapper));
    r->recvPackets[i]->packet = malloc(sizeof(packet_t));
    r->recvPackets[i]->acked = 0;
  }

  r->LAST_PACKET_ACKED = 0;
  r->LAST_PACKET_SENT = 0;

  r->NEXT_PACKET_EXPECTED = 1;

  r->eofSent = 0;
  r->eofRecv = 0;

  return r;
}

void
rel_destroy (rel_t *r)  
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* Free any other allocated memory here */

  int i;
  for (i = 0; i < r->windowSize; i++) {
    free(r->sentPackets[i]->packet);
    free(r->sentPackets[i]);
    free(r->recvPackets[i]->packet);
    free(r->recvPackets[i]);
  }
  free(r->sentPackets);
  free(r->recvPackets);
  free(r);
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{
}

void
shiftRecvPacketList(rel_t *r) {
  int i, shift;
  int numPacketsInWindow = r->LAST_PACKET_SENT - r->LAST_PACKET_ACKED;

  fprintf(stderr, "Shift recv numpackets: %d\n", numPacketsInWindow);

  for (shift = 0; shift < r->windowSize; shift++) {
    if (r->recvPackets[shift]->acked != 0) {
      break;
    }
  }

  // fprintf(stderr, "shift recv shift offset: %d\n", shift);

  for (i = 0; i < r->windowSize - shift; i++) {

    wrapper *prev_wrap = r->recvPackets[i];
    if (prev_wrap->acked == 0) {
      break;
    }
    wrapper *new_wrap = r->recvPackets[i+shift];
    memcpy(prev_wrap->packet, new_wrap->packet, sizeof(packet_t));
    prev_wrap->acked = 1;
  }

  int j;
  for (j = r->windowSize - shift; j < r->windowSize; j++) {
    free(r->recvPackets[j]->packet);
    free(r->recvPackets[j]);
    r->recvPackets[j] = malloc(sizeof(wrapper));
    r->recvPackets[j]->packet = malloc(sizeof(packet_t));
    r->recvPackets[j]->acked = 0;
    r->recvPackets[j]->sentTime = 0;
  }
}

void
shiftSentPacketList (rel_t *r, int ackno) {
  int shift = ackno - r->LAST_PACKET_ACKED - 1;
  int numPacketsInWindow = r->LAST_PACKET_SENT - r->LAST_PACKET_ACKED;

  int i;
  for (i = 0; i < numPacketsInWindow - shift; i++) {
    wrapper *prev_wrap = r->sentPackets[i];
    wrapper *new_wrap = r->sentPackets[i+shift];

    if(new_wrap->acked == 0) {
      prev_wrap->acked = 0;
      break;
    }
    memcpy(prev_wrap->packet, new_wrap->packet, sizeof(packet_t));
    prev_wrap->acked = new_wrap->acked;
    prev_wrap->sentTime = new_wrap->sentTime;
  }

  // Clear values for shifted entries of at end of array
  int j;
  for (j = numPacketsInWindow - shift; j < numPacketsInWindow; j++) {
    free(r->sentPackets[j]->packet);
    free(r->sentPackets[j]);
    r->sentPackets[j] = malloc(sizeof(wrapper));
    r->sentPackets[j]->packet = malloc(sizeof(packet_t));
    r->sentPackets[j]->acked = 0;
    r->sentPackets[j]->sentTime = 0;
  }
  return;
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
  uint16_t len = ntohs(pkt->len);
  uint32_t ackno = ntohl(pkt->ackno);

  int verified = verifyChecksum(r, pkt, n);
  if (!verified || (len != n)) { // Drop packets with bad length
    fprintf(stderr, "Packet w/ sequence number %d dropped\n", ntohl(pkt->seqno));
    return;
  }

  if (len == ACK_PACKET_SIZE) { // Received packet is an ack packet
    fprintf(stderr, "Received ack number: %d\n", ackno);
    if (ackno <= r->LAST_PACKET_ACKED + 1) { // Drop duplicate acks
      fprintf(stderr, "Duplicate ack: %d received\n", ackno);
      return;
    }

    //if (ackno == r->LAST_PACKET_SENT + 1) { // REMOVE THIS AFTER WE FIX ACK SENDING!!!!
      shiftSentPacketList(r, ackno);

      r->LAST_PACKET_ACKED = ackno - 1;

      rel_read(r);
    //}
  }
  else { // data packet
    fprintf(stderr, "%s\n", "======================RECEIVED DATA PACKET=========================");
    uint32_t seqno = ntohl(pkt->seqno);

    // fprintf(stderr, "Received data: %s\n", pkt->data);


    // if (seqno > r->NEXT_PACKET_EXPECTED) {
    //   // Ghetto fix
    //   return;
    // }

    if (seqno < r->NEXT_PACKET_EXPECTED) { // duplicate packet
      fprintf(stderr, "Received duplicate packet w/ sequence number: %d\n", seqno);
      struct ack_packet *ack = createAckPacket(r, r->NEXT_PACKET_EXPECTED);
      conn_sendpkt(r->c, (packet_t *)ack, ACK_PACKET_SIZE);
      free(ack);
      return;
    }

    if (seqno - r->NEXT_PACKET_EXPECTED > r->windowSize) {  // Packet outside window
      return;
    }

    fprintf(stderr, "Received sequence number: %d\n", seqno);

    int slot = seqno - r->NEXT_PACKET_EXPECTED;
    fprintf(stderr, "RecvPacket slot number: %d\n", slot);
    memcpy(r->recvPackets[slot]->packet, pkt, sizeof(packet_t));
    r->recvPackets[slot]->sentTime = getCurrentTime();
    r->recvPackets[slot]->acked = 1;

    rel_output(r);

    // if (seqno == r->NEXT_PACKET_EXPECTED) {
    //   struct ack_packet *ack = createAckPacket(r, r->NEXT_PACKET_EXPECTED + 1);

    //   conn_sendpkt(r->c, (packet_t *)ack, ACK_PACKET_SIZE);
    //   conn_output(r->c, pkt->data, len - HEADER_SIZE);
    //   // rel_output(r);
    //   r->NEXT_PACKET_EXPECTED++;
    //   free(ack);
    // }

  }
}

void
rel_read (rel_t *s)
{
  int numPacketsInWindow = s->LAST_PACKET_SENT - s->LAST_PACKET_ACKED;
  fprintf(stderr, "REL_READ -- lastpacketsent: %d, lastacked: %d\n", s->LAST_PACKET_SENT, s->LAST_PACKET_ACKED);

  if (numPacketsInWindow == 0 && s->eofSent == 1 && s->eofRecv == 1) {
    rel_destroy(s);
    return;
  }

  if (numPacketsInWindow >= s->windowSize || s->eofSent) {
    // don't send, window's full, waiting for acks
    return;
  }

  // can send packet
  char payloadBuffer[MAX_PAYLOAD_SIZE];

  int bytesReceived = conn_input(s->c, payloadBuffer, MAX_PAYLOAD_SIZE);
  if (bytesReceived == 0) {
    return; // no data is available at the moment, just return
  }
  else if (bytesReceived == -1) { // eof or error
    s->eofSent = 1;
    bytesReceived = 0;

    // Why do we need to create and send a packet here?

    // packet_t *packet = createDataPacket(s, payloadBuffer, bytesReceived);
    // conn_sendpkt(s->c, packet, HEADER_SIZE + bytesReceived);

    // free(packet);
    // return;
  }

  // TODO: Need to handle overflow bytes here as well

  packet_t *packet = createDataPacket(s, payloadBuffer, bytesReceived);
  s->LAST_PACKET_SENT++;
  fprintf(stderr, "Sent sequence number: %d\n", ntohl(packet->seqno));
  conn_sendpkt(s->c, packet, HEADER_SIZE + bytesReceived);

  // Save packet until it's acked/in case it needs to be retransmitted
  int slot = s->LAST_PACKET_SENT - s->LAST_PACKET_ACKED - 1;
  // fprintf(stderr, "Slot: %d\n", slot);
  memcpy(s->sentPackets[slot]->packet, packet, HEADER_SIZE + bytesReceived);
  s->sentPackets[slot]->sentTime = getCurrentTime();
  s->sentPackets[slot]->acked = 1;

  fprintf(stderr, "%s\n", "====================SENDING PACKET================");
  // fprintf(stderr, "Packet data: %s\n", packet->data);

  free(packet);
}

void
rel_output (rel_t *r)
{
  int numPacketsInWindow = r->LAST_PACKET_SENT - r->LAST_PACKET_ACKED;
  int i;
  fprintf(stderr, "lastpacksent: %d, lackPackacked: %d\n", r->LAST_PACKET_SENT, r->LAST_PACKET_ACKED);
  for (i = 0; i < r->windowSize; i++) {
    fprintf(stderr, "Recvpacket of %d has ack %d\n", i, r->recvPackets[i]->acked);
    if(r->recvPackets[i]->acked == 0) {
      break;
    }
    packet_t *pkt = r->recvPackets[i]->packet;
    uint16_t packet_len = ntohs(pkt->len);
    size_t len = conn_bufspace(r->c);

    if(len >= packet_len - HEADER_SIZE) {
      if(packet_len == HEADER_SIZE) {
        r->eofRecv = 1;
      }
      fprintf(stderr, "Outputting packet %d from recvPackets \n", i);
      conn_output(r->c, pkt->data, packet_len - HEADER_SIZE);
      r->recvPackets[i]->acked = 0;
    } 
    else {
      break;
    }
  }

  fprintf(stderr, "value of i: %d\n", i);
  fprintf(stderr, "Next Packet Expected Before: %d\n", r->NEXT_PACKET_EXPECTED );

  r->NEXT_PACKET_EXPECTED += i;
  struct ack_packet *ack = createAckPacket(r, r->NEXT_PACKET_EXPECTED);

  fprintf(stderr, "Next Packet Expected: %d\n", r->NEXT_PACKET_EXPECTED);

  conn_sendpkt(r->c, (packet_t *)ack, ACK_PACKET_SIZE);
  free(ack);


  if(numPacketsInWindow == 0 && r->eofRecv == 1 && r->eofSent == 1) {
    rel_destroy(r);
    return;
  }
  shiftRecvPacketList(r);
}

void
rel_timer ()
{
  //fprintf(stderr, "%s\n", "REL_TIMER");
  /* Retransmit any packets that need to be retransmitted */
  rel_t *r = rel_list;
  uint32_t curTime = getCurrentTime();

  while (r != NULL) {
    int numPacketsInWindow = r->LAST_PACKET_SENT - r->LAST_PACKET_ACKED;
    int i;
    for (i = 0; i < numPacketsInWindow; i++) {
      wrapper *curPacketNode = r->sentPackets[i];
      // uint32_t timediff = curTime - curPacketNode->sentTime;
      if (curTime - curPacketNode->sentTime > r->timeout) {
        fprintf(stderr, "Retransmitted packet w/ sequence number: %d\n", ntohl(curPacketNode->packet->seqno));
        // retransmit package
        conn_sendpkt(r->c, curPacketNode->packet, ntohs(curPacketNode->packet->len));
      }
    }
    r = r->next;
  }
}
