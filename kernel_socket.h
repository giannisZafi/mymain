#ifndef __KERNEL_SOCKET_H
#define __KERNEL_SOCKET_H


#include "bios.h"
#include "tinyos.h"
#include "kernel_streams.h"
#include "util.h"
#include "kernel_dev.h"


/**
  @brief Socket type
  There are three types of sockets
  A socket can be either unbound, listener, or peer
 */
enum socket_type{
  SOCKET_UNBOUND,
  SOCKET_LISTENER,
  SOCKET_PEER
};
/**
  @brief Unbound socket.
  Unbound socket (not a listener or a peer yet).
  This structure holds all information pertaining to a unbound socket.
 */
typedef struct unbound_socket{
  rlnode unbound_socket;  /** The node through which the socket will be attached to queues. */
}unbound_socket;

/**
  @brief Listener socket.
  Listener socket (waiting for inbound connections).
  This structure holds all information pertaining to a listener socket.
 */
typedef struct listener_socket{
  rlnode queue;/** The queue in which requests to connect with this socket will be stored.*/
  CondVar req_available;/** The conditional variable that incoming requests will signal
   to wake up the listener. */
}listener_socket;

/**
  @brief Peer socket.
  Peer socket (connected with another one and able to comunicate).
  This structure holds all information pertaining to a peer socket.
 */
typedef struct peer_socket{
  struct socket_cb* peer;
  pipe_cb* write_pipe; /** The pipe which this socket writes to. */
  pipe_cb* read_pipe; /** The pipe which this socket reads from. */
}peer_socket;

/**
  @brief Socket Control Block.

  This structure holds all information pertaining to a process.
 */
typedef struct socket_control_block{
  uint refcount;/** The counter of references to this socket. When it 
  becomes zero (0), we can safely delete it. */
  FCB* fcb;/** The FCB through which this socket is accessible (its file descriptor)
  for backwards connectivity. Can be used as socket state indicator (when it is NULL,
  the sockets is closed). */
  enum socket_type type;/** The type of this socket. The type of the socket matches
  the union member with the rest of the data. */
  port_t port;/** The port that the socket is bound to. */

  union {
    unbound_socket unbound_s;
    listener_socket listener_s;
    peer_socket peer_s;
  };
}socket_cb;
/**
  @brief Connection Request Control Block.

  This structure holds all information pertaining to a request.
 */
typedef struct connection_request{
  int admitted;/** A flag that shows whether the request was served.
  If it is 1, a connection was successfully established. */
  socket_cb* peer;/** The socket that makes the request. */

  CondVar connected_cv;/** The conditional variable on which
  the connecting socket sleeps until the request is served or
  the timeout time has passed. */
  rlnode queue_node;/** The node through which the request is
  attached to the listeners request queue. */
}connection_request;


int socket_write(void* socket, const char *buf, unsigned int n);
int socket_read(void* socket, char *buf, unsigned int n);
int socket_close(void* socket);

#endif

