
















#include "microlink_internal.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ml_h2";


#define H2_FRAME_DATA           0x00
#define H2_FRAME_HEADERS        0x01
#define H2_FRAME_SETTINGS       0x04
#define H2_FRAME_PING           0x06
#define H2_FRAME_GOAWAY         0x07
#define H2_FRAME_WINDOW_UPDATE  0x08


#define H2_FLAG_END_STREAM      0x01
#define H2_FLAG_END_HEADERS     0x04
#define H2_FLAG_ACK             0x01


#define H2_SETTINGS_INITIAL_WINDOW_SIZE  0x04


static const char H2_CONNECTION_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
#define H2_PREFACE_LEN 24





static int write_frame_header(uint8_t *out, uint32_t length, uint8_t type,
                                uint8_t flags, uint32_t stream_id) {
    
    out[0] = (length >> 16) & 0xFF;
    out[1] = (length >> 8) & 0xFF;
    out[2] = length & 0xFF;
    out[3] = type;
    out[4] = flags;
    out[5] = (stream_id >> 24) & 0x7F;  
    out[6] = (stream_id >> 16) & 0xFF;
    out[7] = (stream_id >> 8) & 0xFF;
    out[8] = stream_id & 0xFF;
    return 9;
}






static int hpack_indexed(uint8_t *out, int index) {
    
    out[0] = 0x80 | (index & 0x7F);
    return 1;
}


static int hpack_literal_indexed(uint8_t *out, int name_index,
                                   const char *value) {
    int pos = 0;
    
    out[pos++] = 0x40 | (name_index & 0x3F);

    
    size_t vlen = strlen(value);
    out[pos++] = (uint8_t)vlen;
    memcpy(out + pos, value, vlen);
    pos += vlen;
    return pos;
}


static int hpack_literal_new(uint8_t *out, const char *name, const char *value) {
    int pos = 0;
    out[pos++] = 0x00;  

    size_t nlen = strlen(name);
    out[pos++] = (uint8_t)nlen;
    memcpy(out + pos, name, nlen);
    pos += nlen;

    size_t vlen = strlen(value);
    out[pos++] = (uint8_t)vlen;
    memcpy(out + pos, value, vlen);
    pos += vlen;
    return pos;
}


#define HPACK_METHOD_POST   3
#define HPACK_METHOD_GET    2
#define HPACK_PATH_SLASH    4
#define HPACK_SCHEME_HTTP   6
#define HPACK_STATUS_200    8
#define HPACK_AUTHORITY     1
#define HPACK_CONTENT_TYPE  31










int ml_h2_build_preface(uint8_t *out, size_t out_size) {
    
    if (out_size < H2_PREFACE_LEN + 9 + 6) return -1;

    int pos = 0;

    
    memcpy(out, H2_CONNECTION_PREFACE, H2_PREFACE_LEN);
    pos += H2_PREFACE_LEN;

    



    uint32_t window_size = ML_H2_BUFFER_SIZE;
    pos += write_frame_header(out + pos, 6, H2_FRAME_SETTINGS, 0, 0);

    
    out[pos++] = 0x00;
    out[pos++] = H2_SETTINGS_INITIAL_WINDOW_SIZE;
    out[pos++] = (window_size >> 24) & 0xFF;
    out[pos++] = (window_size >> 16) & 0xFF;
    out[pos++] = (window_size >> 8) & 0xFF;
    out[pos++] = window_size & 0xFF;

    ESP_LOGI(TAG, "H2 preface built: %d bytes (INITIAL_WINDOW_SIZE=%lu)",
             pos, (unsigned long)window_size);
    return pos;
}




int ml_h2_build_settings_ack(uint8_t *out, size_t out_size) {
    if (out_size < 9) return -1;
    return write_frame_header(out, 0, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0);
}




int ml_h2_build_window_update(uint8_t *out, size_t out_size,
                                uint32_t stream_id, uint32_t increment) {
    if (out_size < 13) return -1;

    int pos = write_frame_header(out, 4, H2_FRAME_WINDOW_UPDATE, 0, stream_id);

    out[pos++] = (increment >> 24) & 0x7F;
    out[pos++] = (increment >> 16) & 0xFF;
    out[pos++] = (increment >> 8) & 0xFF;
    out[pos++] = increment & 0xFF;

    return pos;
}








int ml_h2_build_headers_frame(uint8_t *out, size_t out_size,
                                const char *method, const char *path,
                                const char *authority, const char *content_type,
                                uint32_t stream_id, bool end_stream) {
    if (out_size < 128) return -1;

    
    uint8_t hpack[256];
    int hpack_len = 0;

    
    if (strcmp(method, "POST") == 0) {
        hpack_len += hpack_indexed(hpack + hpack_len, HPACK_METHOD_POST);
    } else if (strcmp(method, "GET") == 0) {
        hpack_len += hpack_indexed(hpack + hpack_len, HPACK_METHOD_GET);
    } else {
        hpack_len += hpack_literal_indexed(hpack + hpack_len, 2, method);
    }

    
    if (strcmp(path, "/") == 0) {
        hpack_len += hpack_indexed(hpack + hpack_len, HPACK_PATH_SLASH);
    } else {
        hpack_len += hpack_literal_indexed(hpack + hpack_len, 4, path);
    }

    
    hpack_len += hpack_indexed(hpack + hpack_len, HPACK_SCHEME_HTTP);

    
    if (authority) {
        hpack_len += hpack_literal_indexed(hpack + hpack_len, HPACK_AUTHORITY, authority);
    }

    
    if (content_type) {
        hpack_len += hpack_literal_indexed(hpack + hpack_len, HPACK_CONTENT_TYPE, content_type);
    }

    if (hpack_len > 256) {
        ESP_LOGE(TAG, "HPACK too large: %d", hpack_len);
        return -1;
    }

    
    uint8_t flags = H2_FLAG_END_HEADERS;
    if (end_stream) flags |= H2_FLAG_END_STREAM;

    int pos = write_frame_header(out, hpack_len, H2_FRAME_HEADERS, flags, stream_id);

    
    memcpy(out + pos, hpack, hpack_len);
    pos += hpack_len;

    ESP_LOGD(TAG, "H2 HEADERS frame: %d bytes (stream=%lu, hpack=%d)",
             pos, (unsigned long)stream_id, hpack_len);
    return pos;
}






int ml_h2_build_data_frame(uint8_t *out, size_t out_size,
                             const uint8_t *data, size_t data_len,
                             uint32_t stream_id, bool end_stream) {
    if (out_size < 9 + data_len) return -1;

    uint8_t flags = end_stream ? H2_FLAG_END_STREAM : 0;
    int pos = write_frame_header(out, data_len, H2_FRAME_DATA, flags, stream_id);

    if (data_len > 0 && data) {
        memcpy(out + pos, data, data_len);
        pos += data_len;
    }

    ESP_LOGD(TAG, "H2 DATA frame: %d bytes (stream=%lu, end=%d)",
             pos, (unsigned long)stream_id, end_stream);
    return pos;
}
