/// \file stream.cpp
/// Utilities for handling streams.

#include "stream.h"
#include "config.h"
#include "defines.h"
#include "dtsc.h"
#include "json.h"
#include "procs.h"
#include "shared_memory.h"
#include "socket.h"
#include "mp4_generic.h"
#include <semaphore.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

std::string Util::codecString(const std::string & codec, const std::string & initData){
  if (codec == "H264"){ 
    std::stringstream r;
    MP4::AVCC avccBox;
    avccBox.setPayload(initData);
    r << "avc1.";
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[1] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[2] << std::dec;
    r << std::hex << std::setw(2) << std::setfill('0') << (int)initData[3] << std::dec;
    return r.str();
  }
  if (codec == "AAC"){return "mp4a.40.2";}
  if (codec == "MP3"){return "mp4a.40.34";}
  if (codec == "AC3"){return "ec-3";}
  return "";
}

std::string Util::getTmpFolder(){
  std::string dir;
  char *tmp_char = 0;
  if (!tmp_char){tmp_char = getenv("TMP");}
  if (!tmp_char){tmp_char = getenv("TEMP");}
  if (!tmp_char){tmp_char = getenv("TMPDIR");}
  if (tmp_char){
    dir = tmp_char;
    dir += "/mist";
  }else{
#if defined(_WIN32) || defined(_CYGWIN_)
    dir = "C:/tmp/mist";
#else
    dir = "/tmp/mist";
#endif
  }
  if (access(dir.c_str(), 0) != 0){
    mkdir(dir.c_str(),
          S_IRWXU | S_IRWXG | S_IRWXO); // attempt to create mist folder - ignore failures
  }
  return dir + "/";
}

/// Filters the streamname, removing invalid characters and converting all
/// letters to lowercase. If a '?' character is found, everything following
/// that character is deleted. The original string is modified. If a '+' or space
/// exists, then only the part before that is sanitized.
void Util::sanitizeName(std::string &streamname){
  // strip anything that isn't numbers, digits or underscores
  size_t index = streamname.find_first_of("+ ");
  if (index != std::string::npos){
    std::string preplus = streamname.substr(0, index);
    sanitizeName(preplus);
    std::string postplus = streamname.substr(index + 1);
    if (postplus.find('?') != std::string::npos){
      postplus = postplus.substr(0, (postplus.find('?')));
    }
    streamname = preplus + "+" + postplus;
    return;
  }
  for (std::string::iterator i = streamname.end() - 1; i >= streamname.begin(); --i){
    if (*i == '?'){
      streamname.erase(i, streamname.end());
      break;
    }
    if (!isalpha(*i) && !isdigit(*i) && *i != '_' && *i != '.'){
      streamname.erase(i);
    }else{
      *i = tolower(*i);
    }
  }
}

JSON::Value Util::getStreamConfig(const std::string &streamname){
  JSON::Value result;
  if (streamname.size() > 100){
    FAIL_MSG("Stream opening denied: %s is longer than 100 characters (%lu).", streamname.c_str(),
             streamname.size());
    return result;
  }
  std::string smp = streamname.substr(0, streamname.find_first_of("+ "));

  char tmpBuf[NAME_BUFFER_SIZE];
  snprintf(tmpBuf, NAME_BUFFER_SIZE, SHM_STREAM_CONF, smp.c_str());
  Util::DTSCShmReader rStrmConf(tmpBuf);
  DTSC::Scan stream_cfg = rStrmConf.getScan();
  if (!stream_cfg){
    WARN_MSG("Could not get stream '%s' config!", smp.c_str());
    return result;
  }
  return stream_cfg.asJSON();
}

DTSC::Meta Util::getStreamMeta(const std::string &streamname){
  DTSC::Meta ret;
  char pageId[NAME_BUFFER_SIZE];
  snprintf(pageId, NAME_BUFFER_SIZE, SHM_STREAM_INDEX, streamname.c_str());
  IPC::sharedPage mPage(pageId, DEFAULT_STRM_PAGE_SIZE);
  if (!mPage.mapped){
    FAIL_MSG("Could not connect to metadata for %s", streamname.c_str());
    return ret;
  }
  DTSC::Packet tmpMeta(mPage.mapped, mPage.len, true);
  if (tmpMeta.getVersion()){ret.reinit(tmpMeta);}
  return ret;
}

/// Checks if the given streamname has an active input serving it. Returns true if this is the case.
/// Assumes the streamname has already been through sanitizeName()!
bool Util::streamAlive(std::string &streamname){
  char semName[NAME_BUFFER_SIZE];
  snprintf(semName, NAME_BUFFER_SIZE, SEM_INPUT, streamname.c_str());
  IPC::semaphore playerLock(semName, O_RDWR, ACCESSPERMS, 0, true);
  if (!playerLock){return false;}
  if (!playerLock.tryWait()){
    playerLock.close();
    return true;
  }else{
    playerLock.post();
    playerLock.close();
    return false;
  }
}

/// Assures the input for the given stream name is active.
/// Does stream name sanitation first, followed by a stream name length check (<= 100 chars).
/// Then, checks if an input is already active by running streamAlive(). If yes, return true.
/// If no, loads up the server configuration and attempts to start the given stream according to
/// current configuration. At this point, fails and aborts if MistController isn't running.
bool Util::startInput(std::string streamname, std::string filename, bool forkFirst, bool isProvider,
                      const std::map<std::string, std::string> &overrides, pid_t *spawn_pid){
  sanitizeName(streamname);
  if (streamname.size() > 100){
    FAIL_MSG("Stream opening denied: %s is longer than 100 characters (%lu).", streamname.c_str(),
             streamname.size());
    return false;
  }
  // Check if the stream is already active.
  // If yes, don't activate again to prevent duplicate inputs.
  // It's still possible a duplicate starts anyway, this is caught in the inputs initializer.
  // Note: this uses the _whole_ stream name, including + (if any).
  // This means "test+a" and "test+b" have separate locks and do not interact with each other.
  uint8_t streamStat = getStreamStatus(streamname);
  // Wait for a maximum of 240 x 250ms sleeps = 60 seconds
  size_t sleeps = 0;
  while (++sleeps < 240 && streamStat != STRMSTAT_OFF && streamStat != STRMSTAT_READY &&
         (!isProvider || streamStat != STRMSTAT_WAIT)){
    if (streamStat == STRMSTAT_BOOT && overrides.count("throughboot")){break;}
    Util::sleep(250);
    streamStat = getStreamStatus(streamname);
  }
  if (streamAlive(streamname) && !overrides.count("alwaysStart")){
    MEDIUM_MSG("Stream %s already active; continuing", streamname.c_str());
    return true;
  }

  // Find stream base name
  std::string smp = streamname.substr(0, streamname.find_first_of("+ "));
  // check if base name (everything before + or space) exists
  const JSON::Value stream_cfg = getStreamConfig(streamname);
  if (!stream_cfg){
    HIGH_MSG("Stream %s not configured - attempting to ignore", streamname.c_str());
  }

  // Only use configured source if not manually overridden. Abort if no config is available.
  if (!filename.size()){
    if (!stream_cfg){
      MEDIUM_MSG("Stream %s not configured, no source manually given, cannot start", streamname.c_str());
      return false;
    }
    filename = stream_cfg["source"].asStringRef();
  }

  const JSON::Value input = getInputBySource(filename, isProvider);
  if (!input){return false;}

  // copy the necessary arguments to separate storage so we can unlock the config semaphore safely
  std::map<std::string, std::string> str_args;
  // check required parameters
  if (input.isMember("required")){
    jsonForEachConst(input["required"], prm){
      if (!prm->isMember("option")){continue;}
      const std::string opt = (*prm)["option"].asStringRef();
      // check for overrides
      if (overrides.count(opt)){
        HIGH_MSG("Overriding option '%s' to '%s'", prm.key().c_str(), overrides.at(prm.key()).c_str());
        str_args[opt] = overrides.at(opt);
      }else{
        if (!stream_cfg.isMember(prm.key())){
          FAIL_MSG("Required parameter %s for stream %s missing", prm.key().c_str(),
                   streamname.c_str());
          return false;
        }
        if (stream_cfg[prm.key()].isString()){
          str_args[opt] = stream_cfg[prm.key()].asStringRef();
        }else{
          str_args[opt] = stream_cfg[prm.key()].toString();
        }
      }
    }
  }
  // check optional parameters
  if (input.isMember("optional")){
    jsonForEachConst(input["optional"], prm){
      if (!prm->isMember("option")){continue;}
      const std::string opt = (*prm)["option"].asStringRef();
      // check for overrides
      if (overrides.count(opt)){
        HIGH_MSG("Overriding option '%s' to '%s'", prm.key().c_str(), overrides.at(prm.key()).c_str());
        str_args[opt] = overrides.at(opt);
      }else{
        if (stream_cfg.isMember(prm.key()) && stream_cfg[prm.key()]){
          if (stream_cfg[prm.key()].isString()){
            str_args[opt] = stream_cfg[prm.key()].asStringRef();
          }else{
            str_args[opt] = stream_cfg[prm.key()].toString();
          }
          INFO_MSG("Setting option '%s' to '%s' = '%s'", opt.c_str(), stream_cfg[prm.key()].toString().c_str(), str_args[opt].c_str());
        }
      }
      if (!prm->isMember("type") && str_args.count(opt)){str_args[opt] = "";}
    }
  }

  if (isProvider){
    // Set environment variable so we can know if we have a provider when re-exec'ing.
    setenv("MISTPROVIDER", "1", 1);
  }

  std::string player_bin = Util::getMyPath() + "MistIn" + input["name"].asStringRef();
  INFO_MSG("Starting %s -s %s %s", player_bin.c_str(), streamname.c_str(), filename.c_str());
  char *argv[30] ={(char *)player_bin.c_str(), (char *)"-s", (char *)streamname.c_str(),
                    (char *)filename.c_str()};
  int argNum = 3;
  std::string debugLvl;
  if (Util::Config::printDebugLevel != DEBUG && !str_args.count("--debug")){
    debugLvl = JSON::Value(Util::Config::printDebugLevel).asString();
    argv[++argNum] = (char *)"--debug";
    argv[++argNum] = (char *)debugLvl.c_str();
  }
  for (std::map<std::string, std::string>::iterator it = str_args.begin(); it != str_args.end();
       ++it){
    argv[++argNum] = (char *)it->first.c_str();
    if (it->second.size()){argv[++argNum] = (char *)it->second.c_str();}
  }
  argv[++argNum] = (char *)0;

  Util::Procs::setHandler();

  int pid = 0;
  if (forkFirst){
    DONTEVEN_MSG("Forking");
    pid = fork();
    if (pid == -1){
      FAIL_MSG("Forking process for stream %s failed: %s", streamname.c_str(), strerror(errno));
      return false;
    }
    if (pid && overrides.count("singular")){
      Util::Procs::setHandler();
      Util::Procs::remember(pid);
    }
  }else{
    DONTEVEN_MSG("Not forking");
  }

  if (pid == 0){
    Socket::Connection io(0, 1);
    io.drop();
    DONTEVEN_MSG("execvp");
    execvp(argv[0], argv);
    FAIL_MSG("Starting process %s for stream %s failed: %s", argv[0], streamname.c_str(),
             strerror(errno));
    _exit(42);
  }else if (spawn_pid != NULL){
    *spawn_pid = pid;
  }

  unsigned int waiting = 0;
  while (!streamAlive(streamname) && ++waiting < 240){
    Util::wait(250);
    if (!Util::Procs::isRunning(pid)){
      FAIL_MSG("Input process shut down before stream coming online, aborting.");
      break;
    }
  }

  return streamAlive(streamname);
}

JSON::Value Util::getInputBySource(const std::string &filename, bool isProvider){
  std::string tmpFn = filename;
  if (tmpFn.find('?') != std::string::npos){tmpFn.erase(tmpFn.find('?'), std::string::npos);}
  JSON::Value ret;

  // Attempt to load up configuration and find this stream
  Util::DTSCShmReader rCapa(SHM_CAPA);
  DTSC::Scan inputs = rCapa.getMember("inputs");
  // Abort if not available
  if (!inputs){
    FAIL_MSG("Capabilities not available, aborting! Is MistController running?");
    return false;
  }

  // check in curConf for <naam>-priority/source_match
  bool selected = false;
  long long int curPrio = -1;
  DTSC::Scan input;
  unsigned int input_size = inputs.getSize();
  bool noProviderNoPick = false;
  for (unsigned int i = 0; i < input_size; ++i){
    DTSC::Scan tmp_input = inputs.getIndice(i);

    // if match voor current stream && priority is hoger dan wat we al hebben
    if (tmp_input.getMember("source_match") && curPrio < tmp_input.getMember("priority").asInt()){
      if (tmp_input.getMember("source_match").getSize()){
        for (unsigned int j = 0; j < tmp_input.getMember("source_match").getSize(); ++j){
          std::string source = tmp_input.getMember("source_match").getIndice(j).asString();
          std::string front = source.substr(0, source.find('*'));
          std::string back = source.substr(source.find('*') + 1);
          MEDIUM_MSG("Checking input %s: %s (%s)", inputs.getIndiceName(i).c_str(),
                     tmp_input.getMember("name").asString().c_str(), source.c_str());

          if (tmpFn.substr(0, front.size()) == front &&
              tmpFn.substr(tmpFn.size() - back.size()) == back){
            if (tmp_input.getMember("non-provider") && !isProvider){
              noProviderNoPick = true;
              continue;
            }
            curPrio = tmp_input.getMember("priority").asInt();
            selected = true;
            input = tmp_input;
          }
        }
      }else{
        std::string source = tmp_input.getMember("source_match").asString();
        std::string front = source.substr(0, source.find('*'));
        std::string back = source.substr(source.find('*') + 1);
        MEDIUM_MSG("Checking input %s: %s (%s)", inputs.getIndiceName(i).c_str(),
                   tmp_input.getMember("name").asString().c_str(), source.c_str());

        if (tmpFn.substr(0, front.size()) == front && tmpFn.substr(tmpFn.size() - back.size()) == back){
          if (tmp_input.getMember("non-provider") && !isProvider){
            noProviderNoPick = true;
            continue;
          }
          curPrio = tmp_input.getMember("priority").asInt();
          selected = true;
          input = tmp_input;
        }
      }
    }
  }
  if (!selected){
    if (noProviderNoPick){
      INFO_MSG("Not a media provider for input: %s", tmpFn.c_str());
    }else{
      FAIL_MSG("No compatible input found for: %s", tmpFn.c_str());
    }
  }else{
    ret = input.asJSON();
  }
  return ret;
}

uint8_t Util::getStreamStatus(const std::string &streamname){
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_STATE, streamname.c_str());
  IPC::sharedPage streamStatus(pageName, 1, false, false);
  if (!streamStatus){return STRMSTAT_OFF;}
  return streamStatus.mapped[0];
}

/// Checks if a given user agent is allowed according to the given exception.
bool Util::checkException(const JSON::Value & ex, const std::string & useragent){
  //No user agent? Always allow everything.
  if (!useragent.size()){return true;}
  if (!ex.isArray() || !ex.size()){return true;}
  bool ret = true;
  jsonForEachConst(ex, e){
    if (!e->isArray() || !e->size()){continue;}
    bool setTo = false;
    bool except = false;
    //whitelist makes the return value true if any value is contained in the UA, blacklist makes it false.
    //the '_except' variants do so only if none of the values are contained in the UA.
    if ((*e)[0u].asStringRef() == "whitelist"){setTo = true; except = false;}
    if ((*e)[0u].asStringRef() == "whitelist_except"){setTo = true; except = true;}
    if ((*e)[0u].asStringRef() == "blacklist"){setTo = false; except = false;}
    if ((*e)[0u].asStringRef() == "blacklist_except"){setTo = false; except = true;}
    if (e->size() == 1){
      ret = setTo;
      continue;
    }
    if (!(*e)[1].isArray()){continue;}
    bool match = false;
    jsonForEachConst((*e)[1u], i){
      if (useragent.find(i->asStringRef()) != std::string::npos){
        match = true;
      }
    }
    //set the (temp) return value if this was either a match in regular mode, or a non-match in except-mode.
    if (except != match){ret = setTo;}
  }
  return ret;
}

Util::DTSCShmReader::DTSCShmReader(const std::string &pageName){
  rPage.init(pageName, 0, false, false);
  if (rPage){rAcc = Util::RelAccX(rPage.mapped);}
}

DTSC::Scan Util::DTSCShmReader::getMember(const std::string &indice){
  if (!rPage){return DTSC::Scan();}
  return DTSC::Scan(rAcc.getPointer("dtsc_data"), rAcc.getSize("dtsc_data"))
      .getMember(indice.c_str());
}

DTSC::Scan Util::DTSCShmReader::getScan(){
  if (!rPage){return DTSC::Scan();}
  return DTSC::Scan(rAcc.getPointer("dtsc_data"), rAcc.getSize("dtsc_data"));
}

