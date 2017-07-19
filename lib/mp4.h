#pragma once
#include <string>
#include <vector>
#include <set>
#include <iostream>
#include <iomanip>
#include <cstdio>
#include <stdint.h>
#include <sstream>
#include <deque>
#include <algorithm>
#include <vector>
#include "json.h"
#include "dtsc.h"

/// Contains all MP4 format related code.
namespace MP4 {
  std::string readBoxType(FILE * newData);
  bool skipBox(FILE * newData);
  uint64_t calcBoxSize(const char * p);


  class Box {
    public:
      Box(char * datapointer = 0, bool manage = true);
      Box(const Box & rs);
      Box & operator = (const Box & rs);
      ~Box();
      operator bool() const {
        return data && data_size >= 8 && !isType("erro");
      }
      void copyFrom(const Box & rs);

      std::string getType();
      bool isType(const char * boxType) const;
      bool read(FILE * newData);
      bool read(std::string & newData);

      uint64_t boxedSize();
      uint64_t payloadSize();
      char * asBox();
      char * payload();
      void clear();
      std::string toPrettyString(uint32_t indent = 0);
    protected:
      //integer functions
      void setInt8(char newData, size_t index);
      char getInt8(size_t index);
      void setInt16(short newData, size_t index);
      short getInt16(size_t index);
      void setInt24(uint32_t newData, size_t index);
      uint32_t getInt24(size_t index);
      void setInt32(uint32_t newData, size_t index);
      uint32_t getInt32(size_t index);
      void setInt64(uint64_t newData, size_t index);
      uint64_t getInt64(size_t index);
      //string functions
      void setString(std::string newData, size_t index);
      void setString(char * newData, size_t size, size_t index);
      char * getString(size_t index);
      size_t getStringLen(size_t index);
      //box functions
      Box & getBox(size_t index);
      size_t getBoxLen(size_t index);
      void setBox(Box & newEntry, size_t index);
      //data functions
      bool reserve(size_t position, size_t current, size_t wanted);
      //internal variables
      char * data; ///< Holds the data of this box
      unsigned int data_size; ///< Currently reserved size
      bool managed; ///< If false, will not attempt to resize/free the data pointer.
      unsigned int payloadOffset; ///<The offset of the payload with regards to the data
  };
  //Box Class

  class fullBox: public Box {
    public:
      fullBox();
      void setVersion(char newVersion);
      char getVersion();
      void setFlags(uint32_t newFlags);
      uint32_t getFlags();
      std::string toPrettyString(uint32_t indent = 0);
  };

  class containerBox: public Box {
    public:
      containerBox();
      uint32_t getContentCount();
      void setContent(Box & newContent, uint32_t no);
      Box & getContent(uint32_t no, bool unsafe = false);
      std::string toPrettyString(uint32_t indent = 0);
      Box getChild(const char * boxName);
      template <typename T>
      T getChild(){
        T a;
        MP4::Box r = getChild(a.getType().c_str());
        return (T&)r;
      }
      std::deque<Box> getChildren(const char * boxName);
      template <typename T>
      std::deque<T> getChildren(){
        T a;
        std::deque<Box> tmpRes = getChildren(a.getType().c_str());
        std::deque<T> res;
        for (std::deque<Box>::iterator it = tmpRes.begin(); it != tmpRes.end(); it++){
          res.push_back((T&)*it);
        }
        return res;
      }
  };

  class containerFullBox: public fullBox {
    public:
      uint32_t getContentCount();
      void setContent(Box & newContent, uint32_t no);
      Box & getContent(uint32_t no);
      std::string toPrettyCFBString(uint32_t indent, std::string boxName);
  };
}
