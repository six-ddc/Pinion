// media_decoder_sim — sim 端 CreateMediaDecoder 分派：MP3 → minimp3，AAC → AudioToolbox。
// 真机的对应物是 media_decoder_esp.cc（esp_audio_codec 单库两 codec，不需要分派层）。
#include "media_decoder.h"

namespace media {

std::unique_ptr<MediaDecoder> CreateMediaDecoder(MediaCodec codec) {
    switch (codec) {
        case MediaCodec::Mp3: return CreateMinimp3Decoder();
        case MediaCodec::AacAdts: return CreateSimAacDecoder();
    }
    return nullptr;
}

}  // namespace media
