#include "output_ts_base.h"
#include "output_http.h"

namespace Mist {
  class OutHLS : public TSOutput{
    public:
      OutHLS(Socket::Connection & conn);
      ~OutHLS();
      static void init(Util::Config * cfg);
      void sendTS(const char * tsData, unsigned int len=188);
      void sendNext();
      void onHTTP();      
      bool isReadyForPlay();
    protected:
      bool hasSessionIDs(){return true;}
      std::string liveIndex();
      std::string liveIndex(int tid, std::string & sessId);

      
      std::string pushLiveIndex();
      std::string pushLiveIndex(int tid, unsigned long bTime, unsigned long eTime);


      std::string generatePushList();
      int canSeekms(unsigned int ms);
      int keysToSend;      
      unsigned int vidTrack;
      unsigned int audTrack;
      long long unsigned int until;
  };
}

typedef Mist::OutHLS mistOut;
