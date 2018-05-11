
#include "ts_proto_client.h"

#include <qbluetoothsocket.h>
#include <qbytearray.h>

#include <sys/time.h>


#include "ts_proto.h"




void tsProtoClient::syncTime()
{
    m_timeIsSynchronized = false;

    tsMsg_t msg = {
        .timestamp =  get_ts_time(),
        .cmd = tsProtoCmds_t::timeSyncReq,
        .data = {"Hello Kolsha"},
    };

    if(msg.timestamp.tv_sec == 0){
        return ;
    }

    prepare_msg(&msg);




    char *msg_bytes = reinterpret_cast<char*>(&msg);
    if(msg_bytes == nullptr){
        // error msg
        return ;
    }

    QByteArray ts_msg;
    ts_msg.append(msg_bytes, sizeof(tsMsg_t));

    socket->write(ts_msg);
}

void tsProtoClient::initTimer()
{
    m_timeIsSynchronized = false;
    m_syncTimer = new QTimer(this);
    if(!m_syncTimer){
        return ;
    }
    connect(m_syncTimer, SIGNAL(timeout()), this, SLOT(syncTime()));
    m_syncTimer->setInterval(5000);
    m_syncTimer->start();
    syncTime();
}

void tsProtoClient::freeTimer()
{
    m_timeIsSynchronized = false;
    if(m_syncTimer){
        while(m_syncTimer->isActive()){
            m_syncTimer->stop();
        }

        delete m_syncTimer;
        m_syncTimer = 0;
    }
}

tsProtoClient::tsProtoClient(QObject *parent)
    :   QObject(parent), socket(0)
{
    m_lastMsgTS = {0, 0};
    m_maxdT = std::unique_ptr<AverageBuffer<uint32_t>>(new AverageBuffer<uint32_t>(30, 100));
}

tsProtoClient::~tsProtoClient()
{
    stopClient();
}


void tsProtoClient::startClient(const QBluetoothServiceInfo &remoteService)
{
    if (socket)
        return;

    // Connect to service
    socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol);
    qDebug() << "Create socket";
    socket->connectToService(remoteService);
    qDebug() << "ConnectToService done";

    connect(socket, SIGNAL(readyRead()), this, SLOT(readSocket()));
    connect(socket, SIGNAL(connected()), this, SLOT(connected()));
    connect(socket, SIGNAL(disconnected()), this, SLOT(m_disconnected()));

    qDebug() << "Signaling done";
}


void tsProtoClient::stopClient()
{
    delete socket;
    socket = 0;

    freeTimer();
}


void tsProtoClient::readSocket()
{
    if (!socket)
        return;

    tsTime_t curTime = get_ts_time();

    while (socket->bytesAvailable() > 0) {
        /*emit messageReceived(socket->peerName(),
                             QString("----------------------------------"));
                             */
        QByteArray line = socket->readAll();
        for(size_t pos = 0; pos < (line.length() / sizeof(tsMsg_t)); pos++){

            uint8_t * tmp = (uint8_t*)(line.data() + pos * sizeof(tsMsg_t));
            if(*tmp != tsProto_Version){
                qDebug() << "Skip msg, cause version wrong";
                continue;
            }

            tsMsg_t *msg = reinterpret_cast< tsMsg_t *>(tmp);
            if(!msg)
                continue;
            if(msg->sign != sign_msg(msg)){
                qDebug() << "Skip msg: " << msg->data << "cause sign wrong";
                continue;
            }
            if(msg->cmd == timeSyncResponse){
                m_timeIsSynchronized = true;
                m_lastMsgTS = msg->timestamp;
                continue;
            }
            if(!m_timeIsSynchronized){
                continue;
            }



            uint32_t dt = get_ts_delta_time(&curTime, &(msg->timestamp));
            m_maxdT->put(dt);
            m_latency = m_maxdT->getAverage();
            emit latencyChanged(m_latency);

            uint32_t maxdT = m_latency * 1.5;
            if(dt > maxdT){
                m_maxdT->put(dt * 10);
                qDebug() << "Skip msg: " << (char*)msg->data <<
                            "cause dT exceeded " << maxdT << " - " << dt;
                continue;
            }

            m_lastMsgTS = msg->timestamp;

            if(msg->cmd == dataOut){
                emit messageReceived(socket->peerName(),
                                     QString::fromUtf8((char*)msg->data, tsProto_MSG_DATA_LEN));
            }

        }
        //qDebug() << line;

    }
}


void tsProtoClient::sendMessage(const QString &message)
{
    QByteArray text = message.toUtf8() + '\n';
    socket->write(text);
}


void tsProtoClient::connected()
{
    initTimer();
    emit connected(socket->peerName());
}

void tsProtoClient::m_disconnected()
{
    freeTimer();
    emit disconnected();
}

