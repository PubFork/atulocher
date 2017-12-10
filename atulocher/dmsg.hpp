#ifndef atulocher_dmsg
#define atulocher_dmsg
#include <map>
#include <set>
#include <atomic>
#include <sys/epoll.h>
#include <memory.h>
#include <string>
#include <stdio.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/file.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <mutex>
#include "threadpool.hpp"
#include "mempool.hpp"
#include "rwmutex.hpp"
namespace atulocher{
  class Dmsg_base{
    public:
    static void setnonblocking(int sock){
      int opts;
      opts = fcntl(sock, F_GETFL);
      if(opts < 0) {
        perror("fcntl(sock, GETFL)");
        return;
      }
      opts = opts | O_NONBLOCK;
      if(fcntl(sock, F_SETFL, opts) < 0) {
        perror("fcntl(sock, SETFL, opts)");
        return;
      }
    }
    static int startup(u_short *port){
      int rpcd = 0;
      struct sockaddr_in name;
      rpcd = socket(PF_INET, SOCK_STREAM, 0);
      if (rpcd == -1)
        return -1;
      int reuse = 1;
      if (setsockopt(rpcd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        return -1;
      }
      memset(&name, 0, sizeof(name));
      name.sin_family = AF_INET;
      name.sin_port = htons(*port);
      name.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(rpcd, (struct sockaddr *)&name, sizeof(name)) < 0)
         return -1;
      if (*port == 0){
        int namelen = sizeof(name);
        if (getsockname(rpcd, (struct sockaddr *)&name, (socklen_t *)&namelen) == -1)
          return -1;
        *port = ntohs(name.sin_port);
      }
      if (listen(rpcd, 5) < 0)
        return -1;
      return (rpcd);
    }
    std::atomic<bool> running;
    void run(u_short p,int maxnum=256){
      running=true;
      int listenfd = -1;
      u_short port = p;
      int connfd = -1;

      struct sockaddr_in client;
      int client_len = sizeof(client);
      
      signal(SIGPIPE,[](int){});
      //绑定监听端口
      listenfd = startup(&port);
      
      
      struct epoll_event ev, events[20];
      int epfd = epoll_create(maxnum);
      setnonblocking(listenfd);
      ev.data.fd = listenfd;
      ev.events = EPOLLIN;
      epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);
      
      
      while (running){
        
        int nfds = epoll_wait(epfd, events, 20, 2000);
        
        for(int i = 0; i < nfds; ++i) {
          if(events[i].data.fd == listenfd) {
            
            connfd = accept(listenfd, (struct sockaddr *)&client, (socklen_t *)&client_len);
            if(connfd < 0) {
              continue;
            }
            
            onConnect(connfd);
            
            setnonblocking(connfd);
            ev.data.fd = connfd;
            ev.events = EPOLLIN | EPOLLHUP;
            epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev);
            
          }else if(events[i].events & EPOLLIN) {
            if((connfd = events[i].data.fd) < 0) continue;
            //处理请求
            
            onMessage(connfd);
            ev.data.fd = connfd;
            ev.events = EPOLLIN | EPOLLHUP;
            epoll_ctl(epfd, EPOLL_CTL_MOD, connfd, &ev);
            
          }else if(events[i].events & EPOLLHUP) {
            if((connfd = events[i].data.fd) < 0) continue;
            
            onQuit(connfd);
            
          }
        }
      }
      close(listenfd);
      close(epfd);
      destruct();
      return;
    }
    virtual void onMessage(int)=0;
    virtual void onConnect(int)=0;
    virtual void onQuit(int)=0;
    virtual void destruct()=0;
  };
  class Dmsg_client_base{
    public:
    struct node{
      node * next;
      char data[4096];
      unsigned int  len;
    };
    int sendMsg(int fd,node * d){
      return send(fd,d->data,4096,0);
    }
  };
  class Dmsg_client:public Dmsg_client_base{
    public:
    int sockfd;
    void init(sockaddr_in address){
      sockfd = socket(AF_INET, SOCK_STREAM, 0);
      connect(sockfd, (struct sockaddr *)&address, sizeof(address));
    }
    Dmsg_client(const char * addr,u_short port){
      struct sockaddr_in address;
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = inet_addr(addr);
      address.sin_port = htons(port);
      init(address);
    }
    ~Dmsg_client(){
      if(sockfd!=-1)close(sockfd);
    }
    inline int sendMsg(node * d){
      return send(sockfd,d->data,4096,0);
    }
    inline int recvMsg(node * d){
      return read(sockfd,d->data,4096);
    }
  };
  class Dmsg_queue:public Dmsg_base,public Dmsg_client_base{
    private:
    struct conn{
      conn * next;
      node * data;
      int    sessionid;
    };
    std::atomic<int> session;
    std::map<int,conn*> conns;
    struct status{
      status * next;
      int fd;
      node * data;
      Dmsg_queue * self;
    };
    mempool<status>    spool;
    mempool<conn>      cpool;
    mempool<node>      npool;
    static void * accreq(void * arg){
      auto self=(status*)arg;
      self->self->onGetMessage(self->data,self->fd);
      self->self->npool.del(self->data);
      self->self->spool.del(self);
    }
    inline void resetConn(int connfd){
      auto it=conns.find(connfd);
      conn * cp;
      if(it==conns.end()){
        cp=cpool.get();
        conns[connfd]=cp;
      }else{
        cp=it->second;
      }
      cp->sessionid=session;
      session++;
      cleanConn(cp);
    }
    inline void cleanConn(conn * cp){
      if(cp->data==NULL)cp->data=npool.get();
      auto p=cp->data;
      p->len=0;
    }
    inline void removeConn(conn * cp){
      if(cp->data){
        npool.del(cp->data);
      }
      cpool.del(cp);
    }
    inline bool connAppend(conn * cp,int connfd){
      auto p=cp->data;
      if(p->len>=4096){
        cp->data=npool.get();
        cp->data->len=0;
        //onGetMessage(p);
        
        auto sp=spool.get();
        sp->self=this;
        sp->fd=connfd;
        sp->data=p;
        threadpool::add(accreq,sp);
        
        //auto it=conns.find(connfd);
        //if(it==conns.end())return true;
        //removeConn(it->second);
        //conns.erase(it);
        
        return false;
      }
      
      char buf;
      if(read(connfd,&buf,1)<=0)return true;
      p->data[p->len]=buf;
      p->len++;
      return false;
    }
    virtual void onMessage(int connfd){
      auto it=conns.find(connfd);
      conn * cp;
      if(it==conns.end())return;
      cp=it->second;
      if(cp->data==NULL)
        cleanConn(cp);
      while(1){
        if(connAppend(cp,connfd))return;
      }
    }
    virtual void onConnect(int connfd){
      resetConn(connfd);
    }
    virtual void onQuit(int connfd){
      auto it=conns.find(connfd);
      if(it==conns.end())return;
      removeConn(it->second);
      conns.erase(it);
    }
    virtual void destruct(){
      for(auto it:conns){
        removeConn(it.second);
      }
    }
    public:
    inline void delnode(node * p){
      npool.del(p);
    }
    virtual void onGetMessage(node * p,int fd)=0;
  };
}
#endif