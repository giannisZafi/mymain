
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_streams.h"


/*This function is used to initialize a PTCB. It is used when we create the first PTCB
  connected to PCB and when we create the other PTCBs inside of Create_Threads*/

PTCB* createPTCB(Task call,int argl,void *args){
  PTCB*  ptcb=(PTCB*)xmalloc(sizeof(PTCB));
  ptcb->task=call;
  ptcb->argl=argl;
  ptcb->args=args;
  //ptcb->exitval=-1;
  ptcb->exited=0;
  ptcb->detached=0;
  ptcb->refcount=0;
  rlnode_init(&ptcb->ptcb_list_node,ptcb);
  ptcb->exit_cv=COND_INIT;
  return ptcb;
}


  /*@brief Create a new thread in the current process.*/

Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  if(task==NULL){
    return NOTHREAD;
  }

  PCB* curproc=CURPROC;   //get current process
  PTCB* newptcb=createPTCB(task,argl,args);  //initialize PTCB with curproc information
  rlnode* ptcb_node=rlnode_init(&newptcb->ptcb_list_node,newptcb); 

  /*Add created ptcb into ptcb list in pcb*/
  rlist_push_back(&curproc->ptcb_list,ptcb_node);

  /*Call spawn thread to create tcb and connect it with ptcb*/
  TCB* newtcb=spawn_thread(curproc,start_new_thread);
  newtcb->ptcb=newptcb; 
  newptcb->tcb=newtcb;
  curproc->thread_count++; //increase thread counts
  wakeup(newtcb);         

return (Tid_t) newptcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread()->ptcb;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{

  PCB* curproc=CURPROC;  //get current process
  /*Find thread with the given tid inside the ptcb list in current pcb
  and if its not found or tries to join itself or is detached return -1;*/
  rlnode* find_node=rlist_find(&curproc->ptcb_list,(PTCB*)tid,NULL);
  if(find_node==NULL || tid==sys_ThreadSelf() || find_node->ptcb->detached==1){
    return -1;
  }
  else {   

    /*Else we increase refcount */
    PTCB* our_sweet_ptcb=find_node->ptcb;
    our_sweet_ptcb->refcount++;

    /*while ptcb is not exited or detached wait*/
    while(!our_sweet_ptcb->exited && !our_sweet_ptcb->detached){
      kernel_wait(&our_sweet_ptcb->exit_cv,SCHED_USER);
    }

    //when ptcb stops waiting decrease refcount
    our_sweet_ptcb->refcount--;

    if(our_sweet_ptcb->detached){ 
      return -1;
    }


    //if we have an exitval set the current exitval to the given exitval
    if(exitval!=NULL){
      *exitval=our_sweet_ptcb->exitval;
    }

    //if no one is waiting for ptcb we clean it
    if(our_sweet_ptcb->refcount==0){
      rlist_remove(&our_sweet_ptcb->ptcb_list_node);
      free(our_sweet_ptcb);

      }

      return 0;


  }
  
}  


/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
	PCB* curproc=CURPROC;   //get current process
  rlnode* find_node=rlist_find(&curproc->ptcb_list,(PTCB*)tid,NULL); /*search for given Tid in ptcb list
  inside the pcb of current process*/
  if(find_node==NULL){     
    return -1;}      //if the node is not found return -1
  else if (find_node->ptcb->exited==1){  //if the thread is exited it can't be detached so it returns -1
    return -1;}
  else{
    find_node->ptcb->detached=1;       //we make the thread detached and wake up all the connected threads
    kernel_broadcast(&find_node->ptcb->exit_cv);
    return 0;}

  }
  

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{

  PTCB* ptcb=(PTCB*) sys_ThreadSelf();  //get current thread
  ptcb->exitval=exitval;       //save exitval          
  ptcb->exited=1;             //make thread exited

  PCB *curproc = CURPROC;    //get curproc
  curproc->thread_count--;   //decrease thread count

  kernel_broadcast(&ptcb->exit_cv);  //wake up all the threads that are waiting from Thread_Join


  if(curproc->thread_count==0){   //if it's the last thread from the current process 

    if(get_pid(curproc)!=1){


    /* Reparent any children of the exiting process to the 
       initial task */
    PCB* initpcb = get_pcb(1);
    while(!is_rlist_empty(& curproc->children_list)) {
      rlnode* child = rlist_pop_front(& curproc->children_list);
      child->pcb->parent = initpcb;
      rlist_push_front(& initpcb->children_list, child);
    }

    /* Add exited children to the initial task's exited list 
       and signal the initial task */
    if(!is_rlist_empty(& curproc->exited_list)) {
      rlist_append(& initpcb->exited_list, &curproc->exited_list);
      kernel_broadcast(& initpcb->child_exit);
    }

    /* Put me into my parent's exited list */
    if(curproc->parent!=NULL){
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit);
    }
  }

  assert(is_rlist_empty(& curproc->children_list));
  assert(is_rlist_empty(& curproc->exited_list));

  /* 
    Do all the other cleanup we want here, close files etc. 
   */

  /* Release the args data */
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }
  
  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }
  }


  /*We clean ptcb from memory if it's exited and no one waits for it */
  while(rlist_find(&curproc->ptcb_list,ptcb,FREE)!=NULL){
    if(ptcb->refcount<1){
      rlist_remove(&ptcb->ptcb_list_node);
      free(ptcb);
    }
  }
  

  }
  

  /* Disconnect my main_thread */
  curproc->main_thread = NULL;

  /* Now, mark the process as exited. */
  curproc->pstate = ZOMBIE;

  /* Bye-bye cruel world */
  kernel_sleep(EXITED, SCHED_USER);
}




