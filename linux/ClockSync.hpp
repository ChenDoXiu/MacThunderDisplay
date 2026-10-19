#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>

// Four timestamp exchange on monotonic clocks. This estimates offset; it cannot
// determine asymmetric one-way network delay or physical display latency.
class ClockSync {
 struct Sample {int64_t offset;uint64_t rtt,at;};
 std::mutex mutex;
 std::deque<Sample> samples;
 uint64_t session=0;
public:
 struct Estimate {int64_t serverMinusClient=0;uint64_t errorNs=0,rttNs=0;bool valid=false;};
 void reset(uint64_t id){std::lock_guard<std::mutex>lock(mutex);session=id;samples.clear();}
 void accept(uint64_t id,uint64_t t1,uint64_t t2,uint64_t t3,uint64_t t4){
  if(!t1||t4<t1||t3<t2||t4-t1>250000000||t3-t2>t4-t1)return;
  Sample s{((int64_t(t2)-int64_t(t1))+(int64_t(t3)-int64_t(t4)))/2,(t4-t1)-(t3-t2),t4};
  std::lock_guard<std::mutex>lock(mutex);if(id!=session)return;samples.push_back(s);
  while(!samples.empty()&&(t4-samples.front().at>1500000000||samples.size()>8))samples.pop_front();
 }
 Estimate estimate(uint64_t id,uint64_t now){
  std::lock_guard<std::mutex>lock(mutex);if(id!=session||samples.empty())return {};
  auto best=std::min_element(samples.begin(),samples.end(),[](auto&a,auto&b){return a.rtt<b.rtt;});
  if(now<best->at||now-best->at>2000000000||best->rtt>20000000)return {};
  // RTT/2 covers possible path asymmetry for the sampled exchange. Add a
  // conservative 1000 ppm age allowance; this is an estimate, not a guarantee.
  return {best->offset,best->rtt/2+(now-best->at)/1000,best->rtt,true};
 }
};
