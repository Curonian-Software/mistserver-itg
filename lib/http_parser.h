/// \file http_parser.h
/// Holds all headers for the HTTP namespace.

#pragma once
#include <map>
#include <string>
#include <stdlib.h>
#include <stdio.h>
#include "socket.h"

/// Holds all HTTP processing related code.
namespace HTTP {

  ///HTTP variable parser to std::map<std::string, std::string> structure.
  ///Reads variables from data, decodes and stores them to storage.
  void parseVars(const std::string & data, std::map<std::string, std::string> & storage);


  /// Simple class for reading and writing HTTP 1.0 and 1.1.
  class Parser {
    public:
      Parser();
      bool Read(Socket::Connection & conn);
      bool Read(std::string & strbuf);
      const std::string & GetHeader(const std::string & i) const;
      bool hasHeader(const std::string & i) const;
      void clearHeader(const std::string & i);
      const std::string & GetVar(const std::string & i) const;
      std::string getUrl();
      std::string allVars();
      void SetHeader(std::string i, std::string v);
      void SetHeader(std::string i, long long v);
      void setCORSHeaders();
      void SetVar(std::string i, std::string v);
      void SetBody(std::string s);
      void SetBody(const char * buffer, int len);
      std::string & BuildRequest();
      std::string & BuildResponse();
      std::string & BuildResponse(std::string code, std::string message);
      void SendRequest(Socket::Connection & conn);
      void SendResponse(std::string code, std::string message, Socket::Connection & conn);
      void StartResponse(std::string code, std::string message, Parser & request, Socket::Connection & conn, bool bufferAllChunks = false);
      void StartResponse(Parser & request, Socket::Connection & conn, bool bufferAllChunks = false);
      void Chunkify(const std::string & bodypart, Socket::Connection & conn);
      void Chunkify(const char * data, unsigned int size, Socket::Connection & conn);
      void Proxy(Socket::Connection & from, Socket::Connection & to);
      void Clean();
      void CleanPreserveHeaders();
      std::string body;
      std::string method;
      std::string url;
      std::string protocol;
      unsigned int length;
      bool headerOnly; ///< If true, do not parse body if the length is a known size.
      bool bufferChunks;
      //this bool was private
      bool sendingChunks;

    private:
      bool seenHeaders;
      bool seenReq;
      bool getChunks;
      unsigned int doingChunk;
      bool parse(std::string & HTTPbuffer);
      std::string builder;
      std::string read_buffer;
      std::map<std::string, std::string> headers;
      std::map<std::string, std::string> vars;
      void Trim(std::string & s);
  };
  
  ///URL parsing class. Parses full URL into its subcomponents
  class URL {
    public:
      URL(const std::string & url);
      uint32_t getPort() const;
      std::string host;///< Hostname or IP address of URL
      std::string protocol;///<Protocol of URL
      std::string port;///<Port of URL
      std::string path;///<Path after the first slash (not inclusive) but before any question mark
      std::string args;///<Everything after the question mark in the path, if it was present
  };

}//HTTP namespace
