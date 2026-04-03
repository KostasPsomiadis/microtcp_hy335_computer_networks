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

// include microtcp definitions and constants
#include "microtcp.h"
// include crc32 checksum implementation
#include "../utils/crc32.h"
// standard c libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/time.h>

// macro to get minimum of two values
#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

// verify crc checksum of received header
static int verify_crc(microtcp_header_t *header)
{
  uint32_t recv_crc = ntohl(header->checksum);
  header->checksum = 0;
  uint32_t calc_crc = crc32((const uint8_t *)header, sizeof(*header));
  header->checksum = htonl(recv_crc);
  return (recv_crc == calc_crc);
}

// send a microtcp packet (data or control)
static int send_packet(microtcp_sock_t *socket, uint16_t flags, const void *data, uint32_t data_len)
{
  // create and clear header
  microtcp_header_t header;
  memset(&header, 0, sizeof(header));

  // fill header fields
  header.seq_number = htonl(socket->seq_number);
  header.ack_number = htonl(socket->ack_number);
  header.control = htons(flags);
  header.window = htons((uint16_t)socket->curr_win_size);
  header.data_len = htonl(data_len);

  // calculate checksum
  header.checksum = 0;
  uint32_t crc = crc32((const uint8_t *)&header, sizeof(header));
  header.checksum = htonl(crc);

  // allocate packet buffer
  size_t packet_len = sizeof(header) + data_len;
  uint8_t *packet_buf = malloc(packet_len);
  if (!packet_buf) return -1;

  // copy header and payload
  memcpy(packet_buf, &header, sizeof(header));
  if (data_len > 0 && data != NULL) {
      memcpy(packet_buf + sizeof(header), data, data_len);
  }

  // send packet through udp socket
  ssize_t sent = sendto(socket->sd, packet_buf, packet_len, 0,
                (struct sockaddr*)&socket->peer_addr, socket->peer_addr_len);

  free(packet_buf);

  // update statistics on success
  if (sent > 0) {
    socket->packets_send++;
    socket->bytes_send += sent;
    return 0;
  }
  return -1;
}

// create and initialize a microtcp socket
microtcp_sock_t
microtcp_socket (int domain, int type, int protocol)
{
  // initialize socket structure
  microtcp_sock_t sock;
  memset (&sock, 0, sizeof (microtcp_sock_t));

  // create udp socket
  if ((sock.sd = socket(domain, SOCK_DGRAM, protocol))==-1) {
    perror ( " SOCKET COULD NOT BE OPENED " );
    exit ( EXIT_FAILURE );
  }

  // allow address reuse
  int reuse = 1;
  if (setsockopt(sock.sd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
      perror("setsockopt failed");
  }

  // initialize connection state and windows
  sock.state = CLOSED;
  sock.init_win_size = MICROTCP_WIN_SIZE;
  sock.curr_win_size = MICROTCP_WIN_SIZE;
  sock.cwnd = MICROTCP_INIT_CWND;
  sock.ssthresh = MICROTCP_INIT_SSTHRESH;

  // seed random generator for sequence numbers
  srand (time (NULL));
  return sock;
}

// bind socket to local address
int
microtcp_bind (microtcp_sock_t *socket, const struct sockaddr *address,
               socklen_t address_len)
{
  // bind underlying udp socket
  if (bind(socket->sd, address, address_len) == -1) {
    perror("MICROTCP BIND Failed!");
    exit (EXIT_FAILURE);
  }
  // move socket to listen state
  socket->state = LISTEN;
  return EXIT_SUCCESS;
}

// initiate connection using three-way handshake
int
microtcp_connect (microtcp_sock_t *socket, const struct sockaddr *address,
                  socklen_t address_len)
{
  // store peer address
  memcpy(&socket->peer_addr, address, address_len);
  socket->peer_addr_len = address_len;

  // initialize sequence number
  socket->seq_number = rand();
  // send syn packet
  if (send_packet(socket, 0x0002, NULL, 0) == -1) return -1;
  socket->state = SYN_SENT;

  // wait for syn+ack
  microtcp_header_t header;
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);

  if (recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr*)&from, &fromlen) < (ssize_t)sizeof(header)) return -1;
  if (!verify_crc(&header)) return -1;

  // check control flags
  uint16_t control = ntohs(header.control);
  if ((control & 0x0002) && (control & 0x0008)) {
    socket->ack_number = ntohl(header.seq_number) + 1;
    socket->seq_number += 1;

    // update initial window sizes
    socket->curr_win_size = ntohs(header.window);
    socket->init_win_size = socket->curr_win_size;

    // send final ack
    if (send_packet(socket, 0x0008, NULL, 0) == -1) return -1;

    socket->state = ESTABLISHED;
    socket->cwnd = MICROTCP_INIT_CWND;
    socket->ssthresh = MICROTCP_INIT_SSTHRESH;

    return 0;
  }
  return -1;
}

// accept incoming connection request
int
microtcp_accept (microtcp_sock_t *socket, struct sockaddr *address,
                 socklen_t address_len)
{
  microtcp_header_t header;
  struct sockaddr_in client_address;
  socklen_t client_address_len = sizeof (client_address);

  // wait for syn packet
  while(1) {
    if(recvfrom(socket->sd, &header, sizeof (header), 0, (struct sockaddr *)&client_address, &client_address_len) < (ssize_t)sizeof(header)) continue;
    if(!verify_crc(&header)) continue;

    uint16_t control = ntohs(header.control);
    if (control & 0x0002) {
      // store client address
      memcpy(&socket->peer_addr, &client_address, client_address_len);
      socket->peer_addr_len = client_address_len;
      if (address) memcpy(address, &client_address, client_address_len);

      // initialize sequence and ack numbers
      socket->ack_number = ntohl(header.seq_number) + 1;
      socket->seq_number = rand();

      socket->curr_win_size = ntohs(header.window);
      socket->init_win_size = socket->curr_win_size;

      // send syn+ack
      if (send_packet(socket, 0x0002 | 0x0008, NULL, 0) == -1) return -1;
      socket->state = SYN_RECEIVED;
      break;
    }
  }

  // wait for final ack
  if (recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr *)&client_address, &client_address_len) >= (ssize_t)sizeof(header)) {
     if (verify_crc(&header)) {
        uint16_t control = ntohs(header.control);
        if ((control & 0x0008) && !(control & 0x0002)) {
           socket->seq_number += 1;
           socket->state = ESTABLISHED;

           socket->cwnd = MICROTCP_INIT_CWND;
           socket->ssthresh = MICROTCP_INIT_SSTHRESH;

           return 0;
        }
     }
  }
  return -1;
}

// gracefully shutdown connection
int
microtcp_shutdown (microtcp_sock_t *socket, int how)
{
    if (socket->state == CLOSING_BY_PEER) {
        // respond to peer fin
        if (send_packet(socket, 0x0001 | 0x0008, NULL, 0) == -1) return -1;

        microtcp_header_t header;
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr*)&from, &fromlen);

        socket->state = CLOSED;
        return 0;
    }
    else {
          // initiate fin
          if (send_packet(socket, 0x0001 | 0x0008, NULL, 0) == -1) return -1;
          socket->state = FIN_WAIT_1;

          microtcp_header_t header;
          struct sockaddr_in from;
          socklen_t fromlen = sizeof(from);

          // wait for ack
          recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr*)&from, &fromlen);
          if ((ntohs(header.control) & 0x0008) && verify_crc(&header)) {
            socket->state = FIN_WAIT_2;
          }

          // wait for peer fin
          while(1) {
              recvfrom(socket->sd, &header, sizeof(header), 0, (struct sockaddr*)&from, &fromlen);
              if(!verify_crc(&header)) continue;
              if (ntohs(header.control) & 0x0001) break;
          }

          socket->ack_number = ntohl(header.seq_number) + 1;
          send_packet(socket, 0x0008, NULL, 0);
          socket->state = CLOSED;
          return 0;
    }
}

// send data reliably with flow and congestion control
ssize_t
microtcp_send (microtcp_sock_t *socket, const void *buffer, size_t length, int flags)
{
    size_t data_sent = 0;
    const uint8_t *payload = (const uint8_t *)buffer;

    // set timeout for ack reception
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = MICROTCP_ACK_TIMEOUT_US;
    if (setsockopt(socket->sd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt");
    }

    // send until all data is transmitted
    while (data_sent < length) {
        size_t win = min(socket->curr_win_size, socket->cwnd);
        size_t remaining = length - data_sent;
        size_t bytes_to_send = min(win, remaining);

        // handle zero window probing
        if (socket->curr_win_size == 0) {
             usleep((useconds_t)(rand() % (MICROTCP_ACK_TIMEOUT_US + 1)));
             send_packet(socket, 0x0008, NULL, 0);
             microtcp_header_t h;
             struct sockaddr_in f;
             socklen_t fl = sizeof(f);
             if (recvfrom(socket->sd, &h, sizeof(h), 0, (struct sockaddr*)&f, &fl) > 0) {
                 if (verify_crc(&h)) socket->curr_win_size = ntohs(h.window);
             }
             continue;
        }

        if (bytes_to_send == 0) bytes_to_send = MICROTCP_MSS;

        int chunks = (int)(bytes_to_send / MICROTCP_MSS);
        if (bytes_to_send % MICROTCP_MSS) chunks++;

        uint32_t start_seq = socket->seq_number;
        size_t burst_bytes = 0;

        // send a burst of packets
        for (int i = 0; i < chunks; i++) {
            size_t chunk_size = min((size_t)MICROTCP_MSS, remaining - burst_bytes);

            if (send_packet(socket, 0x0008, payload + data_sent + burst_bytes, (uint32_t)chunk_size) == -1) {
                return -1;
            }

            socket->seq_number += (uint32_t)chunk_size;
            burst_bytes += chunk_size;
        }

        int dup_ack_count = 0;
        uint32_t last_ack_num = start_seq;
        uint32_t acked_upto = start_seq;

        // wait for acknowledgments
        while (acked_upto < start_seq + (uint32_t)burst_bytes) {
            microtcp_header_t h;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);

            ssize_t res = recvfrom(socket->sd, &h, sizeof(h), 0, (struct sockaddr*)&from, &fl);

            if (res < 0) {
                // timeout handling
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    socket->ssthresh = socket->cwnd / 2;
                    if (socket->ssthresh < MICROTCP_MSS) socket->ssthresh = MICROTCP_MSS;
                    socket->cwnd = min((size_t)MICROTCP_MSS, (size_t)socket->ssthresh);
                    if (socket->cwnd < MICROTCP_MSS) socket->cwnd = MICROTCP_MSS;

                    socket->seq_number = acked_upto;
                    dup_ack_count = 0;
                    goto retransmit_label;
                }
                continue;
            }

            if (!verify_crc(&h)) continue;

            uint32_t recv_ack = ntohl(h.ack_number);

            // detect duplicate acks
            if (recv_ack == last_ack_num) {
                dup_ack_count++;
                if (dup_ack_count == 3) {
                    socket->ssthresh = socket->cwnd / 2;
                    if (socket->ssthresh < MICROTCP_MSS) socket->ssthresh = MICROTCP_MSS;
                    socket->cwnd = socket->ssthresh + 1;
                    if (socket->cwnd < MICROTCP_MSS) socket->cwnd = MICROTCP_MSS;

                    socket->seq_number = acked_upto;
                    goto retransmit_label;
                }
            } else if (recv_ack > acked_upto) {
                dup_ack_count = 0;
                last_ack_num = recv_ack;

                if (recv_ack > start_seq + (uint32_t)burst_bytes) recv_ack = start_seq + (uint32_t)burst_bytes;
                acked_upto = recv_ack;

                // update congestion window
                if (socket->cwnd < socket->ssthresh) {
                    socket->cwnd += MICROTCP_MSS;
                } else {
                    socket->cwnd += (MICROTCP_MSS * MICROTCP_MSS) / socket->cwnd;
                }
            } else {
                last_ack_num = recv_ack;
            }

            socket->curr_win_size = ntohs(h.window);
        }

        data_sent += burst_bytes;
        continue;

        retransmit_label:
        continue;
    }

    // disable timeout
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    setsockopt(socket->sd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    return (ssize_t)data_sent;
}

// receive data reliably from peer
ssize_t
microtcp_recv (microtcp_sock_t *socket, void *buffer, size_t length, int flags)
{
    microtcp_header_t h;
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    uint8_t *user_buf = (uint8_t *)buffer;
    size_t received_bytes = 0;

    uint8_t packet_buf[sizeof(microtcp_header_t) + MICROTCP_MSS];

    // receive until requested length is reached
    while (received_bytes < length) {
        ssize_t n = recvfrom(socket->sd, packet_buf, sizeof(packet_buf), 0, (struct sockaddr*)&from, &len);

        if (n < 0) return -1;
        if (n < (ssize_t)sizeof(microtcp_header_t)) continue;

        memcpy(&h, packet_buf, sizeof(microtcp_header_t));

        // verify checksum
        if (!verify_crc(&h)) {
            continue;
        }

        // handle fin packet
        if (ntohs(h.control) & 0x0001) {
            socket->ack_number = ntohl(h.seq_number) + 1;
            send_packet(socket, 0x0008, NULL, 0);
            socket->state = CLOSING_BY_PEER;
            return -1;
        }

        uint32_t seq = ntohl(h.seq_number);
        // check for in-order packet
        if (seq != socket->ack_number) {
            send_packet(socket, 0x0008, NULL, 0);
            continue;
        }

        uint32_t data_len = ntohl(h.data_len);
        if (data_len > 0) {
            if ((size_t)n < sizeof(microtcp_header_t) + (size_t)data_len) {
                continue;
            }
            if (received_bytes + data_len > length) {
                break;
            }

            // check available receive window
            if (socket->curr_win_size < data_len) {
                send_packet(socket, 0x0008, NULL, 0);
                continue;
            }

            socket->curr_win_size -= data_len;

            // copy payload to user buffer
            memcpy(user_buf + received_bytes, packet_buf + sizeof(microtcp_header_t), data_len);
            received_bytes += data_len;

            socket->ack_number += data_len;

            // update receive window
            socket->curr_win_size += data_len;
            if (socket->curr_win_size > socket->init_win_size) socket->curr_win_size = socket->init_win_size;
        }

        // send acknowledgment
        send_packet(socket, 0x0008, NULL, 0);

        if (received_bytes >= length) break;
    }

    return (ssize_t)received_bytes;
}