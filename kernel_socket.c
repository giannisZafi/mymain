
#include "tinyos.h"
#include "kernel_cc.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_socket.h"

socket_cb* PORT_MAP[MAX_PORT] = {NULL};



int socket_read(void* socket_cb_t, char* buf, unsigned n){

	/*get the socket*/
	socket_cb* peer = (socket_cb*) socket_cb_t;
	/*if the socket is Null or not peer cannot read so return error*/
	if(peer->type != SOCKET_PEER || peer->peer_s.read_pipe == NULL){
		return -1;

	}
		/* Call pipe_read on the appropriate pipe. */
		return pipe_read(peer->peer_s.read_pipe,buf,n);
}


int socket_write(void* socket_cb_t, const char* buf, unsigned n){

	/*get the socket*/
	socket_cb* peer = (socket_cb*) socket_cb_t;

	/*if the socket is Null or not peer cannot write so return error*/
	if(peer->type != SOCKET_PEER || peer->peer_s.write_pipe == NULL)
		return -1;
	
	/* Call pipe_write on the appropriate pipe. */
	return pipe_write(peer->peer_s.write_pipe,buf,n);	
}

int socket_close(void* socket_cb_t){

	/*get the socket*/
	socket_cb* socket_to_close = (socket_cb*) socket_cb_t;

	if(socket_to_close->refcount == 0){
		/* Decide what to do depending on the type of socket. */
	  switch(socket_to_close->type)
	  {
		case SOCKET_UNBOUND:
			break;
		case SOCKET_LISTENER:
			/* uninstall the socket from the port table. */
			PORT_MAP[socket_to_close->port] = NULL;
			break;
		case SOCKET_PEER: //SHUTDOWN DOTH
			/* Close the reader and make the connection NULL. */
			if(socket_to_close->peer_s.read_pipe != NULL){
				pipe_reader_close(socket_to_close->peer_s.read_pipe);
				socket_to_close->peer_s.read_pipe = NULL;
			}

			/* Close the writer and make the connection NULL. */
			if(socket_to_close->peer_s.write_pipe != NULL){
				pipe_writer_close(socket_to_close->peer_s.write_pipe);
				socket_to_close->peer_s.write_pipe = NULL;
			}
			break;
		default:
			return -1;
	  }

	  free(socket_to_close);
	  return 0;
	}
	else{
		socket_to_close->refcount--;

		if(socket_to_close->type == SOCKET_LISTENER){
			/* uninstall the socket from the port table. */
			PORT_MAP[socket_to_close->port] = NULL;
			/* Broadcast the listener's condvar to wakeup all waiting sockets. */
			kernel_broadcast(&socket_to_close->listener_s.req_available);
		}
		return 0;
	}

	
}

static file_ops socket_file_ops = {
	.Open = NULL,
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

Fid_t sys_Socket(port_t port)
{
	/* Check if the port is valid*/
	if(port > MAX_PORT || port < 0){
		return NOFILE;
	}

	FCB* fcb;
	Fid_t fid;
	/*reserve 1 fcb and 1 fid to connect with the socket*/
	if(!FCB_reserve(1, &fid, &fcb)){
		return NOFILE;
	}

	/*create and initialize the socket control block*/
	socket_cb* socket = (socket_cb*)xmalloc(sizeof(socket_cb));
	socket->port = port;
	socket->refcount = 0;
	/*At first when a socket is created it is unbound type*/
	socket->type = SOCKET_UNBOUND;
	socket->fcb = fcb;

	fcb->streamfunc = &socket_file_ops;
	fcb->streamobj = socket;
	
	return fid;
}

int sys_Listen(Fid_t sock)
{
    /*check if the given fid is legal*/
	if(sock < 0 || sock > MAX_FILEID-1 ){
		return -1;
	}

    /*get the fcb from the given fid*/
	FCB* fcb = get_fcb(sock);

	/*check if the fcb is NULL and that is connected with the correct file ops*/
	if(fcb == NULL || fcb->streamfunc != &socket_file_ops){
		return -1;
	}
    
    /*get the (unbound) socket */
	socket_cb* socket = (socket_cb*) fcb->streamobj;

	/*check if the socket is Null or if the socket is not unbound*/
    if(socket==NULL || socket->type != SOCKET_UNBOUND ){
    	return -1;
    }    
    
    /* Check if its port is NOPORT:0 or port is occupied by another listener*/
	if( socket->port == NOPORT || PORT_MAP[socket->port] != NULL ){
		return -1;
	}


	/* Install the socket on the port map*/
	PORT_MAP[socket->port] = socket;

	/*change the type of the socket to listener*/
	socket->type = SOCKET_LISTENER;

	/*initialize the listener socket*/
	rlnode_init(&socket->listener_s.queue,NULL);/* Initialize the request queue of the listener to NULL*/
	socket->listener_s.req_available = COND_INIT;
	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{

	/* Check if given fid is legal */
	if(lsock < 0 || lsock > MAX_FILEID-1){
		return NOFILE;
	}

	/*get the fcb from the given fid*/
	FCB* socketfcb = get_fcb(lsock);

	/*check if the fcb is NULL and that is connected with the correct file ops*/
	if(socketfcb == NULL || socketfcb->streamfunc != &socket_file_ops){
		return NOFILE;
	}
    
    /*Get the socket that will start listening*/
	socket_cb* socket = (socket_cb*) socketfcb->streamobj;

	/*check if the socket is null and its type*/
	if(socket==NULL || socket->type != SOCKET_LISTENER){
		return NOFILE;
	}

	/* Increase listener socket's refcount. */
	socket->refcount++;

	/* Wait for a request */
	while(is_rlist_empty(&socket->listener_s.queue) && PORT_MAP[socket->port] != NULL) {
		kernel_wait(&socket->listener_s.req_available, SCHED_USER);
	}

	if(PORT_MAP[socket->port] == NULL){
		socket->refcount--;
		return NOFILE;
	}

	/* Wake up when a request is found. */
	/* Check if the port is still valid and correctly installed in the port map (the socket may have been closed while we were sleeping). */
	if(socket==NULL || socket->type!=SOCKET_LISTENER || PORT_MAP[socket->port]!=socket)
	{
		socket->refcount--;
		return NOFILE;
	}


    /* Take the first request from the queue and try to honor it. */
	connection_request* request = (connection_request*) rlist_pop_front(& socket->listener_s.queue)->obj;
	

	/* Try to construct peer (bound on the same port as the listener). */
	Fid_t peer_fid = sys_Socket(socket->port);

	if(peer_fid==-1 || get_fcb(peer_fid)==NULL)
	{
		socket->refcount--;
		return NOFILE;
	}

	request->admitted = 1;

	/* Get the two peers. */
	socket_cb *peer_socket = (socket_cb*) get_fcb(peer_fid)->streamobj;
	socket_cb *req__socket = request->peer;

	/* Set the fields of each of the peers. */
	peer_socket->type = SOCKET_PEER;
	req__socket->type = SOCKET_PEER;


    /*create and initialize the pipes*/
	pipe_cb* pipe1 = (pipe_cb*) xmalloc(sizeof(pipe_cb));
	pipe_cb* pipe2 = (pipe_cb*) xmalloc(sizeof(pipe_cb));
	pipe1->reader = peer_socket->fcb;
	pipe1->writer = req__socket->fcb;
	pipe1->r_position = 0;
	pipe1->w_position = 0;
	pipe1->has_data = COND_INIT;
	pipe1->has_space = COND_INIT;
	pipe1->total_data = 0;
	pipe2->reader = req__socket->fcb;
	pipe2->writer = peer_socket->fcb;
	pipe2->r_position = 0;
	pipe2->w_position = 0;
	pipe2->has_data = COND_INIT;
	pipe2->has_space = COND_INIT;
	pipe2->total_data = 0;
	
	/* Set the peer's pipe fields */
	peer_socket->peer_s.read_pipe = pipe1;
	peer_socket->peer_s.write_pipe = pipe2;
	req__socket->peer_s.read_pipe = pipe2;
	req__socket->peer_s.write_pipe = pipe1;

	/* Signal the Connect side. */
	kernel_signal(& request->connected_cv);

	/* In the end, decrease refcount. */
	socket->refcount--;

	return peer_fid;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{

	/*check if given fid and port is legal*/
	if(sock < 0 || sock > MAX_FILEID-1 ||  port > MAX_PORT || port < 0 ) 
	{
		return -1;
	}
	  /*get the fcb from the given fid*/
	FCB* fcb = get_fcb(sock);

	/*check if the fcb is NULL and that is connected with the correct file ops*/
	if(fcb == NULL || fcb->streamfunc != &socket_file_ops){
		return -1;
	}

    /*get the socket that make the request*/
	socket_cb* connect = (socket_cb*) fcb->streamobj;

	/*get the listener socket*/
	socket_cb* listener = PORT_MAP[port];

	/*check if the type of the socket that makes the request and check if the listener is Null 
	 or its type in not listener*/
	if(connect->type != SOCKET_UNBOUND || listener == NULL || listener->type != SOCKET_LISTENER){
		return -1;
	}

	/*increase refcount */
	listener->refcount++;

	/*create and initialize the connection request*/
	connection_request* req = (connection_request*) xmalloc(sizeof(connection_request));
	req->admitted = 0;
	req->connected_cv = COND_INIT;
	req->peer = connect;
	rlnode_init(&req->queue_node, req);

	/*Add request to the listeners queue and signal listener*/
	rlist_push_back(&listener->listener_s.queue, &req->queue_node);
	kernel_signal(&listener->listener_s.req_available);

	/*Block for the specified amount of time*/
	int timed_wait;
	while(req->admitted == 0){
		timed_wait = kernel_timedwait(& req->connected_cv, SCHED_USER, timeout);
		if (timed_wait == 0) {
			break;
		}
	}
    
    /*decrease refcount*/
	listener->refcount--;
    
    /*If the request was admitted return 0, else -1*/

	int admitted = req->admitted;
	 if(admitted==1)
	 	return 0;
	 else 
	 return -1;

}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	FCB* fcb = get_fcb(sock);
	if(fcb == NULL || fcb->streamfunc != &socket_file_ops){
		return -1;
	}
	socket_cb* peer = (socket_cb*) fcb->streamobj;
	if(peer->type != SOCKET_PEER){
		return -1;
	}

	switch (how)
	{
	case SHUTDOWN_WRITE:
		if(peer->peer_s.write_pipe != NULL){
			pipe_writer_close(peer->peer_s.write_pipe);
			peer->peer_s.write_pipe = NULL;
		}
		break;
	case SHUTDOWN_READ:
		if(peer->peer_s.read_pipe != NULL){
			pipe_reader_close(peer->peer_s.read_pipe);
			peer->peer_s.read_pipe = NULL;
		}
		break;
	case SHUTDOWN_BOTH:
		if(peer->peer_s.write_pipe != NULL){
			pipe_writer_close(peer->peer_s.write_pipe);
			peer->peer_s.write_pipe = NULL;
		}
		if(peer->peer_s.read_pipe != NULL){
			pipe_reader_close(peer->peer_s.read_pipe);
			peer->peer_s.read_pipe = NULL;
		}
		break;	
	default:
		return -1;
		break;
	}

	return 0;
}

