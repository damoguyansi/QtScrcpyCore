#ifndef SERVER_H
#define SERVER_H

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QTimer>

#include "../../../include/QtScrcpyCoreDef.h"
#include "adbprocess.h"
#include "tcpserver.h"
#include "videosocket.h"

class Server : public QObject
{
    Q_OBJECT

    enum SERVER_START_STEP
    {
        SSS_NULL,
        SSS_PUSH,
        SSS_ENABLE_TUNNEL_REVERSE,
        SSS_ENABLE_TUNNEL_FORWARD,
        SSS_EXECUTE_SERVER,
        SSS_RUNNING,
    };

public:
    struct ServerParams
    {
        // necessary
        QString serial = "";              // 设备序列号
        QString serverLocalPath = "";     // 本地安卓server路径

        // optional
        QString serverRemotePath = "/data/local/tmp/scrcpy-server.jar";    // 要推送到远端设备的server路径
        quint16 localPort = 27183;     // reverse时本地监听端口
        quint16 maxSize = 720;         // 视频分辨率
        quint32 bitRate = 8000000;     // 视频比特率
        quint32 maxFps = 0;            // 视频最大帧率
        qsc::VideoSource videoSource = qsc::VIDEO_SOURCE_DISPLAY;
        qsc::CameraFacing cameraFacing = qsc::CAMERA_FACING_BACK;
        QString cameraId = "";
        bool useReverse = true;        // true:先使用adb reverse，失败后自动使用adb forward；false:直接使用adb forward
        int captureOrientationLock = 0; // 是否锁定采集方向 0不锁定 1锁定指定方向 2锁定原始方向
        int captureOrientation = 0;     // 采集方向 0 90 180 270
        int stayAwake = false;         // 是否保持唤醒
        QString serverVersion = "4.1"; // server版本
        QString logLevel = "debug";  // log级别 verbose/debug/info/warn/error
        // 编码选项 ""表示默认
        // 例如 CodecOptions="profile=1,level=2"
        // 更多编码选项参考 https://d.android.com/reference/android/media/MediaFormat
        QString codecOptions = "";
        // 指定编码器名称(必须是H.264编码器)，""表示默认
        // 例如 CodecName="OMX.qcom.video.encoder.avc"
        QString codecName = "";

        QString crop = "";             // 视频裁剪
        qint32 displayId = 0;
        QString newDisplay = "";
        bool flexDisplay = false;
        bool vdDestroyContent = true;
        bool vdSystemDecorations = true;
        QString displayImePolicy = "";
        bool keepActive = false;
        bool clipboardAutosync = true;
        bool control = true;           // 安卓端是否接收键鼠控制
        qint32 scid = -1;             // 随机数，作为localsocket名字后缀，方便同时连接同一个设备多次
    };

    explicit Server(QObject *parent = nullptr);
    virtual ~Server();

    bool start(Server::ServerParams params);
    void stop();
    bool isReverse();
    Server::ServerParams getParams();
    VideoSocket *removeVideoSocket();
    QTcpSocket *getControlSocket();

    // Parse the 80-byte scrcpy handshake (64-byte device name + 16-byte video
    // meta). header[0] must be the first byte of the device name (the forward
    // mode dummy byte already stripped). Pure; exposed for unit tests.
    static bool parseDeviceInfo(const QByteArray &header, QString &deviceName, QSize &size);

signals:
    void serverStarted(bool success, const QString &deviceName = "", const QSize &size = QSize());
    void serverStoped();

private slots:
    void onWorkProcessResult(qsc::AdbProcess::ADB_EXEC_RESULT processResult);

protected:
    void timerEvent(QTimerEvent *event);

private:
    bool pushServer();
    bool enableTunnelReverse();
    bool disableTunnelReverse();
    bool enableTunnelForward();
    bool disableTunnelForward();
    bool execute();
    bool connectTo();
    bool startServerByStep();
    void startAcceptTimeoutTimer(int timeoutMs);
    void stopAcceptTimeoutTimer();
    void startConnectTimeoutTimer();
    void stopConnectTimeoutTimer();
    void onConnectTimer();

    // Non-blocking header read: consumes exactly skipBytes + 80 bytes once they
    // are buffered, never more, so following video bytes stay in the socket.
    bool tryReadDeviceInfo(QTcpSocket *socket, int skipBytes, QString &deviceName, QSize &size);

    // Forward mode ("adb forward"): one asynchronous attempt at a time, paced
    // by the 300ms connect timer. Server and Device live on the GUI thread, so
    // the former waitForConnected/waitForReadyRead loops froze the UI for up to
    // several seconds per device during batch starts.
    void startForwardAttempt();
    void onForwardVideoReadyRead();
    void onForwardSocketError(QAbstractSocket::SocketError error);
    void onHandshakeDeadline();
    void finishForwardAttempt();
    void failForwardAttempt(bool fatal);
    void abortPendingAttempt();

    // Reverse mode ("adb reverse"): the device connects to our TcpServer.
    void startReverseHandshake();
    void onReverseVideoReadyRead();
    void tryFinishReverse();

private:
    qsc::AdbProcess m_workProcess;
    qsc::AdbProcess m_serverProcess;
    TcpServer m_serverSocket; // only used if !tunnel_forward
    QPointer<VideoSocket> m_videoSocket = Q_NULLPTR;
    QPointer<QTcpSocket> m_controlSocket = Q_NULLPTR;
    QPointer<VideoSocket> m_pendingVideoSocket = Q_NULLPTR;
    QPointer<QTcpSocket> m_pendingControlSocket = Q_NULLPTR;
    QTimer m_attemptDeadline;
    quint32 m_attemptGeneration = 0;
    bool m_pendingVideoConnected = false;
    bool m_pendingControlConnected = false;
    bool m_reverseInfoReady = false;
    bool m_tunnelEnabled = false;
    bool m_tunnelForward = false; // use "adb forward" instead of "adb reverse"
    int m_acceptTimeoutTimer = 0;
    int m_connectTimeoutTimer = 0;
    quint32 m_connectCount = 0;
    quint32 m_restartCount = 0;
    QString m_deviceName = "";
    QSize m_deviceSize = QSize();
    ServerParams m_params;

    SERVER_START_STEP m_serverStartStep = SSS_NULL;
};

#endif // SERVER_H
