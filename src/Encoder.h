#ifndef RENCODER_ENCODER_H
#define RENCODER_ENCODER_H

#include <string>
#include <map>
#include <vector>
#include "Sound.h"

namespace rmixer
{

class Encoder
{
public:
  Encoder(const Sound &sound);
  virtual ~Encoder();

  void SetMetadata(const std::string& key, const std::string& value);
  void SetMetadata(const std::string& key, int8_t* p, size_t s);
  bool IsMetaData(const std::string& key);
  bool GetMetadata(const std::string& key, std::string& value);
  bool GetMetadata(const std::string& key, const int8_t** p, size_t& s);
  void DeleteMetadata(const std::string& key);
  void SetQuality(double quality);
  virtual bool Write(const std::string& path);
  virtual bool Write(const std::string& path, const SoundInfo &soundinfo);
  virtual void Close();
protected:
  void CreateBufferListFromSound();
  struct BufferInfo
  {
    const int8_t* p;
    size_t s;
  };
  struct MetaData
  {
    struct {
      int8_t* p;
      size_t s;
    } b;
    std::string s;
  };
  std::map<std::string, MetaData> metadata_;
  const Sound *curr_sound_;
  SoundInfo info_;
  std::vector<BufferInfo> buffers_;
  size_t total_buffer_size_;
  double quality_;
};

class Encoder_WAV : public Encoder
{
public:
  Encoder_WAV(const Sound& sound);
  virtual bool Write(const std::string& path);
};

class Encoder_OGG: public Encoder
{
public:
  Encoder_OGG(const Sound& sound);
  virtual bool Write(const std::string& path);
  virtual bool Write(const std::string& path, const SoundInfo &soundinfo);
private:
  int quality_level;
  SoundInfo dest_info_;

  size_t current_buffer_index;
  size_t current_buffer_offset;
  void initbufferread();
  long bufferread(char* pOut, size_t size);
};

class Encoder_FLAC : public Encoder
{
public:
  Encoder_FLAC(const Sound& sound);
  virtual bool Write(const std::string& path);
  virtual bool Write(const std::string& path, const SoundInfo &soundinfo);
private:
  SoundInfo dest_info_;
};

}

#endif