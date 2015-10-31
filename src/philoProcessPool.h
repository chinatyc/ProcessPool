#ifndef PHILO_PROCESS_POOL_H
#define PHILP_PROCESS_POOL_H
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<sys/epoll.h>
#include<signal.h>
#include<sys/wait.h>
#include<sys/stat.h>

class Process
{
public:
	Process(){
		m_pid=-1;
	};
	pid_t m_pid;//进程id
	int m_pipefd[2];//用于和父进程进行通信
}; 

template<typename T>
class PhiloProcessPool
{
private:
	PhiloProcessPool(int listenfd, int process_number=4);//单例模式
public:
	static PhiloProcessPool<T>* create(int listenfd, int process_number=4){
		if(!m_instance){
			m_instance=new PhiloProcessPool<T>(listenfd,process_number);
			return m_instance;
		}
	};
	void run();//入口函数；
	~PhiloProcessPool(){
		delete [] m_sub_process;
	};
private:
	static PhiloProcessPool<T>* m_instance;

	void setup_sig_pipe();
	void run_parent();
	void run_child();

	static const int MAX_PROCESS_NUMBER=16;//进程池最大进程数
	static const int USER_PER_PROCESS=65536;//每个进程处理的最大连接数；
	static const int MAX_EVENT_NUMBER=10000;
	int m_process_number; //进程池中当前进程数
	int m_idx;//当前进程的进程序号，父进程为-1；
	int m_epollfd;//每个进程都一个epoll;
	int m_listenfd;//当前进程池监听的套接字
	int m_stop;//进程是否执行标记；
	Process* m_sub_process;

};

#endif