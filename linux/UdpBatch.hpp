#pragma once
#include "Transport.hpp"
#include "UdpPlacement.hpp"
#include "UdpRealtime.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <linux/futex.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

// The reader owns socket I/O only. recvmmsg writes into a preallocated batch;
// the parser borrows a packet until its next next() call. Packet slots are
// published in batches through an SPSC ring, with no payload copy or per-packet
// allocation. Futex wakeups are needed only when the parser has gone to sleep.
class UdpBatch {
 static constexpr unsigned BatchSize=64,Capacity=8192;
 static constexpr uint64_t MaxVideoQueueNs=25000000;
 struct Packet {std::array<uint8_t,2048>data; sockaddr_in peer{};uint64_t at=0;unsigned size=0,flags=0;bool batchEnd=false;};
 static_assert((Capacity&(Capacity-1))==0);
 std::vector<Packet>pool{Capacity};
 std::vector<int>sockets;
 std::vector<uint32_t>lastDrops;
 sockaddr_in server;
 alignas(64) std::atomic<uint64_t>written{0};
 alignas(64) std::atomic<uint64_t>consumed{0};
 alignas(64) std::atomic<uint32_t>sleeping{0};
 static_assert(sizeof(std::atomic<uint32_t>)==sizeof(uint32_t));
 static_assert(std::atomic<uint32_t>::is_always_lock_free);
 std::atomic<bool>active{true};
 std::atomic<uint64_t>queuePeak{0},poolDrops{0},ioErrors{0};
 std::thread reader;
 uint64_t readPosition=0,cachedHead=0,queueWaitSum=0,queueWaitCount=0,queueWaitMax=0,staleDrops=0;
 Packet*borrowed=nullptr;
 void wakeParser(){
  // The exchange pairs with the parser's arm/recheck sequence. A producer
  // racing with futex_wait either wakes it or changes the expected value.
  if(sleeping.exchange(0,std::memory_order_acq_rel))syscall(SYS_futex,reinterpret_cast<uint32_t*>(&sleeping),FUTEX_WAKE_PRIVATE,1,nullptr,nullptr,0);
 }
 void receive(){
  pthread_setname_np(pthread_self(),"td-udp");
  std::string interface;int cpu=td::placeThunderboltReader(server,interface);
  std::cerr<<"[UDP] interface="<<interface<<" reader_cpu="<<cpu<<" (-1: scheduler default)\n";
  std::array<iovec,BatchSize>vectors{};
  std::array<mmsghdr,BatchSize>messages{};
  struct Control {alignas(cmsghdr) unsigned char bytes[CMSG_SPACE(sizeof(uint32_t))];};
  std::array<Control,BatchSize>controls{};
  std::array<Packet,BatchSize>discard;
  bool realtime=td::enableReaderRealtime();
  std::cerr<<"[UDP] reader_mode=event realtime="<<realtime<<" (drain until EAGAIN, then ppoll)\n";
  std::vector<pollfd>events;for(int fd:sockets)events.push_back({fd,POLLIN,0});
  unsigned nextLane=0,readyMask=(1u<<sockets.size())-1;
  uint64_t lastBudgetBlock=td::nowNs(),lastPolicyCheck=lastBudgetBlock;
  while(active.load(std::memory_order_relaxed)){
   if(!readyMask){
    timespec timeout{0,20000000};int n=ppoll(events.data(),events.size(),&timeout,nullptr);
    if(n<0){if(errno!=EINTR)ioErrors.fetch_add(1,std::memory_order_relaxed);continue;}
    if(!n)continue;
    for(unsigned lane=0;lane<events.size();lane++)if(events[lane].revents&POLLIN)readyMask|=1u<<lane;
    if(!readyMask){ioErrors.fetch_add(1,std::memory_order_relaxed);continue;}
   }
   unsigned firstLane=nextLane;
   for(unsigned offset=0;offset<sockets.size()&&active.load(std::memory_order_relaxed);offset++){
    unsigned lane=(firstLane+offset)%sockets.size();
    if(!(readyMask&(1u<<lane)))continue;
    uint64_t head=written.load(std::memory_order_relaxed),tail=consumed.load(std::memory_order_acquire);
    unsigned free=unsigned(Capacity-(head-tail));bool full=!free;
    unsigned limit=full?BatchSize:std::min(BatchSize,free);
    for(unsigned i=0;i<limit;i++){
     auto&p=full?discard[i]:pool[(head+i)&(Capacity-1)];vectors[i]={p.data.data(),p.data.size()};
     auto&h=messages[i].msg_hdr;h.msg_name=&p.peer;h.msg_namelen=sizeof(p.peer);h.msg_iov=&vectors[i];h.msg_iovlen=1;h.msg_control=controls[i].bytes;h.msg_controllen=sizeof(Control);h.msg_flags=0;
    }
    calls.fetch_add(1,std::memory_order_relaxed);
    int n=recvmmsg(sockets[lane],messages.data(),limit,MSG_DONTWAIT,nullptr);
    int error=errno;uint64_t at=td::nowNs();nextLane=(lane+1)%sockets.size();
    if(n<0){if(error==EAGAIN||error==EWOULDBLOCK){emptyCalls.fetch_add(1,std::memory_order_relaxed);readyMask&=~(1u<<lane);}else if(error!=EINTR){ioErrors.fetch_add(1,std::memory_order_relaxed);readyMask&=~(1u<<lane);}continue;}
    if(!n){readyMask&=~(1u<<lane);continue;}
    dataCalls.fetch_add(1,std::memory_order_relaxed);
    uint64_t newDrops=0,newTruncated=0;
    for(int i=0;i<n;i++){
     auto&p=full?discard[i]:pool[(head+i)&(Capacity-1)];auto&h=messages[i].msg_hdr;p.size=messages[i].msg_len;p.flags=h.msg_flags;p.at=at;p.batchEnd=i==n-1;
     if(p.flags&MSG_TRUNC)newTruncated++;
     for(auto*c=CMSG_FIRSTHDR(&h);c;c=CMSG_NXTHDR(&h,c))if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SO_RXQ_OVFL&&c->cmsg_len>=CMSG_LEN(sizeof(uint32_t))){
      uint32_t total=0;memcpy(&total,CMSG_DATA(c),sizeof(total));newDrops+=uint32_t(total-lastDrops[lane]);lastDrops[lane]=total;
     }
    }
    packets.fetch_add(n,std::memory_order_relaxed);
    if(newDrops)drops.fetch_add(newDrops,std::memory_order_relaxed);
    if(newTruncated)truncated.fetch_add(newTruncated,std::memory_order_relaxed);
    if(full){poolDrops.fetch_add(n,std::memory_order_relaxed);continue;}
    uint64_t queued=head+n-consumed.load(std::memory_order_acquire),peak=queuePeak.load(std::memory_order_relaxed);
    while(queued>peak&&!queuePeak.compare_exchange_weak(peak,queued,std::memory_order_relaxed)){}
    written.store(head+n,std::memory_order_release);wakeParser();
   }
   uint64_t now=td::nowNs();
   // RTKit requires an RTTIME limit. Even an uninterrupted input flood must
   // periodically block, resetting that budget instead of monopolizing a CPU.
   if(realtime&&now-lastBudgetBlock>=5000000){
    timespec pause{0,20000};while(nanosleep(&pause,&pause)<0&&errno==EINTR&&active.load(std::memory_order_relaxed)){}
    budgetYields.fetch_add(1,std::memory_order_relaxed);lastBudgetBlock=td::nowNs();
   }
   if(now-lastPolicyCheck>=1000000000){
    int policy=sched_getscheduler(0);bool current=policy>=0&&(policy&~SCHED_RESET_ON_FORK)==SCHED_RR;
    if(realtime&&!current){std::cerr<<"[UDPRealtime] realtime scheduling revoked; continuing normal event receive\n";realtime=false;}
    lastPolicyCheck=now;
   }
  }
  wakeParser();
 }
public:
 alignas(64) std::atomic<uint64_t>calls{0};
 std::atomic<uint64_t>dataCalls{0},emptyCalls{0},packets{0},budgetYields{0};
 // Keep busy-poll counters away from the drop counter read by the parser.
 alignas(64) std::atomic<uint64_t>drops{0};
 std::atomic<uint64_t>truncated{0};
 bool dropReporting=false;
 struct QueueStatistics {uint64_t peakPackets,poolDrops,staleDrops,ioErrors;double averageMs,maximumMs;};
 QueueStatistics takeQueueStatistics(){
  QueueStatistics s{queuePeak.exchange(0,std::memory_order_relaxed),poolDrops.load(std::memory_order_relaxed),staleDrops,ioErrors.load(std::memory_order_relaxed),queueWaitCount?double(queueWaitSum)/queueWaitCount/1e6:0,double(queueWaitMax)/1e6};
  queueWaitSum=queueWaitCount=queueWaitMax=0;return s;
 }
 bool batchFinished()const{return borrowed&&borrowed->batchEnd;}
 explicit UdpBatch(const std::vector<int>&fds,const sockaddr_in&peer):lastDrops(fds.size()),server(peer){
  int enabled=1;dropReporting=true;
  for(int fd:fds){sockets.push_back(fd);dropReporting&=setsockopt(fd,SOL_SOCKET,SO_RXQ_OVFL,&enabled,sizeof(enabled))==0;}
  std::cerr<<"[UDPQueue] capacity_packets="<<Capacity<<" packets_per_batch="<<BatchSize<<" pool_bytes="<<pool.size()*sizeof(Packet)<<" max_video_age_ms="<<MaxVideoQueueNs/1000000<<'\n';
  reader=std::thread([this]{receive();});
 }
 ~UdpBatch(){active.store(false,std::memory_order_relaxed);wakeParser();if(reader.joinable())reader.join();}
 UdpBatch(const UdpBatch&)=delete;
 UdpBatch&operator=(const UdpBatch&)=delete;
 ssize_t next(uint8_t*&data,sockaddr_in&peer,uint64_t&arrival){
  if(borrowed){
   ++readPosition;borrowed=nullptr;
   if(!(readPosition%BatchSize)||readPosition==cachedHead)consumed.store(readPosition,std::memory_order_release);
  }
  if(readPosition==cachedHead){
   cachedHead=written.load(std::memory_order_acquire);
   if(readPosition==cachedHead){
    sleeping.exchange(1,std::memory_order_acq_rel);
    if(active.load(std::memory_order_relaxed)&&readPosition==written.load(std::memory_order_acquire)){
     timespec timeout{0,2000000};syscall(SYS_futex,reinterpret_cast<uint32_t*>(&sleeping),FUTEX_WAIT_PRIVATE,1,&timeout,nullptr,0);
    }
    sleeping.exchange(0,std::memory_order_acq_rel);cachedHead=written.load(std::memory_order_acquire);
    if(readPosition==cachedHead){errno=EAGAIN;return -1;}
   }
  }
  borrowed=&pool[readPosition&(Capacity-1)];auto&p=*borrowed;data=p.data.data();peer=p.peer;arrival=p.at;
  uint64_t now=td::nowNs(),age=now>=arrival?now-arrival:0;
  queueWaitSum+=age;queueWaitCount++;queueWaitMax=std::max(queueWaitMax,age);
  if(p.flags&MSG_TRUNC)return 0;
  // Only stale video data can be skipped. Control and cursor messages still
  // reach the parser, even after a scheduling stall.
  if(age>MaxVideoQueueNs&&p.size>=td::HeaderSize&&!memcmp(data,"TDP1",4)&&data[4]==1&&data[5]==1){staleDrops++;return 0;}
  return p.size;
 }
};
