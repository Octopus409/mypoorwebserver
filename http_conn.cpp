#include "http_conn.h"

// 日志系统控制
extern int m_close_log;

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

//网站的根目录
const char* doc_root = "/home/yohanna/Desktop/test_project/mywebserver/resource";

//设置文件描述符为非阻塞
int setnonblocking(int fd){
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
}

//添加fd到epoll中，oneshot代表是否添加事件EPOLL_ONESHOT
void addfd(int epollfd, int fd, bool one_shot){

    epoll_event event;
    event.data.fd = fd;
    //event.events = EPOLLIN | EPOLLRDHUP;
    event.events =  EPOLLET | EPOLLIN  | EPOLLRDHUP;
    if(one_shot){
        //注册oneshot后，该事件只会触发一次，之后都需要重置
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);

}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// 修改文件描述符，重置EPOLLONESHOT事件
void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET |  EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

//定义静态变量
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(){
    if(m_sockfd!=-1){
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;

    //端口复用？
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 添加到epoll对象中
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    //初始化一些其他信息
    init();

}

void http_conn::init(){

    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;   //初始化状态为解析请求首行
    m_checked_idx = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = 0;

    bzero(m_read_buf,READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

bool http_conn::read(){

    if(m_read_idx>=READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read = 0;
    while(true){
        bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE - m_read_idx,0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                //没有数据
                break;
            }
            return false;
        } else if(bytes_read==0){
            //对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    //printf("读取到了数据： %s\n",m_read_buf);
    return true;
}

http_conn::HTTP_CODE http_conn::process_read(){
    //自己写一次主状态机
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char* text = 0;
    while((line_status=parse_line())==LINE_OK
        ||((m_check_state==CHECK_STATE_CONTENT) && (line_status==LINE_OK))){
        //对于这里的一些思考：parse_line会修改m_check_idx的值，然而如果一行没有读取成功
        //也就是没有遇到/r/n的话。这样会不会影响下一次read呢。答案是不会的。虽然m_checked_idx
        //被修改了。但是text = m_read_buffer + m_start_line是不会受到影响的。

        text = get_line();
        //很重要一句，m_start_line是正要解析的句子头。m_checked_idx是解析到的字符位。
        //parse_line会改变m_checked_idx的值。每次parse_line后，checked_idx都会移到
        //下一个句子的头部。
        m_start_line = m_checked_idx;
        //printf("got 1 http line : %s\n", text);

        LOG_INFO("%s",text);

        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }

    }
    return NO_REQUEST;
}

//解析请求首行
http_conn::HTTP_CODE http_conn::parse_request_line(char *text){
    // GET http://jsuacm.cn/ HTTP/1.1
    // 自己写一遍解析请求行

    m_url = strpbrk(text," \t");

    *m_url++ = '\0';
    char * method = text;
    if(strcasecmp(method,"GET")!=0){
        return BAD_REQUEST;
    }

    m_version = strpbrk(m_url," \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';

    if(strncasecmp(m_url,"http://",7)==0){
        m_url += 7;
        //strchr查找m_url中第二个参数char开头的坐标
        m_url = strchr(m_url,'/');
    }
    if(!m_url || m_url[0]!='/'){
        return BAD_REQUEST;
    }

    if(strcasecmp(m_version,"HTTP/1.1")!=0){
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析头部字段
http_conn::HTTP_CODE http_conn::parse_headers(char * text){
    //自己写一遍解析头部字段

    //请求头和请求首行的区别就是，请求头每一句都是独立分开的（自带\r\n）。请求首行需要自己分割
    if(text[0]=='\0'){
        //结束符，
        if(m_content_length!=0){
            //还要继续解析请求体
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    } else if(strncasecmp(text,"Connection:",11)==0){
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn(text," \t");
        if(strncasecmp(text,"keep-alive",10)==0){
            m_linger = true;
        }
    } else if(strncasecmp(text,"Host:",5)==0){
        // 处理Host头部字段
        text += 5;
        text += strspn(text," \t");
        m_host = text;
    } else if(strncasecmp(text,"Content-Length:",15)==0){
        text += 15;
        text += strspn(text," \t");
        //atol 和 atoi 的区别就是一个转换为Long整型，一个是int整型
        m_content_length = atol(text);
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}


//解析请求体
http_conn::HTTP_CODE http_conn::parse_content(char * text){

    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//获取一行
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    //工作原理：遇到\r\n两个符号就要把\r\n的置为\0，这样就能通过\0分隔开字符串const char*
    for(; m_checked_idx < m_read_idx; ++m_checked_idx){

        temp = m_read_buf[m_checked_idx];
        if(temp == '\r'){
            if((m_checked_idx + 1)==m_read_idx){
                return LINE_OPEN;
            } else if(m_read_buf[m_checked_idx+1]=='\n'){
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if(temp =='\n'){
            if((m_checked_idx>1)&&(m_read_buf[m_checked_idx-1]=='\n')){
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    //  "/test_project/mywebserver"
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    //拼接S2到S1上，最多拼接n个字符
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限 
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    }

}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;

    bytes_to_send = m_write_idx;

    return true;
}

void http_conn::process(){

    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){
        //NO_REQUEST代表请求不完整，需要重新接收一次pack。
        //将fd重新注册oneshot可以再次检测
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;

        // 下一次再进入process_read的时候，check_idx和m_start_line还有m_check_state
        // 是保持不变的，也就是上次解析的状态、数据还保留着，等待接收到新数据后，再进行解析。
    }


    //printf("proecss  队列中还有: %d个用户\n", http_conn::m_user_count);

    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }

    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}
