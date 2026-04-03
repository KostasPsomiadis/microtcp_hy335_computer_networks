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

#ifndef LIB_MICROTCP_H_
#define LIB_MICROTCP_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <netinet/in.h>

#define MICROTCP_ACK_TIMEOUT_US 200000
#define MICROTCP_MSS 1400
#define MICROTCP_RECVBUF_LEN 8192
#define MICROTCP_WIN_SIZE MICROTCP_RECVBUF_LEN
#define MICROTCP_INIT_CWND (3 * MICROTCP_MSS)
#define MICROTCP_INIT_SSTHRESH MICROTCP_WIN_SIZE

typedef enum
{
  LISTEN,
  SYN_SENT,
  SYN_RECEIVED,
  ESTABLISHED,
  FIN_WAIT_1,
  FIN_WAIT_2,
  CLOSING_BY_PEER,
  CLOSING_BY_HOST,
  CLOSED,
  INVALID
} mircotcp_state_t;


typedef struct
{
  int sd;
  mircotcp_state_t state;

  struct sockaddr_in peer_addr;
  socklen_t peer_addr_len;

  size_t init_win_size;
  size_t curr_win_size;

  uint8_t *recvbuf;
  size_t buf_fill_level;

  size_t cwnd;
  size_t ssthresh;

  size_t seq_number;
  size_t ack_number;
  uint64_t packets_send;
  uint64_t packets_received;
  uint64_t packets_lost;
  uint64_t bytes_send;
  uint64_t bytes_received;
  uint64_t bytes_lost;
} microtcp_sock_t;


typedef struct
{
  uint32_t seq_number;
  uint32_t ack_number;
  uint16_t control;
  uint16_t window;
  uint32_t data_len;
  uint32_t future_use0;
  uint32_t future_use1;
  uint32_t future_use2;
  uint32_t checksum;
} microtcp_header_t;


microtcp_sock_t
microtcp_socket (int domain, int type, int protocol);

int
microtcp_bind (microtcp_sock_t *socket, const struct sockaddr *address,
               socklen_t address_len);

int
microtcp_connect (microtcp_sock_t *socket, const struct sockaddr *address,
                  socklen_t address_len);

int
microtcp_accept (microtcp_sock_t *socket, struct sockaddr *address,
                 socklen_t address_len);

int
microtcp_shutdown(microtcp_sock_t *socket, int how);

ssize_t
microtcp_send (microtcp_sock_t *socket, const void *buffer, size_t length,
               int flags);

ssize_t
microtcp_recv (microtcp_sock_t *socket, void *buffer, size_t length, int flags);

#endif /* LIB_MICROTCP_H_ */