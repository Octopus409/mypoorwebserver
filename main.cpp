#include <sys/types.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"

#define MAX_FD 65536    //最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  //监听的最大的事件数量
#define TIMESLOT 5      // 每5秒检测一次非活跃连接




static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;
int m_close_log = 0;  // 是否关闭日志系统

extern void addfd(int epollfd,int fd, bool one_shot);
extern void removefd( int epollfd, int fd );
extern int setnonblocking(int fd);

void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void addsig(int sig){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig,&sa,NULL);
}

void timer_handler(http_conn* users)
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick(users);
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 每个定时器都接向这个cb_func,他只负责把timer从链表中删除
// epoll中删除文件描述符和关闭连接都由user列表的close_connd()负责
void cb_func( client_data* user_data )
{
    assert( user_data );
    user_data->sockfd = -1;

}

int main(int argc, char* argv[]){

    if(argc <=1){
        printf("按照如下格式运行: %s port_number\n",basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    //atoi函数，把const char*转换为整数,例如"123"转为123
    int port = atoi(argv[1]);

    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    } catch(...){
        return 1;
    }


    // 日志系统，初始化
    if(0==m_close_log) Log::get_instance()->init("ServerLog", 0, 2000, 800000, 800);

    http_conn* users = new http_conn[MAX_FD];

    int listenfd = socket(AF_INET,SOCK_STREAM,0);

    int ret = 0;
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = INADDR_ANY;

    //端口复用，为什么的？
    // 端口复用的原因：端口复用最常用用途:
    //1、防止服务器重启时之前绑定的端口还未释放
    //2、程序突然退出而系统没有释放端口
    // 端口复用要在bind前
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,&reuse,sizeof(reuse));
    ret = bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    ret = listen(listenfd, 5);

    epoll_event events[MAX_EVENT_NUMBER];
    //参数5是随便打的
    epollfd = epoll_create(5);
    //处理Listenfd的只有主线程一个，没有线程竞争，所以没有必要设置oneshot
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0] ,false);

    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_webser = false;   //停止整个服务器，收到SIG_TERM就会置为1

    // user_timer 是用户定时信息，和users有点类似，用sockfd选定用户
    client_data* users_timer;
    try{
        users_timer = new client_data[MAX_FD];
    }catch(...){
        return 1;
    }
    bool timeout = false;   // 定时时间到，准备调用tick()
    alarm(TIMESLOT);   // 开始首次定时

    while( !stop_webser ){
        //最后一个参数，-1，也就是阻塞
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        //其中EINTR导致的不算是错误，继续循环。
        if((num<0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }
        //printf("num=%d\n",num);
        for(int i=0; i<num; ++i){

            int sockfd = events[i].data.fd;
            //监听socket
            if(sockfd==listenfd){

                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);
                //往epoll中添加connfd
                int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &len);

                if(connfd < 0){
                    printf("errno is: %d\n", errno);
                    continue;
                }

                if( http_conn::m_user_count >= MAX_FD){
                    // 这里直接用close，是因为connfd尚未加入epoll，也还没影响m_user_num，
                    // 所以直接close就行
                    close(connfd);
                    continue;
                }
                printf("Get one connect.... connfd = %d\n",connfd);
                LOG_INFO("get one connect connfd = %d",connfd);
                users[connfd].init(connfd, client_addr);

                // 初始化user_timer的信息
                users_timer[connfd].address = client_addr;
                users_timer[connfd].sockfd = connfd;

                // 创建定时器，设置回调函数和超时时间，然后绑定定时器和用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3*TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer( timer );

            } else if(events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                
                // 因为设计上缺陷，每次关闭连接，都要调用user[sockfd].close_con()和回调函数cb_func()
                util_timer* timer = users_timer[sockfd].timer;
                users[sockfd].close_conn();
                cb_func( &users_timer[sockfd] );
                timer_lst.del_timer( timer );
                LOG_INFO("close one connect = %d",sockfd);

            } else if(( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN )){

                // 处理信号
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if(ret==-1){
                    continue;
                } else if(ret==0){
                    continue;
                } else{
                    for(int i=0; i<ret; ++i){
                        switch(signals[i]){
                            case SIGALRM:
                            {
                                // timeout标记时间已到，但不立即处理
                                // 因为定时任务的优先级不是很高，其他IO操作更重要
                                timeout = true;
                                LOG_INFO("time tick");
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_webser = true;
                            }
                        }
                    }
                }

            }else if( events[i].events & EPOLLIN){
                util_timer* timer = users_timer[sockfd].timer;
                if(users[sockfd].read()){
                    pool->append(users+sockfd);
                    
                    if( timer ){
                        // timer 正常，调整timer的位置
                        time_t cur = time(NULL);
                        timer->expire = cur + 3*TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }else {
                    users[sockfd].close_conn();
                    cb_func( &users_timer[sockfd] );
                    timer_lst.del_timer( timer );
                }

            } else if(events[i].events & EPOLLOUT){
                util_timer* timer = users_timer[sockfd].timer;
                if(!users[sockfd].write()){
                    users[sockfd].close_conn();
                    cb_func( &users_timer[sockfd] );
                    timer_lst.del_timer( timer );
                }
            }

        }

        if(timeout){
            timer_handler(users);
            timeout = false;
        }

    }
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    close(epollfd);
    delete [] users;
    delete pool;

    return 0;
}
