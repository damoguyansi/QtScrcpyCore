#include <QCoreApplication>
#include <QDebug>
#include <QThread>

#include "videosocket.h"

VideoSocket::VideoSocket(QObject *parent) : QTcpSocket(parent)
{
}

VideoSocket::~VideoSocket()
{
}

qint32 VideoSocket::subThreadRecvData(quint8 *buf, qint32 bufSize)
{
    if (!buf) {
        return 0;
    }
    // this function cant call in main thread
    Q_ASSERT(QCoreApplication::instance()->thread() != QThread::currentThread());

    while (bytesAvailable() < bufSize) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return 0;
        }
        if (!waitForReadyRead(1000)) {
            if (state() == QAbstractSocket::UnconnectedState) {
                return 0;
            }
            continue;
        }
    }

    // recv data
    return static_cast<qint32>(
            read(reinterpret_cast<char *>(buf), static_cast<qint64>(bufSize)));
}
