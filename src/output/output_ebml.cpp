#include "output_ebml.h"
#include <mist/ebml_socketglue.h>
#include <mist/opus.h>
#include <mist/riff.h>

namespace Mist{
  OutEBML::OutEBML(Socket::Connection &conn) : HTTPOutput(conn){
    currentClusterTime = 0;
    newClusterTime = 0;
    segmentSize = 0xFFFFFFFFFFFFFFFFull;
    tracksSize = 0;
    infoSize = 0;
    cuesSize = 0;
    seekheadSize = 0;
    doctype = "matroska";
    if (config->getString("target").size()){
      if (config->getString("target").find(".webm") != std::string::npos){doctype = "webm";}
      initialize();
      if (myMeta.vod){calcVodSizes();}
      if (!streamName.size()){
        WARN_MSG("Recording unconnected EBML output to file! Cancelled.");
        conn.close();
        return;
      }
      if (config->getString("target") == "-"){
        parseData = true;
        wantRequest = false;
        INFO_MSG("Outputting %s to stdout in EBML format", streamName.c_str());
        return;
      }
      if (!myMeta.tracks.size()){
        INFO_MSG("Stream not available - aborting");
        conn.close();
        return;
      }
      if (connectToFile(config->getString("target"))){
        parseData = true;
        wantRequest = false;
        INFO_MSG("Recording %s to %s in EBML format", streamName.c_str(),
                 config->getString("target").c_str());
        return;
      }
      conn.close();
    }
  }

  void OutEBML::init(Util::Config *cfg){
    HTTPOutput::init(cfg);
    capa["name"] = "EBML";
    capa["friendly"] = "WebM/MKV over HTTP";
    capa["desc"] = "Pseudostreaming in MKV and WebM (EBML) formats over HTTP";
    capa["url_rel"] = "/$.webm";
    capa["url_match"].append("/$.mkv");
    capa["url_match"].append("/$.webm");
    capa["codecs"][0u][0u].append("H264");
    capa["codecs"][0u][0u].append("HEVC");
    capa["codecs"][0u][0u].append("VP8");
    capa["codecs"][0u][0u].append("VP9");
    capa["codecs"][0u][0u].append("theora");
    capa["codecs"][0u][0u].append("MPEG2");
    capa["codecs"][0u][0u].append("AV1");
    capa["codecs"][0u][1u].append("AAC");
    capa["codecs"][0u][1u].append("vorbis");
    capa["codecs"][0u][1u].append("opus");
    capa["codecs"][0u][1u].append("PCM");
    capa["codecs"][0u][1u].append("ALAW");
    capa["codecs"][0u][1u].append("ULAW");
    capa["codecs"][0u][1u].append("MP2");
    capa["codecs"][0u][1u].append("MP3");
    capa["codecs"][0u][1u].append("FLOAT");
    capa["codecs"][0u][1u].append("AC3");
    capa["codecs"][0u][1u].append("DTS");
    capa["codecs"][0u][2u].append("+JSON");
    capa["methods"][0u]["handler"] = "http";
    capa["methods"][0u]["type"] = "html5/video/webm";
    capa["methods"][0u]["priority"] = 9;
    // Browsers only support VP8/VP9/Opus codecs, except Chrome which is more lenient.
    JSON::Value blacklistNonChrome = JSON::fromString(
        "[[\"blacklist\", [\"Mozilla/\"]], [\"whitelist\",[\"Chrome\",\"Chromium\"]], "
        "[\"blacklist\",[\"Edge\",\"OPR/\"]], [\"blacklist\",[\"Android\"]]]");
    capa["exceptions"]["codec:H264"] = blacklistNonChrome;
    capa["exceptions"]["codec:HEVC"] = blacklistNonChrome;
    capa["exceptions"]["codec:theora"] = blacklistNonChrome;
    capa["exceptions"]["codec:MPEG2"] = blacklistNonChrome;
    capa["exceptions"]["codec:AAC"] = blacklistNonChrome;
    capa["exceptions"]["codec:vorbis"] = blacklistNonChrome;
    capa["exceptions"]["codec:PCM"] = blacklistNonChrome;
    capa["exceptions"]["codec:ALAW"] = blacklistNonChrome;
    capa["exceptions"]["codec:ULAW"] = blacklistNonChrome;
    capa["exceptions"]["codec:MP2"] = blacklistNonChrome;
    capa["exceptions"]["codec:MP3"] = blacklistNonChrome;
    capa["exceptions"]["codec:FLOAT"] = blacklistNonChrome;
    capa["exceptions"]["codec:AC3"] = blacklistNonChrome;
    capa["exceptions"]["codec:DTS"] = blacklistNonChrome;
    capa["push_urls"].append("/*.mkv");
    capa["push_urls"].append("/*.webm");

    JSON::Value opt;
    opt["arg"] = "string";
    opt["default"] = "";
    opt["arg_num"] = 1;
    opt["help"] = "Target filename to store EBML file as, or - for stdout.";
    cfg->addOption("target", opt);
  }

  bool OutEBML::isRecording(){return config->getString("target").size();}

  /// Calculates the size of a Cluster (contents only) and returns it.
  /// Bases the calculation on the currently selected tracks and the given start/end time for the cluster.
  uint32_t OutEBML::clusterSize(uint64_t start, uint64_t end){
    uint32_t sendLen = EBML::sizeElemUInt(EBML::EID_TIMECODE, start);
    for (std::set<long unsigned int>::iterator it = selectedTracks.begin(); it != selectedTracks.end(); it++){
      DTSC::Track &thisTrack = myMeta.tracks[*it];
      uint32_t firstPart = 0;
      unsigned long long int prevParts = 0;
      uint64_t curMS = 0;
      for (std::deque<DTSC::Key>::iterator it2 = thisTrack.keys.begin(); it2 != thisTrack.keys.end(); it2++){
        if (it2->getTime() > start && it2 != thisTrack.keys.begin()){break;}
        firstPart += prevParts;
        prevParts = it2->getParts();
        curMS = it2->getTime();
      }
      size_t maxParts = thisTrack.parts.size();
      for (size_t i = firstPart; i < maxParts; i++){
        if (curMS >= end){break;}
        if (curMS >= start){
          uint32_t blkLen = EBML::sizeSimpleBlock(thisTrack.trackID, thisTrack.parts[i].getSize());
          sendLen += blkLen;
        }
        curMS += thisTrack.parts[i].getDuration();
      }
    }
    return sendLen;
  }

  void OutEBML::sendNext(){
    if (thisPacket.getTime() >= newClusterTime){
      if (liveSeek()){return;}
      currentClusterTime = thisPacket.getTime();
      if (myMeta.vod){
        // In case of VoD, clusters are aligned with the main track fragments
        // EXCEPT when they are more than 30 seconds long, because clusters are limited to -32 to 32 seconds.
        DTSC::Track &Trk = myMeta.tracks[getMainSelectedTrack()];
        uint32_t fragIndice = Trk.timeToFragnum(currentClusterTime);
        newClusterTime = Trk.getKey(Trk.fragments[fragIndice].getNumber()).getTime() +
                         Trk.fragments[fragIndice].getDuration();
        // Limit clusters to 30s, and the last fragment should always be 30s, just in case.
        if ((newClusterTime - currentClusterTime > 30000) || (fragIndice == Trk.fragments.size() - 1)){
          newClusterTime = currentClusterTime + 30000;
        }
        EXTREME_MSG("Cluster: %llu - %llu (%lu/%lu) = %llu", currentClusterTime, newClusterTime,
                    fragIndice, Trk.fragments.size(), clusterSize(currentClusterTime, newClusterTime));
      }else{
        // In live, clusters are aligned with the lookAhead time
        newClusterTime = currentClusterTime + (needsLookAhead ? needsLookAhead : 1);
        // EXCEPT if there's a keyframe within the lookAhead window, then align to that keyframe
        // instead This makes sure that inlineRestartCapable works as intended
        uint64_t nxtKTime = nextKeyTime();
        if (nxtKTime && nxtKTime < newClusterTime){newClusterTime = nxtKTime;}
      }
      EBML::sendElemHead(myConn, EBML::EID_CLUSTER, clusterSize(currentClusterTime, newClusterTime));
      EBML::sendElemUInt(myConn, EBML::EID_TIMECODE, currentClusterTime);
    }

    EBML::sendSimpleBlock(myConn, thisPacket, currentClusterTime,
                          myMeta.tracks[thisPacket.getTrackId()].type != "video");
  }

  std::string OutEBML::trackCodecID(const DTSC::Track &Trk){
    if (Trk.codec == "opus"){return "A_OPUS";}
    if (Trk.codec == "H264"){return "V_MPEG4/ISO/AVC";}
    if (Trk.codec == "HEVC"){return "V_MPEGH/ISO/HEVC";}
    if (Trk.codec == "VP8"){return "V_VP8";}
    if (Trk.codec == "VP9"){return "V_VP9";}
    if (Trk.codec == "AV1"){return "V_AV1";}
    if (Trk.codec == "AAC"){return "A_AAC";}
    if (Trk.codec == "vorbis"){return "A_VORBIS";}
    if (Trk.codec == "theora"){return "V_THEORA";}
    if (Trk.codec == "MPEG2"){return "V_MPEG2";}
    if (Trk.codec == "PCM"){return "A_PCM/INT/BIG";}
    if (Trk.codec == "MP2"){return "A_MPEG/L2";}
    if (Trk.codec == "MP3"){return "A_MPEG/L3";}
    if (Trk.codec == "AC3"){return "A_AC3";}
    if (Trk.codec == "ALAW"){return "A_MS/ACM";}
    if (Trk.codec == "ULAW"){return "A_MS/ACM";}
    if (Trk.codec == "FLOAT"){return "A_PCM/FLOAT/IEEE";}
    if (Trk.codec == "DTS"){return "A_DTS";}
    if (Trk.codec == "JSON"){return "M_JSON";}
    return "E_UNKNOWN";
  }

  void OutEBML::sendElemTrackEntry(const DTSC::Track &Trk){
    // First calculate the sizes of the TrackEntry and Audio/Video elements.
    uint32_t sendLen = 0;
    uint32_t subLen = 0;
    sendLen += EBML::sizeElemUInt(EBML::EID_TRACKNUMBER, Trk.trackID);
    sendLen += EBML::sizeElemUInt(EBML::EID_TRACKUID, Trk.trackID);
    sendLen += EBML::sizeElemStr(EBML::EID_CODECID, trackCodecID(Trk));
    sendLen += EBML::sizeElemStr(EBML::EID_LANGUAGE, Trk.lang.size() ? Trk.lang : "und");
    sendLen += EBML::sizeElemUInt(EBML::EID_FLAGLACING, 0);
    if (Trk.codec == "ALAW" || Trk.codec == "ULAW"){
      sendLen += EBML::sizeElemStr(EBML::EID_CODECPRIVATE, std::string((size_t)18, '\000'));
    }else{
      if (Trk.init.size()){sendLen += EBML::sizeElemStr(EBML::EID_CODECPRIVATE, Trk.init);}
    }
    if (Trk.codec == "opus" && Trk.init.size() > 11){
      sendLen += EBML::sizeElemUInt(EBML::EID_CODECDELAY, Opus::getPreSkip(Trk.init.data()) * 1000000 / 48);
      sendLen += EBML::sizeElemUInt(EBML::EID_SEEKPREROLL, 80000000);
    }
    if (Trk.type == "video"){
      sendLen += EBML::sizeElemUInt(EBML::EID_TRACKTYPE, 1);
      subLen += EBML::sizeElemUInt(EBML::EID_PIXELWIDTH, Trk.width);
      subLen += EBML::sizeElemUInt(EBML::EID_PIXELHEIGHT, Trk.height);
      subLen += EBML::sizeElemUInt(EBML::EID_DISPLAYWIDTH, Trk.width);
      subLen += EBML::sizeElemUInt(EBML::EID_DISPLAYHEIGHT, Trk.height);
      sendLen += EBML::sizeElemHead(EBML::EID_VIDEO, subLen);
    }
    if (Trk.type == "audio"){
      sendLen += EBML::sizeElemUInt(EBML::EID_TRACKTYPE, 2);
      subLen += EBML::sizeElemUInt(EBML::EID_CHANNELS, Trk.channels);
      subLen += EBML::sizeElemDbl(EBML::EID_SAMPLINGFREQUENCY, Trk.rate);
      subLen += EBML::sizeElemUInt(EBML::EID_BITDEPTH, Trk.size);
      sendLen += EBML::sizeElemHead(EBML::EID_AUDIO, subLen);
    }
    if (Trk.type == "meta"){sendLen += EBML::sizeElemUInt(EBML::EID_TRACKTYPE, 3);}
    sendLen += subLen;

    // Now actually send.
    EBML::sendElemHead(myConn, EBML::EID_TRACKENTRY, sendLen);
    EBML::sendElemUInt(myConn, EBML::EID_TRACKNUMBER, Trk.trackID);
    EBML::sendElemUInt(myConn, EBML::EID_TRACKUID, Trk.trackID);
    EBML::sendElemStr(myConn, EBML::EID_CODECID, trackCodecID(Trk));
    EBML::sendElemStr(myConn, EBML::EID_LANGUAGE, Trk.lang.size() ? Trk.lang : "und");
    EBML::sendElemUInt(myConn, EBML::EID_FLAGLACING, 0);
    if (Trk.codec == "ALAW" || Trk.codec == "ULAW"){
      std::string init = RIFF::fmt::generate(((Trk.codec == "ALAW") ? 6 : 7), Trk.channels, Trk.rate,
                                             Trk.bps, Trk.channels * (Trk.size << 3), Trk.size);
      EBML::sendElemStr(myConn, EBML::EID_CODECPRIVATE, init.substr(8));
    }else{
      if (Trk.init.size()){EBML::sendElemStr(myConn, EBML::EID_CODECPRIVATE, Trk.init);}
    }
    if (Trk.codec == "opus"){
      EBML::sendElemUInt(myConn, EBML::EID_CODECDELAY, Opus::getPreSkip(Trk.init.data()) * 1000000 / 48);
      EBML::sendElemUInt(myConn, EBML::EID_SEEKPREROLL, 80000000);
    }
    if (Trk.type == "video"){
      EBML::sendElemUInt(myConn, EBML::EID_TRACKTYPE, 1);
      EBML::sendElemHead(myConn, EBML::EID_VIDEO, subLen);
      EBML::sendElemUInt(myConn, EBML::EID_PIXELWIDTH, Trk.width);
      EBML::sendElemUInt(myConn, EBML::EID_PIXELHEIGHT, Trk.height);
      EBML::sendElemUInt(myConn, EBML::EID_DISPLAYWIDTH, Trk.width);
      EBML::sendElemUInt(myConn, EBML::EID_DISPLAYHEIGHT, Trk.height);
    }
    if (Trk.type == "audio"){
      EBML::sendElemUInt(myConn, EBML::EID_TRACKTYPE, 2);
      EBML::sendElemHead(myConn, EBML::EID_AUDIO, subLen);
      EBML::sendElemUInt(myConn, EBML::EID_CHANNELS, Trk.channels);
      EBML::sendElemDbl(myConn, EBML::EID_SAMPLINGFREQUENCY, Trk.rate);
      EBML::sendElemUInt(myConn, EBML::EID_BITDEPTH, Trk.size);
    }
    if (Trk.type == "meta"){EBML::sendElemUInt(myConn, EBML::EID_TRACKTYPE, 3);}
  }

  uint32_t OutEBML::sizeElemTrackEntry(const DTSC::Track &Trk){
    // Calculate the sizes of the TrackEntry and Audio/Video elements.
    uint32_t sendLen = 0;
    uint32_t subLen = 0;
    sendLen += EBML::sizeElemUInt(EBML::EID_TRACKNUMBER, Trk.trackID);
    sendLen += EBML::sizeElemUInt(EBML::EID_TRACKUID, Trk.trackID);
    sendLen += EBML::sizeElemStr(EBML::EID_CODECID, trackCodecID(Trk));
    sendLen += EBML::sizeElemStr(EBML::EID_LANGUAGE, Trk.lang.size() ? Trk.lang : "und");
    sendLen += EBML::sizeElemUInt(EBML::EID_FLAGLACING, 0);
    if (Trk.codec == "ALAW" || Trk.codec == "ULAW"){
      sendLen += EBML::sizeElemStr(EBML::EID_CODECPRIVATE, std::string((size_t)18, '\000'));
    }else{
      if (Trk.init.size()){sendLen += EBML::sizeElemStr(EBML::EID_CODECPRIVATE, Trk.init);}
    }
    if (Trk.codec == "opus"){
      sendLen += EBML::sizeElemUInt(EBML::EID_CODECDELAY, Opus::getPreSkip(Trk.init.data()) * 1000000 / 48);
      sendLen += EBML::sizeElemUInt(EBML::EID_SEEKPREROLL, 80000000);
    }
    if (Trk.type == "video"){
      sendLen += EBML::sizeElemUInt(EBML::EID_TRACKTYPE, 1);
      subLen += EBML::sizeElemUInt(EBML::EID_PIXELWIDTH, Trk.width);
      subLen += EBML::sizeElemUInt(EBML::EID_PIXELHEIGHT, Trk.height);
      subLen += EBML::sizeElemUInt(EBML::EID_DISPLAYWIDTH, Trk.width);
      subLen += EBML::sizeElemUInt(EBML::EID_DISPLAYHEIGHT, Trk.height);
      sendLen += EBML::sizeElemHead(EBML::EID_VIDEO, subLen);
    }
    if (Trk.type == "audio"){
      sendLen += EBML::sizeElemUInt(EBML::EID_TRACKTYPE, 2);
      subLen += EBML::sizeElemUInt(EBML::EID_CHANNELS, Trk.channels);
      subLen += EBML::sizeElemDbl(EBML::EID_SAMPLINGFREQUENCY, Trk.rate);
      subLen += EBML::sizeElemUInt(EBML::EID_BITDEPTH, Trk.size);
      sendLen += EBML::sizeElemHead(EBML::EID_AUDIO, subLen);
    }
    if (Trk.type == "meta"){sendLen += EBML::sizeElemUInt(EBML::EID_TRACKTYPE, 3);}
    sendLen += subLen;
    return EBML::sizeElemHead(EBML::EID_TRACKENTRY, sendLen) + sendLen;
  }

  void OutEBML::sendHeader(){
    double duration = 0;
    DTSC::Track &Trk = myMeta.tracks[getMainSelectedTrack()];
    if (myMeta.vod){duration = Trk.lastms - Trk.firstms;}
    if (myMeta.live){needsLookAhead = 420;}
    // EBML header and Segment
    EBML::sendElemEBML(myConn, doctype);
    EBML::sendElemHead(myConn, EBML::EID_SEGMENT, segmentSize); // Default = Unknown size
    if (myMeta.vod){
      // SeekHead
      EBML::sendElemHead(myConn, EBML::EID_SEEKHEAD, seekSize);
      EBML::sendElemSeek(myConn, EBML::EID_INFO, seekheadSize);
      EBML::sendElemSeek(myConn, EBML::EID_TRACKS, seekheadSize + infoSize);
      EBML::sendElemSeek(myConn, EBML::EID_CUES, seekheadSize + infoSize + tracksSize);
    }
    // Info
    EBML::sendElemInfo(myConn, "MistServer " PACKAGE_VERSION, duration);
    // Tracks
    uint32_t trackSizes = 0;
    for (std::set<long unsigned int>::iterator it = selectedTracks.begin(); it != selectedTracks.end(); it++){
      trackSizes += sizeElemTrackEntry(myMeta.tracks[*it]);
    }
    EBML::sendElemHead(myConn, EBML::EID_TRACKS, trackSizes);
    for (std::set<long unsigned int>::iterator it = selectedTracks.begin(); it != selectedTracks.end(); it++){
      sendElemTrackEntry(myMeta.tracks[*it]);
    }
    if (myMeta.vod){
      EBML::sendElemHead(myConn, EBML::EID_CUES, cuesSize);
      uint64_t tmpsegSize = infoSize + tracksSize + seekheadSize + cuesSize +
                            EBML::sizeElemHead(EBML::EID_CUES, cuesSize);
      for (std::map<uint64_t, uint64_t>::iterator it = clusterSizes.begin(); it != clusterSizes.end(); ++it){
        EBML::sendElemCuePoint(myConn, it->first, Trk.trackID, tmpsegSize, 0);
        tmpsegSize += it->second;
      }
    }
    sentHeader = true;
  }

  /// Seeks to the given byte position by doing a regular seek and remembering the byte offset from that point
  void OutEBML::byteSeek(uint64_t startPos){
    INFO_MSG("Seeking to %llu bytes", startPos);
    sentHeader = false;
    newClusterTime = 0;
    if (startPos == 0){
      seek(0);
      return;
    }
    uint64_t headerSize = EBML::sizeElemEBML(doctype) +
                          EBML::sizeElemHead(EBML::EID_SEGMENT, segmentSize) + seekheadSize + infoSize +
                          tracksSize + EBML::sizeElemHead(EBML::EID_CUES, cuesSize) + cuesSize;
    if (startPos < headerSize){
      HIGH_MSG("Seek went into or before header");
      seek(0);
      myConn.skipBytes(startPos);
      return;
    }
    startPos -= headerSize;
    sentHeader = true; // skip the header
    DTSC::Track &Trk = myMeta.tracks[getMainSelectedTrack()];
    for (std::map<uint64_t, uint64_t>::iterator it = clusterSizes.begin(); it != clusterSizes.end(); ++it){
      VERYHIGH_MSG("Cluster %llu (%llu bytes) -> %llu to go", it->first, it->second, startPos);
      if (startPos < it->second){
        HIGH_MSG("Seek to fragment at %llu ms", it->first);
        myConn.skipBytes(startPos);
        seek(it->first);
        newClusterTime = it->first;
        return;
      }
      startPos -= it->second;
    }
    // End of file. This probably won't work right, but who cares, it's the end of the file.
  }

  void OutEBML::onHTTP(){
    std::string method = H.method;
    if (method == "OPTIONS" || method == "HEAD"){
      H.Clean();
      H.setCORSHeaders();
      H.SetHeader("Content-Type", "video/MP4");
      H.SetHeader("Accept-Ranges", "bytes, parsec");
      H.SendResponse("200", "OK", myConn);
      return;
    }
    if (H.url.find(".webm") != std::string::npos){
      doctype = "webm";
    }else{
      doctype = "matroska";
    }

    // Calculate the sizes of various parts, if we're VoD.
    uint64_t totalSize = 0;
    if (myMeta.vod){
      calcVodSizes();
      // We now know the full size of the segment, thus can calculate the total size
      totalSize = EBML::sizeElemEBML(doctype) + EBML::sizeElemHead(EBML::EID_SEGMENT, segmentSize) + segmentSize;
    }

    uint64_t byteEnd = totalSize - 1;
    uint64_t byteStart = 0;

    /*LTS-START*/
    // allow setting of max lead time through buffer variable.
    // max lead time is set in MS, but the variable is in integer seconds for simplicity.
    if (H.GetVar("buffer") != ""){maxSkipAhead = JSON::Value(H.GetVar("buffer")).asInt() * 1000;}
    // allow setting of play back rate through buffer variable.
    // play back rate is set in MS per second, but the variable is a simple multiplier.
    if (H.GetVar("rate") != ""){
      long long int multiplier = JSON::Value(H.GetVar("rate")).asInt();
      if (multiplier){
        realTime = 1000 / multiplier;
      }else{
        realTime = 0;
      }
    }
    if (H.GetHeader("X-Mist-Rate") != ""){
      long long int multiplier = JSON::Value(H.GetHeader("X-Mist-Rate")).asInt();
      if (multiplier){
        realTime = 1000 / multiplier;
      }else{
        realTime = 0;
      }
    }
    /*LTS-END*/

    char rangeType = ' ';
    if (!myMeta.live){
      if (H.GetHeader("Range") != ""){
        if (parseRange(byteStart, byteEnd)){
          if (H.GetVar("buffer") == ""){
            DTSC::Track &Trk = myMeta.tracks[getMainSelectedTrack()];
            maxSkipAhead = (Trk.lastms - Trk.firstms) / 20 + 7500;
          }
        }
        rangeType = H.GetHeader("Range")[0];
      }
    }
    H.Clean(); // make sure no parts of old requests are left in any buffers
    H.setCORSHeaders();
    H.SetHeader("Content-Type", "video/webm");
    if (myMeta.vod){H.SetHeader("Accept-Ranges", "bytes, parsec");}
    if (rangeType != ' '){
      if (!byteEnd){
        if (rangeType == 'p'){
          H.SetBody("Starsystem not in communications range");
          H.SendResponse("416", "Starsystem not in communications range", myConn);
          return;
        }else{
          H.SetBody("Requested Range Not Satisfiable");
          H.SendResponse("416", "Requested Range Not Satisfiable", myConn);
          return;
        }
      }else{
        std::stringstream rangeReply;
        rangeReply << "bytes " << byteStart << "-" << byteEnd << "/" << totalSize;
        H.SetHeader("Content-Length", byteEnd - byteStart + 1);
        H.SetHeader("Content-Range", rangeReply.str());
        /// \todo Switch to chunked?
        H.SendResponse("206", "Partial content", myConn);
        // H.StartResponse("206", "Partial content", HTTP_R, conn);
        byteSeek(byteStart);
      }
    }else{
      if (myMeta.vod){H.SetHeader("Content-Length", byteEnd - byteStart + 1);}
      /// \todo Switch to chunked?
      H.SendResponse("200", "OK", myConn);
      // HTTP_S.StartResponse(HTTP_R, conn);
    }
    parseData = true;
    wantRequest = false;
  }

  void OutEBML::calcVodSizes(){
    if (segmentSize != 0xFFFFFFFFFFFFFFFFull){
      // Already calculated
      return;
    }
    DTSC::Track &Trk = myMeta.tracks[getMainSelectedTrack()];
    double duration = Trk.lastms - Trk.firstms;
    // Calculate the segment size
    // Segment contains SeekHead, Info, Tracks, Cues (in that order)
    // Howeveer, SeekHead is dependent on Info/Tracks sizes, so we calculate those first.
    // Calculating Info size
    infoSize = EBML::sizeElemInfo("MistServer " PACKAGE_VERSION, duration);
    // Calculating Tracks size
    tracksSize = 0;
    for (std::set<long unsigned int>::iterator it = selectedTracks.begin(); it != selectedTracks.end(); it++){
      tracksSize += sizeElemTrackEntry(myMeta.tracks[*it]);
    }
    tracksSize += EBML::sizeElemHead(EBML::EID_TRACKS, tracksSize);
    // Calculating SeekHead size
    // Positions are relative to the first Segment, byte 0 = first byte of contents of Segment.
    // Tricky starts here: the size of the SeekHead element is dependent on the seek offsets contained inside,
    // which are in turn dependent on the size of the SeekHead element. Fun times! We loop until it stabilizes.
    uint32_t oldseekSize = 0;
    do{
      oldseekSize = seekSize;
      seekSize = EBML::sizeElemSeek(EBML::EID_INFO, seekheadSize) +
                 EBML::sizeElemSeek(EBML::EID_TRACKS, seekheadSize + infoSize) +
                 EBML::sizeElemSeek(EBML::EID_CUES, seekheadSize + infoSize + tracksSize);
      seekheadSize = EBML::sizeElemHead(EBML::EID_SEEKHEAD, seekSize) + seekSize;
    }while (seekSize != oldseekSize);
    // The Cues are tricky: the Cluster offsets are dependent on the size of Cues itself.
    // Which, in turn, is dependent on the Cluster offsets.
    // We make this a bit easier by pre-calculating the sizes of all clusters first
    uint64_t fragNo = 0;
    for (std::deque<DTSC::Fragment>::iterator it = Trk.fragments.begin(); it != Trk.fragments.end(); ++it){
      uint64_t clusterStart = Trk.getKey(it->getNumber()).getTime();
      uint64_t clusterEnd = clusterStart + it->getDuration();
      // The first fragment always starts at time 0, even if the main track does not.
      if (!fragNo){clusterStart = 0;}
      uint64_t clusterTmpEnd = clusterEnd;
      do{
        clusterTmpEnd = clusterEnd;
        // The last fragment always ends at the end, even if the main track does not.
        if (fragNo == Trk.fragments.size() - 1){clusterTmpEnd = clusterStart + 30000;}
        // Limit clusters to 30 seconds.
        if (clusterTmpEnd - clusterStart > 30000){clusterTmpEnd = clusterStart + 30000;}
        uint64_t cSize = clusterSize(clusterStart, clusterTmpEnd);
        clusterSizes[clusterStart] = cSize + EBML::sizeElemHead(EBML::EID_CLUSTER, cSize);
        clusterStart = clusterTmpEnd; // Continue at the end of this cluster, if continuing.
      }while (clusterTmpEnd < clusterEnd);
      ++fragNo;
    }
    // Calculating Cues size
    // We also calculate Clusters here: Clusters are grouped by fragments of the main track.
    // CueClusterPosition uses the same offsets as SeekPosition.
    // CueRelativePosition is the offset from that Cluster's first content byte.
    // All this uses the same technique as above. More fun times!
    uint32_t oldcuesSize = 0;
    do{
      oldcuesSize = cuesSize;
      segmentSize = infoSize + tracksSize + seekheadSize + cuesSize +
                    EBML::sizeElemHead(EBML::EID_CUES, cuesSize);
      uint32_t cuesInside = 0;
      for (std::map<uint64_t, uint64_t>::iterator it = clusterSizes.begin(); it != clusterSizes.end(); ++it){
        cuesInside += EBML::sizeElemCuePoint(it->first, Trk.trackID, segmentSize, 0);
        segmentSize += it->second;
      }
      cuesSize = cuesInside;
    }while (cuesSize != oldcuesSize);
  }

}// namespace Mist
