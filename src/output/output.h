#pragma once
#include <set>
#include <cstdlib>
#include <map>
#include <mist/config.h>
#include <mist/json.h>
#include <mist/flv_tag.h>
#include <mist/timing.h>
#include <mist/dtsc.h>
#include <mist/socket.h>
#include <mist/shared_memory.h>
#include "../io.h"

namespace Mist {

  /// This struct keeps packet information sorted in playback order, so the
  /// Mist::Output class knows when to buffer which packet.
  struct sortedPageInfo {
    bool operator < (const sortedPageInfo & rhs) const {
      if (time < rhs.time) {
        return true;
      }
      return (time == rhs.time && tid < rhs.tid);
    }
    unsigned int tid;
    long long unsigned int time;
    unsigned int offset;
  };

  /// The output class is intended to be inherited by MistOut process classes.
  /// It contains all generic code and logic, while the child classes implement
  /// anything specific to particular protocols or containers.
  /// It contains several virtual functions, that may be overridden to "hook" into
  /// the streaming process at those particular points, simplifying child class
  /// logic and implementation details.
  class Output : public InOutBase {
    public:
      //constructor and destructor
      Output(Socket::Connection & conn);
      //static members for initialization and capabilities
      static void init(Util::Config * cfg);
      static JSON::Value capa;
      /*LTS-START*/
      std::string reqUrl;
      /*LTS-END*/
      //non-virtual generic functions
      virtual int run();
      virtual void stats(bool force = false);
      void seek(unsigned long long pos);
      bool seek(unsigned int tid, unsigned long long pos, bool getNextKey = false);
      void stop();
      uint64_t currentTime();
      uint64_t startTime();
      uint64_t endTime();
      uint64_t liveTime();
      void setBlocking(bool blocking);
      void updateMeta();
      void selectTrack(const std::string &trackType, const std::string &trackVal); /*LTS*/
      void selectDefaultTracks();
      bool connectToFile(std::string file);
      static bool listenMode(){return true;}
      uint32_t currTrackCount() const;
      virtual bool isReadyForPlay();
      //virtuals. The optional virtuals have default implementations that do as little as possible.
      /// This function is called whenever a packet is ready for sending.
      /// Inside it, thisPacket is guaranteed to contain a valid packet.
      virtual void sendNext() {}//REQUIRED! Others are optional.
      bool prepareNext();
      virtual void dropTrack(uint32_t trackId, std::string reason, bool probablyBad = true);
      virtual void onRequest();
      static void listener(Util::Config & conf, int (*callback)(Socket::Connection & S));
      virtual void initialSeek();
      virtual bool liveSeek();
      virtual bool onFinish() {
        return false;
      }
      void reconnect();
      void disconnect();
      virtual void initialize();
      virtual void sendHeader();
      virtual void onFail();
      virtual void requestHandler();
    private://these *should* not be messed with in child classes.
      /*LTS-START*/
      void Log(std::string type, std::string message);
      bool checkLimits();
      bool isBlacklisted(std::string host, std::string streamName, int timeConnected);
      std::string hostLookup(std::string ip);
      bool onList(std::string ip, std::string list);
      std::string getCountry(std::string ip);
      void doSync(bool force = false);
      /*LTS-END*/
      std::map<unsigned long, unsigned int> currKeyOpen;
      void loadPageForKey(long unsigned int trackId, long long int keyNum);
      int pageNumForKey(long unsigned int trackId, long long int keyNum);
      int pageNumMax(long unsigned int trackId);
      bool isRecordingToFile;
      unsigned int lastStats;///<Time of last sending of stats.
      std::map<unsigned long, unsigned long> nxtKeyNum;///< Contains the number of the next key, for page seeking purposes.
      std::set<sortedPageInfo> buffer;///< A sorted list of next-to-be-loaded packets.
      bool sought;///<If a seek has been done, this is set to true. Used for seeking on prepareNext().
    protected://these are to be messed with by child classes
      bool pushing;
      std::map<std::string, std::string> targetParams; /*LTS*/
      std::string UA; ///< User Agent string, if known.
      uint16_t uaDelay;///<Seconds to wait before setting the UA.
      uint64_t lastRecv;
      long long unsigned int firstTime;///< Time of first packet after last seek. Used for real-time sending.
      virtual std::string getConnectedHost();
      virtual std::string getConnectedBinHost();
      virtual std::string getStatsName();
      virtual bool hasSessionIDs(){return false;}

      IPC::sharedClient statsPage;///< Shared memory used for statistics reporting.
      bool isBlocking;///< If true, indicates that myConn is blocking.
      uint32_t crc;///< Checksum, if any, for usage in the stats.
      unsigned int getKeyForTime(long unsigned int trackId, long long timeStamp);
      
      //stream delaying variables
      unsigned int maxSkipAhead;///< Maximum ms that we will go ahead of the intended timestamps.
      unsigned int realTime;///< Playback speed in ms of data per second. eg: 0 is infinite, 1000 real-time, 5000 is 0.2X speed, 500 = 2X speed.
      uint32_t needsLookAhead;///< Amount of millis we need to be able to look ahead in the metadata

      //Read/write status variables
      Socket::Connection & myConn;///< Connection to the client.

      bool wantRequest;///< If true, waits for a request.
      bool parseData;///< If true, triggers initalization if not already done, sending of header, sending of packets.
      bool isInitialized;///< If false, triggers initialization if parseData is true.
      bool sentHeader;///< If false, triggers sendHeader if parseData is true.

      std::map<int,DTSCPageData> bookKeeping;
      virtual bool isRecording();
      virtual bool isFileTarget();
      virtual bool isPushing(){return pushing;};
      bool allowPush(const std::string & passwd);
      void waitForStreamPushReady();
      bool pushIsOngoing;
      void bufferLivePacket(const DTSC::Packet & packet);
      uint64_t firstPacketTime;
      uint64_t lastPacketTime;
      inline bool keepGoing(){
        return config->is_active && myConn;
      }
  };

}

