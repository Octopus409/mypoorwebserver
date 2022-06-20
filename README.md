# mypoorwebserver
一个轻量级HTTP服务器，实现网页浏览功能。

模拟proactor的IO模型，主线程负责建立、读写数据，线程池负责解析、处理HTTP请求。

1、模拟proactor的IO模型，主线程负责建立、读写数据

2、使用Epoll + ET 的IO处理方式

3、使用了线程池，线程池从工作队列中获取任务，然后执行

4、实现了GET请求，包括请求体内容读取、是否长连接

5、利用定时器+信号，实现了非活跃连接的断开


压测结果

配置环境：Linux Ubuntu 16  2G内存  处理器i7-8700 3.2GHz

使用webbench进行压力测试

1、未加定时器 7000连接，10秒，ET 

**7000 clients, running 10 sec.**

**Speed=343380 pages/min, 905218 bytes/sec.**
**Requests: 57230 susceed, 0 failed.**

2、未加定时器 7000连接，5秒，LT

**7000 clients, running 5 sec.**

**Speed=329940 pages/min, 848901 bytes/sec.**
**Requests: 27495 susceed, 0 failed.**

3、未加定时器 7000连接，120秒 ET

**7000 clients, running 120 sec.**

**Speed=365754 pages/min, 968001 bytes/sec.**
**Requests: 731283 susceed, 225 failed.**

4、未加定时器 7000连接，120秒 LT

**7000 clients, running 120 sec.**

**Speed=371892 pages/min, 984408 bytes/sec.**
**Requests: 743553 susceed, 231 failed.**

5、 加了定时器 7000连接 ， 10秒 ET

**7000 clients, running 10 sec.**

**Speed=423420 pages/min, 1122015 bytes/sec.**
**Requests: 70570 susceed, 0 failed.**

6、加了定时器 7000连接，120秒 ET

**7000 clients, running 120 sec.**

**Speed=522794 pages/min, 1384106 bytes/sec.**
**Requests: 1044663 susceed, 926 failed.**
