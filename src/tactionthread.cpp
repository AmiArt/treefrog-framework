/* Copyright (c) 2010-2019, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include "tfcore.h"
#include "thttpsocket.h"
#include "tsessionmanager.h"
#include "tsystemglobal.h"
#include "twebsocket.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <TActionThread>
#include <TAppSettings>
#include <TApplicationServerBase>
#include <THttpRequest>
#include <TSession>
#include <TWebApplication>
#include <atomic>

namespace {
std::atomic<int> threadCounter(0);
int keepAliveTimeout = -1;
}


int TActionThread::threadCount()
{
    return threadCounter.load();
}


bool TActionThread::waitForAllDone(int msec)
{
    int cnt;
    QElapsedTimer time;
    time.start();

    while ((cnt = threadCount()) > 0) {
        if (time.elapsed() > msec) {
            break;
        }

        Tf::msleep(5);
        qApp->processEvents();
    }
    tSystemDebug("waitForAllDone : remaining:%d", cnt);
    return cnt == 0;
}

/*!
  \class TActionThread
  \brief The TActionThread class provides a thread context.
*/

TActionThread::TActionThread(int socket, int maxThreads) :
    QThread(),
    TActionContext(),
    _maxThreads(maxThreads)
{
    TActionContext::socketDesc = socket;

    if (keepAliveTimeout < 0) {
        int timeout = Tf::appSettings()->value(Tf::HttpKeepAliveTimeout, "10").toInt();
        keepAliveTimeout = qMax(timeout, 0);
    }
}


TActionThread::~TActionThread()
{
    if (_httpSocket) {
        _httpSocket->deleteLater();
    }

    if (TActionContext::socketDesc > 0) {
        tf_close(TActionContext::socketDesc);
    }
}


void TActionThread::setSocketDescriptor(qintptr socket)
{
    if (TActionContext::socketDesc > 0) {
        tSystemWarn("Socket still open : %d   [%s:%d]", TActionContext::socketDesc, __FILE__, __LINE__);
        tf_close(TActionContext::socketDesc);
    }
    TActionContext::socketDesc = (int)socket;
}


void TActionThread::run()
{
    class Counter {
        std::atomic<int> &_num;

    public:
        Counter(std::atomic<int> &n) :
            _num(n) { _num++; }
        ~Counter() { _num--; }
    };
    Counter counter(threadCounter);

    QTimer checkKeepAliveTimer;
    checkKeepAliveTimer.setInterval(1000);
    connect(&checkKeepAliveTimer, &QTimer::timeout, this, &TActionThread::checkKeepAliveTimeout, Qt::DirectConnection);

    _httpSocket = new THttpSocket();
    connect(_httpSocket, &THttpSocket::newRequest, this, &TActionThread::processNewRequest, Qt::DirectConnection);
    connect(_httpSocket, &THttpSocket::stateChanged, this, &TActionThread::socketStateChanged, Qt::DirectConnection);

    if (Q_UNLIKELY(!_httpSocket->setSocketDescriptor(TActionContext::socketDesc))) {
        tSystemError("Failed setSocketDescriptor  sd:%d", TActionContext::socketDesc);
        emitError(_httpSocket->error());
        tf_close(TActionContext::socketDesc);
        goto socket_error;
    }
    TActionContext::socketDesc = 0;
    TDatabaseContext::setCurrentDatabaseContext(this);

    checkKeepAliveTimer.start(); // start keep-alive timer

    exec();  // run event loop

    closeHttpSocket();  // disconnect

socket_error:
    TActionContext::socketDesc = 0;
    TActionContext::release();
    TDatabaseContext::setCurrentDatabaseContext(nullptr);
    _httpSocket->deleteLater();
    _httpSocket = nullptr;
}


void TActionThread::emitError(int socketError)
{
    emit error(socketError);
}


void TActionThread::processNewRequest()
{
    while (_httpSocket->canReadRequest()) {
        try {
            QList<THttpRequest> reqs = _httpSocket->read();
            tSystemDebug("HTTP request count: %d", reqs.count());

            if (Q_UNLIKELY(reqs.isEmpty())) {
                break;
            }

            // WebSocket?
            QByteArray connectionHeader = reqs.at(0).header().rawHeader(QByteArrayLiteral("Connection")).toLower();
            if (Q_UNLIKELY(connectionHeader.contains("upgrade"))) {
                QByteArray upgradeHeader = reqs.at(0).header().rawHeader(QByteArrayLiteral("Upgrade")).toLower();
                tSystemDebug("Upgrade: %s", upgradeHeader.data());
                if (upgradeHeader == "websocket") {
                    // Switch to WebSocket
                    handshakeForWebSocket(reqs.at(0).header());
                }
                quit();
                break;
            }

            for (auto &req : reqs) {
                TActionContext::execute(req, _httpSocket->socketId());
            }

            if (keepAliveTimeout == 0) {
                quit();
                break;
            }

            if (_maxThreads > 0 && threadCount() >= _maxThreads) {
                // Do not keep-alive
                quit();
                break;
            }

        } catch (ClientErrorException &e) {
            tWarn("Caught ClientErrorException: status code:%d", e.statusCode());
            tSystemWarn("Caught ClientErrorException: status code:%d", e.statusCode());
            THttpResponseHeader header;
            TActionContext::writeResponse(e.statusCode(), header);
        } catch (std::exception &e) {
            tError("Caught Exception: %s", e.what());
            tSystemError("Caught Exception: %s", e.what());
            THttpResponseHeader header;
            TActionContext::writeResponse(Tf::InternalServerError, header);
        }
    }
}


void TActionThread::socketStateChanged()
{
    QAbstractSocket::SocketState ostate =_httpSocket->state();
    if (_httpSocket->state() != QAbstractSocket::ConnectedState) {
        if (_httpSocket->error() != QAbstractSocket::RemoteHostClosedError) {
            tSystemWarn("Socket error %d. Descriptor:%d", _httpSocket->error(), (int)_httpSocket->socketDescriptor());
        }
        quit();
    }
}


void TActionThread::checkKeepAliveTimeout()
{
    if (Q_UNLIKELY(keepAliveTimeout > 0 && _httpSocket->idleTime() >= keepAliveTimeout)) {
        tSystemWarn("KeepAlive timeout. Descriptor:%d", keepAliveTimeout, (int)_httpSocket->socketDescriptor());
        quit();
    }
}


qint64 TActionThread::writeResponse(THttpResponseHeader &header, QIODevice *body)
{
    if (keepAliveTimeout > 0) {
        header.setRawHeader(QByteArrayLiteral("Connection"), QByteArrayLiteral("Keep-Alive"));
    }
    return _httpSocket->write(&header, body);
}


void TActionThread::closeHttpSocket()
{
    _httpSocket->close();
}


bool TActionThread::handshakeForWebSocket(const THttpRequestHeader &header)
{
    if (!TWebSocket::searchEndpoint(header)) {
        return false;
    }

    // Switch to WebSocket
    int sd = TApplicationServerBase::duplicateSocket(_httpSocket->socketDescriptor());
    TWebSocket *ws = new TWebSocket(sd, _httpSocket->peerAddress(), header);
    connect(ws, SIGNAL(disconnected()), ws, SLOT(deleteLater()));
    ws->moveToThread(Tf::app()->thread());

    // WebSocket opening
    TSession session;
    QByteArray sessionId = header.cookie(TSession::sessionName());
    if (!sessionId.isEmpty()) {
        // Finds a session
        session = TSessionManager::instance().findSession(sessionId);
    }

    ws->startWorkerForOpening(session);
    return true;
}
