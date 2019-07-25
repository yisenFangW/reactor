#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include "thread_pool.h"


static int thpool_keepalive = 1 ;   //线程池保持存活
pthread_mutex_t  mutex  = PTHREAD_MUTEX_INITIALIZER ;  //静态赋值法初始化互斥锁


/*
 *    初始化线程池
 *    为线程池， 工作队列， 申请内存空间，信号等申请内存空间
 *    @param  :将被使用的线程ID
 *    @return :成功返回的线程池结构体，错误返回null
 */
thpool_t * thpool_init (int threadsN){
    thpool_t  *tp_p ;

    if (!threadsN || threadsN < 1){
        threadsN = 1 ;
    }

    tp_p =  (thpool_t *)malloc (sizeof (thpool_t)) ;
    if (tp_p == NULL){
        fprintf (stderr ,"thpool_init (): could not allocate memory for thread pool\n");
        return NULL ;
    }
    //线程指向这个malloc分配的地址，应该是线程池的线程的首地址；
    tp_p->threads = (pthread_t *)malloc (threadsN * sizeof (pthread_t));
    if (tp_p->threads == NULL){
        fprintf( stderr , "could not allocation memory for thread id\n");
        return NULL;
    }
    tp_p->threadsN = threadsN ;

    //初始化线程池队列，传入新建的线程池参数；
    if (thpool_jobqueue_init (tp_p) == -1){
        fprintf (stderr ,"could not allocate memory for job queue\n");
        return NULL;
    }

    /*初始化信号*/
    tp_p->jobqueue->queueSem = (sem_t *)malloc (sizeof (sem_t));

    /*定位一个匿名信号量，第二个参数是1表示。这个信号量将在进程内的线程是共享的，第三个参数是信号量的初始值*/
    sem_init (tp_p->jobqueue->queueSem, 0 , 0 );

    int  t ;

    for (t = 0 ; t < threadsN ; t++){
        printf ("Create thread %d in pool\n", t);

        //第四个参数是传递给函数指针的一个参数，这个函数指针就是我们所说的线程指针
        if (pthread_create (&(tp_p->threads[t]) , NULL , thpool_thread_do , (void *)tp_p)){
            free (tp_p->threads);
            free (tp_p->jobqueue->queueSem);
            free (tp_p->jobqueue);
            free (tp_p);
        }
    }
    return  tp_p ;
}

/*
 * 初始化完线程应该处理的事情
 * 这里存在两个信号量，
 */

void* thpool_thread_do (void *p){
    thpool_t *tp_p = (thpool_t *)p;
    while (thpool_keepalive)
    {
        if (sem_wait (tp_p->jobqueue->queueSem))  //如果工作队列中没有工作，那么所有的线程都将在这里阻塞，当他调用成功的时候，信号量-1
        {
            fprintf(stderr , "Waiting for semaphore\n");
            exit (1);
        }

        if (thpool_keepalive)
        {
            void (*func_buff) (void *arg);
            void *arg_buff;
            thpool_job_t *job_p;

            pthread_mutex_lock (&mutex);
            job_p = thpool_jobqueue_peek (tp_p);
            func_buff = job_p->function ;
            arg_buff= job_p->arg ;
            thpool_jobqueue_removelast (tp_p);
            pthread_mutex_unlock (&mutex);

            func_buff (arg_buff);

            free (job_p);
        }
        else
        {
            return nullptr;
        }
    }
    return nullptr;
}

/*
 * 结构体应该是函数指针*/
//function_p是一个指针，指向参数为void* 且返回类型为void的函数
int thpool_add_work (thpool_t *tp_p ,void* (*function_p)(void *), void *arg_p){

    thpool_job_t   *newjob ;

    newjob = (thpool_job_t *)malloc (sizeof (thpool_job_t));
    if (newjob == NULL)
    {
        fprintf (stderr,"couldnot allocate memory for new job\n");
        exit (1);
    }
    newjob->function = function_p ;
    newjob->arg = arg_p ;

    pthread_mutex_lock (&mutex);
    thpool_jobqueue_add (tp_p ,newjob);
    pthread_mutex_unlock (&mutex);
    return 0 ;
}


void thpool_destory (thpool_t *tp_p){
    int    t ;

    //这个好似保活的全部变量参数
    thpool_keepalive = 0 ;  //让所有的线程运行的线程都退出循环

    for (t = 0 ; t < (tp_p->threadsN) ; t++ ){
        //sem_post 会使在这个线程上阻塞的线程，不再阻塞
        if (sem_post (tp_p->jobqueue->queueSem) ){
            fprintf (stderr,"thpool_destory () : could not bypass sem_wait ()\n");
        }
    }
    if (sem_destroy (tp_p->jobqueue->queueSem)!= 0){
        fprintf (stderr, "thpool_destory () : could not destroy semaphore\n");
    }

    //回收线程
    for (t = 0 ; t< (tp_p->threadsN) ; t++)
    {
        pthread_join (tp_p->threads[t], NULL);
    }
    //回收线程池资源
    thpool_jobqueue_empty (tp_p);
    free (tp_p->threads);
    free (tp_p->jobqueue->queueSem);
    free (tp_p->jobqueue);
    free (tp_p);
}

/*
 * 初始化队列
 * @param: 指向线程池的指针
 * @return :成功的时候返回是 0 ，分配内存失败的时候，返回是-1
 */
int thpool_jobqueue_init (thpool_t *tp_p)
{
    tp_p->jobqueue = (thpool_jobqueue *)malloc (sizeof (thpool_jobqueue));
    if (tp_p->jobqueue == NULL)
    {
        fprintf (stderr ,"thpool_jobqueue malloc is error\n");
        return -1 ;
    }
    tp_p->jobqueue->tail = NULL ;
    tp_p->jobqueue->head = NULL ;
    tp_p->jobqueue->jobsN = 0 ;
    return 0 ;

}

/*
 *添加任务到队列
 *一个新的工作任务将被添加到队列，在使用这个函数或者其他向别的类似这样
 *函数 thpool_jobqueue_empty ()之前，这个新的任务要被申请内存空间
 *
 * @param: 指向线程池的指针
 * @param:指向一个已经申请内存空间的任务
 * @return   nothing
 */
//插入到队列头，
void thpool_jobqueue_add (thpool_t *tp_p , thpool_job_t *newjob_p){
    newjob_p->next = NULL ;
    newjob_p->prev = NULL ;

    thpool_job_t   *oldfirstjob ;
    oldfirstjob = tp_p->jobqueue->head;

    //判断队列中是否有元素
    switch (tp_p->jobqueue->jobsN)
    {
        case 0 :
            tp_p->jobqueue->tail = newjob_p;
            tp_p->jobqueue->head = newjob_p;
            break;
        default :
            oldfirstjob->prev= newjob_p ;
            newjob_p->next = oldfirstjob ;
            tp_p->jobqueue->head= newjob_p;
            break;
    }

    (tp_p->jobqueue->jobsN)++ ;
    sem_post (tp_p->jobqueue->queueSem);  //原子操作，信号量增加1 ，保证线程安全

    int sval ;
    sem_getvalue (tp_p->jobqueue->queueSem , &sval);   //sval表示当前正在阻塞的线程数量
}

int thpool_jobqueue_removelast (thpool_t *tp_p){
    thpool_job_t *oldlastjob  , *tmp;
    oldlastjob = tp_p->jobqueue->tail ;

    switch (tp_p->jobqueue->jobsN)
    {
        case 0 :
            return -1 ;
            break;
        case 1 :
            tp_p->jobqueue->head = NULL ;
            tp_p->jobqueue->tail = NULL ;
            break;
        default :
            tmp = oldlastjob->prev ;
            tmp->next = NULL ;
            tp_p->jobqueue->tail = oldlastjob->prev;
    }
    (tp_p->jobqueue->jobsN) -- ;
    int sval ;
    sem_getvalue (tp_p->jobqueue->queueSem, &sval);
    return 0 ;
}

//返回任务队列中的
thpool_job_t * thpool_jobqueue_peek (thpool_t *tp_p){
    return tp_p->jobqueue->tail ;
}

/*
 *移除和撤销这个队列中的所有任务
 *这个函数将删除这个队列中的所有任务，将任务对列恢复到初始化状态，因此队列的头和对列的尾都设置为NULL ，此时队列中任务= 0
 *
 *参数：指向线程池结构体的指针
 *
 */
void thpool_jobqueue_empty (thpool_t *tp_p)
{
    thpool_job_t *curjob;
    curjob = tp_p->jobqueue->tail ;
    while (tp_p->jobqueue->jobsN){
        tp_p->jobqueue->tail = curjob->prev ;
        free (curjob);
        curjob = tp_p->jobqueue->tail ;
        tp_p->jobqueue->jobsN -- ;
    }
    tp_p->jobqueue->tail = NULL ;
    tp_p->jobqueue->head = NULL ;
}