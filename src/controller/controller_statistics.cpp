#include <cstdio>
#include <list>
#include <mist/config.h>
#include <mist/shared_memory.h>
#include <mist/dtsc.h>
#include <mist/stream.h>
#include <mist/bitfields.h>
#include "controller_statistics.h"
#include "controller_storage.h"

// These are used to store "clients" field requests in a bitfield for speedup.
#define STAT_CLI_HOST 1
#define STAT_CLI_STREAM 2
#define STAT_CLI_PROTO 4
#define STAT_CLI_CONNTIME 8
#define STAT_CLI_POSITION 16
#define STAT_CLI_DOWN 32
#define STAT_CLI_UP 64
#define STAT_CLI_BPS_DOWN 128
#define STAT_CLI_BPS_UP 256
#define STAT_CLI_CRC 512
#define STAT_CLI_ALL 0xFFFF
// These are used to store "totals" field requests in a bitfield for speedup.
#define STAT_TOT_CLIENTS 1
#define STAT_TOT_BPS_DOWN 2
#define STAT_TOT_BPS_UP 4
#define STAT_TOT_INPUTS 8
#define STAT_TOT_OUTPUTS 16
#define STAT_TOT_ALL 0xFF

#define COUNTABLE_BYTES 128*1024


std::map<Controller::sessIndex, Controller::statSession> Controller::sessions; ///< list of sessions that have statistics data available
std::map<unsigned long, Controller::sessIndex> Controller::connToSession; ///< Map of socket IDs to session info.
tthread::mutex Controller::statsMutex;

//For server-wide totals. Local to this file only.
struct streamTotals {
  uint64_t upBytes;
  uint64_t downBytes;
  uint64_t inputs;
  uint64_t outputs;
  uint64_t viewers;
  uint64_t currIns;
  uint64_t currOuts;
  uint64_t currViews;
  uint8_t status;
};
static std::map<std::string, struct streamTotals> streamStats;

Controller::sessIndex::sessIndex(std::string dhost, unsigned int dcrc, std::string dstreamName, std::string dconnector){
  host = dhost;
  crc = dcrc;
  streamName = dstreamName;
  connector = dconnector;
}

Controller::sessIndex::sessIndex(){
  crc = 0;
}

std::string Controller::sessIndex::toStr(){
  std::stringstream s;
  s << host << " " << crc << " " << streamName << " " << connector;
  return s.str();
}

/// Initializes a sessIndex from a statExchange object, converting binary format IP addresses into strings.
/// This extracts the host, stream name, connector and crc field, ignoring everything else.
Controller::sessIndex::sessIndex(IPC::statExchange & data){
  Socket::hostBytesToStr(data.host().c_str(), 16, host);
  streamName = data.streamName();
  connector = data.connector();
  crc = data.crc();
}


bool Controller::sessIndex::operator== (const Controller::sessIndex &b) const{
  return (host == b.host && crc == b.crc && streamName == b.streamName && connector == b.connector);
}

bool Controller::sessIndex::operator!= (const Controller::sessIndex &b) const{
  return !(*this == b);
}

bool Controller::sessIndex::operator> (const Controller::sessIndex &b) const{
  return host > b.host || (host == b.host && (crc > b.crc || (crc == b.crc && (streamName > b.streamName || (streamName == b.streamName && connector > b.connector)))));
}

bool Controller::sessIndex::operator< (const Controller::sessIndex &b) const{
  return host < b.host || (host == b.host && (crc < b.crc || (crc == b.crc && (streamName < b.streamName || (streamName == b.streamName && connector < b.connector)))));
}

bool Controller::sessIndex::operator<= (const Controller::sessIndex &b) const{
  return !(*this > b);
}

bool Controller::sessIndex::operator>= (const Controller::sessIndex &b) const{
  return !(*this < b);
}

/// \todo Make this prettier.
IPC::sharedServer * statPointer = 0;


/// This function runs as a thread and roughly once per second retrieves
/// statistics from all connected clients, as well as wipes
/// old statistics that have disconnected over 10 minutes ago.
void Controller::SharedMemStats(void * config){
  HIGH_MSG("Starting stats thread");
  IPC::sharedServer statServer(SHM_STATISTICS, STAT_EX_SIZE, true);
  statPointer = &statServer;
  std::set<std::string> inactiveStreams;
  Controller::initState();
  bool shiftWrites = true;
  bool firstRun = true;
  while(((Util::Config*)config)->is_active){
    {
      tthread::lock_guard<tthread::mutex> guard(Controller::configMutex);
      tthread::lock_guard<tthread::mutex> guard2(statsMutex);
      //parse current users
      statServer.parseEach(parseStatistics);
      if (firstRun){
        firstRun = false;
        for (std::map<std::string, struct streamTotals>::iterator it = streamStats.begin(); it != streamStats.end(); ++it){
          it->second.upBytes = 0;
          it->second.downBytes = 0;
        }
      }
      //wipe old statistics
      if (sessions.size()){
        std::list<sessIndex> mustWipe;
        unsigned long long cutOffPoint = Util::epoch() - STAT_CUTOFF;
        unsigned long long disconnectPointIn = Util::epoch() - STATS_INPUT_DELAY;
        unsigned long long disconnectPointOut = Util::epoch() - STATS_DELAY;
        for (std::map<sessIndex, statSession>::iterator it = sessions.begin(); it != sessions.end(); it++){
          unsigned long long dPoint = it->second.getSessType() == SESS_INPUT ? disconnectPointIn : disconnectPointOut; 
          it->second.ping(it->first, dPoint);
          it->second.wipeOld(cutOffPoint);
          if (!it->second.hasData()){
            mustWipe.push_back(it->first);
          }
        }
        while (mustWipe.size()){
          sessions.erase(mustWipe.front());
          mustWipe.pop_front();
        }
      }
      Util::RelAccX * strmStats = streamsAccessor();
      if (!strmStats || !strmStats->isReady()){strmStats = 0;}
      uint64_t strmPos = 0;
      if (strmStats){
        if (shiftWrites || (strmStats->getEndPos() - strmStats->getDeleted() != streamStats.size())){
          shiftWrites = true;
          strmPos = strmStats->getEndPos();
        }else{
          strmPos = strmStats->getDeleted();
        }
      }
      if (streamStats.size()){
        for (std::map<std::string, struct streamTotals>::iterator it = streamStats.begin(); it != streamStats.end(); ++it){
          uint8_t newState = Util::getStreamStatus(it->first);
          uint8_t oldState = it->second.status;
          if (newState != oldState){
            it->second.status = newState;
          }
          if (newState == STRMSTAT_OFF){
            inactiveStreams.insert(it->first);
          }
          if (strmStats){
            if (shiftWrites){
              strmStats->setString("stream", it->first, strmPos);
            }
            strmStats->setInt("status", it->second.status, strmPos);
            strmStats->setInt("viewers", it->second.currViews, strmPos);
            strmStats->setInt("inputs", it->second.currIns, strmPos);
            strmStats->setInt("outputs", it->second.currOuts, strmPos);
            ++strmPos;
          }
        }
      }
      if (strmStats && shiftWrites){
        shiftWrites = false;
        uint64_t prevEnd = strmStats->getEndPos();
        strmStats->setEndPos(strmPos);
        strmStats->setDeleted(prevEnd);
      }
      while (inactiveStreams.size()){
        streamStats.erase(*inactiveStreams.begin());
        inactiveStreams.erase(inactiveStreams.begin());
        shiftWrites = true;
      }
    }
    Util::wait(1000);
  }
  statPointer = 0;
  HIGH_MSG("Stopping stats thread");
  if (Util::Config::is_restarting){
    statServer.abandon();
  }
  Controller::deinitState(Util::Config::is_restarting);
}

/// Gets a complete list of all streams currently in active state, with optional prefix matching
std::set<std::string> Controller::getActiveStreams(const std::string & prefix){
  std::set<std::string> ret;
  Util::RelAccX * strmStats = streamsAccessor();
  if (!strmStats || !strmStats->isReady()){return ret;}
  uint64_t endPos = strmStats->getEndPos();
  if (prefix.size()){
    for (uint64_t i = strmStats->getDeleted(); i < endPos; ++i){
      if (strmStats->getInt("status", i) != STRMSTAT_READY){continue;}
      const char * S = strmStats->getPointer("stream", i);
      if (!strncmp(S, prefix.data(), prefix.size())){
        ret.insert(S);
      }
    }
  }else{
    for (uint64_t i = strmStats->getDeleted(); i < endPos; ++i){
      if (strmStats->getInt("status", i) != STRMSTAT_READY){continue;}
      ret.insert(strmStats->getPointer("stream", i));
    }
  }
  return ret;
}

/// Updates the given active connection with new stats data.
void Controller::statSession::update(uint64_t index, IPC::statExchange & data){
  long long prevDown = getDown();
  long long prevUp = getUp();
  curConns[index].update(data);
  //store timestamp of first received data, if older
  if (firstSec > data.now()){
    firstSec = data.now();
  }
  //store timestamp of last received data, if newer
  if (data.now() > lastSec){
    lastSec = data.now();
    if (!tracked){
      tracked = true;
      firstActive = firstSec;
    }
  }
  long long currDown = getDown();
  long long currUp = getUp();
  if (currUp - prevUp < 0 || currDown-prevDown < 0){
    INFO_MSG("Negative data usage! %lldu/%lldd (u%lld->%lld) in %s over %s, #%lu", currUp-prevUp, currDown-prevDown, prevUp, currUp, data.streamName().c_str(), data.connector().c_str(), index);
  }
  if (currDown + currUp >= COUNTABLE_BYTES){
    std::string streamName = data.streamName();
    if (sessionType == SESS_UNSET){
      if (data.connector() == "INPUT"){
        streamStats[streamName].inputs++;
        streamStats[streamName].currIns++;
        sessionType = SESS_INPUT;
      }else if (data.connector() == "OUTPUT"){
        streamStats[streamName].outputs++;
        streamStats[streamName].currOuts++;
        sessionType = SESS_OUTPUT;
      }else{
        streamStats[streamName].viewers++;
        streamStats[streamName].currViews++;
        sessionType = SESS_VIEWER;
      }
    }
    //If previous < COUNTABLE_BYTES, we haven't counted any data so far.
    //We need to count all the data in that case, otherwise we only count the difference.
    if (prevUp + prevDown < COUNTABLE_BYTES){
      if (!streamName.size() || streamName[0] == 0){
        if (streamStats.count(streamName)){streamStats.erase(streamName);}
      }else{
        streamStats[streamName].upBytes += currUp;
        streamStats[streamName].downBytes += currDown;
      }
    }else{
      if (!streamName.size() || streamName[0] == 0){
        if (streamStats.count(streamName)){streamStats.erase(streamName);}
      }else{
        streamStats[streamName].upBytes += currUp - prevUp;
        streamStats[streamName].downBytes += currDown - prevDown;
      }
    }
  }
}

Controller::sessType Controller::statSession::getSessType(){
  return sessionType;
}

/// Archives the given connection.
void Controller::statSession::wipeOld(uint64_t cutOff){
  if (firstSec > cutOff){
    return;
  }
  firstSec = 0xFFFFFFFFFFFFFFFFull;
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      while (it->log.size() && it->log.begin()->first < cutOff){
        if (it->log.size() == 1){
          wipedDown += it->log.begin()->second.down;
          wipedUp += it->log.begin()->second.up;
        }
        it->log.erase(it->log.begin());
      }
      if (it->log.size()){
        if (firstSec > it->log.begin()->first){
          firstSec = it->log.begin()->first;
        }
      }
    }
    while (oldConns.size() && !oldConns.begin()->log.size()){
      oldConns.pop_front();
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      while (it->second.log.size() > 1 && it->second.log.begin()->first < cutOff){
        it->second.log.erase(it->second.log.begin());
      }
      if (it->second.log.size()){
        if (firstSec > it->second.log.begin()->first){
          firstSec = it->second.log.begin()->first;
        }
      }
    }
  }
}

void Controller::statSession::ping(const Controller::sessIndex & index, uint64_t disconnectPoint){
  if (!tracked){return;}
  if (lastSec < disconnectPoint){
    switch (sessionType){
      case SESS_INPUT:
        if (streamStats[index.streamName].currIns){streamStats[index.streamName].currIns--;}
        break;
      case SESS_OUTPUT:
        if (streamStats[index.streamName].currOuts){streamStats[index.streamName].currOuts--;}
        break;
      case SESS_VIEWER:
        if (streamStats[index.streamName].currViews){streamStats[index.streamName].currViews--;}
        break;
      default:
        break;
    }
    uint64_t duration = lastSec - firstActive;
    if (duration < 1){duration = 1;}
    Controller::logAccess("", index.streamName, index.connector, index.host, duration, getUp(), getDown(), "");
    tracked = false;
    firstActive = 0;
    firstSec = 0xFFFFFFFFFFFFFFFFull;
    lastSec = 0;
    wipedUp = 0;
    wipedDown = 0;
    oldConns.clear();
    sessionType = SESS_UNSET;
  }
}

/// Archives the given connection.
void Controller::statSession::finish(uint64_t index){
  oldConns.push_back(curConns[index]);
  curConns.erase(index);
}

/// Constructs an empty session
Controller::statSession::statSession(){
  firstActive = 0;
  tracked = false;
  firstSec = 0xFFFFFFFFFFFFFFFFull;
  lastSec = 0;
  wipedUp = 0;
  wipedDown = 0;
  sessionType = SESS_UNSET;
}

/// Moves the given connection to the given session
void Controller::statSession::switchOverTo(statSession & newSess, uint64_t index){
  //add to the given session first
  newSess.curConns[index] = curConns[index];
  //if this connection has data, update firstSec/lastSec if needed
  if (curConns[index].log.size()){
    if (newSess.firstSec > curConns[index].log.begin()->first){
      newSess.firstSec = curConns[index].log.begin()->first;
    }
    if (newSess.lastSec < curConns[index].log.rbegin()->first){
      newSess.lastSec = curConns[index].log.rbegin()->first;
    }
  }
  //remove from current session
  curConns.erase(index);
  //if there was any data, recalculate this session's firstSec and lastSec.
  if (newSess.curConns[index].log.size()){
    firstSec = 0xFFFFFFFFFFFFFFFFull;
    lastSec = 0;
    if (oldConns.size()){
      for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
        if (it->log.size()){
          if (firstSec > it->log.begin()->first){
            firstSec = it->log.begin()->first;
          }
          if (lastSec < it->log.rbegin()->first){
            lastSec = it->log.rbegin()->first;
          }
        }
      }
    }
    if (curConns.size()){
      for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
        if (it->second.log.size()){
          if (firstSec > it->second.log.begin()->first){
            firstSec = it->second.log.begin()->first;
          }
          if (lastSec < it->second.log.rbegin()->first){
            lastSec = it->second.log.rbegin()->first;
          }
        }
      }
    }
  }
}

/// Returns the first measured timestamp in this session.
uint64_t Controller::statSession::getStart(){
  return firstSec;
}

/// Returns the last measured timestamp in this session.
uint64_t Controller::statSession::getEnd(){
  return lastSec;
}

/// Returns true if there is data for this session at timestamp t.
bool Controller::statSession::hasDataFor(uint64_t t){
  if (lastSec < t){return false;}
  if (firstSec > t){return false;}
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      if (it->hasDataFor(t)){return true;}
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.hasDataFor(t)){return true;}
    }
  }
  return false;
}

/// Returns true if there is any data for this session.
bool Controller::statSession::hasData(){
  if (!firstSec && !lastSec){return false;}
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      if (it->log.size()){return true;}
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.log.size()){return true;}
    }
  }
  return false;
}

/// Returns true if this session should count as a viewer on the given timestamp.
bool Controller::statSession::isViewerOn(uint64_t t){
  return getUp(t) + getDown(t) > COUNTABLE_BYTES;
}

/// Returns true if this session should count as a viewer
bool Controller::statSession::isViewer(){
  long long upTotal = wipedUp+wipedDown;
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      if (it->log.size()){
        upTotal += it->log.rbegin()->second.up + it->log.rbegin()->second.down;
        if (upTotal > COUNTABLE_BYTES){return true;}
      }
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.log.size()){
        upTotal += it->second.log.rbegin()->second.up + it->second.log.rbegin()->second.down;
        if (upTotal > COUNTABLE_BYTES){return true;}
      }
    }
  }
  return false;
}

/// Returns the cumulative connected time for this session at timestamp t.
uint64_t Controller::statSession::getConnTime(uint64_t t){
  uint64_t retVal = 0;
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      if (it->hasDataFor(t)){
        retVal += it->getDataFor(t).time;
      }
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.hasDataFor(t)){
        retVal += it->second.getDataFor(t).time;
      }
    }
  }
  return retVal;
}

/// Returns the last requested media timestamp for this session at timestamp t.
uint64_t Controller::statSession::getLastSecond(uint64_t t){
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.hasDataFor(t)){
        return it->second.getDataFor(t).lastSecond;
      }
    }
  }
  if (oldConns.size()){
    for (std::deque<statStorage>::reverse_iterator it = oldConns.rbegin(); it != oldConns.rend(); ++it){
      if (it->hasDataFor(t)){
        return it->getDataFor(t).lastSecond;
      }
    }
  }
  return 0;
}

/// Returns the cumulative downloaded bytes for this session at timestamp t.
uint64_t Controller::statSession::getDown(uint64_t t){
  uint64_t retVal = wipedDown;
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      if (it->hasDataFor(t)){
        retVal += it->getDataFor(t).down;
      }
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.hasDataFor(t)){
        retVal += it->second.getDataFor(t).down;
      }
    }
  }
  return retVal;
}

/// Returns the cumulative uploaded bytes for this session at timestamp t.
uint64_t Controller::statSession::getUp(uint64_t t){
  uint64_t retVal = wipedUp;
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      if (it->hasDataFor(t)){
        retVal += it->getDataFor(t).up;
      }
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.hasDataFor(t)){
        retVal += it->second.getDataFor(t).up;
      }
    }
  }
  return retVal;
}

/// Returns the cumulative downloaded bytes for this session at timestamp t.
uint64_t Controller::statSession::getDown(){
  uint64_t retVal = wipedDown;
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      if (it->log.size()){
        retVal += it->log.rbegin()->second.down;
      }
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.log.size()){
        retVal += it->second.log.rbegin()->second.down;
      }
    }
  }
  return retVal;
}

/// Returns the cumulative uploaded bytes for this session at timestamp t.
uint64_t Controller::statSession::getUp(){
  uint64_t retVal = wipedUp;
  if (oldConns.size()){
    for (std::deque<statStorage>::iterator it = oldConns.begin(); it != oldConns.end(); ++it){
      if (it->log.size()){
        retVal += it->log.rbegin()->second.up;
      }
    }
  }
  if (curConns.size()){
    for (std::map<uint64_t, statStorage>::iterator it = curConns.begin(); it != curConns.end(); ++it){
      if (it->second.log.size()){
        retVal += it->second.log.rbegin()->second.up;
      }
    }
  }
  return retVal;
}

/// Returns the cumulative downloaded bytes per second for this session at timestamp t.
uint64_t Controller::statSession::getBpsDown(uint64_t t){
  uint64_t aTime = t - 5;
  if (aTime < firstSec){
    aTime = firstSec;
  }
  if (t <= aTime){
    return 0;
  }
  uint64_t valA = getDown(aTime);
  uint64_t valB = getDown(t);
  return (valB - valA) / (t - aTime);
}

/// Returns the cumulative uploaded bytes per second for this session at timestamp t.
uint64_t Controller::statSession::getBpsUp(uint64_t t){
  uint64_t aTime = t - 5;
  if (aTime < firstSec){
    aTime = firstSec;
  }
  if (t <= aTime){
    return 0;
  }
  uint64_t valA = getUp(aTime);
  uint64_t valB = getUp(t);
  return (valB - valA) / (t - aTime);
}

/// Returns true if there is data available for timestamp t.
bool Controller::statStorage::hasDataFor(unsigned long long t) {
  if (!log.size()){return false;}
  return (t >= log.begin()->first);
}

/// Returns a reference to the most current data available at timestamp t.
Controller::statLog & Controller::statStorage::getDataFor(unsigned long long t) {
  static statLog empty;
  if (!log.size()){
    empty.time = 0;
    empty.lastSecond = 0;
    empty.down = 0;
    empty.up = 0;
    return empty;
  }
  std::map<unsigned long long, statLog>::iterator it = log.upper_bound(t);
  if (it != log.begin()){
    it--;
  }
  return it->second;
}

/// This function is called by parseStatistics.
/// It updates the internally saved statistics data.
void Controller::statStorage::update(IPC::statExchange & data) {
  statLog tmp;
  tmp.time = data.time();
  tmp.lastSecond = data.lastSecond();
  tmp.down = data.down();
  tmp.up = data.up();
  log[data.now()] = tmp;
  //wipe data older than approx. STAT_CUTOFF seconds
  /// \todo Remove least interesting data first.
  if (log.size() > STAT_CUTOFF){
    log.erase(log.begin());
  }
}
  
/// This function is called by the shared memory page that holds statistics.
/// It updates the internally saved statistics data, moving across sessions or archiving when necessary.
void Controller::parseStatistics(char * data, size_t len, uint32_t id){
  //retrieve stats data
  IPC::statExchange tmpEx(data);
  //calculate the current session index, store as idx.
  sessIndex idx(tmpEx);
  //if the connection was already indexed and it has changed, move it
  if (connToSession.count(id) && connToSession[id] != idx){
    if (sessions[connToSession[id]].getSessType() != SESS_UNSET){
        INFO_MSG("Switching connection %" PRIu32 " from active session %s over to %s", id, connToSession[id].toStr().c_str(), idx.toStr().c_str());
    }else{
        INFO_MSG("Switching connection %" PRIu32 " from inactive session %s over to %s", id, connToSession[id].toStr().c_str(), idx.toStr().c_str());
    }
    sessions[connToSession[id]].switchOverTo(sessions[idx], id);
    if (!sessions[connToSession[id]].hasData()){
      sessions.erase(connToSession[id]);
    }
  }
  if (!connToSession.count(id)){
      INSANE_MSG("New connection: %" PRIu32 " as %s", id, idx.toStr().c_str());
  }
  //store the index for later comparison
  connToSession[id] = idx;
  //update the session with the latest data
  sessions[idx].update(id, tmpEx);
  //check validity of stats data
  char counter = (*(data - 1)) & 0x7F;
  if (counter == 126 || counter == 127){
    //the data is no longer valid - connection has gone away, store for later
    INSANE_MSG("Ended connection: %" PRIu32 " as %s", id, idx.toStr().c_str());
    sessions[idx].finish(id);
    connToSession.erase(id);
  }else{
    if (sessions[idx].getSessType() != SESS_OUTPUT && sessions[idx].getSessType() != SESS_UNSET){
      std::string strmName = tmpEx.streamName();
    }
  }
}

/// Returns true if this stream has at least one connected client.
bool Controller::hasViewers(std::string streamName){
  if (sessions.size()){
    long long currTime = Util::epoch();
    for (std::map<sessIndex, statSession>::iterator it = sessions.begin(); it != sessions.end(); it++){
      if (it->first.streamName == streamName && (it->second.hasDataFor(currTime) || it->second.hasDataFor(currTime-1))){
        return true;
      }
    }
  }
  return false;
}

/// This takes a "clients" request, and fills in the response data.
/// 
/// \api
/// `"clients"` requests take the form of:
/// ~~~~~~~~~~~~~~~{.js}
/// {
///   //array of streamnames to accumulate. Empty means all.
///   "streams": ["streama", "streamb", "streamc"],
///   //array of protocols to accumulate. Empty means all.
///   "protocols": ["HLS", "HSS"],
///   //list of requested data fields. Empty means all.
///   "fields": ["host", "stream", "protocol", "conntime", "position", "down", "up", "downbps", "upbps"],
///   //unix timestamp of measuring moment. Negative means X seconds ago. Empty means now.
///   "time": 1234567
/// }
/// ~~~~~~~~~~~~~~~
/// OR
/// ~~~~~~~~~~~~~~~{.js}
/// [
///   {},//request object as above
///   {}//repeat the structure as many times as wanted
/// ]
/// ~~~~~~~~~~~~~~~
/// and are responded to as:
/// ~~~~~~~~~~~~~~~{.js}
/// {
///   //unix timestamp of data. Always present, always absolute.
///   "time": 1234567,
///   //array of actually represented data fields.
///   "fields": [...]
///   //for all clients, the data in the order they appear in the "fields" field.
///   "data": [[x, y, z], [x, y, z], [x, y, z]]
/// }
/// ~~~~~~~~~~~~~~~
/// In case of the second method, the response is an array in the same order as the requests.
void Controller::fillClients(JSON::Value & req, JSON::Value & rep){
  tthread::lock_guard<tthread::mutex> guard(statsMutex);
  //first, figure out the timestamp wanted
  uint64_t reqTime = 0;
  if (req.isMember("time")){
    reqTime = req["time"].asInt();
  }
  //to make sure no nasty timing business takes place, we store the case "now" as a bool.
  bool now = (reqTime == 0);
  //add the current time, if negative or zero.
  if (reqTime <= 0){
    reqTime += Util::epoch();
  }
  //at this point, reqTime is the absolute timestamp.
  rep["time"] = reqTime; //fill the absolute timestamp
  
  unsigned int fields = 0;
  //next, figure out the fields wanted
  if (req.isMember("fields") && req["fields"].size()){
    jsonForEach(req["fields"], it) {
      if ((*it).asStringRef() == "host"){fields |= STAT_CLI_HOST;}
      if ((*it).asStringRef() == "stream"){fields |= STAT_CLI_STREAM;}
      if ((*it).asStringRef() == "protocol"){fields |= STAT_CLI_PROTO;}
      if ((*it).asStringRef() == "conntime"){fields |= STAT_CLI_CONNTIME;}
      if ((*it).asStringRef() == "position"){fields |= STAT_CLI_POSITION;}
      if ((*it).asStringRef() == "down"){fields |= STAT_CLI_DOWN;}
      if ((*it).asStringRef() == "up"){fields |= STAT_CLI_UP;}
      if ((*it).asStringRef() == "downbps"){fields |= STAT_CLI_BPS_DOWN;}
      if ((*it).asStringRef() == "upbps"){fields |= STAT_CLI_BPS_UP;}
    }
  }
  //select all, if none selected
  if (!fields){fields = STAT_CLI_ALL;}
  //figure out what streams are wanted
  std::set<std::string> streams;
  if (req.isMember("streams") && req["streams"].size()){
    jsonForEach(req["streams"], it) {
      streams.insert((*it).asStringRef());
    }
  }
  //figure out what protocols are wanted
  std::set<std::string> protos;
  if (req.isMember("protocols") && req["protocols"].size()){
    jsonForEach(req["protocols"], it) {
      protos.insert((*it).asStringRef());
    }
  }
  //output the selected fields
  rep["fields"].null();
  if (fields & STAT_CLI_HOST){rep["fields"].append("host");}
  if (fields & STAT_CLI_STREAM){rep["fields"].append("stream");}
  if (fields & STAT_CLI_PROTO){rep["fields"].append("protocol");}
  if (fields & STAT_CLI_CONNTIME){rep["fields"].append("conntime");}
  if (fields & STAT_CLI_POSITION){rep["fields"].append("position");}
  if (fields & STAT_CLI_DOWN){rep["fields"].append("down");}
  if (fields & STAT_CLI_UP){rep["fields"].append("up");}
  if (fields & STAT_CLI_BPS_DOWN){rep["fields"].append("downbps");}
  if (fields & STAT_CLI_BPS_UP){rep["fields"].append("upbps");}
  if (fields & STAT_CLI_CRC){rep["fields"].append("crc");}
  //output the data itself
  rep["data"].null();
  //loop over all sessions
  if (sessions.size()){
    for (std::map<sessIndex, statSession>::iterator it = sessions.begin(); it != sessions.end(); it++){
      unsigned long long time = reqTime;
      if (now && reqTime - it->second.getEnd() < 5){time = it->second.getEnd();}
      //data present and wanted? insert it!
      if ((it->second.getEnd() >= time && it->second.getStart() <= time) && (!streams.size() || streams.count(it->first.streamName)) && (!protos.size() || protos.count(it->first.connector))){
        if (it->second.hasDataFor(time)){
          JSON::Value d;
          if (fields & STAT_CLI_HOST){d.append(it->first.host);}
          if (fields & STAT_CLI_STREAM){d.append(it->first.streamName);}
          if (fields & STAT_CLI_PROTO){d.append(it->first.connector);}
          if (fields & STAT_CLI_CONNTIME){d.append(it->second.getConnTime(time));}
          if (fields & STAT_CLI_POSITION){d.append(it->second.getLastSecond(time));}
          if (fields & STAT_CLI_DOWN){d.append(it->second.getDown(time));}
          if (fields & STAT_CLI_UP){d.append(it->second.getUp(time));}
          if (fields & STAT_CLI_BPS_DOWN){d.append(it->second.getBpsDown(time));}
          if (fields & STAT_CLI_BPS_UP){d.append(it->second.getBpsUp(time));}
          if (fields & STAT_CLI_CRC){d.append(it->first.crc);}
          rep["data"].append(d);
        }
      }
    }
  }
  //all done! return is by reference, so no need to return anything here.
}

/// This takes a "active_streams" request, and fills in the response data.
/// 
/// \api
/// `"active_streams"` and `"stats_streams"` requests may either be empty, in which case the response looks like this:
/// ~~~~~~~~~~~~~~~{.js}
/// [
///   //Array of stream names
///   "streamA",
///   "streamB",
///   "streamC"
/// ]
/// ~~~~~~~~~~~~~~~
/// `"stats_streams"` will list all streams that any statistics data is available for, and only those. `"active_streams"` only lists streams that are currently active, and only those.
/// If the request is an array, which may contain any of the following elements:
/// ~~~~~~~~~~~~~~~{.js}
/// [
///   //Array of requested data types
///   "clients", //Current viewer count
///   "lastms" //Current position in the live buffer, if live
/// ]
/// ~~~~~~~~~~~~~~~
/// In which case the response is changed into this format:
/// ~~~~~~~~~~~~~~~{.js}
/// {
///   //Object of stream names, containing arrays in the same order as the request, with the same data
///   "streamA":[
///     0,
///     60000
///   ]
///   "streamB":[
///      //....
///   ]
///   //...
/// }
/// ~~~~~~~~~~~~~~~
/// All streams that any statistics data is available for are listed, and only those streams.
void Controller::fillActive(JSON::Value & req, JSON::Value & rep, bool onlyNow){
  //collect the data first
  std::set<std::string> streams;
  std::map<std::string, uint64_t> clients;
  unsigned int tOut = Util::epoch() - STATS_DELAY;
  unsigned int tIn = Util::epoch() - STATS_INPUT_DELAY;
  //check all sessions
  {
    tthread::lock_guard<tthread::mutex> guard(statsMutex);
    if (sessions.size()){
      for (std::map<sessIndex, statSession>::iterator it = sessions.begin(); it != sessions.end(); it++){
        if (it->second.getSessType() == SESS_INPUT){
          if (!onlyNow || (it->second.hasDataFor(tIn) && it->second.isViewerOn(tIn))){
            streams.insert(it->first.streamName);
          }
        }else{
          if (!onlyNow || (it->second.hasDataFor(tOut) && it->second.isViewerOn(tOut))){
            streams.insert(it->first.streamName);
            if (it->second.getSessType() == SESS_VIEWER){
              clients[it->first.streamName]++;
            }
          }
        }
      }
    }
  }
  //Good, now output what we found...
  rep.null();
  for (std::set<std::string>::iterator it = streams.begin(); it != streams.end(); it++){
    if (req.isArray()){
      rep[*it].null();
      jsonForEach(req, j){
        if (j->asStringRef() == "clients"){
          rep[*it].append(clients[*it]);
        }
        if (j->asStringRef() == "lastms"){
          char pageId[NAME_BUFFER_SIZE];
          IPC::sharedPage streamIndex;
          snprintf(pageId, NAME_BUFFER_SIZE, SHM_STREAM_INDEX, it->c_str());
          streamIndex.init(pageId, DEFAULT_STRM_PAGE_SIZE, false, false);
          if (streamIndex.mapped){
            static char liveSemName[NAME_BUFFER_SIZE];
            snprintf(liveSemName, NAME_BUFFER_SIZE, SEM_LIVE, it->c_str());
            IPC::semaphore metaLocker(liveSemName, O_CREAT | O_RDWR, (S_IRWXU|S_IRWXG|S_IRWXO), 8);
            metaLocker.wait();
            DTSC::Scan strm = DTSC::Packet(streamIndex.mapped, streamIndex.len, true).getScan();
            uint64_t lms = 0;
            DTSC::Scan trcks = strm.getMember("tracks");
            unsigned int trcks_ctr = trcks.getSize();
            for (unsigned int i = 0; i < trcks_ctr; ++i){
              if (trcks.getIndice(i).getMember("lastms").asInt() > lms){
                lms = trcks.getIndice(i).getMember("lastms").asInt();
              }
            }
            rep[*it].append(lms);
            metaLocker.post();
          }else{
            rep[*it].append(-1);
          }
        }
      }
    }else{
      rep.append(*it);
    }
  }
  //all done! return is by reference, so no need to return anything here.
}

class totalsData {
  public:
    totalsData(){
      clients = 0;
      inputs = 0;
      outputs = 0;
      downbps = 0;
      upbps = 0;
    }
    void add(uint64_t down, uint64_t up, Controller::sessType sT){
      switch (sT){
        case Controller::SESS_VIEWER: clients++; break;
        case Controller::SESS_INPUT: inputs++; break;
        case Controller::SESS_OUTPUT: outputs++; break;
        default: break;
      }
      downbps += down;
      upbps += up;
    }
    uint64_t clients;
    uint64_t inputs;
    uint64_t outputs;
    uint64_t downbps;
    uint64_t upbps;
};

/// This takes a "totals" request, and fills in the response data.
void Controller::fillTotals(JSON::Value & req, JSON::Value & rep){
  tthread::lock_guard<tthread::mutex> guard(statsMutex);
  //first, figure out the timestamps wanted
  long long int reqStart = 0;
  long long int reqEnd = 0;
  if (req.isMember("start")){
    reqStart = req["start"].asInt();
  }
  if (req.isMember("end")){
    reqEnd = req["end"].asInt();
  }
  //add the current time, if negative or zero.
  if (reqStart < 0){
    reqStart += Util::epoch();
  }
  if (reqStart == 0){
    reqStart = Util::epoch() - STAT_CUTOFF;
  }
  if (reqEnd <= 0){
    reqEnd += Util::epoch();
  }
  //at this point, reqStart and reqEnd are the absolute timestamp.
  
  unsigned int fields = 0;
  //next, figure out the fields wanted
  if (req.isMember("fields") && req["fields"].size()){
    jsonForEach(req["fields"], it) {
      if ((*it).asStringRef() == "clients"){fields |= STAT_TOT_CLIENTS;}
      if ((*it).asStringRef() == "inputs"){fields |= STAT_TOT_INPUTS;}
      if ((*it).asStringRef() == "outputs"){fields |= STAT_TOT_OUTPUTS;}
      if ((*it).asStringRef() == "downbps"){fields |= STAT_TOT_BPS_DOWN;}
      if ((*it).asStringRef() == "upbps"){fields |= STAT_TOT_BPS_UP;}
    }
  }
  //select all, if none selected
  if (!fields){fields = STAT_TOT_ALL;}
  //figure out what streams are wanted
  std::set<std::string> streams;
  if (req.isMember("streams") && req["streams"].size()){
    jsonForEach(req["streams"], it) {
      streams.insert((*it).asStringRef());
    }
  }
  //figure out what protocols are wanted
  std::set<std::string> protos;
  if (req.isMember("protocols") && req["protocols"].size()){
    jsonForEach(req["protocols"], it) {
      protos.insert((*it).asStringRef());
    }
  }
  //output the selected fields
  rep["fields"].null();
  if (fields & STAT_TOT_CLIENTS){rep["fields"].append("clients");}
  if (fields & STAT_TOT_INPUTS){rep["fields"].append("inputs");}
  if (fields & STAT_TOT_OUTPUTS){rep["fields"].append("outputs");}
  if (fields & STAT_TOT_BPS_DOWN){rep["fields"].append("downbps");}
  if (fields & STAT_TOT_BPS_UP){rep["fields"].append("upbps");}
  //start data collection
  std::map<uint64_t, totalsData> totalsCount;
  //loop over all sessions
  /// \todo Make the interval configurable instead of 1 second
  if (sessions.size()){
    for (std::map<sessIndex, statSession>::iterator it = sessions.begin(); it != sessions.end(); it++){
      //data present and wanted? insert it!
      if ((it->second.getEnd() >= (unsigned long long)reqStart || it->second.getStart() <= (unsigned long long)reqEnd) && (!streams.size() || streams.count(it->first.streamName)) && (!protos.size() || protos.count(it->first.connector))){
        for (unsigned long long i = reqStart; i <= reqEnd; ++i){
          if (it->second.hasDataFor(i)){
            totalsCount[i].add(it->second.getBpsDown(i), it->second.getBpsUp(i), it->second.getSessType());
          }
        }
      }
    }
  }
  //output the data itself
  if (!totalsCount.size()){
    //Oh noes! No data. We'll just reply with a bunch of nulls.
    rep["start"].null();
    rep["end"].null();
    rep["data"].null();
    rep["interval"].null();
    return;
  }
  //yay! We have data!
  rep["start"] = totalsCount.begin()->first;
  rep["end"] = totalsCount.rbegin()->first;
  rep["data"].null();
  rep["interval"].null();
  uint64_t prevT = 0;
  JSON::Value i;
  for (std::map<uint64_t, totalsData>::iterator it = totalsCount.begin(); it != totalsCount.end(); it++){
    JSON::Value d;
    if (fields & STAT_TOT_CLIENTS){d.append(it->second.clients);}
    if (fields & STAT_TOT_INPUTS){d.append(it->second.inputs);}
    if (fields & STAT_TOT_OUTPUTS){d.append(it->second.outputs);}
    if (fields & STAT_TOT_BPS_DOWN){d.append(it->second.downbps);}
    if (fields & STAT_TOT_BPS_UP){d.append(it->second.upbps);}
    rep["data"].append(d);
    if (prevT){
      if (i.size() < 2){
        i.append(1u);
        i.append(it->first - prevT);
      }else{
        if (i[1u].asInt() != it->first - prevT){
          rep["interval"].append(i);
          i[0u] = 1u;
          i[1u] = it->first - prevT;
        }else{
          i[0u] = i[0u].asInt() + 1;
        }
      }
    }
    prevT = it->first;
  }
  if (i.size() > 1){
    rep["interval"].append(i);
    i.null();
  }
  //all done! return is by reference, so no need to return anything here.
}
