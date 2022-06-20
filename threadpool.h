#ifndef THREADPOLL_H
#define THREADPOLL_H

#include <exception>
#include <pthread.h>
#include <list>
#include <cstdio>
#include "locker.h"

template<typename T>
class threadpool{
public:
    threadpool(int thread_number = 8,int max_requests = 10000);
    ~threadpool();
    bool append(T* request);

private:

    //worker函数为什么必须是static的？
    //因为pthread_create的第三个参数
    //只接受 void * worker(void *)类型的回调函数
    //而类成员内部的函数会被编译器修改为void * worker(threadpool* this,void *arg)。
    //不符合要求。所以改成静态的。

    //改成静态会引发出第二个问题，静态函数无法访问类中其他非静态成员
    //所以我们可以把this指针当参数传给arg

    static void* worker(void *arg);
    void run();

private:
    //线程的数量
    int m_thread_number;

    //线程池指针/数组，大小为m_thread_number
    pthread_t* m_threads;

    //请求队列中最多允许的、等待处理的请求的数量
    int m_max_requests;

    //请求队列
    std::list<T*> m_workqueue;

    //访问请求队列的互斥锁
    locker m_queuelocker;

    //请求队列的信号量
    sem m_queuestat;

    //是否结束线程
    bool m_stop;

};


//线程池构造函数
template< typename T >
threadpool< T >::threadpool(int thread_number,int max_requests):
        m_thread_number(thread_number),m_max_requests(max_requests),
        m_stop(false), m_threads(NULL){

    if(thread_number<=0 || max_requests<=0){
        throw std::exception();
    }

    //创建线程数组，错误则抛出异常
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw std::exception();
    }

    for(int i=0; i<m_thread_number; ++i){
        printf("create no.%d thread\n",i);
        if(pthread_create(m_threads+i,NULL,worker,this)!=0){
            delete[] m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i])){
            delete[] m_threads;
            throw std::exception();
        }
    }

}


//线程池析构函数
template< typename T >
threadpool< T >::~threadpool(){
    delete[] m_threads;
    m_stop = true;
}

//添加请求
template< typename T >
bool threadpool< T >::append(T* request){
    //添加前要给队列加锁

    //这里没有用传统的生产者消费者模型
    //append是非阻塞的形式，队列已满就直接返回false
    //worker才是阻塞的形式，对于线程来说，没有request就阻塞等待

    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template< typename T >
void* threadpool< T >::worker(void * arg){
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

template< typename T >
void threadpool< T >::run(){

    while(!m_stop){
        m_queuestat.wait();
        m_queuelocker.lock();

        //工作
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){
            continue;
        }
        request->process();
    }

}

#endif