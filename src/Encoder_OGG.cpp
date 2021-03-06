#include "Encoder.h"
#include "vorbis/vorbisenc.h"
#include "rparser.h"  /* for rutil module */
#include <memory.h>
#include <time.h>

/** https://svn.xiph.org/trunk/vorbis/examples/encoder_example.c */

namespace rmixer
{

constexpr int kOggStreamBufferSize = 102400;

class VorbisCleanupHelper {
public:
  ogg_stream_state os; /* take physical pages, weld into a logical
                       stream of packets */
  ogg_page         og; /* one Ogg bitstream page.  Vorbis packets are inside */
  ogg_packet       op; /* one raw packet of data for decode */

  vorbis_info      vi; /* struct that stores all the static vorbis bitstream
                       settings */
  vorbis_comment   vc; /* struct that stores all the user comments */

  vorbis_dsp_state vd; /* central working state for the packet->PCM decoder */
  vorbis_block     vb; /* local working space for packet->PCM decode */

  ~VorbisCleanupHelper()
  {
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
  }
};

Encoder_OGG::Encoder_OGG(const Sound& sound)
  : Encoder(sound), quality_level(static_cast<int>(quality_ * 10))
{
  initbufferread();
}

/* shortcut encoder */
static inline float ConvertToFloatSample(char *p, const SoundInfo &info_)
{
  if (info_.is_signed == 0)
  {
    switch (info_.bitsize)
    {
    case 8:
      return *(uint8_t*)p / 128.f - 1.0f;
    case 16:
      return *(uint16_t*)p / 32768.f - 1.0f;
    case 32:
      return *(uint32_t*)p / 2147483647.f - 1.0f;
    default:
      break;
    }
  }
  else if (info_.is_signed == 1)
  {
    switch (info_.bitsize)
    {
    case 8:
      return *(int8_t*)p / 128.f;
    case 16:
      return ((p[1] << 8) | (0x00ff & (int)p[0])) / 32768.f;
    case 32:
      return *(int32_t*)p / 2147483647.f - 1.0f;
    default:
      break;
    }
  }
  else if (info_.is_signed == 2)
  {
    switch (info_.bitsize)
    {
    case 32:
      return *(float*)p;
    case 64:
      return (float)*(double*)p;
    default:
      break;
    }
  }
  return .0f;
}

bool Encoder_OGG::Write(const std::string& path)
{
  FILE *fp = rutil::fopen_utf8(path.c_str(), "wb");
  if (!fp)
    return false;

  VorbisCleanupHelper vorbis;

  ogg_stream_state &os = vorbis.os;
  ogg_page         &og = vorbis.og;
  ogg_packet       &op = vorbis.op;
  vorbis_info      &vi = vorbis.vi;
  vorbis_comment   &vc = vorbis.vc;
  vorbis_dsp_state &vd = vorbis.vd;
  vorbis_block     &vb = vorbis.vb;

  int eos = 0, ret = 0;
  const size_t byte_per_frame = info_.channels * info_.bitsize / 8;
  const size_t byte_per_sample = info_.bitsize / 8;

  vorbis_info_init(&vi);
  ret = vorbis_encode_init_vbr(&vi, info_.channels, info_.rate, quality_level / 10.0f);
  /** in case of ABR,
  ret = vorbis_encode_init(&vi, info_.channels, info_.rate, -1, 128000, -1);
  */

  if (ret) return false;

  /* metadata goes here */
  vorbis_comment_init(&vc);
  vorbis_comment_add_tag(&vc, "ENCODER", "Rhythmus-Encoder");
  {
    std::string metavalue;
    if (GetMetadata("TITLE", metavalue))
      vorbis_comment_add_tag(&vc, "TITLE", metavalue.c_str());
    if (GetMetadata("ARTIST", metavalue))
      vorbis_comment_add_tag(&vc, "ARTIST", metavalue.c_str());
  }

  vorbis_analysis_init(&vd, &vi);
  vorbis_block_init(&vd, &vb);

  srand((unsigned)time(0));
  ogg_stream_init(&os, rand());

  /* write header I/O */
  {
    ogg_packet header;
    ogg_packet header_comm;
    ogg_packet header_code;

    vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
    ogg_stream_packetin(&os, &header); /* automatically placed in its own
                                       page */
    ogg_stream_packetin(&os, &header_comm);
    ogg_stream_packetin(&os, &header_code);

    /* This ensures the actual
     * audio data will start on a new page, as per spec
     */
    while (!eos) {
      int result = ogg_stream_flush(&os, &og);
      if (result == 0)break;
      fwrite(og.header, 1, og.header_len, fp);
      fwrite(og.body, 1, og.body_len, fp);
    }
  }

  /* start writing samples */
  char *readbuffer = (char*)malloc(kOggStreamBufferSize * byte_per_frame);
  while (!eos) {
    long i;
    size_t ch;
    long bytes = bufferread(readbuffer, kOggStreamBufferSize * byte_per_frame);

    if (bytes == 0) {
      /* end of file.  this can be done implicitly in the mainline,
      but it's easier to see here in non-clever fashion.
      Tell the library we're at end of stream so that it can handle
      the last frame and mark end of stream in the output properly */
      vorbis_analysis_wrote(&vd, 0);
    }
    else {
      /* data to encode */

      /* expose the buffer to submit data */
      float **buffer = vorbis_analysis_buffer(&vd, kOggStreamBufferSize);

      /* uninterleave samples */
      /* XXX: need to unfold code to improve performance */
      for (i = 0; i< bytes / (int)byte_per_frame /* frame size */; i++) {
        for (ch = 0; ch < info_.channels; ++ch)
        {
          char *sample_ptr = readbuffer + i * byte_per_frame + ch * byte_per_sample;
          buffer[ch][i] = ConvertToFloatSample(sample_ptr, info_);
        }
      }

      /* tell the library how much we actually submitted */
      vorbis_analysis_wrote(&vd, i);
    }

    /* vorbis does some data preanalysis, then divides up blocks for
    more involved (potentially parallel) processing.  Get a single
    block for encoding now */
    while (vorbis_analysis_blockout(&vd, &vb) == 1) {

      /* analysis, assume we want to use bitrate management */
      vorbis_analysis(&vb, NULL);
      vorbis_bitrate_addblock(&vb);

      while (vorbis_bitrate_flushpacket(&vd, &op)) {

        /* weld the packet into the bitstream */
        ogg_stream_packetin(&os, &op);

        /* write out pages (if any) */
        while (!eos) {
          int result = ogg_stream_pageout(&os, &og);
          if (result == 0) break;
          fwrite(og.header, 1, og.header_len, fp);
          fwrite(og.body, 1, og.body_len, fp);

          /* this could be set above, but for illustrative purposes, I do
          it here (to show that vorbis does know where the stream ends) */

          if (ogg_page_eos(&og))eos = 1;
        }
      }
    }
  }

  /* end */
  free(readbuffer);
  fclose(fp);
  return true;
}

void Encoder_OGG::initbufferread()
{
  current_buffer_index = 0;
  current_buffer_offset = 0;
}

long Encoder_OGG::bufferread(char* pOut, size_t size)
{
  if (current_buffer_index == buffers_.size())
    return 0;

  long readsize = 0;
  while (size > 0 && current_buffer_index < buffers_.size())
  {
    size_t cpysize = buffers_[current_buffer_index].s - current_buffer_offset;
    if (cpysize > size) cpysize = size;
    memcpy(pOut, buffers_[current_buffer_index].p + current_buffer_offset, cpysize);
    pOut += cpysize;
    current_buffer_offset += cpysize;
    size -= cpysize;
    readsize += cpysize;

    if (current_buffer_offset >= buffers_[current_buffer_index].s)
    {
      current_buffer_index++;
      current_buffer_offset = 0;
    }
  }

  return readsize;
}

bool Encoder_OGG::Write(const std::string& path, const SoundInfo &soundinfo)
{
  bool r;
  // must be resampled if samplerate, channel is different.
  if (soundinfo.rate != info_.rate || soundinfo.channels != info_.channels)
    return Encoder::Write(path, soundinfo);
  SoundInfo info_old = dest_info_;
  dest_info_ = soundinfo;
  r = Write(path);
  dest_info_ = info_old;
  return r;
}

}
