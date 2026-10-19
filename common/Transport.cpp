#include "Transport.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
namespace td {
uint64_t nowNs(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
uint64_t random64(){std::random_device r;return (uint64_t(r())<<32)|r();}
void closeSocket(int fd){if(fd>=0){shutdown(fd,SHUT_RDWR);close(fd);}}
void setSocketTimeout(int fd,int ms){timeval t{ms/1000,(ms%1000)*1000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));}
int bindUdp(uint16_t port){int fd=::socket(AF_INET,SOCK_DGRAM,0);if(fd<0)throw std::runtime_error("UDP socket failed");
#ifdef SO_NOSIGPIPE
int noSigpipe=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&noSigpipe,sizeof(noSigpipe));
#endif
int size=8*1024*1024;setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size));setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_ANY);if(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0){closeSocket(fd);throw std::runtime_error("UDP port 42420 is unavailable");}return fd;}
uint16_t socketPort(int fd){sockaddr_in a{};socklen_t n=sizeof(a);getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n);return ntohs(a.sin_port);}
static void put(uint8_t*p,uint64_t n,int bytes){for(int i=bytes-1;i>=0;i--){p[i]=uint8_t(n);n>>=8;}}
static uint64_t get(const uint8_t*p,int bytes){uint64_t n=0;for(int i=0;i<bytes;i++)n=(n<<8)|p[i];return n;}
std::array<uint8_t,HeaderSize> encodeHeader(const Header &h){std::array<uint8_t,HeaderSize>b{};memcpy(b.data(),"TDP1",4);b[4]=1;b[5]=h.type;put(&b[6],h.flags,2);put(&b[8],h.session,8);put(&b[16],h.frame,8);put(&b[24],h.captureNs,8);put(&b[32],h.epoch,4);put(&b[36],h.width,2);put(&b[38],h.height,2);put(&b[40],h.unit,2);put(&b[42],h.units,2);put(&b[44],h.fragment,2);put(&b[46],h.fragments,2);put(&b[48],h.unitSize,4);put(&b[52],h.offset,4);put(&b[56],h.payload,2);b[58]=h.chroma;return b;}
bool decodeHeader(const uint8_t *b,size_t n,Header &h){if(n<HeaderSize||memcmp(b,"TDP1",4)||b[4]!=1)return false;h.type=b[5];h.flags=get(b+6,2);h.session=get(b+8,8);h.frame=get(b+16,8);h.captureNs=get(b+24,8);h.epoch=get(b+32,4);h.width=get(b+36,2);h.height=get(b+38,2);h.unit=get(b+40,2);h.units=get(b+42,2);h.fragment=get(b+44,2);h.fragments=get(b+46,2);h.unitSize=get(b+48,4);h.offset=get(b+52,4);h.payload=get(b+56,2);h.chroma=b[58];bool fragmentsValid=h.payload<=ChunkSize&&n==HeaderSize+h.payload&&h.unitSize>0&&h.unitSize<=256*1024&&h.fragments>0&&h.fragment<h.fragments&&h.fragments==(h.unitSize+ChunkSize-1)/ChunkSize&&h.offset==uint32_t(h.fragment)*ChunkSize&&h.offset+h.payload<=h.unitSize&&h.payload==std::min<size_t>(ChunkSize,h.unitSize-h.offset);
if(!fragmentsValid)return false;
if(h.type==2)return h.units==1&&h.unit==0&&h.width>0&&h.height>0&&h.width<=128&&h.height<=128&&h.flags<=1&&(h.epoch>>16)<h.width&&(h.epoch&65535)<h.height&&h.unitSize<=uint32_t(h.width)*h.height*6;
return h.type==1&&h.units>0&&h.units<=32768&&h.unit<h.units&&h.width>=128&&h.height>=128&&h.width<=8192&&h.height<=8192&&uint64_t(h.width)*h.height<=33554432&&h.chroma<=1;}
}
