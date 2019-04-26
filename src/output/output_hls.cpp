#include "output_hls.h"
#include <mist/stream.h>
#include <mist/langcodes.h> /*LTS*/
#include <unistd.h>

namespace Mist {
  bool OutHLS::isReadyForPlay() {
    if (myMeta.tracks.size()){
      if (myMeta.mainTrack().fragments.size() > 4){
        return true;
      }
    }
    return false;
  }

  std::string OutHLS::h264init(const std::string & initData){
    std::stringstream r;
    MP4::AVCC avccBox;
    avccBox.setPayload(initData);
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[1] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[2] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[3] << std::dec;
    return r.str();
  }

  std::string OutHLS::h265init(const std::string & initData){
    std::stringstream r;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[1] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[6] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[7] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[8] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[9] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[10] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[11] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[12] << std::dec;
    return r.str();
  }

  ///\brief Builds an index file for HTTP Live streaming.
  ///\return The index file for HTTP Live Streaming.
  std::string OutHLS::liveIndex() {
    std::stringstream result;
    selectDefaultTracks();
    result << "#EXTM3U\r\n";
    int audioId = -1;
    unsigned int vidTracks = 0;
    bool hasSubs = false;
    for (std::set<unsigned long>::iterator it = selectedTracks.begin(); it != selectedTracks.end(); ++it){
      if (audioId == -1 && myMeta.tracks[*it].type == "audio"){audioId = *it;}
      if (!hasSubs && myMeta.tracks[*it].codec == "subtitle"){hasSubs = true;}
    }
    for (std::set<unsigned long>::iterator it = selectedTracks.begin(); it != selectedTracks.end(); ++it){
      if (myMeta.tracks[*it].type == "video") {
        vidTracks++;
        int bWidth = myMeta.tracks[*it].bps;
        if (bWidth < 5) {
          bWidth = 5;
        }
        if (audioId != -1) {
          bWidth += myMeta.tracks[audioId].bps;
        }
        result << "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=" << (bWidth * 8);
        result << ",RESOLUTION=" << myMeta.tracks[*it].width << "x" << myMeta.tracks[*it].height;
        if (myMeta.tracks[*it].fpks){
          result << ",FRAME-RATE=" << (float)myMeta.tracks[*it].fpks / 1000; 
        }
        if (hasSubs){
          result << ",SUBTITLES=\"sub1\"";
        }
        result << ",CODECS=\"";
        result << Util::codecString(myMeta.tracks[*it].codec, myMeta.tracks[*it].init);
        if (audioId != -1){
          result << "," << Util::codecString(myMeta.tracks[audioId].codec, myMeta.tracks[audioId].init);
        }
        result << "\"";
        result <<"\r\n";
        result << *it;
        if (audioId != -1) {
          result << "_" << audioId;
        }
        if (hasSessionIDs()){
          result << "/index.m3u8?sessId=" << getpid() << "\r\n";
        }else{
          result << "/index.m3u8\r\n";
        }
      }else if(myMeta.tracks[*it].codec == "subtitle"){

        if(myMeta.tracks[*it].lang.empty()){
          myMeta.tracks[*it].lang = "und";
        }

        result << "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sub1\",LANGUAGE=\"" << myMeta.tracks[*it].lang << "\",NAME=\"" << Encodings::ISO639::decode(myMeta.tracks[*it].lang) << "\",AUTOSELECT=NO,DEFAULT=NO,FORCED=NO,URI=\"" << *it << "/index.m3u8\"" << "\r\n";
      }
    }
    if (!vidTracks && audioId) {
      result << "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=" << (myMeta.tracks[audioId].bps * 8);
      result << ",CODECS=\"" << Util::codecString(myMeta.tracks[audioId].codec, myMeta.tracks[audioId].init) << "\"";
      result << "\r\n";
      result << audioId << "/index.m3u8\r\n";
    }
    DEBUG_MSG(DLVL_HIGH, "Sending this index: %s", result.str().c_str());
    return result.str();
  }

  std::string OutHLS::pushLiveIndex(){
    std::stringstream result;
    result << "#EXTM3U\r\n";
    std::set<unsigned int> audioTracks;
    for (std::map<unsigned int, DTSC::Track>::iterator it = myMeta.tracks.begin(); it != myMeta.tracks.end(); it++) {
      if (it->second.codec == "AAC" || it->second.codec == "MP3" || it->second.codec == "AC3" || it->second.codec == "MP2") {
        audioTracks.insert(it->first);
      }
    }
    if (!audioTracks.size()){
      audioTracks.insert(-1);
    }
    unsigned int vidTracks = 0;
    for (std::map<unsigned int, DTSC::Track>::iterator it = myMeta.tracks.begin(); it != myMeta.tracks.end(); it++) {
      if (it->second.codec == "H264" || it->second.codec == "HEVC" || it->second.codec == "MPEG2") {
        for (std::set<unsigned int>::iterator audIt = audioTracks.begin(); audIt != audioTracks.end(); audIt++){
          vidTracks++;
          int bWidth = it->second.bps;
          if (bWidth < 5) {
            bWidth = 5;
          }
          if (*audIt != -1) {
            bWidth += myMeta.tracks[*audIt].bps;
          }
          result << "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=" << (bWidth * 8) << "\r\n";
          result << it->first;
          if (*audIt != -1) {
            result << "_" << *audIt;
          }
          result << "/index.m3u8\r\n";
        }
      }
    }
    if (!vidTracks && audioTracks.size()) {
      result << "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=" << (myMeta.tracks[*audioTracks.begin()].bps * 8) << "\r\n";
      result << *audioTracks.begin() << "/index.m3u8\r\n";
    }
    return result.str();
  }

  std::string OutHLS::pushLiveIndex(int tid, unsigned long bTime, unsigned long eTime){
    updateMeta();
    std::stringstream result;
    //parse single track
    result << "#EXTM3U\r\n#EXT-X-TARGETDURATION:" << (myMeta.tracks[tid].biggestFragment() / 1000) + 1 << "\r\n";

    std::deque<std::string> lines;
    unsigned int skippedLines = 0;
    for (std::deque<DTSC::Fragment>::iterator it = myMeta.tracks[tid].fragments.begin(); it != myMeta.tracks[tid].fragments.end(); it++) {
      long long int starttime = myMeta.tracks[tid].getKey(it->getNumber()).getTime();
      long long duration = it->getDuration();
      if (duration <= 0) {
        duration = myMeta.tracks[tid].lastms - starttime;
      }
      if (starttime < bTime){
        skippedLines++;
      }
      if (starttime >= bTime && (starttime + duration) <= eTime){
        char lineBuf[400];
        snprintf(lineBuf, 400, "#EXTINF:%lld, no desc\r\n%lld_%lld.ts\r\n", ((duration + 500) / 1000), starttime, starttime + duration);
        lines.push_back(lineBuf);
      }
    }

    result << "#EXT-X-MEDIA-SEQUENCE:" << myMeta.tracks[tid].missedFrags + skippedLines << "\r\n";

    while (lines.size()) {
      result << lines.front();
      lines.pop_front();
    }
    if (!myMeta.live && eTime >= myMeta.tracks[tid].lastms) {
      result << "#EXT-X-ENDLIST\r\n";
    }
    return result.str();
  }


  std::string OutHLS::liveIndex(int tid, std::string & sessId) {
    updateMeta();
    std::stringstream result;
    //parse single track
    uint32_t target_dur = (myMeta.tracks[tid].biggestFragment() / 1000) + 1;
    result << "#EXTM3U\r\n#EXT-X-VERSION:3\r\n#EXT-X-TARGETDURATION:" << target_dur << "\r\n";

    std::deque<std::string> lines;
    std::deque<uint16_t> durs;
    uint32_t total_dur = 0;
    for (std::deque<DTSC::Fragment>::iterator it = myMeta.tracks[tid].fragments.begin(); it != myMeta.tracks[tid].fragments.end(); it++) {
      long long int starttime = myMeta.tracks[tid].getKey(it->getNumber()).getTime();
      long long duration = it->getDuration();
      if (duration <= 0) {
        duration = myMeta.tracks[tid].lastms - starttime;
      }
      char lineBuf[400];

      if(myMeta.tracks[tid].codec == "subtitle"){
          snprintf(lineBuf, 400, "#EXTINF:%f,\r\n../../../%s.vtt?track=%d&from=%lld&to=%lld\r\n", (double)duration/1000,streamName.c_str(),tid, starttime, starttime + duration);
      }else{
        if (sessId.size()){
          snprintf(lineBuf, 400, "#EXTINF:%f,\r\n%lld_%lld.ts?sessId=%s\r\n", (double)duration/1000, starttime, starttime + duration, sessId.c_str());
        }else{
          snprintf(lineBuf, 400, "#EXTINF:%f,\r\n%lld_%lld.ts\r\n", (double)duration/1000, starttime, starttime + duration);
        }
      }
      durs.push_back(duration);
      total_dur += duration;
      lines.push_back(lineBuf);
    }
    unsigned int skippedLines = 0;
    if (myMeta.live && lines.size()) {
      //only print the last segment when VoD
      lines.pop_back();
      total_dur -= durs.back();
      durs.pop_back();
      //skip the first two segments when live, unless that brings us under 4 target durations
      while ((total_dur-durs.front()) > (target_dur * 4000) && skippedLines < 2) {
        lines.pop_front();
        total_dur -= durs.front();
        durs.pop_front();
        ++skippedLines;
      }
      /*LTS-START*/
      //remove lines to reduce size towards listlimit setting - but keep at least 4X target duration available
      if (config->getInteger("listlimit")) {
        unsigned long listlimit = config->getInteger("listlimit");
        while (lines.size() > listlimit && (total_dur-durs.front()) > (target_dur * 4000)) {
          lines.pop_front();
          total_dur -= durs.front();
          durs.pop_front();
          ++skippedLines;
        }
      }
      /*LTS-END*/
    }

    result << "#EXT-X-MEDIA-SEQUENCE:" << myMeta.tracks[tid].missedFrags + skippedLines << "\r\n";

    while (lines.size()) {
      result << lines.front();
      lines.pop_front();
    }
    if (!myMeta.live || total_dur == 0) {
      result << "#EXT-X-ENDLIST\r\n";
    }
    DEBUG_MSG(DLVL_HIGH, "Sending this index: %s", result.str().c_str());
    return result.str();
  } //liveIndex

  std::string OutHLS::generatePushList() {
    updateMeta();
    std::set<unsigned int> videoTracks;
    std::set<unsigned int> audioTracks;
    for (std::map<unsigned int, DTSC::Track>::iterator it = myMeta.tracks.begin(); it != myMeta.tracks.end(); it++) {
      if (it->second.codec == "AAC" || it->second.codec == "MP3" || it->second.codec == "AC3") {
        audioTracks.insert(it->first);
      }
      if (it->second.codec == "H264" || it->second.codec == "HEVC"){
        videoTracks.insert(it->first);
      }
    }
    JSON::Value result;
    for (std::map<unsigned int, DTSC::Track>::iterator it = myMeta.tracks.begin(); it != myMeta.tracks.end(); it++) {
      std::stringstream tid;
      tid << it->second.trackID;
      result["tracks"][tid.str()] = it->second.toJSON(true);
    }
    for(std::set<unsigned int>::iterator it = videoTracks.begin(); it != videoTracks.end(); it++){
      for(std::set<unsigned int>::iterator it2 = audioTracks.begin(); it2 != audioTracks.end(); it2++){
        JSON::Value quality;
        std::stringstream identifier;
        identifier << "/" << *it << "_" << *it2;
        quality["index"] = "/push" + identifier.str() + "/index_\%llu_\%llu.m3u8"; 
        quality["segment"] = identifier.str() + "/\%llu_\%llu.ts"; 
        quality["video"] = *it;
        quality["audio"] = *it2;
        quality["id"] = identifier.str();
        std::deque<DTSC::Fragment>::iterator it3 = myMeta.tracks[*it].fragments.begin();
        for (int i = 0; i < 2; i++){
          if (it3 != myMeta.tracks[*it].fragments.end()){
            ++it3;
          }
        }
        for (; it3 != myMeta.tracks[*it].fragments.end(); it3++) {
          if (myMeta.live && it3 == (myMeta.tracks[*it].fragments.end() - 1)){
            //Skip the current last fragment if we are live
            continue;
          }
          uint64_t starttime = myMeta.tracks[*it].getKey(it3->getNumber()).getTime();
          std::stringstream line;
          uint64_t duration = it3->getDuration();
          if (duration <= 0) {
            duration = myMeta.tracks[*it].lastms - starttime;
          }
          std::stringstream segmenturl;
          segmenturl << identifier.str() << "/" << starttime << "_" << duration + starttime << ".ts";
          JSON::Value segment;
          //segment["url"] = segmenturl.str();
          segment["time"] = starttime;
          segment["duration"] = duration;
          segment["number"] = (uint64_t)it3->getNumber();
          quality["segments"].append(segment);
        }
        result["qualities"].append(quality);
      }
    }
    return result.toString();;
  }




  OutHLS::OutHLS(Socket::Connection & conn) : TSOutput(conn) {
    uaDelay = 0;
    realTime = 0;
    until=0xFFFFFFFFFFFFFFFFull;
  }

  OutHLS::~OutHLS() {}

  void OutHLS::init(Util::Config * cfg) {
    HTTPOutput::init(cfg);
    capa["name"] = "HLS";
    capa["friendly"] = "Apple segmented over HTTP (HLS)";
    capa["desc"] = "Segmented streaming in Apple (TS-based) format over HTTP ( = HTTP Live Streaming)";
    capa["url_rel"] = "/hls/$/index.m3u8";
    capa["url_prefix"] = "/hls/$/";
    capa["url_pushlist"] = "/hls/$/push/list";
    capa["codecs"][0u][0u].append("+HEVC");
    capa["codecs"][0u][1u].append("+H264");
    capa["codecs"][0u][2u].append("+MPEG2");
    capa["codecs"][0u][3u].append("+AAC");
    capa["codecs"][0u][4u].append("+MP3");
    capa["codecs"][0u][5u].append("+AC3");
    capa["codecs"][0u][6u].append("+MP2");
    capa["methods"][0u]["handler"] = "http";
    capa["methods"][0u]["type"] = "html5/application/vnd.apple.mpegurl";
    capa["methods"][0u]["priority"] = 9;
    //MP3 only works on Edge/Apple
    capa["exceptions"]["codec:MP3"] = JSON::fromString("[[\"blacklist\",[\"Mozilla/\"]],[\"whitelist\",[\"iPad\",\"iPhone\",\"iPod\",\"MacIntel\",\"Edge\"]]]");
    capa["exceptions"]["codec:HEVC"] = JSON::fromString("[[\"blacklist\"]]");
    /*LTS-START*/
    cfg->addOption("listlimit", JSON::fromString("{\"arg\":\"integer\",\"default\":0,\"short\":\"y\",\"long\":\"list-limit\",\"help\":\"Maximum number of parts in live playlists (0 = infinite).\"}"));
    capa["optional"]["listlimit"]["name"] = "Live playlist limit";
    capa["optional"]["listlimit"]["help"] = "Maximum number of parts in live playlists. (0 = infinite)";
    capa["optional"]["listlimit"]["default"] = 0;
    capa["optional"]["listlimit"]["type"] = "uint";
    capa["optional"]["listlimit"]["option"] = "--list-limit";
    
    cfg->addOption("nonchunked", JSON::fromString("{\"short\":\"C\",\"long\":\"nonchunked\",\"help\":\"Do not send chunked, but buffer whole segments.\"}"));
    capa["optional"]["nonchunked"]["name"] = "Send whole segments";
    capa["optional"]["nonchunked"]["help"] = "Disables chunked transfer encoding, forcing per-segment buffering. Reduces performance significantly, but increases compatibility somewhat.";
    capa["optional"]["nonchunked"]["option"] = "--nonchunked";

    cfg->addOption("mergesessions", JSON::fromString("{\"short\":\"M\",\"long\":\"mergesessions\",\"help\":\"Merge together sessions from one user into a single session.\"}"));
    capa["optional"]["mergesessions"]["name"] = "Merge sessions";
    capa["optional"]["mergesessions"]["help"] = "If enabled, merges together all views from a single user into a single combined session. If disabled, each view (main playlist request) is a separate session.";
    capa["optional"]["mergesessions"]["option"] = "--mergesessions";
    /*LTS-END*/
  }

  void OutHLS::onHTTP() {
    std::string method = H.method;
    std::string sessId = H.GetVar("sessId");

    if (H.url == "/crossdomain.xml") {
      H.Clean();
      H.SetHeader("Content-Type", "text/xml");
      H.SetHeader("Server", "MistServer/" PACKAGE_VERSION);
      H.setCORSHeaders();
      if(method == "OPTIONS" || method == "HEAD"){
        H.SendResponse("200", "OK", myConn);
        H.Clean();
        return;
      }
      H.SetBody("<?xml version=\"1.0\"?><!DOCTYPE cross-domain-policy SYSTEM \"http://www.adobe.com/xml/dtds/cross-domain-policy.dtd\"><cross-domain-policy><allow-access-from domain=\"*\" /><site-control permitted-cross-domain-policies=\"all\"/></cross-domain-policy>");
      H.SendResponse("200", "OK", myConn);
      H.Clean(); //clean for any possible next requests
      return;
    } //crossdomain.xml

    if (H.method == "OPTIONS") {
      bool isTS = (HTTP::URL(H.url).getExt().substr(0, 3) != "m3u");
      H.Clean();
      H.setCORSHeaders();
      H.SetHeader("Content-Type", "application/octet-stream");
      if (isTS && !hasSessionIDs()){
        H.SetHeader("Cache-Control", "public, max-age=600, immutable");
        H.SetHeader("Pragma", "");
        H.SetHeader("Expires", "");
      }else{
        H.SetHeader("Cache-Control", "no-cache");
      }
      H.SetBody("");
      H.SendResponse("200", "OK", myConn);
      H.Clean();
      return;
    }
    
    if (H.url.find("hls") == std::string::npos){
      onFail("HLS handler active, but this is not a HLS URL. Eh... What...?");
      return;
    }


    appleCompat = (H.GetHeader("User-Agent").find("iPad") != std::string::npos) || (H.GetHeader("User-Agent").find("iPod") != std::string::npos) || (H.GetHeader("User-Agent").find("iPhone") != std::string::npos);
    bool VLCworkaround = false;
    if (H.GetHeader("User-Agent").substr(0, 3) == "VLC") {
      std::string vlcver = H.GetHeader("User-Agent").substr(4);
      if (vlcver[0] == '0' || vlcver[0] == '1' || (vlcver[0] == '2' && vlcver[2] < '2')) {
        DEBUG_MSG(DLVL_INFO, "Enabling VLC version < 2.2.0 bug workaround.");
        VLCworkaround = true;
      }
    }

    initialize();
    if (!keepGoing()){return;}

    if (H.url.substr(5 + streamName.size(), 5) == "/push"){
      std::string relPushUrl = H.url.substr(10 + streamName.size());
      H.Clean();
      if (relPushUrl == "/list"){
        H.SetBody(generatePushList());
        H.SendResponse("200", "OK", myConn);
        H.Clean();
        return;
      }
      H.SetHeader("Content-Type", "application/vnd.apple.mpegurl");
      if (relPushUrl == "/index.m3u8"){
        H.setCORSHeaders();
        H.SetBody(pushLiveIndex());
        H.SendResponse("200", "OK", myConn);
        H.Clean(); //clean for any possible next requests
        return;
      }else {
        unsigned int vTrack;
        unsigned int aTrack;
        unsigned long long bTime;
        unsigned long long eTime;
        if (sscanf(relPushUrl.c_str(), "/%u_%u/index_%llu_%llu.m3u", &vTrack, &aTrack, &bTime, &eTime) == 4) {
          if (eTime < bTime){
            eTime = bTime;
          }
          H.setCORSHeaders();
          H.SetBody(pushLiveIndex(vTrack, bTime, eTime));
          H.SendResponse("200", "OK", myConn);
          H.Clean(); //clean for any possible next requests
          return;
        }
      }
      H.SetBody("The HLS URL wasn't understood - what did you want, exactly?\n");
      myConn.SendNow(H.BuildResponse("404", "URL mismatch"));
      H.Clean(); //clean for any possible next requests
      return;
    }else if (HTTP::URL(H.url).getExt().substr(0, 3) != "m3u") {
      std::string tmpStr = H.getUrl().substr(5 + streamName.size());
      long long unsigned int from;
      if (sscanf(tmpStr.c_str(), "/%u_%u/%llu_%llu.ts", &vidTrack, &audTrack, &from, &until) != 4) {
        if (sscanf(tmpStr.c_str(), "/%u/%llu_%llu.ts", &vidTrack, &from, &until) != 3) {
          DEBUG_MSG(DLVL_MEDIUM, "Could not parse URL: %s", H.getUrl().c_str());
          H.Clean();
          H.setCORSHeaders();
          H.SetBody("The HLS URL wasn't understood - what did you want, exactly?\n");
          myConn.SendNow(H.BuildResponse("404", "URL mismatch"));
          H.Clean(); //clean for any possible next requests
          return;
        } else {
          selectedTracks.clear();
          selectedTracks.insert(vidTrack);
        }
      } else {
        selectedTracks.clear();
        selectedTracks.insert(vidTrack);
        selectedTracks.insert(audTrack);
      }
      for (std::map<unsigned int,DTSC::Track>::iterator it = myMeta.tracks.begin(); it != myMeta.tracks.end(); it++){
        if (it->second.codec == "ID3"){
          selectedTracks.insert(it->first);
        }
      }

      //Keep a reference to the main track
      //This is called vidTrack, even for audio-only streams
      DTSC::Track & Trk = myMeta.tracks[vidTrack];

      if (myMeta.live) {
        if (from < Trk.firstms){
          H.Clean();
          H.setCORSHeaders();
          H.SetBody("The requested fragment is no longer kept in memory on the server and cannot be served.\n");
          myConn.SendNow(H.BuildResponse("404", "Fragment out of range"));
          H.Clean(); //clean for any possible next requests
          WARN_MSG("Fragment @ %llu too old", from);
          return;
        }
      }

      H.SetHeader("Content-Type", "video/mp2t");
      H.setCORSHeaders();
      if (hasSessionIDs()){
        H.SetHeader("Cache-Control", "no-cache");
      }else{
        H.SetHeader("Cache-Control", "public, max-age=600, immutable");
        H.SetHeader("Pragma", "");
        H.SetHeader("Expires", "");
      }
      if(method == "OPTIONS" || method == "HEAD"){
        H.SendResponse("200", "OK", myConn);
        H.Clean();
        return;
      }

      H.StartResponse(H, myConn, VLCworkaround || config->getBool("nonchunked"));
      //we assume whole fragments - but timestamps may be altered at will
      uint32_t fragIndice = Trk.timeToFragnum(from);
      contPAT = Trk.missedFrags + fragIndice; //PAT continuity counter
      contPMT = Trk.missedFrags + fragIndice; //PMT continuity counter
      contSDT = Trk.missedFrags + fragIndice; //SDT continuity counter
      packCounter = 0;
      parseData = true;
      wantRequest = false;
      seek(from);
      ts_from = from;
    } else {
      initialize();
      std::string request = H.url.substr(H.url.find("/", 5) + 1);
      H.Clean();
      H.setCORSHeaders();
      H.SetHeader("Content-Type", "application/vnd.apple.mpegurl");
      if (!myMeta.tracks.size()){
        H.SendResponse("404", "Not online or found", myConn);
        H.Clean();
        return;
      }
      if(method == "OPTIONS" || method == "HEAD"){
        H.SendResponse("200", "OK", myConn);
        H.Clean();
        return;
      }
      std::string manifest;
      if (request.find("/") == std::string::npos) {
        manifest = liveIndex();
      } else {
        int selectId = atoi(request.substr(0, request.find("/")).c_str());
        manifest = liveIndex(selectId, sessId);
      }
      H.SetBody(manifest);
      H.SendResponse("200", "OK", myConn);
    }
  }

  void OutHLS::sendNext(){
    //First check if we need to stop.
    if (thisPacket.getTime() >= until){
      stop();
      wantRequest = true;
      parseData = false;

      //Ensure alignment of contCounters for selected tracks, to prevent discontinuities.
      for (std::set<unsigned long>::iterator it = selectedTracks.begin(); it != selectedTracks.end(); ++it){
        DTSC::Track & Trk = myMeta.tracks[*it];
        uint32_t pkgPid = 255 + *it;
        int & contPkg = contCounters[pkgPid];
        if (contPkg % 16 != 0){
          packData.clear();
          packData.setPID(pkgPid);
          packData.addStuffing();
          while (contPkg % 16 != 0){
            packData.setContinuityCounter(++contPkg);
            sendTS(packData.checkAndGetBuffer());
          }
          packData.clear();
        }
      }

      //Signal end of data
      H.Chunkify("", 0, myConn);
      return;
    }
    //Invoke the generic TS output sendNext handler
    TSOutput::sendNext();
  }

  void OutHLS::sendTS(const char * tsData, unsigned int len) {
    H.Chunkify(tsData, len, myConn);
  }

  void OutHLS::onFail(const std::string & msg, bool critical){
    if (H.url.find(".m3u") == std::string::npos){
      HTTPOutput::onFail(msg, critical);
      return;
    }
    H.Clean(); //make sure no parts of old requests are left in any buffers
    H.SetHeader("Server", "MistServer/" PACKAGE_VERSION);
    H.setCORSHeaders();
    H.SetHeader("Content-Type", "application/vnd.apple.mpegurl");
    H.SetHeader("Cache-Control", "no-cache");
    H.SetBody("#EXTM3U\r\n#EXT-X-ERROR: "+msg+"\r\n#EXT-X-ENDLIST\r\n");
    H.SendResponse("200", "OK", myConn);
    Output::onFail(msg, critical);
  }
}
