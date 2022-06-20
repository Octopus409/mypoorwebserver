#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <exception>
#include <semaphore.h>

class locker{
public:
    locker(){
        if(pthread_mutex_init(&m_mutex,NULL)!=0){
            throw std::exception();
        }
    }
    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock(){
        int ret = pthread_mutex_lock(&m_mutex);
        return ret==0;
    }

    bool unlock(){
        int ret = pthread_mutex_unlock(&m_mutex);
        return ret==0;
    }

    //得到m_mutex的指针
    pthread_mutex_t* get(){
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

class sem{
public:

    //初始化，默认0
    sem(){
        if(sem_init(&m_sem,0,0)!=0){
            throw std::exception();
        }
    }
    //初始化，用num
    sem(int num){
        if(sem_init(&m_sem,0,num)!=0){
            throw std::exception();
        }
    }
    bool wait(){
        return sem_wait(&m_sem)==0;
    }
    bool post(){
        return sem_post(&m_sem)==0;
    }

    ~sem(){
        sem_destroy(&m_sem);
    }


private:
    sem_t m_sem;
};

class cond{
public:
    cond(){
        if(pthread_cond_init(&m_cond,NULL)!=0){
            throw std::exception();
        }
    }
    ~cond(){
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t* mutex){
        return pthread_cond_wait(&m_cond,mutex)==0;
    }
    bool timedwait(pthread_mutex_t *mutex,struct timespec t){
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond,mutex,&t);
        return ret==0;
    }
    bool signal(){
        return pthread_cond_signal(&m_cond)==0;
    }
    bool broadcast(){
        return pthread_cond_broadcast(&m_cond)==0;
    }
private:
    pthread_cond_t m_cond;
};

#endif