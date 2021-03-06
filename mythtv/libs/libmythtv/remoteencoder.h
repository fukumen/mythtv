#ifndef REMOTEENCODER_H_
#define REMOTEENCODER_H_

#include <stdint.h>

#include <QString>
#include <QMutex>
#include <QHash>
#include <QMap>

#include "mythexp.h"
#include "videoouttypes.h"
#include "tv.h"

class QStringList;
class ProgramInfo;
class MythSocket;

class MPUBLIC RemoteEncoder
{
  public:
    RemoteEncoder(int num, const QString &host, short port);
   ~RemoteEncoder(void);

    bool Setup(void);
    bool IsValidRecorder(void);
    int GetRecorderNumber(void);

    ProgramInfo *GetRecording(void);
    bool IsRecording(bool *ok = NULL);
    float GetFrameRate(void);
    long long GetFramesWritten(void);
    /// \brief Return value last returned by GetFramesWritten().
    long long GetCachedFramesWritten(void) const { return cachedFramesWritten; }
    long long GetFilePosition(void);
    long long GetFreeDiskSpace();
    long long GetMaxBitrate();
    int64_t GetKeyframePosition(uint64_t desired);
    void FillPositionMap(long long start, long long end,
                         QMap<long long, long long> &positionMap);
    void StopPlaying(void);
    void SpawnLiveTV(QString chainid, bool pip, QString startchan);
    void StopLiveTV(void);
    void PauseRecorder(void);
    void FinishRecording(void);
    void FrontendReady(void);
    void CancelNextRecording(bool cancel);

    void SetLiveRecording(bool recording);
    QString GetInput(void);
    QString SetInput(QString);
    int  GetPictureAttribute(PictureAttribute attr);
    int  ChangePictureAttribute(
        PictureAdjustType type, PictureAttribute attr, bool up);
    void ChangeChannel(int channeldirection);
    void ChangeDeinterlacer(int deint_mode);
    void ToggleChannelFavorite(QString);
    void SetChannel(QString channel);
    int  SetSignalMonitoringRate(int msec, bool notifyFrontend = true);
    uint GetSignalLockTimeout(QString input);
    bool CheckChannel(QString channel);
    bool ShouldSwitchToAnotherCard(QString channelid);
    bool CheckChannelPrefix(const QString&,uint&,bool&,QString&);
    void GetNextProgram(int direction,
                        QString &title, QString &subtitle, QString &desc, 
                        QString &category, QString &starttime, QString &endtime,
                        QString &callsign, QString &iconpath,
                        QString &channelname, QString &chanid,
                        QString &seriesid, QString &programid);
    void GetChannelInfo(QHash<QString, QString> &infoMap, uint chanid = 0);
    bool SetChannelInfo(const QHash<QString, QString> &infoMap);
    bool GetErrorStatus(void) { bool v = backendError; backendError = false; 
                                return v; }
 
  private:
    bool SendReceiveStringList(QStringList &strlist, uint min_reply_length = 0);

    int recordernum;

    MythSocket *controlSock;
    QMutex lock;

    QString remotehost;
    short remoteport;

    QString lastchannel;
    QString lastinput;

    bool backendError;
    long long cachedFramesWritten;
    QMap<QString,uint> cachedTimeout;
};

#endif

