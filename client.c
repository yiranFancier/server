#include "client.h"
#include "stdlib.h"
#include "stdio.h"
#include "arpa/inet.h"  //struct in_addr
#include "sys/socket.h"  //使用套接字
#include "sys/syslog.h"
#include "sys/time.h"
#include "sys/epoll.h"
#include "sys/types.h" //使用消息队列
#include "sys/ipc.h"  //使用消息队列
#include "sys/msg.h"  //使用消息队列
#include "time.h"
#include "signal.h"
#include "errno.h"   //错误号定义
//#include "list.h"   //使用链表
#include "string.h"
#include "fcntl.h"
#include "mysql.h"
#include "openssl/err.h"
#include "comunication.h"
#include "unistd.h"
#include "pthread.h"
#include "mmpool.h"  //自己实现的简单的内存池
#include "tdpool.h"



struct usr_info{  //保存着验证信息
    char name[CLIENT_NAME_LENGTH];
    char passwd[MAX_PASSWD_LENGTH];
    int id;
    int lv;
    char pub_key[1024];  //通信所用的公钥
    char priv_key[1024];  //私钥
};


/* 通过共享内存的办法，专门用来传送数据的结构体。可用在一些请求队列,比如数据在
 * 发送之前需要加密的请求队列。
 */
struct tr_data{
    int client_id;  //是哪个客户端的数据
    unsigned data_size;  //数据大小
    void *data;  //数据的指针
};

MYSQL g_client_mysql;   //客户端数据库
sem_t client_list_mutex;  //互斥访问客户端列表
sem_t g_client_rb_mutex; //客户端红黑树互斥锁
struct rb_tree g_client_rb_t; //客户端红黑树
int g_msqid_dec;   //解密线程所需要的消息队列ID
struct client_info client_i_h;   //链表头客户端
int g_socket_epoll_fd;
int g_socket_snd_epoll_fd;
int g_socket_disc_epoll_fd;
//解密线程管道,通过管道给aes解密线程传送客户端ID，解密线程就会把客户端的数据接收
//缓存中的数据进行解密
int g_dec_thrd_pipfd[2];  
//加密线程管道,通过管道给aes加密线程传送客户端ID，加密线程就会把将要发送给客户端
//的报文进行加密
int g_enc_thrd_pipfd[2];   

struct msgq_s *g_msgq_enc;
struct msgq_s *g_msgq_del;  //客户端删除队列

struct mm_pool_s *g_client_mmpl;  //客户端信息结构体所使用的内存池
struct mm_pool_s *g_trdata_mmpl;  //传送数据所用到的内存池
struct tdpl_s *g_tdpl_req;  //请求处理所用的线程池
struct tdpl_s *g_tdpl_reqack; //请求响应处理所用的线程池



/* 函数名: int client_pipe_init()
 * 功能: 生成各种所需要的管道
 * 参数:
 * 返回值: -1,成功
 *          1,失败
 */
int client_pipe_init(){
    int ppfds_tmp[2];

    if(pipe(ppfds_tmp) == -1){
        return -1;
    }
    g_dec_thrd_pipfd[0] = ppfds_tmp[0];
    g_dec_thrd_pipfd[1] = ppfds_tmp[1];
    if(pipe(ppfds_tmp) == -1){
        return -1;
    }
    g_enc_thrd_pipfd[0] = ppfds_tmp[0];
    g_enc_thrd_pipfd[1] = ppfds_tmp[1];
    
    return 1;
}
/* 函数: int client_manage_init()
 * 功能: 初始化客户端管理
 * 参数: 
 * 返回值: -1,
 *          1,
 */
int client_manage_init(){
    int err_ret;
    client_i_h.id = -1;  //链表头的客户端的ID是-1
    unsigned long tid_irecv_thread;   //指令接收线程ID
    INIT_LIST_HEAD(&client_i_h.list);   //初始化客户端链表
    rb_init(&g_client_rb_t); //初始化客户端红黑树
    sem_init(&client_list_mutex,0,1);  //初始化信号量(锁)
    sem_init(&g_client_rb_mutex,0,1);  //初始化客户端红黑树互斥锁
    encdec_init();
    /*不吝啬地使用if来判断初始化是否成功*/
    if (client_wd_init() == -1){   //初始化看门狗
        syslog(LOG_DEBUG,"client_manage_init-->client_wd_init error:return -1");
        return -1;
    }
    if((err_ret = client_mysql_connect(&g_client_mysql)) < 0){  //连接数据库
        return err_ret;
    }   
    if((g_socket_epoll_fd = epoll_create(1)) == -1){
        syslog(LOG_DEBUG,"Epoll create error:%s",strerror(errno));
        return -1;
    }
    if((g_socket_snd_epoll_fd = epoll_create(1)) == -1){
        syslog(LOG_DEBUG,"Send epoll create error:%s",strerror(errno));
        return -1;
    }
    if((g_socket_disc_epoll_fd = epoll_create(1)) == -1){
        syslog(LOG_DEBUG,"Disconnect epoll create error:%s",strerror(errno));
        return -1;
    }
    if(com_msgq_create(&g_msgq_enc) == -1){
        syslog(LOG_DEBUG,"Failed to create msgq:%s",strerror(errno));
        return -1;
    }
    if(com_msgq_create(&g_msgq_del) == -1){
        syslog(LOG_DEBUG,"Failed to create delete msgq:%s",strerror(errno));
        return -1;
    }
    if(client_pipe_init() == -1){
        syslog(LOG_DEBUG,"Pipe init error:%s",strerror(errno));
        return -1;
    }
    if(client_msq_init() == -1){
        syslog(LOG_DEBUG,"MSQ init error:%s",strerror(errno));
        return -1;
    }
    if(pthread_create(&tid_irecv_thread,NULL,client_recv_thread,NULL) < 0){  //创建专门接收信息的线程
        syslog(LOG_DEBUG,"Failed to create client_recv_thread:%s",strerror(errno));
        return -1;
    }
    if(pthread_create(&tid_irecv_thread,NULL,client_del_thread,NULL) < 0){ 
        syslog(LOG_DEBUG,"Failed to create client_del_thread:%s",strerror(errno));
        return -1;
    }
    if(pthread_create(&tid_irecv_thread,NULL,client_discnct_monitor_t,NULL) < 0){ 
        syslog(LOG_DEBUG,"Failed to create client_discnct_monitor_t:%s",strerror(errno));
        return -1;
    }
    if(pthread_create(&tid_irecv_thread,NULL,client_dec_parse_thread,NULL) < 0){  //创建专门解密信息的线程
        syslog(LOG_DEBUG,"Failed to create client_dec_parse_thread:%s",strerror(errno));
        return -1;
    }
    if(pthread_create(&tid_irecv_thread,NULL,client_enc_thread,NULL) < 0){  //创建专门加密信息的线程
        syslog(LOG_DEBUG,"Failed to create client_enc_thread:%s",strerror(errno));
        return -1;
    }
    if(pthread_create(&tid_irecv_thread,NULL,client_snd_thread,NULL) < 0){  //创建专门发送信息的线程
        syslog(LOG_DEBUG,"Failed to create client_snd_thread:%s",strerror(errno));
        return -1;
    }
    if(mmpl_create(&g_client_mmpl) == -1){
        syslog(LOG_DEBUG,"Failed to create the memory pool of client_info_struct:%s",strerror(errno));
        return -1;
    }
    if(mmpl_create(&g_trdata_mmpl) == -1){
        syslog(LOG_DEBUG,"Failed to create the memory pool of transmit_data:%s",strerror(errno));
        return -1;
    }
    /*创建线程池*/
    if((g_tdpl_req = tdpl_create(CLIENT_TDPL_REQTHREAD_NUM,CLIENT_TDPL_MAX_REQQ)) == NULL){
        syslog(LOG_DEBUG,"Failed to create thread pool of request:%s",strerror(errno));
        return -1;
    }
    if((g_tdpl_reqack = tdpl_create(CLIENT_TDPL_REQTHREAD_NUM,CLIENT_TDPL_MAX_REQQ)) == NULL){
        syslog(LOG_DEBUG,"Failed to create thread pool of request_ack:%s",strerror(errno));
        return -1;
    }

    return 1;
}

/* 函数名: int client_mysql_connect()
 * 功能: 连接数据库
 * 参数:
 * 返回值:
 */
int client_mysql_connect(MYSQL *p_mysql){
    if(mysql_init(p_mysql) == NULL){   //初始化mysql
        //初始化失败
        syslog(LOG_DEBUG,"init client mysql failed:%s",mysql_error(p_mysql));
        return -1;
    }
    if(NULL == mysql_real_connect(p_mysql, //链接数据库
                "localhost",
                "root",
                "snivwkbsk123",
                "muma",
                0,
                NULL,
                0
                )){
        //链接失败
        syslog(LOG_DEBUG,"connect to mysql failed:%s",mysql_error(p_mysql));
        return -2;
    }
    syslog(LOG_DEBUG,"connect to mysql successfully");
    
}
/* 函数名: int client_msq_init()
 * 功能: 消息队列初始化,获得消息队列ID
 * 参数:
 * 返回值: -1,
 *          1,
 */
int client_msq_init(){
    key_t key;
    if((key = ftok("./msqf_dec",1)) == -1){
        return -1;
    }
    if((g_msqid_dec = msgget(key,IPC_CREAT)) == -1){
        return -1;
    }
    return 1;
}
/* 函数名: int client_show_users()
 * 功能: 展示已经注册的用户
 * 参数:
 * 返回值:
 */
int client_users_show(MYSQL *p_client_mysql){
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;
    char *query_str = NULL;
    int rows,fields;
    int err_ret;
    int i;

    syslog(LOG_DEBUG,"Showing all users");
    query_str = "select * from client_info";
    err_ret = mysql_real_query(p_client_mysql,query_str,strlen(query_str));
    if(err_ret != 0){
        syslog(LOG_DEBUG,"error:client_users_show()-->mysql_real_query():%s",mysql_error(p_client_mysql));
        return -1;
    }
    res = mysql_store_result(p_client_mysql);
    if(res == NULL){
        syslog(LOG_DEBUG,"error:client_users_show()-->mysql_store_result():%s",mysql_error(p_client_mysql));
        return -1;
    }
    rows = mysql_num_rows(res);
    syslog(LOG_DEBUG,"The total rows is:%d",rows);
    fields = mysql_num_fields(res);
    syslog(LOG_DEBUG,"The total fields is:%d",rows);
    while((row = mysql_fetch_row(res)) != NULL){
        for(i = 0;i < fields;i++){
            syslog(LOG_DEBUG,"%s",row[i]);
        }
    }
    return 1;
}


/* 函数名: int client_is_recv_ready(struct client_info *p_c_i)
 * 功能: 查看客户端是否可以接收数据
 * 参数: struct client_info *p_c_i,客户端信息结构体的指针
 * 返回值: 0,还没做好接收数据的准备，即不能够接收数据
 *         1,可以接收数据
 */
int client_is_recv_ready(struct client_info *p_c_i){
    return p_c_i->recv_ready;
}


/* 函数名: int client_set_recv_ready(struct client_info *p_c_i,int new_ready_stat)
 * 功能: 设置客户端是否可以做好接收数据的准备,当从没做好准备设置到做好准备的时候
 *       ，设置已经接收到的大小为0,!!!!!注意!!!!!对收到的信息做完相关的动作之后，
 *       一定要设置准备好接收数据，不然客户端不会再接收到数据的了
 * 参数: struct client_info *p_c_i,客户端信息结构体指针
 *       int new_ready_stat,新的准备状态
 * 返回值: -1,设置有误
 *          1,设置成功
 */
int client_set_recv_ready(struct client_info *p_c_i,int new_ready_stat){
    if(new_ready_stat != 0 && new_ready_stat != 1 && p_c_i->recv_ready == new_ready_stat){
        return -1;
    }
    p_c_i->recv_ready = new_ready_stat;
    if(new_ready_stat == 1){  //做好接收数据的准备
        p_c_i->recv_size = 0;
    }
    return 1;
}

/* 函数名: struct client_info* client_get_ci(int client_id)
 * 功能: 根据客户端的ID，获取到客户端信息结构体的指针，该函数必须要与client_rele
 *       ase_struct()成对使用。!!!!!注意!!!!!需要访问客户端信息结构体的时候，一定
 *       在访问前调用该函数。
 * 参数: int client_id,客户端的ID
 * 返回值: NULL,内存里不存在客户端,也即客户端没有登录
 *         !=0,客户端信息结构体的指针
 */
struct client_info* client_get_ci(int client_id){
    struct client_info *p_c_i;
    sem_wait(&g_client_rb_mutex);
    p_c_i = __client_search(client_id);
    if(p_c_i != NULL){
        sem_wait(&(p_c_i->queote_cnt_mutex));
        if(p_c_i->queote_cnt++ == 0){
            sem_wait(&(p_c_i->del_enable)); //拿取删除使能锁,不让别的线程删除该信息结构体
        }
        sem_post(&(p_c_i->queote_cnt_mutex));
    }
    sem_post(&g_client_rb_mutex);
    return p_c_i;
}

/* 函数名: int client_rls_ci(struct client_info *p_c_i)
 * 功能: 释放所拿到的客户端信息结构体,与client_get_ci()成对使用,不然该客户端信息
 *       结构体无法被删除掉
 * 参数: struct client_info *p_c_i,所需释放的客户端信息结构体的指针
 * 返回值: -1,
 *          1,
 */
int client_rls_ci(struct client_info *p_c_i){
    if(p_c_i == NULL){
        return -1;
    }
    sem_wait(&p_c_i->queote_cnt_mutex);
    if(--p_c_i->queote_cnt == 0){
        sem_post(&p_c_i->del_enable); //释放删除锁，允许其他线程删除此信息结构体
    }
    sem_post(&p_c_i->queote_cnt_mutex);
    return 1;
}

/* 函数名: int client_get_usrinfo(MYSQL *p_client_mysql,char *usr_name)
 * 功能: 根据用户的名称获得用户的信息
 * 参数: MYSQL,指向数据库结构体的指针
 *       char *usr_name,需要查找用户的信息的名称
 *       struct usr_info *p_u_i,指向用户信息结构体的指针，用来保存查询到的用户信息
 * 返回值: -1,获取用户信息失败
 *          1,获取用户信息成功
 */
int client_get_usrinfo(MYSQL *p_client_mysql,char *usr_name,struct usr_info *p_u_i){
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;
    char query_str[MAX_QUERY_STR_LENGTH];
    int err_ret;

    sprintf(query_str,"select * from client_info where name=\"%s\"",usr_name);
    err_ret = mysql_real_query(p_client_mysql,query_str,strlen(query_str));
    if(err_ret != 0){
        syslog(LOG_DEBUG,"error:client_get_usrinfo()-->mysql_real_query():%s",mysql_error(p_client_mysql));
        return -1;
    }
    res = mysql_store_result(p_client_mysql);
    if(res == NULL){
        syslog(LOG_DEBUG,"error:client_get_usrinfo()-->mysql_store_result():%s",mysql_error(p_client_mysql));
        return -1;
    }
    row = mysql_fetch_row(res);
    if(row == NULL){
        syslog(LOG_DEBUG,"user name %s not found",usr_name);
        return -1;
    }
    p_u_i->id = atoi(row[0]);
    strcpy(p_u_i->name,row[1]);
    strcpy(p_u_i->passwd,row[2]);
    p_u_i->lv = atoi(row[3]);

    return 1;
}

/* 函数名: int client_get_passwdandkey(char *src_str,char *passwd,char *pub_key)
 * 功能: 获取密码和公钥,str_str格式必须如下:passwd+pub_key
 * 参数: 
 * 返回值: -1,
 *          1,
 */
int client_get_passwdandkey(char *src_str,char *passwd,char *pub_key){
    int i = 0;
    while(src_str[i++] != '+'); 
    strncpy(passwd,src_str,i - 1);
    strcpy(pub_key,src_str + i);
    return 1;
}
/* 函数: int client_verify(int sockfd,int ip,struct usr_info)
 * 功能: 验证客户端,验证成功后，创建客户端
 * 说明: 验证步骤如下:
 *       1.接收需要登录的客户端的用户名，并且检验是否已经注册，如没有注册，则告知
 *         客户端用户名不存在,而不发送服务端的公钥
 *       2.如果用户名存在，则产生秘钥，而且与客户端交换公钥，具体步骤是：
 *          1)先给客户端发送服务端的公钥
 *          2)等待接收经过服务端公钥加密的客户端认证内容，认证内容包括:用户的密码
 *            +客户端公钥，密码是用来验证的，公钥用来在验证成功后给客户端加密发送
 *            aes秘钥。
 *       3.验证密码
 *       4.若验证成功，则告知成功，否则告知失败，并且说明原因
 * 参数: int sockfd,客户端的控制套接字
 *       int ip,客户端的ip
 *       struct usr_info,返回的验证信息
 * 返回值: -1,通信出问题
 *         -2,不存在账户(名称)
 *         -3,账户密码错误
 *         -4,重复登录
 *          1,验证成功
 */
int client_verify(int sockfd,struct in_addr ip,struct usr_info *p_usr_i){
    char passwd[MAX_RECV_STR_LENGTH] = {0};
    char recv_str[MAX_RECV_STR_LENGTH];
    char local_pub_key[4096];
    char plain[MAX_RECV_STR_LENGTH];
    int result_value[2] = {0};
    int size;

    syslog(LOG_DEBUG,"ip:%s attempt to login",inet_ntoa(ip));
    if(com_rcv_data(sockfd,recv_str,MAX_RECV_STR_LENGTH) == -1){
        syslog(LOG_DEBUG,"Error:failed to recv user's name,%s",strerror(errno));
        return -1;
    }
    syslog(LOG_DEBUG,"login name:%s",recv_str);
    if(client_get_usrinfo(&g_client_mysql,recv_str,p_usr_i) == -1){
        syslog(LOG_DEBUG,"Client login failed,user name:%s does not exist",recv_str);
        result_value[0] = 29;
        if(com_snd_data(sockfd,(char *)&result_value,sizeof(int),0) == 1)return -1;
        return -2;
    }
    syslog(LOG_DEBUG,"Genarating RSA key and share the pub key");
    rsa_gen_keys(RSA_KEY_LENGTH,local_pub_key,p_usr_i->priv_key);  //生成密钥对
    if((size = com_snd_data(sockfd,local_pub_key,strlen(local_pub_key) + 1,0)) == -1){
        return -1;
    }
    if(com_rcv_data(sockfd,recv_str,MAX_RECV_STR_LENGTH) == -1){
        return -1;
    }
    rsa_priv_decrypt(p_usr_i->priv_key,recv_str,plain);  //解密客户端发来的认证内容
    client_get_passwdandkey(plain,passwd,p_usr_i->pub_key);
    syslog(LOG_DEBUG,"Checking the passwd of %s,login passwd:%s",p_usr_i->name,passwd);
    printf("Client pub_key:\n%s\n",p_usr_i->pub_key);
    if(strcmp(passwd,p_usr_i->passwd) != 0){
        syslog(LOG_DEBUG,"Client login failed:incorrect passwd");
        result_value[0] = 30;
        if(com_snd_data(sockfd,(char *)result_value,sizeof(int),0) == -1)return -1;
        return -3;
    }
    if(client_search(p_usr_i->id) != NULL){
        syslog(LOG_DEBUG,"Client login failed:multiple login");
        result_value[0] = 31;
        if(com_snd_data(sockfd,(char *)result_value,sizeof(int),0) == -1)return -1;
        return -4;
    }
    syslog(LOG_DEBUG,"Client login successfully");
    result_value[0] = 1;
    result_value[1] = p_usr_i->id;
    if((size = com_snd_data(sockfd,(char *)result_value,2*sizeof(int),0)) <= 0)return -1;
    return 1;
}

/* 函数名: struct snd_queue* client_desndq(struct client_info *p_c_i)
 * 功能: 从客户端的发送队列头取出一个发送请求
 * 参数: struct client_info *p_c_i,客户端的信息结构体
 * 返回值: NULL,队列为空，或者出现某些错误
 *       !=NULL,队列节点的指针
 */
struct snd_queue* client_desndq(struct client_info *p_c_i){
    struct snd_queue *sndq_node;
    sem_wait(&p_c_i->sndq_mutex);
    if(p_c_i->sndq_head.next == &p_c_i->sndq_head){ //如果队列是空的
        sndq_node = NULL;
    }else{
        /*搞不懂vim的YouCompleteMe为嘛报list_entry出错，它看不懂宏？*/
        sndq_node = list_entry(p_c_i->sndq_head.next,struct snd_queue,queue);
        list_del(p_c_i->sndq_head.next);
        p_c_i->sndq_l--;
    }
    sem_post(&p_c_i->sndq_mutex);
    return sndq_node;
}

/* 函数名: int client_ensndq_h(struct client_info *p_c_i,struct snd_queue *p_sndq_node)
 * 功能: 把发送请求插入到队头
 * 参数: struct client_info *p_c_i,客户端的信息结构体
 *       struct snd_queue *sndq_node,发送请求的结构体的指针,即是队列节点的指针
 * 返回值: -1,
 *          1,
 */
int client_ensndq_h(struct client_info *p_c_i,struct snd_queue *p_sndq_node){
    sem_wait(&p_c_i->sndq_mutex);
    my_list_add(&p_sndq_node->queue,&p_c_i->sndq_head);
    p_c_i->sndq_l++;
    sem_post(&p_c_i->sndq_mutex);
    return 1;
}


/* 函数名: int client_ensndq_t(struct client_info *p_c_i,struct snd_queue *p_sndq_node)
 * 功能: 把发送请求插入到队尾
 * 参数: struct client_info *p_c_i,客户端的信息结构体
 *       struct snd_queue *p_sndq_node,发送请求的结构体的指针,即是队列节点的指针
 * 返回值: -1,
 *          1,
 */
int client_ensndq_t(struct client_info *p_c_i,struct snd_queue *p_sndq_node){
    struct epoll_event event;
    sem_wait(&p_c_i->sndq_mutex);  //互斥访问链表
    if(p_c_i->sndq_head.next == p_c_i->sndq_head.prev){
        /*如果插入队列之前，队列是空的，则把该sockfd发送事件放入epoll。其实在发
         * 送线程里，把发送队列里的请求都处理完了之后，就会把客户端对应的sockfd
         * 移出epoll。发送队列为空的时候，发送线程有可能正在发送最后一个报文，
         * 也即epoll中也有可能存在该sockfd的。
         */
        event.data.u32 = p_c_i->id;  
        event.events = EPOLLOUT;  //设置读写提醒，水平触发模式
        if(epoll_ctl(g_socket_snd_epoll_fd,
                    EPOLL_CTL_ADD,
                    p_c_i->sockfd,
                    &event) == -1){  //把客户端的socket加入epoll
            if(errno != EEXIST){
                syslog(LOG_DEBUG,"Failed to add sockfd to snd_epoll:%s",strerror(errno));
                list_del(&p_sndq_node->queue);
                sem_post(&p_c_i->sndq_mutex);
                return -1;
            }
        }
        //syslog(LOG_DEBUG,"Add the sockfd to epoll");
    }
    list_add_tail(&p_sndq_node->queue,&p_c_i->sndq_head); //加到发送队列的队尾
    p_c_i->sndq_l++;
    sem_post(&p_c_i->sndq_mutex);
    return 1;
}


/* 函数名: int client_snd_ready(struct client_info *p_c_i,struct snd_queue *p_sndq_node,int enc_flag)
 * 功能: 把将要发送的数据插入发送队列,做好发送的准备
 * 参数: struct client_info *p_c_i,数据发送目的客户端的信息结构体指针
 *       struct snd_queue *p_sndq_node,需要插入的数据发送请求节点
 *       int enc_flag,加密标志，如果为1则是加密发送，如果为0是明文发送
 * 返回值: -1,
 *          1,
 */
int client_snd_ready(struct client_info *p_c_i,unsigned char *snd_data,unsigned short data_size,unsigned int enc_flag){
    struct snd_queue *p_sndq_node;
    unsigned char head_snd[4]; //真正发送的head数据
    char syc_char = '\r';  //同步字符
    int head_snd_size = 0;
    int i;
    union{
        unsigned short u16;
        unsigned char u8[2];

    }head;
    head.u16 = 0;
    head.u16 = (data_size << 1) | enc_flag;
    for(i = 0;i < 2;i++){  //透明传输head
        if(head.u8[i] == syc_char || head.u8[i] == 0x10){
            head_snd[head_snd_size++] = 0x10;
        }
        head_snd[head_snd_size++] = head.u8[i];
    }
    p_sndq_node = mmpl_getmem(g_trdata_mmpl,sizeof(struct snd_queue));
    if(p_sndq_node == NULL){
        syslog(LOG_DEBUG,"Error:client_snd_ready()-->mmpl_getmem():%s",strerror(errno));
        return -1;
    }
    /*data_size + 4 + CLIENT_SYC_CHAR_NUM是报文大小加上head最大大小和同步字符个数*/
    p_sndq_node->snd_data = mmpl_getmem(g_trdata_mmpl,data_size + 4 + CLIENT_SYC_CHAR_NUM);  
    if(p_sndq_node->snd_data == NULL){
        syslog(LOG_DEBUG,"Error:client_snd_ready()-->mmpl_getmem():%s",strerror(errno));
        mmpl_rlsmem(g_trdata_mmpl,p_sndq_node);
        return -1;
    }
    memset(p_sndq_node->snd_data,syc_char,CLIENT_SYC_CHAR_NUM); //5个同步字符
    memcpy(p_sndq_node->snd_data + CLIENT_SYC_CHAR_NUM,head_snd,head_snd_size);//head
    memcpy(p_sndq_node->snd_data + CLIENT_SYC_CHAR_NUM + head_snd_size,snd_data,data_size);
    p_sndq_node->snd_size = data_size + head_snd_size + CLIENT_SYC_CHAR_NUM;
    p_sndq_node->snd_start = 0;
    /*把发送请求插入到发送请求队列队尾*/
    if(client_ensndq_t(p_c_i,p_sndq_node) == -1){
        syslog(LOG_DEBUG,"Failed to add the p_sndq_node to snd_queue");
        mmpl_rlsmem(g_trdata_mmpl,p_sndq_node);
        mmpl_rlsmem(g_trdata_mmpl,p_sndq_node->snd_data);
        return -1;
    }
    return 1;
}


/* 函数名: int client_snd_msg(struct cm_msg *msg)
 * 功能: 给客户端发送数据。发送数据的过程是这样的：
 *       1.通过消息队列把要发送的数据报送到加密线程
 *       2.加密线程对数据报文进行加密
 *       3.把加密好的报文插入到客户端相应的发送队列
 *       4.在发送线程里，从客户端的发送队列取出要发送的加密报文进行发送
 * 参数: unsigned char *data,需要发送的数据
 *       int enc_flag,加密标志，1加密发送，0明文发送
 * 返回值: -1,出现了一些错误
 *         -2,发送队列太长
 *          1,
 */
int client_snd_msg(struct cm_msg *msg,int enc_flag){
    struct client_info *pci;
    unsigned short check_sum;
    int i;
    int msg_size;
    int ret_val;
    pci = client_get_ci(msg->dest_id);
    if(pci == NULL){
        syslog(LOG_DEBUG,"Error:client_snd_msg():client(id:%d) not found",msg->client_id);
        return -1;
    }
    /*发送队列队长超过了CLIENT_MAX_SNDQ_L - 1,则告知繁忙*/
    if(pci->sndq_l >= CLIENT_MAX_SNDQ_L - 1){
        client_rls_ci(pci);
        return -2;
    }
    /*计算校验和*/
    msg_size = CLIENT_MSG_SIZE(msg->data_size);
    check_sum = 0;
    msg->check_sum = 0;
    for(i = 0;i < msg_size;i++){
        check_sum += ((unsigned char*)msg)[i];
    }
    msg->check_sum = ~check_sum;
    if(enc_flag == 0){
        ret_val = client_snd_ready(pci,(unsigned char*)msg,CLIENT_MSG_SIZE(msg->data_size),0);
        client_rls_ci(pci);
        return ret_val;
    }else if(enc_flag == 1){
        client_rls_ci(pci);
        return com_msgq_snd(g_msgq_enc,msg,CLIENT_MSG_SIZE(msg->data_size));
    }else{
        syslog(LOG_DEBUG,"Error:client_snd_msg():invalid enc_flag");
        client_rls_ci(pci);
        return -1;
    }
}


/* 函数名: void *client_enc_thread()
 * 功能: 通信报文加密线程，从加密线程管道中取出客户端ID，然后把将要发送给客户端的
 *       报文进行加密，加密完了之后，释放客户端的报文加密锁，而且把已经加密好的报
 *       文保存在客户端的发送缓存里
 * 参数:
 * 返回值:
 */
void *client_enc_thread(){
    struct client_info *pci;
    struct cm_msg msg;
    unsigned char cipher[CLIENT_MAX_MSG_DATA_SIZE];
    int ret_val;

    while(1){
        //memset(&msg,0,sizeof(struct cm_msg));
        ret_val = com_msgq_recv(g_msgq_enc,&msg);
        if(ret_val == -1){
            syslog(LOG_DEBUG,"Error:client_enc_thread()-->com_pipe_rd_data:%s",strerror(errno));
            sleep(1);
            continue;
        }
        pci = client_get_ci(msg.dest_id);
        if(pci == NULL){
            syslog(LOG_DEBUG,"ERROR:client_enc_thread():client(id:%d) not found!",msg.client_id);
            continue;
        }
        aes_cbc_enc(&pci->aes_enc_key,(unsigned char*)&msg,cipher,CLIENT_MSG_SIZE(msg.data_size));
        client_snd_ready(pci,cipher,SIZE_ALIGN_16B(CLIENT_MSG_SIZE(msg.data_size)),1);  //把发送请求插入到客户端发送队列
        client_rls_ci(pci);
    }
}



/* 函数名: void *client_snd_test_thread()
 * 功能: 测试发送线程的函数，调试用
 * 参数:
 * 返回值:
 */
void *client_snd_test_thread(){
    struct epoll_event events[CLIENT_MAX_EPOLL_EVENTS];
    struct client_info *p_c_i;
    struct snd_queue *p_sndq_node;
    struct cm_msg msg_test;
    unsigned char snd_data[CLIENT_MAX_MSG_DATA_SIZE];
    int c_id;
    int i,n;
    int rcv_size;

    while(1){
        n = epoll_wait(g_socket_snd_epoll_fd,events,CLIENT_MAX_EPOLL_EVENTS,-1);
        if(n == -1){
            syslog(LOG_DEBUG,"ERROR:client_snd_test_thread()-->epoll_wait():%s",strerror(errno));
            sleep(1);
            continue;
        }
        for(i = 0;i < n;i++){
            c_id = events[i].data.u32;
            p_c_i = client_get_ci(c_id);
            if(p_c_i == NULL){
                syslog(LOG_DEBUG,"Error:client(id:%d) not found!",c_id);
                continue;
            }
            if(!(events[i].events & EPOLLOUT) ||
                (events[i].events & EPOLLHUP) ||
                (events[i].events & EPOLLERR)){
                if(epoll_ctl(g_socket_snd_epoll_fd,EPOLL_CTL_DEL,p_c_i->sockfd,NULL) == -1){
                    syslog(LOG_DEBUG,"Failed to remove sockfd from epoll:%s",strerror(errno));
                }
                syslog(LOG_DEBUG,"It is not EPOLLOUT event,remove sockfd from snd_epoll");
                client_rls_ci(p_c_i);
                continue;
            }
            p_sndq_node = client_desndq(p_c_i);
            if(p_sndq_node == NULL){
                if(epoll_ctl(g_socket_snd_epoll_fd,EPOLL_CTL_DEL,p_c_i->sockfd,NULL) == -1){
                    syslog(LOG_DEBUG,"Failed to remove sockfd from epoll:%s",strerror(errno));
                }
                client_rls_ci(p_c_i);
                continue;
            }
            memset(snd_data,0,CLIENT_MAX_MSG_DATA_SIZE);
            if((rcv_size = com_remove_ctlsymbols(p_sndq_node->snd_data,snd_data,CLIENT_MAX_MSG_DATA_SIZE)) == -1){
                syslog(LOG_DEBUG,"Error in com_remove_ctlsymbols():buf size is overflow");
                client_rls_ci(p_c_i);
                mmpl_rlsmem(g_trdata_mmpl,p_sndq_node->snd_data);
                mmpl_rlsmem(g_trdata_mmpl,p_sndq_node);
                continue;
            }
            aes_cbc_dec(&p_c_i->aes_dec_key,
                    snd_data,
                    (unsigned char *)&msg_test,
                    SIZE_ALIGN_16B(rcv_size));
            //syslog(LOG_DEBUG,"Test msg:%s",msg_test.data);
            mmpl_rlsmem(g_trdata_mmpl,p_sndq_node->snd_data);
            mmpl_rlsmem(g_trdata_mmpl,p_sndq_node);
            client_rls_ci(p_c_i);
        }
    }
}



/* 函数名: void *client_discnct_monitor_t()
 * 功能: 断开链接监听线程
 * 参数:
 * 返回值:
 */
void *client_discnct_monitor_t(){
    struct epoll_event events[CLIENT_MAX_EPOLL_EVENTS];
    int c_id;
    int n,i;
    while(1){
        n = epoll_wait(g_socket_disc_epoll_fd,events,CLIENT_MAX_EPOLL_EVENTS,-1);
        if(n == -1){
            syslog(LOG_DEBUG,"ERROR:client_snd_thread()-->epoll_wait():%s",strerror(errno));
            sleep(1);
            continue;
        }
        for(i = 0;i < n;i++){
            c_id = events[i].data.u32;
            if( (events[i].events & EPOLLRDHUP) ||
                (events[i].events & EPOLLHUP) ||
                (events[i].events & EPOLLERR)){
                client_remove(c_id);
                continue;
            }
        }
    }
    
}


/* 函数名: void *client_snd_thread()
 * 功能: 发送数据线程
 * 参数:
 * 返回值:
 */
void *client_snd_thread(){
    struct epoll_event events[CLIENT_MAX_EPOLL_EVENTS];
    struct client_info *p_c_i;
    struct snd_queue *p_sndq_node;
    int c_id;
    int i,n;
    int send_size;

    while(1){
        n = epoll_wait(g_socket_snd_epoll_fd,events,CLIENT_MAX_EPOLL_EVENTS,-1);
        if(n == -1){
            syslog(LOG_DEBUG,"ERROR:client_snd_thread()-->epoll_wait():%s",strerror(errno));
            sleep(1);
            continue;
        }
        for(i = 0;i < n;i++){
            c_id = events[i].data.u32;  //获得客户端ID
            p_c_i = client_get_ci(c_id); //根据客户端ID获得客户端信息结构体的指针
            if(p_c_i == NULL){
                syslog(LOG_DEBUG,"Error:client id(%d) not found!",c_id);
                client_rls_ci(p_c_i);
                continue;
            }
            if( (events[i].events & EPOLLHUP) ||
                (events[i].events & EPOLLERR)){
                client_rls_ci(p_c_i);
                //client_remove(c_id);
                continue;
            }
            if(!(events[i].events & EPOLLOUT)){
                client_rls_ci(p_c_i);
                continue;
            }
            p_sndq_node = client_desndq(p_c_i);
            /*如果发送请求队列是空的，则把sockfd移出epoll*/
            if(p_sndq_node == NULL){  
                if(epoll_ctl(g_socket_snd_epoll_fd,EPOLL_CTL_DEL,p_c_i->sockfd,NULL) == -1){
                    syslog(LOG_DEBUG,"Failed to remove sockfd from snd_epoll:%s",strerror(errno));
                }
                client_rls_ci(p_c_i);
                continue;
            }
            /*非阻塞方式发送数据*/
            if((send_size = send(p_c_i->sockfd,
                                p_sndq_node->snd_data + p_sndq_node->snd_start,
                                p_sndq_node->snd_size,
                                MSG_DONTWAIT)) == -1){
                syslog(LOG_DEBUG,"Error:client_snd_thread()-->send():%s",strerror(errno));
                mmpl_rlsmem(g_trdata_mmpl,p_sndq_node->snd_data);
                mmpl_rlsmem(g_trdata_mmpl,p_sndq_node);
                client_rls_ci(p_c_i);
                break;
            }
            if((errno != EWOULDBLOCK) && (p_sndq_node->snd_size != send_size)){
                syslog(LOG_DEBUG,"send_size:%d  p_sndq_node->snd_size:%d",send_size,p_sndq_node->snd_size);
            }
            //syslog(LOG_DEBUG,"send_size:%d  p_sndq_node->snd_size:%d",send_size,p_sndq_node->snd_size);
            p_sndq_node->snd_size -= send_size;
            if((int)p_sndq_node->snd_size > 0){
            /*如果还没有发送完,把剩下数据的请求插入到队头*/
                p_sndq_node->snd_start += send_size;
                client_ensndq_h(p_c_i,p_sndq_node);
                syslog(LOG_DEBUG,"debug test re_snd,start:%d total size:%d",p_sndq_node->snd_start,p_sndq_node->snd_size);
            }else{
            /*如果发送完了，则释放掉p_sndq_node所占用的存储空间，还给内存池*/
                mmpl_rlsmem(g_trdata_mmpl,p_sndq_node->snd_data);
                mmpl_rlsmem(g_trdata_mmpl,p_sndq_node);
            }
            client_rls_ci(p_c_i);
        }
    }
    
}


/* 函数名: int client_recv_msg(struct client_info *pci)
 * 功能: 负责接收报文，在线程client_recv_thread()调用
 * 参数:
 * 返回值:
 */
int client_recv_msg(struct client_info *pci){
    int recv_size;
    char recv_c;
    union{
        unsigned short u16;
        char u8[2];
    }head;
    int recv_h_i = 0;
    int recv_monitor_cnt = 0;

    if(pci->msg_rcv_status == 0){
        /*如果处于报文接收的监听状态*/
        while(1){
            if(recv_monitor_cnt++ == CLIENT_MAX_RECV_MONITOR_CNT){ //监听一定的字符则放弃此次监听
                return 1;
            }
            if(recv(pci->sockfd,&recv_c,1,MSG_DONTWAIT) <= 0){ //非阻塞接收
                if(errno == 11)return 1;  //errno等于11表示没有可接收的了,可以直接退出循环
                syslog(LOG_DEBUG,"client_recv_msg:failed to recv char:%s",strerror(errno));
                return -1;
            }
            if(recv_c == '\r'){
                if(++pci->msg_rcv_ready_cnt == 3){ 
                    /*监听到有报文发送过来*/
                    pci->msg_rcv_ready_cnt = 0;
                    while(1){
                        recv(pci->sockfd,&recv_c,1,0);
                        if(recv_c == '\r'){
                            continue;
                        }else if(recv_c == 0x10){ //控制字符
                            recv(pci->sockfd,&head.u8[recv_h_i++],1,0);
                        }else{
                            head.u8[recv_h_i++] = recv_c;
                        }
                        if(recv_h_i == 2)break;
                    }
                    /*创建接收结构体*/
                    pci->recv_size = head.u16 >> 1;
                    if(pci->recv_size == 0)return 1; //大小是0则放弃接收
                    pci->recv_buf_offset = 0;
                    pci->p_rcv_s = mmpl_getmem(g_trdata_mmpl,pci->recv_size + sizeof(struct cm_rcv_s));
                    if(pci->p_rcv_s == NULL){
                        syslog(LOG_DEBUG,"Failed to get memory from g_trdata_mmpl for cm_rcv_s");
                        return -1;
                    }
                    pci->p_rcv_s->msg_size = pci->recv_size;
                    pci->p_rcv_s->is_enc = 1 & head.u16; //是否加密
                    pci->p_rcv_s->buf = (void *)pci->p_rcv_s + sizeof(struct cm_rcv_s);
                    pci->msg_rcv_status = 1; //设置为报文接收状态
                    break;
                }
            }else{
                //syslog(LOG_DEBUG,"gaga");
                pci->msg_rcv_ready_cnt = 0;
            }
        }
    }else{
        /*如果处于报文接收状态*/
        recv_size = recv(pci->sockfd,
                         pci->p_rcv_s->buf + pci->recv_buf_offset,
                         pci->recv_size,
                         MSG_DONTWAIT); //非阻塞接收
        if(recv_size == -1 && errno != 11){
            syslog(LOG_DEBUG,"client_recv_msg():failed to recv msg:%s",strerror(errno));
            syslog(LOG_DEBUG,"client_id:%d",pci->id);
            syslog(LOG_DEBUG,"Rcv buf addr:0x%x",(unsigned int)pci->p_rcv_s->buf);
            syslog(LOG_DEBUG,"Offset:%d",(unsigned int)pci->recv_buf_offset);
            return -1;
        }
        pci->recv_size -= recv_size;
        pci->recv_buf_offset += recv_size;
        if(pci->recv_size <= 0){  //这里一定要小于等于0，因为可能会接收过头而溢出的！
            pci->msg_rcv_status = 0; //退出报文接收状态，置为监听状态
            client_set_recv_ready(pci,0);                      
            if(com_pipe_wr_data(g_dec_thrd_pipfd[1],(char*)&pci->id,sizeof(pci->id)) == -1){
                syslog(LOG_DEBUG,"ERROR:client_recv_thread()-->com_pipe_wr_data():%s",strerror(errno));
            }
        }
    }
    return 1;
}



/* 函数名: void *client_recv_thread()
 * 功能: 接收数据线程  啊！突然感觉这函数好长,不是很想看到它  总有一天我会改掉它的
 * 参数: 
 * 返回值:
 */
void *client_recv_thread(){
    int n,i;
    int c_id;
    struct epoll_event events[CLIENT_MAX_EPOLL_EVENTS];
    struct client_info *p_c_i;
    syslog(LOG_DEBUG,"Start client_recv_thread");
    while(1){
        n = epoll_wait(g_socket_epoll_fd,events,CLIENT_MAX_EPOLL_EVENTS,-1);
        if(n == -1){
            syslog(LOG_DEBUG,"ERROR:client_recv_thread()-->epoll_wait():%s",strerror(errno));
            sleep(1);
            continue;
        }
        for(i = 0;i < n;i++){
            //获取到可读的sockfd对应的客户端信息结构体的指针
            c_id = events[i].data.u32;  //获得客户端ID
            p_c_i = client_get_ci(c_id);  //根据客户端ID获取到信息结构体的指针
            if(p_c_i == NULL){
                syslog(LOG_DEBUG,"ERROR:Client_recv_thread()-->client_get_ci():struct_info of client(id:%d) not found",c_id);
                continue;
            }
            if( (events[i].events & EPOLLHUP) ||
                (events[i].events & EPOLLERR)){
                client_rls_ci(p_c_i);
                client_remove(c_id);
                continue;
            }
            if(!(events[i].events & EPOLLIN)){
                if(epoll_ctl(g_socket_epoll_fd,EPOLL_CTL_DEL,p_c_i->sockfd,NULL) == -1){
                    syslog(LOG_DEBUG,"Failed to remove sockfd from epoll:%s",strerror(errno));
                }
                client_rls_ci(p_c_i);
                continue;
                
            }
            if(client_is_recv_ready(p_c_i) == 0){  //客户端还未准备好接收数据
                client_rls_ci(p_c_i);
                continue;
            }
            client_recv_msg(p_c_i);  //监听或者接收报文(非阻塞)
            client_rls_ci(p_c_i); //释放信息结构体
        }
    }
}

/* 函数名: int client_parse_info(char *info)
 * 功能: 解析接收到的消息,并且执行相关的命令
 * 参数: char *info原信息
 * 返回值: -1,出现错误
 *         1,调用成功
 */
int client_parse_do(char *info_src,struct client_info *p_c_i){
    char cmd[32];
    char param[MAX_RECV_STR_LENGTH];
    int i = 0;

    while(info_src[i++] != ':');
    strncpy(cmd,info_src,i - 1);
    strcpy(param,info_src + i);
    //if(strcmp(cmd,"cnt") == 0){
    //    p_c_i->msg_cnt = atoi(param);
    //}
    //syslog(LOG_DEBUG,"msg_cnt:%d",p_c_i->msg_cnt);

    return 1;
}
/* 函数名: void *client_dec_parse_thread(void *arg)
 * 功能: 客户端信息的解密和解析的线程
 * 参数:
 * 返回值:
 */
void *client_dec_parse_thread(){
    int ret_value;
    int c_id;
    struct client_info *pci,*dest_pci;
    struct cm_msg msg;
    while(1){
        c_id = 0;
        ret_value = com_pipe_rd_data(g_dec_thrd_pipfd[0],(char *)&c_id,sizeof(int));
        if(ret_value == -1){
            syslog(LOG_DEBUG,"ERROR:client_dec_parse()-->com_pipe_rd_data:%s",strerror(errno));
            sleep(1);
            continue;
        }
        pci = client_get_ci(c_id);  //msg_type保存着客户端的ID 
        if(pci == NULL){
            syslog(LOG_DEBUG,"Error:client_dec_parse()-->client_get_ci():the struct of client(id:%d) not found",c_id);
            continue;
        }
        //AES解密
        memset(&msg,0,sizeof(struct cm_msg));
        aes_cbc_dec(&pci->aes_dec_key,(unsigned char*)pci->p_rcv_s->buf,(unsigned char*)&msg,pci->p_rcv_s->msg_size);
        mmpl_rlsmem(g_trdata_mmpl,pci->p_rcv_s);
        syslog(LOG_DEBUG,"Msg client_id:%d",msg.client_id);
        syslog(LOG_DEBUG,"Msg data:%s",msg.data);
        syslog(LOG_DEBUG,"Msg type:%d",msg.type);
        syslog(LOG_DEBUG,"DATA size:%d",msg.data_size);
        client_set_recv_ready(pci,1);  //解密好报文之后，可以继续接收信息
        client_rls_ci(pci);
        if((int)msg.client_id != c_id){
            syslog(LOG_DEBUG,"Msg error:client_id:%d,dest_id:%d",msg.client_id,msg.dest_id);
            continue;
        }
        if(msg.type == 4){
            client_wd_resume(c_id);//喂狗
        }else if(msg.type == 2){
            if(msg.dest_id == 0){
                switch(msg.req_er_type){ //非转发请求报文
                    case 1:
                        //msg.msg_cnt = pci->msg_cnt++;
                        strcpy((char *)msg.data,"You have not the permission of knowing that!");
                        msg.data_size = strlen((char *)msg.data) + 1;
                        client_snd_msg(&msg,1);
                        break;
                    case 2:
                        break;
                    case 4:
                        client_remove(msg.client_id);
                        break;
                }
            }else{ //转发请求报文
                dest_pci = client_get_ci(msg.dest_id);  //检查是否存在报文目的客户端
                if(dest_pci == NULL){
                    syslog(LOG_DEBUG,"dest_pci:%d is not exist,failed to transmit the msg",msg.dest_id);
                    continue;
                }
                client_rls_ci(dest_pci);
                syslog(LOG_DEBUG,"Transmit msg");
                client_snd_msg(&msg,1);
            }
        }
        //if((pci->msg_cnt != (int)msg.msg_cnt) || (pci->id != c_id)){  
        //    /*如果报文计数不符,或者报文的ID不符,则回送错误报文*/
        //    syslog(LOG_DEBUG,"Msg cnt error");
        //    msg.client_id = c_id;
        //    msg.type = 1;  
        //    msg.req_er_type = 1; //报文错误类型
        //    msg.data_size = 4;
        //    *(int*)msg.data = pci->msg_cnt;  //应该发送过来的报文计数
        //    client_snd_msg(&msg);
        //    client_rls_ci(pci);
        //    continue;
        //}
        client_rls_ci(pci);
    }
}

/* 函数名: int client_create(int sockfd,struct sockaddr_in);
 * 功能: 创建客户端
 * 参数: int sockfd,请求登录的客户端的套接字
 *       struct sockaddr_in,请求登录的客户端的IP
 * 返回值: -1,
 *          1,
 */
int client_create(int sockfd,struct sockaddr_in addr_in){
    int err_ret;
    pthread_t thread_id;
    struct client_info *p_client_i;
    //创建客户端
    p_client_i = (struct client_info*)mmpl_getmem(g_client_mmpl,sizeof(struct client_info));
    if(p_client_i == NULL){
        syslog(LOG_DEBUG,"Failed to get memory from client_mmpl:%s",strerror(errno));
        return -1;
    }
    memset(p_client_i,0,sizeof(struct client_info));
    p_client_i->sockfd = sockfd;
    p_client_i->ip = addr_in.sin_addr;
    //创建线程（要想提高性能，可以使用线程池的）
    if((err_ret = pthread_create(&thread_id,NULL,client_create_thread,p_client_i) < 0)){
        syslog(LOG_DEBUG,"Failed to create client:%s",strerror(errno));
        mmpl_rlsmem(g_client_mmpl,p_client_i);
    }
    return 1;
}
    



/* 函数: void client_create_thread()
 * 功能: 创建客户端,而且把新建的客户端链入链表,作为线程被调用
 * 参数: 
 * 返回值: -1,
 *          1,
 */
void *client_create_thread(void *p_client_i){
    struct usr_info u_i;
    struct client_info *p_new_client_i;
    struct epoll_event event;
    unsigned char aes_key[CLIENT_AES_KEY_LENGTH];
    int err_ret;
    int i;
    p_new_client_i = (struct client_info*)p_client_i;
    if((err_ret = client_verify(p_new_client_i->sockfd,p_new_client_i->ip,&u_i)) < 0){  //验证客户端
        syslog(LOG_DEBUG,"Error:failed to verify client!");
        shutdown(p_new_client_i->sockfd,2);  //关闭套接字
        mmpl_rlsmem(g_client_mmpl,p_new_client_i);
        return NULL;
    }
    /*复制认证好的信息*/
    p_new_client_i->id = u_i.id;
    p_new_client_i->rb_n.key = u_i.id; //红黑树节点的key值就是客户端ID
    strcpy(p_new_client_i->pub_key,u_i.pub_key);
    strcpy(p_new_client_i->priv_key,u_i.priv_key);
    strcpy(p_new_client_i->name,u_i.name);
    /*生成AESKEY，并且发给客户端*/
    syslog(LOG_DEBUG,"Genarating AES key and share the key"); 
    aes_gen_key(aes_key,CLIENT_AES_KEY_LENGTH);  //随机生成AES秘钥
    /*此处是调试用的代码*/
    printf("以下是aes密钥\n");   
    for(i = 0;i < CLIENT_AES_KEY_LENGTH/8;i++){
        printf("0x%x ",aes_key[i]);
    }
    fflush(stdout);
    /*以上是调试用的代码*/
    AES_set_encrypt_key(aes_key,CLIENT_AES_KEY_LENGTH,&p_new_client_i->aes_enc_key);
    AES_set_decrypt_key(aes_key,CLIENT_AES_KEY_LENGTH,&p_new_client_i->aes_dec_key);
    com_rsa_send_aeskey(p_new_client_i->sockfd,p_new_client_i->pub_key,aes_key,CLIENT_AES_KEY_LENGTH);  //rsa公钥加密发送AESkey
    p_new_client_i->wd_cnt = WD_RESUME_CNT;  //初始化看门狗计数值
    p_new_client_i->queote_cnt = 0; //客户端信息结构体引用次数
    sem_init(&(p_new_client_i->queote_cnt_mutex),0,1);  //初始化引用次数互斥锁
    sem_init(&p_new_client_i->del_enable,0,1);  //初始化结构体删除锁
    sem_init(&p_new_client_i->sndq_mutex,0,1);  //初始化发送队列锁
    INIT_LIST_HEAD(&p_new_client_i->sndq_head);   //初始化发送队列
    /*链入链表*/
    sem_wait(&client_list_mutex);  //互斥访问链表
    my_list_add(&p_new_client_i->list,&client_i_h.list);//把新创建的客户端链入链表
    sem_post(&client_list_mutex);  //互斥访问链表
    sem_wait(&g_client_rb_mutex);  //互斥访问客户端红黑树
    rb_insert(&g_client_rb_t,&p_new_client_i->rb_n); //把新创建的客户端插入红黑树
    sem_post(&g_client_rb_mutex);
    p_new_client_i->recv_ready = 1;  //做好接收准备
    //free(p_new_client_i); //测试用代码，可删
    //return NULL;//测试用代码，可删
    /*配置epoll*/
    event.data.u32 = u_i.id;  
    event.events = EPOLLIN;  //设置读提醒，水平触发模式
    if(epoll_ctl(g_socket_epoll_fd,
                EPOLL_CTL_ADD,
                p_new_client_i->sockfd,
                &event) == -1){  //把客户端的socket加入epoll
        syslog(LOG_DEBUG,"Failed to add sockfd to epoll:%s",strerror(errno));
        return NULL;
    }
    event.data.u32 = u_i.id;  
    event.events = EPOLLRDHUP|EPOLLET;  
    if(epoll_ctl(g_socket_disc_epoll_fd,
                EPOLL_CTL_ADD,
                p_new_client_i->sockfd,
                &event) == -1){  //把客户端的socket加入epoll
        syslog(LOG_DEBUG,"Failed to add sockfd to disconnect epoll:%s",strerror(errno));
        return NULL;
    }
    syslog(LOG_DEBUG,"Client:%s create complete!",p_new_client_i->name);
    return &*p_client_i;
}

/* 函数: client_info* __client_search(int client_id)
 * 功能: 根据客户端ID查找客户端，访问红黑树没有加锁
 * 参数: int client_id,客户端ID
 * 返回值: NULL,查找失败
 *         非空，为客户端信息的指针
 */
struct client_info* __client_search(int client_id){
    struct client_info *p_client_i;
    struct rb_node *p_rb_n;
    p_rb_n = rb_search(&g_client_rb_t,client_id);
    if(p_rb_n == NULL){
        return NULL;
    }
    p_client_i = rb_entry(p_rb_n,struct client_info,rb_n);
    return p_client_i;
}
/* 函数: client_info* client_search(int client_id)
 * 功能: 根据客户端ID查找客户端,访问列表加锁
 * 参数: int client_id,客户端ID
 * 返回值: NULL,查找失败
 *         非空，为客户端信息的指针
 */
struct client_info* client_search(int client_id){
    struct client_info *p_client_i;
    sem_wait(&g_client_rb_mutex);
    p_client_i = __client_search(client_id);
    sem_post(&g_client_rb_mutex);
    return p_client_i;
}
/* 函数: int client_wd_resume()
 * 功能: 喂狗
 * 参数: int client_id,客户端ID
 * 返回值: -1,无客户端
 *          1,喂狗成功
 */
int client_wd_resume(int client_id){
    struct client_info *pci;
    struct cm_msg heart_beat_msg;
    sem_wait(&client_list_mutex);  //互斥访问客户端列表
    pci = client_get_ci(client_id);
    if(pci == NULL){
        client_rls_ci(pci);
        sem_post(&client_list_mutex);
        return -1;
    }
    heart_beat_msg.type = 4;  //心跳报文
    heart_beat_msg.client_id = client_id;
    heart_beat_msg.dest_id = client_id;
    heart_beat_msg.data_size = 0;
    pci->wd_cnt = WD_RESUME_CNT;
    client_snd_msg(&heart_beat_msg,1);  //回送心跳报文
    client_rls_ci(pci);
    sem_post(&client_list_mutex);
    return 1;
}

/* 函数: int client_wd_init()
 * 功能: 初始化看门狗
 * 参数: 
 * 返回值: -1,定时器设置失败
 *          1,初始化成功
 */
int client_wd_init(){
    struct itimerval tick;  //定时器配置结构体
    timer_t timerid;
    struct sigevent evp;
    memset(&evp,0,sizeof(struct sigevent));
    evp.sigev_value.sival_int = 111;
    evp.sigev_notify = SIGEV_THREAD;   //线程通知方式,派驻线程
    evp.sigev_notify_function = client_wd_decline;//每计时一秒，调用一次client_wd_decline()函数
    if(timer_create(CLOCK_REALTIME,&evp,&timerid) == -1){
        syslog(LOG_DEBUG,"Failed to create timer ");
        return -1;
    }

    tick.it_value.tv_sec = 1;  //使能定时器之后，等待tv_sec秒之后，才真正启动定时器
    tick.it_value.tv_usec = 0;
    tick.it_interval.tv_sec = 1;  //周期为一秒
    tick.it_interval.tv_usec = 0;  

    if(timer_settime(timerid,0,&tick,NULL) == -1){
        syslog(LOG_DEBUG,"Failed to set timer");
        return -1;
    }
    return 1;
}


/* 函数名: void *client_del_thread()
 * 功能: 客户端删除线程，专门负责删除客户端信息结构体
 * 参数:
 * 返回值:
 */
void *client_del_thread(void *arg){
    struct client_info *pci;
    while(1){
        com_msgq_recv(g_msgq_del,&pci);
        if(__client_del(pci) < 0){  //删除客户端信息结构体
            syslog(LOG_DEBUG,"Error:__client_del() error,err_ret");
        }   
    }
    
}

/* 函数: int __client_del(client_info *p_client_i)
 * 功能: 根据客户端信息指针从内存上删除客户端，当然，在删除之前，把客户端相关的资
 *       源都释放掉
 * 参数: int client_id,需要移除的客户端的id
 * 返回值: -1,指针为空
 *          1,函数执行成功
 */
int __client_del(struct client_info *p_client_i){
    struct snd_queue *p_sndq_node;
    char name_temp[CLIENT_NAME_LENGTH];
    int id_temp;
    strcpy(name_temp,p_client_i->name);
    id_temp = p_client_i->id;
    if(p_client_i == NULL){
        return -1;
    }
    //从epoll中移除socket,如果移除之前epoll_wait正在被调用，则正在调用的epoll_wait
    //还会监听被移除的socket的事件,下次调用epoll_wait()才不会监听该被移除的socke
    //t的事件
    if(epoll_ctl(g_socket_epoll_fd,EPOLL_CTL_DEL,p_client_i->sockfd,NULL) == -1){
        syslog(LOG_DEBUG,"Failed to remove sockfd from epoll:%s",strerror(errno));
    }
    if(epoll_ctl(g_socket_snd_epoll_fd,EPOLL_CTL_DEL,p_client_i->sockfd,NULL) == -1){
        syslog(LOG_DEBUG,"Failed to remove sockfd from snd_epoll:%s",strerror(errno));
    }
    if(epoll_ctl(g_socket_disc_epoll_fd,EPOLL_CTL_DEL,p_client_i->sockfd,NULL) == -1){
        syslog(LOG_DEBUG,"Failed to remove sockfd from snd_epoll:%s",strerror(errno));
    }
    if(p_client_i->sockfd != 0){
        shutdown(p_client_i->sockfd,2);  //关闭套接字
        close(p_client_i->sockfd);
    }
    //syslog(LOG_DEBUG,"cnt:%d",p_client_i->msg_cnt);//测试用的代码，可以删除
    sem_wait(&p_client_i->del_enable);  //等待至可以删除
    /*释放发送队列*/
    while((p_sndq_node = client_desndq(p_client_i)) != NULL){
        mmpl_rlsmem(g_trdata_mmpl,p_sndq_node->snd_data);
        mmpl_rlsmem(g_trdata_mmpl,p_sndq_node);
    }
    mmpl_rlsmem(g_client_mmpl,p_client_i);
    syslog(LOG_DEBUG,"Client(name:%s id:%d) delete complete",name_temp,id_temp);
    return 1;
}
/* 函数: int client_remove(int client_id)
 * 功能: 根据客户端ID移除客户端
 * 参数: int client_id,需要移除的客户端的id
 * 返回值: -1,出问题了
 *          1,函数执行成功
 */
int client_remove(int client_id){
    int err_ret;
    struct client_info *p_c_i,*n;
    struct rb_node *p_rb_n;
    sem_wait(&g_client_rb_mutex);
    p_rb_n = rb_search(&g_client_rb_t,client_id);
    if(p_rb_n == NULL){
        syslog(LOG_DEBUG,"Failed to remove client:client(id:%d) not found",client_id);
        sem_post(&g_client_rb_mutex);
        return -1;
    }
    p_c_i = rb_entry(p_rb_n,struct client_info,rb_n); //得出客户端信息结构体指针
    rb_delete(&g_client_rb_t,p_rb_n);  //移出红黑树
    sem_post(&g_client_rb_mutex);
    sem_wait(&client_list_mutex);  //互斥访问客户端列表
    list_del(&p_c_i->list);  //从链表移除
    sem_post(&client_list_mutex);
    com_msgq_snd(g_msgq_del,&p_c_i,sizeof(void *));
    return 1;
}


/* 函数: void client_wd_decline()
 * 功能: 看门狗计时器衰减(计数)函数
 * 参数: 
 * 返回值: -1,出问题了
 *          1,函数执行成功
 */
void client_wd_decline(union sigval v){
    int err_ret;
    struct client_info *p_client_i,*n;
    sem_wait(&client_list_mutex);  //互斥访问客户端列表
    list_for_each_entry_safe(p_client_i,n,&client_i_h.list,list){  
        if(p_client_i->id == 1){
            //syslog(LOG_DEBUG,"cnt:%d",p_client_i->msg_cnt);//测试用的代码，可以删除
        }
        //syslog(LOG_DEBUG,"client:%s wd_cnt:%d",p_client_i->name,p_client_i->wd_cnt);
        //可以对p_client_i进行删除操作
        if(--(p_client_i->wd_cnt) == 0 && p_client_i->id != -1){  
            //计时到0且非链表头客户端,则移除客户端
            syslog(LOG_DEBUG,"client:%s timeout,removing it",p_client_i->name);
            list_del(&(p_client_i->list));  //从链表移除
            sem_wait(&g_client_rb_mutex);
            rb_delete(&g_client_rb_t,&p_client_i->rb_n);
            sem_post(&g_client_rb_mutex);
            com_msgq_snd(g_msgq_del,&p_client_i,sizeof(void *));
        }    
    }
    sem_post(&client_list_mutex);
}

