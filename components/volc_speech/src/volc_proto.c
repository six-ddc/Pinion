#include "volc_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "zlib.h"

#define PROTOCOL_VERSION 0x1
#define HEADER_SIZE_UNITS 0x1  // 4 字节 header

static void wr_u32be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd_u32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_header(uint8_t* h, uint8_t msg_type, uint8_t flags,
                         uint8_t serialization, uint8_t compression) {
    h[0] = (PROTOCOL_VERSION << 4) | HEADER_SIZE_UNITS;
    h[1] = (uint8_t)(msg_type << 4) | flags;
    h[2] = (uint8_t)(serialization << 4) | compression;
    h[3] = 0x00;
}

bool volc_frame_parse(const uint8_t* data, size_t len, volc_frame_t* out) {
    if (!data || !out || len < 4) return false;
    memset(out, 0, sizeof(*out));

    size_t header_size = (size_t)(data[0] & 0x0F) * 4;
    out->msg_type = data[1] >> 4;
    out->flags = data[1] & 0x0F;
    out->serialization = data[2] >> 4;
    out->compression = data[2] & 0x0F;
    if (header_size < 4 || header_size > len) return false;

    size_t off = header_size;

    // 序号：flags == POS_SEQ / NEG_SEQ（对齐 protocols.ts getReaders）
    if (out->flags == VOLC_FLAG_POS_SEQ || out->flags == VOLC_FLAG_NEG_SEQ) {
        if (off + 4 > len) return false;
        out->sequence = (int32_t)rd_u32be(data + off);
        out->has_seq = true;
        off += 4;
    }
    out->is_last = (out->flags & 0x02) != 0;

    if (out->msg_type == VOLC_MSG_ERROR) {
        if (off + 4 > len) return false;
        out->error_code = rd_u32be(data + off);
        off += 4;
    }

    if (out->flags == VOLC_FLAG_WITH_EVENT) {
        if (off + 4 > len) return false;
        out->event = (int32_t)rd_u32be(data + off);
        out->has_event = true;
        off += 4;

        // sessionId：连接级事件不携带
        bool conn_event = (out->event == VOLC_EVT_START_CONNECTION ||
                           out->event == VOLC_EVT_FINISH_CONNECTION ||
                           out->event == VOLC_EVT_CONNECTION_STARTED ||
                           out->event == VOLC_EVT_CONNECTION_FAILED ||
                           out->event == VOLC_EVT_CONNECTION_FINISHED);
        if (!conn_event) {
            if (off + 4 > len) return false;
            uint32_t sid_len = rd_u32be(data + off);
            off += 4;
            if (off + sid_len > len) return false;
            size_t copy = sid_len < sizeof(out->session_id) - 1
                              ? sid_len
                              : sizeof(out->session_id) - 1;
            memcpy(out->session_id, data + off, copy);
            out->session_id[copy] = '\0';
            off += sid_len;
        }
        // connectId：仅 ConnectionStarted/Failed/Finished 携带，解析跳过
        if (out->event == VOLC_EVT_CONNECTION_STARTED ||
            out->event == VOLC_EVT_CONNECTION_FAILED ||
            out->event == VOLC_EVT_CONNECTION_FINISHED) {
            if (off + 4 > len) return false;
            uint32_t cid_len = rd_u32be(data + off);
            off += 4;
            if (off + cid_len > len) return false;
            off += cid_len;
        }
    }

    if (off + 4 > len) return false;
    uint32_t payload_len = rd_u32be(data + off);
    off += 4;
    if (off + payload_len > len) return false;
    out->payload = data + off;
    out->payload_len = payload_len;
    return true;
}

// header + seq + size + payload（ASR 两种客户端帧的公共骨架）
static uint8_t* build_seq_frame(uint8_t msg_type, uint8_t flags,
                                uint8_t serialization, int32_t seq,
                                const uint8_t* payload, size_t payload_len,
                                size_t* out_len) {
    uint8_t* buf = malloc(4 + 4 + 4 + payload_len);
    if (!buf) return NULL;
    write_header(buf, msg_type, flags, serialization, VOLC_COMP_GZIP);
    wr_u32be(buf + 4, (uint32_t)seq);
    wr_u32be(buf + 8, (uint32_t)payload_len);
    if (payload_len) memcpy(buf + 12, payload, payload_len);
    *out_len = 12 + payload_len;
    return buf;
}

uint8_t* volc_build_asr_full_request(int32_t seq, const char* json,
                                     size_t* out_len) {
    size_t gz_len = 0;
    uint8_t* gz = volc_gzip((const uint8_t*)json, strlen(json), &gz_len);
    if (!gz) return NULL;
    uint8_t* frame =
        build_seq_frame(VOLC_MSG_FULL_CLIENT, VOLC_FLAG_POS_SEQ, VOLC_SER_JSON,
                        seq, gz, gz_len, out_len);
    free(gz);
    return frame;
}

uint8_t* volc_build_asr_audio(int32_t seq, bool is_last, const uint8_t* pcm,
                              size_t pcm_len, size_t* out_len) {
    size_t gz_len = 0;
    uint8_t* gz = volc_gzip(pcm, pcm_len, &gz_len);
    if (!gz) return NULL;
    uint8_t flags = is_last ? VOLC_FLAG_NEG_SEQ : VOLC_FLAG_POS_SEQ;
    int32_t sent_seq = is_last ? -seq : seq;
    uint8_t* frame =
        build_seq_frame(VOLC_MSG_AUDIO_ONLY_CLIENT, flags, VOLC_SER_RAW,
                        sent_seq, gz, gz_len, out_len);
    free(gz);
    return frame;
}

uint8_t* volc_build_tts_event(int32_t event, const char* session_id,
                              const char* json, size_t* out_len) {
    size_t sid_len = session_id ? strlen(session_id) : 0;
    size_t sid_field = session_id ? 4 + sid_len : 0;
    size_t payload_len = json ? strlen(json) : 0;
    uint8_t* buf = malloc(4 + 4 + sid_field + 4 + payload_len);
    if (!buf) return NULL;

    write_header(buf, VOLC_MSG_FULL_CLIENT, VOLC_FLAG_WITH_EVENT,
                 VOLC_SER_JSON, VOLC_COMP_NONE);
    size_t off = 4;
    wr_u32be(buf + off, (uint32_t)event);
    off += 4;
    if (session_id) {
        wr_u32be(buf + off, (uint32_t)sid_len);
        off += 4;
        memcpy(buf + off, session_id, sid_len);
        off += sid_len;
    }
    wr_u32be(buf + off, (uint32_t)payload_len);
    off += 4;
    if (payload_len) memcpy(buf + off, json, payload_len);
    *out_len = off + payload_len;
    return buf;
}

// windowBits 12+16（gzip 容器）+ memLevel 5：单流内存 ~32KB，对 200ms PCM
// 段与短 JSON 足够；服务端 inflate 不受窗口缩小影响。
uint8_t* volc_gzip(const uint8_t* in, size_t in_len, size_t* out_len) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 12 + 16, 5,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return NULL;
    }
    size_t cap = deflateBound(&s, in_len);
    uint8_t* out = malloc(cap);
    if (!out) {
        deflateEnd(&s);
        return NULL;
    }
    s.next_in = (Bytef*)in;
    s.avail_in = (uInt)in_len;
    s.next_out = out;
    s.avail_out = (uInt)cap;
    int rc = deflate(&s, Z_FINISH);
    size_t produced = cap - s.avail_out;
    deflateEnd(&s);
    if (rc != Z_STREAM_END) {
        free(out);
        return NULL;
    }
    *out_len = produced;
    return out;
}

uint8_t* volc_gunzip(const uint8_t* in, size_t in_len, size_t* out_len) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, 15 + 32) != Z_OK) return NULL;  // gzip/zlib 自适应

    size_t cap = in_len * 4 + 256;
    uint8_t* out = malloc(cap);
    if (!out) {
        inflateEnd(&s);
        return NULL;
    }
    s.next_in = (Bytef*)in;
    s.avail_in = (uInt)in_len;
    s.next_out = out;
    s.avail_out = (uInt)cap;

    int rc;
    while ((rc = inflate(&s, Z_NO_FLUSH)) == Z_OK) {
        if (s.avail_out == 0) {
            size_t used = cap;
            cap *= 2;
            uint8_t* grown = realloc(out, cap);
            if (!grown) {
                free(out);
                inflateEnd(&s);
                return NULL;
            }
            out = grown;
            s.next_out = out + used;
            s.avail_out = (uInt)(cap - used);
        } else if (s.avail_in == 0) {
            break;  // 输入耗尽但未见流尾：残缺数据
        }
    }
    size_t produced = cap - s.avail_out;
    inflateEnd(&s);
    if (rc != Z_STREAM_END) {
        free(out);
        return NULL;
    }
    *out_len = produced;
    return out;
}

void volc_gen_uuid(char out[37]) {
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
             b[11], b[12], b[13], b[14], b[15]);
}
