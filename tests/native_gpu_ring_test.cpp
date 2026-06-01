#include "alr_gpu/alr_gpu_ring.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace alr::gpu;
int main(){
  // small ring (256B) to FORCE wrap-around with larger total traffic
  const uint32_t R=256;
  std::vector<uint8_t> region(ring_region_size(R),0);
  if(!ring_init(region.data(),R)){printf("FAIL init\n");return 1;}
  RingProducer p(region.data()); RingConsumer c(region.data());
  if(!p.valid()||!c.valid()){printf("FAIL valid\n");return 1;}

  // push 4KB total through a 256B ring in 50B chunks, draining as we go (forces many wraps)
  uint8_t seq=0; uint64_t pushed=0, drained=0; uint8_t expect=0; bool ok=true;
  std::vector<uint8_t> chunk(50), snap(64);
  for(int iter=0; iter<200 && ok; ++iter){
    for(auto&b:chunk) b=seq++;
    while(!p.append(chunk.data(),50)){          // back-pressure: drain then retry
      uint32_t n=c.snapshot(snap.data(),(uint32_t)snap.size());
      if(n==0) break;
      for(uint32_t i=0;i<n;++i){ if(snap[i]!=expect++){ok=false;printf("FAIL mismatch drain at %llu\n",(unsigned long long)drained+i);break;} }
      c.advance(n); drained+=n;
    }
    pushed+=50;
  }
  // final drain
  while(true){ uint32_t n=c.snapshot(snap.data(),(uint32_t)snap.size()); if(!n)break;
    for(uint32_t i=0;i<n;++i){ if(snap[i]!=expect++){ok=false;printf("FAIL mismatch final\n");break;} }
    c.advance(n); drained+=n; if(!ok)break; }

  printf("pushed=%llu drained=%llu match=%s\n",(unsigned long long)pushed,(unsigned long long)drained, ok&&pushed==drained?"YES":"NO");

  // sync handshake
  p.flush_and_wait(1); c.post_reply(); uint32_t got=p.flush_and_wait(1);
  printf("sync: req->reply got=%u (>=1 expected)\n", got);
  printf("free-when-empty=%llu (expect %u)\n",(unsigned long long)p.free_bytes(), R-1);
  return ok&&pushed==drained ? 0:1;
}
