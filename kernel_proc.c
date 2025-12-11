
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "util.h"
#include "kernel_sched.h"

/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;
  pcb->thread_count=0;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->ptcb_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}

void start_new_thread(){

  int exitval;
  TCB* curThread=cur_thread();

  Task call =  curThread->ptcb->task;
  int argl = curThread->ptcb->argl;
  void* args = curThread->ptcb->args;

  exitval = call(argl,args);
  ThreadExit(exitval);


}




/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  

  
  /* The new process PCB */
  newproc = acquire_PCB();


  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {
    PTCB* ptcb=createPTCB(call,argl,args);
    rlnode* ptcb_node=rlnode_init(&ptcb->ptcb_list_node,ptcb);
    rlist_push_back(&newproc->ptcb_list,ptcb_node);
    TCB* tcb=spawn_thread(newproc,start_main_thread);
    ptcb->tcb=tcb;
    tcb->ptcb=ptcb;
    newproc->thread_count++;
    wakeup(tcb);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* First, store the exit status */
  curproc->exitval = exitval;

  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */
  if(get_pid(curproc)==1) {

    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }
    sys_ThreadExit(exitval);

  
}


/*the file operations of the proc info*/
static file_ops procinfo_ops = {  
    .Open = NULL,
    .Read = procinfo_read,
    .Write = procinfo_write, 
    .Close = procinfo_close
};

/*we don't need write function, it will just return -1*/
int procinfo_write()
{ 
  return -1;      
}

int procinfo_read(void* icb, char *buf, unsigned int n) 
{
  /* In case the PCB doesn't exist, it returns an error. */
  if (icb == NULL)
  { 
        return -1;
  }

  procinfo_cb* info_cb = (procinfo_cb*)icb; 

  /* If the cursor can't be used in PT, return an error. */
    if (info_cb->cursor < 1) 
    {      
      return -1;
    }

 /* If the cursor is a number greater than the max number of processes, return an error. */
    if (info_cb->cursor > MAX_PROC)
    {   
      return -1;
    }

    if (info_cb->cursor == MAX_PROC)
    {   
        return 0;
      
    }

  /* While we are 'falling into' free and empty PCBs, the cursor should move, until it finds an active PCB. */
  while(PT[info_cb->cursor].pstate == FREE)
  {
        info_cb->cursor++; /* Increase cursor to read the next process. */

        if (info_cb->cursor == MAX_PROC)
            return 0;
  }

    /* At this point, we just fill the procinfo_cb with the PCB's information. */
    info_cb->info.pid = info_cb->cursor;  
    info_cb->info.ppid = get_pid((PT[info_cb->cursor]).parent);
    info_cb->info.alive = PT[info_cb->cursor].pstate == ALIVE;
    info_cb->info.thread_count = PT[info_cb->cursor].thread_count;
    info_cb->info.main_task = PT[info_cb->cursor].main_task;
    info_cb->info.argl = PT[info_cb->cursor].argl;

    int argl; 

     /* If the length of the info is greater than the maximum argument size the new integer argl... */
    if (info_cb->info.argl > PROCINFO_MAX_ARGS_SIZE)
    { 
        argl = PROCINFO_MAX_ARGS_SIZE;  /* Will be equal to the maximum argument size and will be used in the following memcpy function.*/
    } else {
        argl = info_cb->info.argl;
  }

  /* Moving the data/ info of the PCB, of the read's buffer that called to make this operation for this PCB */
  memcpy(info_cb->info.args,(char *)PT[info_cb->cursor].args, sizeof(char)* argl ); /* Is used to pass the process name. */
  memcpy(buf, (char*)&info_cb->info, sizeof(procinfo));  /* Pass info to the buffer. */
  
  info_cb->cursor++; /* Move to the next PCB. */

  return 1;
}

int procinfo_close(void* procinfo_cb)
{
    if (procinfo_cb != NULL)
      free(procinfo_cb);
    return 0;
}

Fid_t sys_OpenInfo()
{
    Fid_t fid;
    FCB* fcb;

  if (! FCB_reserve(1, &fid, &fcb))
        return NOFILE;

    procinfo_cb* new_info = xmalloc(sizeof(procinfo_cb));

    new_info->cursor = 1; /* Initialization of the cursor to 1, so that we begin from the root PCB. */
    
    /* The FCB we reserved, now has as stream object the procinfo and as streamfunc the procinfo_ops */
    fcb->streamobj = new_info;
    fcb->streamfunc = &procinfo_ops;  

  return fid;
}

