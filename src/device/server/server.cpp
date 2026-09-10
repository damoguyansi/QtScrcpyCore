#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QTimerEvent>

#include "server.h"

#define DEVICE_NAME_FIELD_LENGTH 64
#define VIDEO_META_LENGTH 16
#define DEVICE_INFO_LENGTH (DEVICE_NAME_FIELD_LENGTH + VIDEO_META_LENGTH)
#define FORWARD_DUMMY_BYTE_LENGTH 1
#define SOCKET_NAME_PREFIX "scrcpy"
#define MAX_CONNECT_COUNT 30
#define MAX_RESTART_COUNT 1
// Same bounds the former blocking code used: waitForConnected(1000) and the
// 3000ms readInfo loop. Accept timeout (reverse mode) stays at 1000ms until the
// video socket arrives, then the header/control wait gets the 3000ms budget.
#define ACCEPT_DEADLINE_MS 1000
#define CONNECT_DEADLINE_MS 1000
#define HEADER_DEADLINE_MS 3000

static quint32 bufferRead32be(const quint8 *buf)
{
    return static_cast<quint32>((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

Server::Server(QObject *parent) : QObject(parent)
{
    connect(&m_workProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);
    connect(&m_serverProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);

    m_attemptDeadline.setSingleShot(true);
    connect(&m_attemptDeadline, &QTimer::timeout, this, &Server::onHandshakeDeadline);

    connect(&m_serverSocket, &QTcpServer::newConnection, this, [this]() {
        QTcpSocket *tmp = m_serverSocket.nextPendingConnection();
        if (VideoSocket *video = dynamic_cast<VideoSocket *>(tmp)) {
            m_videoSocket = video;
            if (!video->isValid()) {
                stop();
                emit serverStarted(false);
                return;
            }
            startReverseHandshake();
        } else {
            m_controlSocket = tmp;
            if (!m_controlSocket || !m_controlSocket->isValid()) {
                stop();
                emit serverStarted(false);
                return;
            }
            tryFinishReverse();
        }
    });
}

Server::~Server()
{
    abortPendingAttempt();
}

bool Server::parseDeviceInfo(const QByteArray &header, QString &deviceName, QSize &size)
{
    if (header.size() < DEVICE_INFO_LENGTH) {
        return false;
    }
    const char *name = header.constData();
    // In case the client sends garbage: never read past the name field.
    deviceName = QString::fromUtf8(name, static_cast<int>(qstrnlen(name, DEVICE_NAME_FIELD_LENGTH - 1)));

    // scrcpy 4.x: codec id (4 bytes), then session metadata (flags, width, height).
    const quint8 *buf = reinterpret_cast<const quint8 *>(header.constData());
    size.setWidth(static_cast<int>(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 8])));
    size.setHeight(static_cast<int>(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 12])));
    return true;
}

bool Server::tryReadDeviceInfo(QTcpSocket *socket, int skipBytes, QString &deviceName, QSize &size)
{
    const qint64 needed = skipBytes + DEVICE_INFO_LENGTH;
    if (!socket || socket->bytesAvailable() < needed) {
        return false;
    }
    const QByteArray buffer = socket->read(needed);
    if (buffer.size() != needed) {
        return false;
    }
    return parseDeviceInfo(buffer.mid(skipBytes), deviceName, size);
}

bool Server::pushServer()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.push(m_params.serial, m_params.serverLocalPath, m_params.serverRemotePath);
    return true;
}

bool Server::enableTunnelReverse()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.reverse(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')), m_params.localPort);
    return true;
}

bool Server::disableTunnelReverse()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->reverseRemove(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}

bool Server::enableTunnelForward()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.forward(m_params.serial, m_params.localPort, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}
bool Server::disableTunnelForward()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->forwardRemove(m_params.serial, m_params.localPort);
    return true;
}

bool Server::execute()
{
    if (m_serverProcess.isRuning()) {
        m_serverProcess.kill();
    }
    QStringList args;
    args << "shell";
    args << QString("CLASSPATH=%1").arg(m_params.serverRemotePath);
    args << "app_process";

#ifdef SERVER_DEBUGGER
#define SERVER_DEBUGGER_PORT "5005"

    args <<
#ifdef SERVER_DEBUGGER_METHOD_NEW
        /* Android 9 and above */
        "-XjdwpProvider:internal -XjdwpOptions:transport=dt_socket,suspend=y,server=y,address="
#else
        /* Android 8 and below */
        "-agentlib:jdwp=transport=dt_socket,suspend=y,server=y,address="
#endif
        SERVER_DEBUGGER_PORT,
#endif

        args << "/"; // unused;
    args << "com.genymobile.scrcpy.Server";
    args << m_params.serverVersion;

    args << QString("video_bit_rate=%1").arg(QString::number(m_params.bitRate));
    const bool cameraMode = m_params.videoSource == qsc::VIDEO_SOURCE_CAMERA;
    if (cameraMode) {
        args << "video_source=camera";
        if (!m_params.cameraId.isEmpty()) {
            args << QString("camera_id=%1").arg(m_params.cameraId);
        } else {
            args << QString("camera_facing=%1")
                        .arg(m_params.cameraFacing == qsc::CAMERA_FACING_FRONT ? "front" : "back");
        }
    }
    if (!m_params.logLevel.isEmpty()) {
        args << QString("log_level=%1").arg(m_params.logLevel);
    }
    if (m_params.maxSize > 0) {
        args << QString("max_size=%1").arg(QString::number(m_params.maxSize));
    }
    if (m_params.maxFps > 0) {
        args << QString("max_fps=%1").arg(QString::number(m_params.maxFps));
    }

    // capture_orientation=@90
    // 有@表示锁定，没@不锁定
    // 有值表示指定方向，没值表示原始方向
    if (1 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@%1").arg(m_params.captureOrientation);
    } else if (2 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@");
    } else {
        args << QString("capture_orientation=%1").arg(m_params.captureOrientation);
    }
    if (m_tunnelForward) {
        args << QString("tunnel_forward=true");
    }
    if (!cameraMode) {
        if (!m_params.crop.isEmpty()) {
            args << QString("crop=%1").arg(m_params.crop);
        }
        if (!m_params.newDisplay.isEmpty()) {
            args << QString("new_display=%1").arg(m_params.newDisplay);
            if (m_params.flexDisplay) {
                args << "flex_display=true";
            }
            if (!m_params.vdDestroyContent) {
                args << "vd_destroy_content=false";
            }
            if (!m_params.vdSystemDecorations) {
                args << "vd_system_decorations=false";
            }
            if (!m_params.displayImePolicy.isEmpty()) {
                args << QString("display_ime_policy=%1").arg(m_params.displayImePolicy);
            }
        } else if (m_params.displayId > 0) {
            args << QString("display_id=%1").arg(m_params.displayId);
        }
    }
    if (!m_params.control) {
        args << QString("control=false");
    }
    // 默认是0，不需要设置
    // args << "display_id=0";
    // 默认是false，不需要设置
    // args << "show_touches=false";
    if (!cameraMode) {
        if (m_params.stayAwake) {
            args << QString("stay_awake=true");
        }
        if (m_params.keepActive) {
            args << "keep_active=true";
        }
    }
    // code option
    // https://github.com/Genymobile/scrcpy/commit/080a4ee3654a9b7e96c8ffe37474b5c21c02852a
    // <https://d.android.com/reference/android/media/MediaFormat>
    if (!m_params.codecOptions.isEmpty()) {
        args << QString("video_codec_options=%1").arg(m_params.codecOptions);
    }
    if (!m_params.codecName.isEmpty()) {
        args << QString("video_encoder=%1").arg(m_params.codecName);
    }
    args << "audio=false";
    if (!m_params.clipboardAutosync) {
        args << "clipboard_autosync=false";
    }
    // 服务端默认-1，可不传
    if (-1 != m_params.scid) {
        args << QString("scid=%1").arg(m_params.scid, 8, 16, QChar('0'));
    }

    // 默认是false，不需要设置
    // args << "power_off_on_close=false";

    // 下面的参数都用服务端默认值即可，尽量减少参数传递，传参太长导致三星手机报错：stack corruption detected (-fstack-protector)
    /*
    args << "clipboard_autosync=true";    
    args << "downsize_on_error=true";
    args << "cleanup=true";
    args << "power_on=true";
    
    args << "send_device_meta=true";
    args << "send_frame_meta=true";
    args << "send_dummy_byte=true";
    args << "raw_video_stream=false";
    */

#ifdef SERVER_DEBUGGER
    qInfo("Server debugger waiting for a client on device port " SERVER_DEBUGGER_PORT "...");
    // From the computer, run
    //     adb forward tcp:5005 tcp:5005
    // Then, from Android Studio: Run > Debug > Edit configurations...
    // On the left, click on '+', "Remote", with:
    //     Host: localhost
    //     Port: 5005
    // Then click on "Debug"
#endif

    // adb -s P7C0218510000537 shell CLASSPATH=/data/local/tmp/scrcpy-server app_process / com.genymobile.scrcpy.Server 0 8000000 false
    // mark: crop input format: "width:height:x:y" or "" for no crop, for example: "100:200:0:0"
    // 这条adb命令是阻塞运行的，m_serverProcess进程不会退出了
    m_serverProcess.execute(m_params.serial, args);
    return true;
}

bool Server::start(Server::ServerParams params)
{
    m_params = params;
    m_serverStartStep = SSS_PUSH;
    return startServerByStep();
}

bool Server::connectTo()
{
    if (SSS_RUNNING != m_serverStartStep) {
        qWarning("server not run");
        return false;
    }

    if (!m_tunnelForward && !m_videoSocket) {
        startAcceptTimeoutTimer(ACCEPT_DEADLINE_MS);
        return true;
    }

    startConnectTimeoutTimer();
    return true;
}

bool Server::isReverse()
{
    return !m_tunnelForward;
}

Server::ServerParams Server::getParams()
{
    return m_params;
}

void Server::timerEvent(QTimerEvent *event)
{
    if (event && m_acceptTimeoutTimer == event->timerId()) {
        stopAcceptTimeoutTimer();
        emit serverStarted(false);
    } else if (event && m_connectTimeoutTimer == event->timerId()) {
        onConnectTimer();
    }
}

VideoSocket* Server::removeVideoSocket()
{
    VideoSocket* socket = m_videoSocket;
    m_videoSocket = Q_NULLPTR;
    return socket;
}

QTcpSocket *Server::getControlSocket()
{
    return m_controlSocket;
}

void Server::stop()
{
    // Cancel an in-flight forward attempt (sockets + lambdas + deadline) and any
    // accepted-but-not-yet-handed-over reverse video socket. removeVideoSocket()
    // has already nulled m_videoSocket once the demuxer owns it, so this never
    // touches a live stream.
    abortPendingAttempt();
    if (m_videoSocket) {
        disconnect(m_videoSocket, Q_NULLPTR, this, Q_NULLPTR);
        m_videoSocket->abort();
        m_videoSocket->deleteLater();
        m_videoSocket = Q_NULLPTR;
    }
    m_reverseInfoReady = false;

    if (m_tunnelForward) {
        stopConnectTimeoutTimer();
    } else {
        stopAcceptTimeoutTimer();
    }

    if (m_controlSocket) {
        m_controlSocket->close();
        m_controlSocket->deleteLater();
    }
    // ignore failure
    m_serverProcess.kill();
    if (m_tunnelEnabled) {
        if (m_tunnelForward) {
            disableTunnelForward();
        } else {
            disableTunnelReverse();
        }
        m_tunnelForward = false;
        m_tunnelEnabled = false;
    }
    m_serverSocket.close();
}

bool Server::startServerByStep()
{
    bool stepSuccess = false;
    // push, enable tunnel et start the server
    if (SSS_NULL != m_serverStartStep) {
        switch (m_serverStartStep) {
        case SSS_PUSH:
            stepSuccess = pushServer();
            break;
        case SSS_ENABLE_TUNNEL_REVERSE:
            stepSuccess = enableTunnelReverse();
            break;
        case SSS_ENABLE_TUNNEL_FORWARD:
            stepSuccess = enableTunnelForward();
            break;
        case SSS_EXECUTE_SERVER:
            // server will connect to our server socket
            stepSuccess = execute();
            break;
        default:
            break;
        }
    }

    if (!stepSuccess) {
        emit serverStarted(false);
    }
    return stepSuccess;
}

void Server::startReverseHandshake()
{
    m_reverseInfoReady = false;
    // The device connected; give the header and the control socket the same
    // 3000ms budget the blocking readInfo loop used to have.
    startAcceptTimeoutTimer(HEADER_DEADLINE_MS);
    const quint32 generation = ++m_attemptGeneration;
    connect(m_videoSocket, &QIODevice::readyRead, this, [this, generation]() {
        if (generation == m_attemptGeneration) {
            onReverseVideoReadyRead();
        }
    });
    // Bytes may already be buffered before the slot was connected.
    onReverseVideoReadyRead();
}

void Server::onReverseVideoReadyRead()
{
    if (m_reverseInfoReady || !m_videoSocket) {
        return;
    }
    if (!tryReadDeviceInfo(m_videoSocket, 0, m_deviceName, m_deviceSize)) {
        // Partial header: wait for the next readyRead.
        return;
    }
    m_reverseInfoReady = true;
    // Demuxer::installVideoSocket moves the socket to its own thread; no Server
    // slot may stay connected to it after this point.
    disconnect(m_videoSocket, Q_NULLPTR, this, Q_NULLPTR);
    tryFinishReverse();
}

void Server::tryFinishReverse()
{
    // Emit once both the parsed header and the control socket are here,
    // whichever arrives last.
    if (!m_reverseInfoReady || !m_controlSocket) {
        return;
    }
    stopAcceptTimeoutTimer();
    // we don't need the server socket anymore, just m_videoSocket is ok
    m_serverSocket.close();
    // we don't need the adb tunnel anymore
    disableTunnelReverse();
    m_tunnelEnabled = false;
    emit serverStarted(true, m_deviceName, m_deviceSize);
}

void Server::startForwardAttempt()
{
    const quint32 generation = ++m_attemptGeneration;
    m_pendingVideoConnected = false;
    m_pendingControlConnected = false;

    // Parentless on purpose: the video socket is later moved to the demux thread.
    VideoSocket *video = new VideoSocket();
    QTcpSocket *control = new QTcpSocket();
    m_pendingVideoSocket = video;
    m_pendingControlSocket = control;

    connect(video, &QAbstractSocket::connected, this, [this, generation]() {
        if (generation != m_attemptGeneration) return;
        m_pendingVideoConnected = true;
        if (m_pendingControlConnected) m_attemptDeadline.start(HEADER_DEADLINE_MS);
        onForwardVideoReadyRead();
    });
    connect(control, &QAbstractSocket::connected, this, [this, generation]() {
        if (generation != m_attemptGeneration) return;
        m_pendingControlConnected = true;
        if (m_pendingVideoConnected) m_attemptDeadline.start(HEADER_DEADLINE_MS);
        onForwardVideoReadyRead();
    });
    connect(video, &QIODevice::readyRead, this, [this, generation]() {
        if (generation == m_attemptGeneration) onForwardVideoReadyRead();
    });
    connect(video, &QAbstractSocket::errorOccurred, this, [this, generation](QAbstractSocket::SocketError error) {
        if (generation == m_attemptGeneration) onForwardSocketError(error);
    });
    connect(control, &QAbstractSocket::errorOccurred, this, [this, generation](QAbstractSocket::SocketError error) {
        if (generation == m_attemptGeneration) onForwardSocketError(error);
    });

    m_attemptDeadline.start(CONNECT_DEADLINE_MS);
    video->connectToHost(QHostAddress::LocalHost, m_params.localPort);
    control->connectToHost(QHostAddress::LocalHost, m_params.localPort);
}

void Server::onForwardVideoReadyRead()
{
    if (!m_pendingVideoConnected || !m_pendingControlConnected || !m_pendingVideoSocket) {
        return;
    }
    QString deviceName;
    QSize deviceSize;
    // devices will send 1 byte first on tunnel forward mode
    if (!tryReadDeviceInfo(m_pendingVideoSocket, FORWARD_DUMMY_BYTE_LENGTH, deviceName, deviceSize)) {
        return;
    }
    m_deviceName = deviceName;
    m_deviceSize = deviceSize;
    finishForwardAttempt();
}

void Server::onForwardSocketError(QAbstractSocket::SocketError error)
{
    const bool connectPhase = !(m_pendingVideoConnected && m_pendingControlConnected);
    if (connectPhase) {
        // Connecting to the local adb server is fast; a failure here is not retried.
        qWarning("socket connect to server failed: %d", static_cast<int>(error));
        failForwardAttempt(true);
    } else {
        // adb accepted but the device side is not listening yet (RemoteHostClosed
        // before the header): retry on the next connect timer tick.
        qWarning("video socket connect to server read device info failed, try again");
        failForwardAttempt(false);
    }
}

void Server::onHandshakeDeadline()
{
    if (!m_pendingVideoSocket && !m_pendingControlSocket) {
        return;
    }
    const bool connectPhase = !(m_pendingVideoConnected && m_pendingControlConnected);
    failForwardAttempt(connectPhase);
}

void Server::finishForwardAttempt()
{
    VideoSocket *video = m_pendingVideoSocket;
    QTcpSocket *control = m_pendingControlSocket;
    // Detach every handshake connection before the socket is moved to the demux thread.
    disconnect(video, Q_NULLPTR, this, Q_NULLPTR);
    disconnect(control, Q_NULLPTR, this, Q_NULLPTR);
    m_pendingVideoSocket = Q_NULLPTR;
    m_pendingControlSocket = Q_NULLPTR;
    m_pendingVideoConnected = false;
    m_pendingControlConnected = false;
    m_attemptDeadline.stop();
    ++m_attemptGeneration;
    stopConnectTimeoutTimer();

    m_videoSocket = video;
    // devices will send 1 byte first on tunnel forward mode
    control->read(FORWARD_DUMMY_BYTE_LENGTH);
    m_controlSocket = control;
    // we don't need the adb tunnel anymore
    disableTunnelForward();
    m_tunnelEnabled = false;
    m_restartCount = 0;
    emit serverStarted(true, m_deviceName, m_deviceSize);
}

void Server::failForwardAttempt(bool fatal)
{
    abortPendingAttempt();
    if (fatal) {
        m_connectCount = MAX_CONNECT_COUNT;
    }
    if (MAX_CONNECT_COUNT <= m_connectCount++) {
        stopConnectTimeoutTimer();
        stop();
        if (MAX_RESTART_COUNT > m_restartCount++) {
            qWarning("restart server auto");
            start(m_params);
        } else {
            m_restartCount = 0;
            emit serverStarted(false);
        }
    }
    // else: the 300ms connect timer starts the next attempt
}

void Server::abortPendingAttempt()
{
    // Invalidate every lambda already queued for this attempt.
    ++m_attemptGeneration;
    m_attemptDeadline.stop();
    QTcpSocket *sockets[2] = { m_pendingVideoSocket.data(), m_pendingControlSocket.data() };
    m_pendingVideoSocket = Q_NULLPTR;
    m_pendingControlSocket = Q_NULLPTR;
    m_pendingVideoConnected = false;
    m_pendingControlConnected = false;
    for (QTcpSocket *socket : sockets) {
        if (!socket) {
            continue;
        }
        // abort() emits errorOccurred/disconnected synchronously: detach first.
        disconnect(socket, Q_NULLPTR, this, Q_NULLPTR);
        socket->abort();
        socket->deleteLater();
    }
}

void Server::startAcceptTimeoutTimer(int timeoutMs)
{
    stopAcceptTimeoutTimer();
    m_acceptTimeoutTimer = startTimer(timeoutMs);
}

void Server::stopAcceptTimeoutTimer()
{
    if (m_acceptTimeoutTimer) {
        killTimer(m_acceptTimeoutTimer);
        m_acceptTimeoutTimer = 0;
    }
}

void Server::startConnectTimeoutTimer()
{
    stopConnectTimeoutTimer();
    m_connectTimeoutTimer = startTimer(300);
}

void Server::stopConnectTimeoutTimer()
{
    if (m_connectTimeoutTimer) {
        killTimer(m_connectTimeoutTimer);
        m_connectTimeoutTimer = 0;
    }
    m_connectCount = 0;
}

void Server::onConnectTimer()
{
    // device server need time to start
    // 这里连接太早时间不够导致安卓监听socket还没有建立，读头会失败，所以采取定时重试策略
    // 每隔300ms检查一次，最多尝试MAX_CONNECT_COUNT次；一次尝试进行中时本 tick 只是等待。
    if (m_pendingVideoSocket || m_pendingControlSocket) {
        return;
    }
    startForwardAttempt();
}

void Server::onWorkProcessResult(qsc::AdbProcess::ADB_EXEC_RESULT processResult)
{
    if (sender() == &m_workProcess) {
        if (SSS_NULL != m_serverStartStep) {
            switch (m_serverStartStep) {
            case SSS_PUSH:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    if (m_params.useReverse) {
                        m_serverStartStep = SSS_ENABLE_TUNNEL_REVERSE;
                    } else {
                        m_tunnelForward = true;
                        m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    }
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb push failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false);
                }
                break;
            case SSS_ENABLE_TUNNEL_REVERSE:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    // At the application level, the device part is "the server" because it
                    // serves video stream and control. However, at the network level, the
                    // client listens and the server connects to the client. That way, the
                    // client can listen before starting the server app, so there is no need to
                    // try to connect until the server socket is listening on the device.
                    m_serverSocket.setMaxPendingConnections(2);
                    if (!m_serverSocket.listen(QHostAddress::LocalHost, m_params.localPort)) {
                        qCritical() << QString("Could not listen on port %1").arg(m_params.localPort).toStdString().c_str();
                        m_serverStartStep = SSS_NULL;
                        disableTunnelReverse();
                        emit serverStarted(false);
                        break;
                    }

                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    // 有一些设备reverse会报错more than o'ne device，adb的bug
                    // https://github.com/Genymobile/scrcpy/issues/5
                    qCritical("adb reverse failed");
                    m_tunnelForward = true;
                    m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    startServerByStep();
                }
                break;
            case SSS_ENABLE_TUNNEL_FORWARD:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb forward failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false);
                }
                break;
            default:
                break;
            }
        }
    }
    if (sender() == &m_serverProcess) {
        if (SSS_EXECUTE_SERVER == m_serverStartStep) {
            if (qsc::AdbProcess::AER_SUCCESS_START == processResult) {
                m_serverStartStep = SSS_RUNNING;
                m_tunnelEnabled = true;
                connectTo();
            } else if (qsc::AdbProcess::AER_ERROR_START == processResult) {
                if (!m_tunnelForward) {
                    m_serverSocket.close();
                    disableTunnelReverse();
                } else {
                    disableTunnelForward();
                }
                qCritical("adb shell start server failed");
                m_serverStartStep = SSS_NULL;
                emit serverStarted(false);
            }
        } else if (SSS_RUNNING == m_serverStartStep) {
            m_serverStartStep = SSS_NULL;
            emit serverStoped();
        }
    }
}
