
#include "tinyos.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "util.h"
#include "kernel_cc.h"



int wrong_call(){
	return -1;
}

void* null_open(){
	return NULL;
}

static file_ops reader_file_ops={
	.Open=null_open,
	.Read=pipe_read,
	.Write=wrong_call,
	.Close=pipe_reader_close
};

static file_ops writer_file_ops={
	.Open=null_open,
	.Read=wrong_call,
	.Write=pipe_write,
	.Close=pipe_writer_close
};

int sys_Pipe(pipe_t* pipe)
{
	Fid_t fid[2];
    FCB* fcb[2];

//reserve 2 FCB and 2 fid 
	if(FCB_reserve(2,fid,fcb)==0){
		return -1;
	}
/*the first fid reserved is connected with the read end 
  of the pipe and the second with the write end*/

	pipe->read=fid[0];
	pipe->write=fid[1];

//create and initialize pipe control block
	pipe_cb* pipeCB=(pipe_cb*)xmalloc(sizeof(pipe_cb));
	pipeCB->reader=fcb[0];
	pipeCB->writer=fcb[1];
	pipeCB->has_space=COND_INIT;
	pipeCB->has_data=COND_INIT;
	pipeCB->r_position=0;
	pipeCB->w_position=0;
	pipeCB->total_data=0;

/*reader and writer end of the pipe is connected with the same
  stream object the pipeCB*/
	pipeCB->reader->streamobj=pipeCB;
	pipeCB->writer->streamobj=pipeCB;

/*reader end is connected with file_ops reader_file_ops
  and writer end is connected with file_ops writer_file_ops*/
	pipeCB->reader->streamfunc=&reader_file_ops;
	pipeCB->writer->streamfunc=&writer_file_ops;

	return 0;
}

int pipe_read(void* pipecb_t, char* buf, unsigned int n){
	pipe_cb* pipe=(pipe_cb*) pipecb_t;

	/*if the reader end is closed we cant read
	 so return error*/
	if(pipe->reader==NULL || pipe==NULL){
		return -1;
	}

  /*while there is no data to read or the writer end is closed wait*/
	while((pipe->total_data == 0) && (pipe->writer != NULL)){
		kernel_broadcast(&pipe->has_space);
		kernel_wait(&pipe->has_data,SCHED_PIPE);
	}

	int count_bytes_read = 0; //num of bytes read 

	/* using a loop to read the data size n from the buf*/
	while((count_bytes_read < n) && (pipe->total_data > 0)){
		
		buf[count_bytes_read] = pipe->BUFFER[pipe->r_position];
    /*if the read position is at the end of the buffer then transfer 
     it in the front else move read one position */   
		if(pipe->r_position == PIPE_BUFFER_SIZE){	
			pipe->r_position = 0;
		}else{
			pipe->r_position++;
		}
	/*we have read data so we decrease the num of data that have not been
	 read*/
		pipe->total_data--;
		count_bytes_read++;

	}
	/*n size data has been read so broadcast as there is space to write in the buffer
	and then return*/
	kernel_broadcast(&pipe->has_space);
	return count_bytes_read;


}
int pipe_write(void* pipecb_t, const char* buf, unsigned int n){
	pipe_cb* pipe=(pipe_cb*) pipecb_t;

 /* if read or writer a=end is closed we cant do anything so return error*/
	if(pipe->reader==NULL || pipe->writer==NULL){
		return -1;
	}

 /*while the buffer is full or the reader is closed wait until the data is read*/
	while((pipe->total_data==PIPE_BUFFER_SIZE) && (pipe->reader != NULL)){
		kernel_broadcast(&pipe->has_data);
		kernel_wait(&pipe->has_space,SCHED_PIPE);
	}

	if (pipe->reader == NULL)
		return -1;

	int count_bytes = 0;

 /*a loop used to write n size data in the buffer*/
	while ((count_bytes < n) && (pipe->total_data < PIPE_BUFFER_SIZE)) {
		pipe->BUFFER[pipe->w_position]=buf[count_bytes];
	 /*if the write position is at the end of the buffer transfer it to the 
	  front else move write one position */
		if(pipe->w_position == PIPE_BUFFER_SIZE){
			pipe->w_position = 0;
		}else{
			pipe->w_position++;
		}
     /*we have write new data so we increase the number of data that have not
     been read*/
		pipe->total_data++;
		count_bytes++;


	}
	/*n size data have been writen and broadcast so we can start reading them
	and return the num of bytes writen*/
	kernel_broadcast(&pipe->has_data);
	return count_bytes;
  
}


int pipe_reader_close(void* _pipecb){
	pipe_cb* pipe=(pipe_cb*) _pipecb;
    
 /*close the read end of the pipe*/
	pipe->reader=NULL;

 /*if the write end is already closed then close
  the pipe*/
	if(pipe->writer==NULL){
		free(pipe);
	}
	else
		kernel_broadcast(&pipe->has_space);
    
  return 0;
}



int pipe_writer_close(void* _pipecb){
	pipe_cb* pipe=(pipe_cb*) _pipecb;

/*close the write end of the pipe*/
	pipe->writer=NULL;

/*if the read end is already closed then close
  the pipe*/
	if(pipe->reader==NULL){
		free(pipe);
	}
	else
		kernel_broadcast(&pipe->has_data);
	return 0;
}
