#pragma once
#include <QHostAddress>
#include <QObject>
#include <QUdpSocket>
#include <functional>
#include <memory>
#include <vector>

class Discovery final : public QObject {
    QUdpSocket socket;
    QByteArray nonce;
    std::vector<std::unique_ptr<QUdpSocket>> interfaceSockets;
    void readReplies(QUdpSocket *source);

  public:
    std::function<void(const QString &, const QHostAddress &)> found;
    explicit Discovery(QObject *parent = nullptr);
    bool scan(const QString &manualHost);
};
