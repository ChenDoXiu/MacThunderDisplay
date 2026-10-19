#include "Discovery.hpp"
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>
#include <QUuid>

Discovery::Discovery(QObject *parent):QObject(parent){
 QObject::connect(&socket,&QUdpSocket::readyRead,this,[this]{readReplies(&socket);});
}
void Discovery::readReplies(QUdpSocket*source){
 while(source->hasPendingDatagrams()){
  const auto packet=source->receiveDatagram(512);
  const QByteArray prefix="OFFER TDP1 "+nonce+' ';
  if(nonce.isEmpty()||packet.senderPort()!=42420||!packet.data().startsWith(prefix))continue;
  QString name=QString::fromUtf8(packet.data().mid(prefix.size())).simplified().left(128);
  if(packet.senderAddress().protocol()!=QAbstractSocket::IPv4Protocol||name.isEmpty())continue;
  if(found)found(name,packet.senderAddress());
 }
}
bool Discovery::scan(const QString&manualHost){
 if(socket.state()!=QAbstractSocket::BoundState&&!socket.bind(QHostAddress(QHostAddress::AnyIPv4),quint16(0)))return false;
 nonce=QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
 const QByteArray probe="DISCOVER TDP1 "+nonce;
 interfaceSockets.clear();bool sent=false;QSet<QHostAddress> targets;
 for(const auto&iface:QNetworkInterface::allInterfaces()){
  if(!(iface.flags()&QNetworkInterface::IsUp)||!(iface.flags()&QNetworkInterface::IsRunning)||(iface.flags()&QNetworkInterface::IsLoopBack))continue;
  for(const auto&entry:iface.addressEntries())if(entry.ip().protocol()==QAbstractSocket::IPv4Protocol){
   if(!entry.broadcast().isNull())targets.insert(entry.broadcast());
   // Limited broadcasts must also originate on each interface, rather than
   // relying on the default route (often Wi-Fi while Thunderbolt is present).
   auto perInterface=std::make_unique<QUdpSocket>();
   if(!perInterface->bind(entry.ip(),quint16(0)))continue;
   auto *source=perInterface.get();QObject::connect(source,&QUdpSocket::readyRead,this,[this,source]{readReplies(source);});
   sent=source->writeDatagram(probe,QHostAddress(QHostAddress::Broadcast),42420)>=0||sent;
   interfaceSockets.push_back(std::move(perInterface));
  }
 }
 targets.insert(QHostAddress(QHostAddress::Broadcast));
 QHostAddress manual(manualHost);if(manual.protocol()==QAbstractSocket::IPv4Protocol)targets.insert(manual);
 for(const auto&address:targets)sent=socket.writeDatagram(probe,address,42420)>=0||sent;
 return sent;
}
