/*
 * microtcp, a lightweight implementation of TCP for teaching,
 * and academic purposes.
 *
 * Copyright (C) 2015-2017  Manolis Surligas <surligas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "microtcp.h"
#include "../utils/crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>

// Helper to verify checksum
static int verify_crc(microtcp_header_t *header)
{
  uint32_t recv_crc = ntohl(header->checksum);
  header->checksum = 0;
  uint32_t calc_crc = crc32((const uint8_t *)header, sizeof(*header));
  header->checksum = htonl(recv_crc);
  return (recv_crc == calc_crc);
}

// Function to send control packets like syn ack fin
static int send_ctrl(microtcp_sock_t *socket, uint16_t flags)
{
  microtcp_header_t header;
  memset(&header, 0, sizeof(header));

  header.seq_number = htonl(socket->seq_number);
  header.ack_number = htonl(socket->ack_number);
  header.control = htons(flags);
  header.window = htons(MICROTCP_WIN_SIZE);
  header.data_len = 0;

  header.checksum = 0;
  uint32_t crc = crc32((const uint8_t *)&header, sizeof(header));
  header.checksum = htonl(crc);

  ssize_t sent = sendto(socket->sd, &header, sizeof(header), 0,
                (struct sockaddr*)&socket->peer_addr, socket->peer_addr_len);

  if (sent > 0) {
    socket->packets_send++;
    socket->bytes_send += sent;
    return 0;
  }
  return -1;
}

// Create and initialize a microtcp socket
microtcp_sock_t
microtcp_socket (int domain, int type, int protocol)
{
  microtcp_sock_t sock;
  memset (&sock, 0, sizeof (microtcp_sock_t));
  if ((sock.sd = socket(domain, SOCK_DGRAM, protocol))==-1) {
    perror ( " SOCKET COULD NOT BE OPENED " );
    exit ( EXIT_FAILURE );
  }
  sock.state = CLOSED;
  sock.init_win_size = MICROTCP_WIN_SIZE;
  srand (time (NULL));
  return sock;
}

// Bind the socket to an address
int
microtcp_bind (microtcp_sock_t *socket, const struct sockaddr *address,
               socklen_t address_len)
{
  if (bind(socket->sd, address, address_len) == -1) {
    perror("MICROTCP BIND Failed!");
    exit (EXIT_FAILURE);
  }
  socket->state = LISTEN;
  return EXIT_SUCCESS;
}

// Initiate a connection as client
int
microtcp_connect (microtcp_sock_t *socket, const struct sockaddr *address,
                  socklen_t address_len)
{
  memcpy(&socket->peer_addr, address, address_len);
  socket->peer_addr_len = address_len;

  socket->seq_number = rand();
  if (send_ctrl(socket, 0x0002) == -1) return -1;
  socket->state = SYN_SENT;

  microtcp_header_t header;
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);

  if (recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr*)&from, &fromlen) < (ssize_t)sizeof(header)) return -1;
  if (!verify_crc(&header)) return -1;

  uint16_t control = ntohs(header.control);
  if ((control & 0x0002) && (control & 0x0008)) {
    socket->ack_number = ntohl(header.seq_number) + 1;
    socket->seq_number += 1;

    if (send_ctrl(socket, 0x0008) == -1) return -1;
    socket->state = ESTABLISHED;
    socket->recvbuf = malloc(MICROTCP_RECVBUF_LEN);
    socket->buf_fill_level = 0;
    return 0;
  }
  return -1;
}

// Accept incoming connections
int
microtcp_accept (microtcp_sock_t *socket, struct sockaddr *address,
                 socklen_t address_len)
{
  microtcp_header_t header;
  struct sockaddr_in client_address;
  socklen_t client_address_len = sizeof (client_address);

  while(1) {
    if(recvfrom(socket->sd, &header, sizeof (header), 0, (struct sockaddr *)&client_address, &client_address_len) < (ssize_t)sizeof(header)) continue;

    // ignore corrupted packets
    if(!verify_crc(&header)) continue;

    uint16_t control = ntohs(header.control);
    if (control & 0x0002) { // SYN received

      memcpy(&socket->peer_addr, &client_address, client_address_len);
      socket->peer_addr_len = client_address_len;
      if (address) memcpy(address, &client_address, client_address_len);

      socket->ack_number = ntohl(header.seq_number) + 1;
      socket->seq_number = rand();

      if (send_ctrl(socket, 0x0002 | 0x0008) == -1) return -1;
      socket->state = SYN_RECEIVED;
      break;
    }
  }

  // Wait for the final ACK
  if (recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr *)&client_address, &client_address_len) >= (ssize_t)sizeof(header)) {
     if (verify_crc(&header)) {
        uint16_t control = ntohs(header.control);
        if ((control & 0x0008) && !(control & 0x0002)) {
           socket->seq_number += 1;
           socket->state = ESTABLISHED;
           socket->recvbuf = malloc(MICROTCP_RECVBUF_LEN);
           socket->buf_fill_level = 0;
           return 0;
        }
     }
  }
  return -1;
}

// Begin shutdown sequence
int
microtcp_shutdown (microtcp_sock_t *socket, int how)
{
      if (send_ctrl(socket, 0x0001 | 0x0008) == -1) return -1;
      socket->state = FIN_WAIT_1;

      microtcp_header_t header;
      struct sockaddr_in from;
      socklen_t fromlen = sizeof(from);


      recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr*)&from, &fromlen);

      if ((ntohs(header.control) & 0x0008) && verify_crc(&header)) {
        socket->state = FIN_WAIT_2;
      }

      recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr*)&from, &fromlen);
      uint16_t control = ntohs(header.control);

      if ((control & 0x0001) && (control & 0x0008) && verify_crc(&header)) {
        socket->ack_number = ntohl(header.seq_number) + 1;
        send_ctrl(socket, 0x0008);
        socket->state = CLOSED;
        if (socket->recvbuf) {
          free(socket->recvbuf);
          socket->recvbuf = NULL;
        }
        return 0;
      }
      return -1;
}

ssize_t
microtcp_send (microtcp_sock_t *socket, const void *buffer, size_t length, int flags){
    return 0;
}


ssize_t
microtcp_recv (microtcp_sock_t *socket, void *buffer, size_t length, int flags){
    return 0;
}