#include <QDebug>

#include "bufferutil.h"
#include "devicemsg.h"

DeviceMsg::DeviceMsg(QObject *parent) : QObject(parent) {}

DeviceMsg::~DeviceMsg()
{
    if (DMT_GET_CLIPBOARD == m_data.type && Q_NULLPTR != m_data.clipboardMsg.text) {
        delete [] m_data.clipboardMsg.text;
        m_data.clipboardMsg.text = Q_NULLPTR;
    }
}

DeviceMsg::DeviceMsgType DeviceMsg::type()
{
    return m_data.type;
}

void DeviceMsg::getClipboardMsgData(QString &text)
{
    text = QString::fromUtf8(m_data.clipboardMsg.text);
}

qint32 DeviceMsg::deserialize(QByteArray &byteArray)
{
    QBuffer buf(&byteArray);
    buf.open(QBuffer::ReadOnly);

    qint64 len = buf.size();
    char c = 0;
    qint32 ret = 0;

    if (len < 1) {
        // at least the message type
        return 0; // not available
    }

    buf.getChar(&c);
    const DeviceMsgType type = static_cast<DeviceMsgType>(c);
    switch (type) {
    case DMT_GET_CLIPBOARD: {
        if (len < 5) {
            ret = 0; // not available
            break;
        }

        quint32 clipboardLen = BufferUtil::read32(buf);
        if (clipboardLen > static_cast<quint32>(len - 5)) {
            ret = 0; // not available
            break;
        }

        QByteArray text = buf.read(clipboardLen);
        const auto textLength = text.size();
        delete [] m_data.clipboardMsg.text;
        m_data.clipboardMsg.text = new char[static_cast<size_t>(textLength) + 1];
        memcpy(m_data.clipboardMsg.text, text.data(), static_cast<size_t>(textLength));
        m_data.clipboardMsg.text[textLength] = '\0';

        ret = static_cast<qint32>(5 + clipboardLen);
        break;
    }
    case DMT_ACK_CLIPBOARD:
        if (len < 9) {
            ret = 0; // not available
            break;
        }
        ret = 9;
        break;
    case DMT_UHID_OUTPUT: {
        if (len < 5) {
            ret = 0; // not available
            break;
        }

        BufferUtil::read16(buf); // id
        quint16 dataLen = BufferUtil::read16(buf);
        if (dataLen > len - 5) {
            ret = 0; // not available
            break;
        }
        ret = 5 + dataLen;
        break;
    }
    default:
        qWarning("Unsupported device msg type: %d", static_cast<int>(type));
        ret = -1; // error, the protocol does not expose a generic frame size
    }

    if (ret > 0) {
        m_data.type = type;
    }
    buf.close();
    return ret;
}
