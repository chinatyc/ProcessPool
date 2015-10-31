#include "philoProcessPool.h"

static int sig_pipefd[2];

template<typename T>
PhiloProcessPool<T>* PhiloProcessPool<T>::m_instance=NULL;

static void exitAndOut(int ret,const char* msg){
	return;
}

//制定文件描述符设置为非阻塞
static int setnoblock(int fd){
	int old_option = fcntl(fd,F_GETFL);
	int new_option = old_option|O_NONBLOCK;
	fcntl(fd,F_SETFL,new_option);
	return old_option;
};

//未指定epoll对象添加监听套接字
static void addfd(int epollfd, int fd){
	epoll_event event;
	event.data.fd=fd;
	event.events=EPOLLIN|EPOLLET;
	epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
	setnoblock(fd);
}

static void removefd(int epollfd, int fd){
	epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
	close(fd);
}

static void sig_handler(int sig){
	int save_errno=errno;
	int msg=sig;
	send(sig_pipefd[1],(char*)&msg,1,0);
	errno=save_errno;
}

static void addsig(int sig,void(handler)(int),bool restart=true){//指针函数
	struct sigaction sa;
	memset(&sa,'\0',sizeof(sa));
	sa.sa_handler=handler;
	if(restart){
		sa.sa_flags |= SA_RESTART;
	}
	sigfillset(&sa.sa_mask);
	if(sigaction(sig,&sa,NULL)==-1){
		exitAndOut(-1,"sigaction error");
	}
}



template<typename T>
PhiloProcessPool<T>::PhiloProcessPool(int listenfd, int process_number){
	m_listenfd=listenfd;
	m_sub_process=new Process[process_number];
	m_process_number=process_number;//进程数
	m_idx=-1;
	m_stop=false;
	if((process_number>MAX_PROCESS_NUMBER)){
		exitAndOut(-1,"process_number is error");
	}

	//创建子进程
	for(int i=0;i<process_number;i++){
		int ret=socketpair(PF_UNIX,SOCK_STREAM,0,m_sub_process[i].m_pipefd);
		if(ret<0){
			exitAndOut(-1,"socketpair error");
		}
		m_sub_process[i].m_pid=fork();
		if(m_sub_process[i].m_pid<0){
			exitAndOut(-1,"fork error");
		}
		
		if(m_sub_process[i].m_pid>0){//父进程
			close(m_sub_process[i].m_pipefd[1]);
		//	continue;
		}
		if(m_sub_process[i].m_pid==0){//子进程
			m_idx=i;
			close(m_sub_process[i].m_pipefd[0]);
			break;
		}
	}

}

template<typename T>
void PhiloProcessPool<T>::run(){
	if(m_idx!=-1){
		run_child();
		return;
	}else{
		run_parent();
	}
}

template<typename T>
void PhiloProcessPool<T>::setup_sig_pipe(){
	m_epollfd=epoll_create(5);//为每个进程创建一个epoll对象；
	if(m_epollfd<0){
		exitAndOut(-1,"epoll create fail");
	}
	int ret=socketpair(PF_UNIX,SOCK_STREAM,0,sig_pipefd);
	if(ret<0){
		exitAndOut(-1,"sig_pipefd create fail");
	}

	setnoblock(sig_pipefd[1]);
	addfd(m_epollfd,sig_pipefd[0]);//信号接收放在epoll里
	
	addsig(SIGCHLD,sig_handler);
	addsig(SIGTERM,sig_handler);
	addsig(SIGINT,sig_handler);
	addsig(SIGPIPE,SIG_IGN);

};

template<typename T>
void PhiloProcessPool<T>::run_child(){
	int pipefd=m_sub_process[m_idx].m_pipefd[1];
	setup_sig_pipe();
	addfd(m_epollfd,pipefd);//监听父进程的发来的消息；
	epoll_event events[MAX_EVENT_NUMBER];

	int number=0;
	int ret=-1;
	T* users=new T[USER_PER_PROCESS];

	while(!m_stop){
		number=epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);
		if((number<0)&&(errno!=EINTR)){//EINTR是什么意思
			printf("epoll failure");
			break;
		}
		for(int i=0;i<number;i++){
			int sockfd=events[i].data.fd;
			if((sockfd==pipefd)&&(events[i].events&EPOLLIN)){
				int client=0;
				ret=recv(sockfd,(char*)&client,sizeof(client),0);
				if(((ret<0)&&(errno!=EAGAIN))){
					continue;
				}
				else{
					struct sockaddr_in client_address;
					socklen_t client_addrlength=sizeof(client_address);
					int connfd=accept(m_listenfd,(struct sockaddr*)&client_address,&client_addrlength);
					if(connfd<0){
						printf("errno is %d\n", errno);
						continue;
					}else{
						addfd(m_epollfd,connfd);

						//开始处理业务逻辑；
						fprintf(stderr, "%d accept new connection \n",getpid());
						fprintf(stderr, "connfd is  %d \n",connfd);
						users[connfd].init(m_epollfd,connfd);
						
					}
				}
			}else if((sockfd==sig_pipefd[0])&&(events[i].events & EPOLLIN)){
				//处理信号逻辑
				int sig;
				char signals[1024];
				ret=recv(sig_pipefd[0],signals,sizeof(signals),0);
				if(ret<=0){
					continue;
				}else{
					for(int i=0;i<ret;i++){//一个信号一个字节？
						switch(signals[i]){
							case SIGCHLD:{
								pid_t pid;
								int stat;
								while((pid=waitpid(-1,&stat,WNOHANG))>0){
									continue;
								}
								break;
							}
							case SIGTERM:
							case SIGINT:{
								m_stop=true;
								break;
							}
							default:{
								break;
							}
						}
					}

				}
			}else if(events[i].events & EPOLLIN){//其他请求都是客户请求过来的
				//处理用户逻辑
				users[sockfd].process();
				fprintf(stderr, "%d deal connection \n",getpid());
			}else{
				continue;
			}

		}
	}
	close(pipefd);
	close(m_epollfd);
}

template<typename T>
void PhiloProcessPool<T>::run_parent(){
	setup_sig_pipe();
	addfd(m_epollfd,m_listenfd);//父进程监听新的连接
	epoll_event events[MAX_EVENT_NUMBER];

	int number=0;
	int new_conn=1;
	int m_sub_counter=0;

	while(!m_stop){
		number=epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);
		if((number<0)&&(errno!=EINTR)){//EINTR是什么意思
			printf("epoll failure");
			break;
		}
		for(int i=0;i<number;i++){
			int sockfd=events[i].data.fd;
			if(sockfd==m_listenfd){
				//使用Roung Robin算法分配
				int i=m_sub_counter;
				do{
					if(m_sub_process[i].m_pid!=-1){
						break;
					}
					i=(i+1)%m_process_number;
				}
				while(i!=m_sub_counter);

				if(m_sub_process[i].m_pid==-1){
					m_stop=true;
					break;
				}
				m_sub_counter=(i+1)%m_process_number;
				send(m_sub_process[i].m_pipefd[0],(char*)&new_conn,sizeof(new_conn),0);
				printf("send request to child %d\n", i);

			}else if((sockfd==sig_pipefd[0])&&(events[i].events&EPOLLIN)){
				int sig;
				char signals[1024];
				int ret=-1;
				ret=recv(sig_pipefd[0],signals,sizeof(signals),0);
				if(ret<=0){
					continue;
				}else{
					for(int i=0;i<ret;i++){
						switch(signals[i]){
							case SIGCHLD:{
								pid_t pid;
								int stat;
								while((pid=waitpid(-1,&stat,WNOHANG))>0){
									//进程id为pid的进程因为某些原因或异常退出，向主进程发出SIGCHILD信号
									for(int i=0;i<m_process_number;i++){
										if(pid==m_sub_process[i].m_pid){
											printf("child %d join\n", i);
											close(m_sub_process[i].m_pipefd[0]);//关闭其通信管道
											m_sub_process[i].m_pid=-1;
										}
									}
								}
								m_stop=true;
								for(int i=0;i<m_process_number;i++){
									if(m_sub_process[i].m_pid!=-1){
										m_stop=false;//如果所偶子进程都结束了，父进程也结束
										break;
									}
								}
								break;
							}
							case SIGTERM:
							case SIGINT:{
								for(int i=0;i<m_process_number;i++){
									int pid=m_sub_process[i].m_pid;
									if(pid!=-1){
										kill(pid,SIGTERM);//kill所有子进程；
									}
								}
								break;
							}
							default:{
								break;
							}
						}
					}
				}
			}else{
				continue;
			}
		}
	}
	close(m_epollfd);
}

class Test{
private:
	static int m_epollfd; //在静态函数里调用，所以要static
	int m_sockfd;
	static const int BUFFER_SIZE=1024;
	char m_buf[BUFFER_SIZE];

public:
	Test(){};
	~Test(){};
	void init(int epollfd,int sockfd){
		m_epollfd=epollfd;
		m_sockfd=sockfd;
	}

	void process(){
		while(true){
			int ret=recv(m_sockfd,m_buf,BUFFER_SIZE,0);
			if(ret<0){
				if(errno!=EAGAIN){
					
					fprintf(stderr, "ret<0 close \n");
					removefd(m_epollfd,m_sockfd);//异常
				}
				break;
			}else if(ret==0){
				fprintf(stderr, "ret=0 close \n");
				removefd(m_epollfd,m_sockfd);
				break;
			}else{
				fprintf(stderr, "sockfd %d %s\n", m_sockfd,m_buf);
			}
		}
	}
};
int Test::m_epollfd=-1;


int  main(){
	const char* ip="101.200.209.230";
	int port=36000;
	int listenfd=socket(PF_INET,SOCK_STREAM,0);
	struct sockaddr_in address;
	bzero(&address,sizeof(address));
	int ret=0;
	address.sin_family=AF_INET;
	inet_pton(AF_INET,ip,&address.sin_addr);
	address.sin_port = htons(port);
	ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
	ret=listen(listenfd,5);

	PhiloProcessPool<Test>* pool=PhiloProcessPool<Test>::create(listenfd);
	if(pool){
		fprintf(stderr,"pid %d run\n",getpid());
		pool->run();
		delete pool;
	}
	close(listenfd);
}

