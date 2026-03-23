//======================================================================================================
// [BINANCE REST ORDER API]
//======================================================================================================
// places and manages orders via Binance REST API (https://api.binance.com/api/v3/order)
// uses HMAC-SHA256 signing for authentication
// separate SSL connection from the websocket data stream
//
// STATUS: UNTESTED — implemented 2026-03-23, needs testnet validation before real money
// the HTTP request/response handling is minimal (assumes responses fit in one SSL_read)
// and may need hardening for production use (chunked transfer, connection reuse, retries)
//
// testnet: testnet.binance.vision (free test API keys, no real money, may need VPN from US)
// production: api.binance.com (real money, be careful)
// binance US: api.binance.us (for US-based users)
//
// all functions return 1 on success, 0 on failure
// order IDs are returned as strings (Binance uses uint64 but string is safer for portability)
//======================================================================================================
#ifndef BINANCE_ORDER_API_HPP
#define BINANCE_ORDER_API_HPP

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

//======================================================================================================
// [ORDER STATUS CODES]
//======================================================================================================
#define ORDER_STATUS_UNKNOWN         0
#define ORDER_STATUS_NEW             1
#define ORDER_STATUS_PARTIALLY_FILLED 2
#define ORDER_STATUS_FILLED          3
#define ORDER_STATUS_CANCELED        4
#define ORDER_STATUS_REJECTED        5
#define ORDER_STATUS_EXPIRED         6

//======================================================================================================
// [API STATE]
//======================================================================================================
struct BinanceOrderAPI {
    int sockfd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    char api_key[128];
    char api_secret[128];
    char host[64];
    char symbol[16];       // uppercase, e.g. "BTCUSDT"
    int connected;
    int64_t time_offset_ms; // local - server time difference
};

//======================================================================================================
// [HELPERS]
//======================================================================================================
static inline int64_t binance_current_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static inline void binance_hmac_sha256(const char *key, const char *data, char *hex_out) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), key, (int)strlen(key),
         (const unsigned char*)data, strlen(data), digest, &digest_len);
    for (unsigned i = 0; i < digest_len; i++)
        sprintf(hex_out + i * 2, "%02x", digest[i]);
    hex_out[digest_len * 2] = '\0';
}

// simple JSON value extractor — finds "key":"value" or "key":number
// returns pointer to value start in buf, writes length to *out_len
// works for the flat JSON objects Binance returns
static inline const char* binance_json_extract(const char *json, const char *key,
                                                int *out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) { *out_len = 0; return NULL; }
    pos += strlen(search);
    // skip whitespace
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos == '"') {
        // string value — find closing quote
        pos++; // skip opening quote
        const char *end = strchr(pos, '"');
        if (!end) { *out_len = 0; return NULL; }
        *out_len = (int)(end - pos);
        return pos;
    } else {
        // numeric value — read until comma/brace/bracket
        const char *end = pos;
        while (*end && *end != ',' && *end != '}' && *end != ']') end++;
        *out_len = (int)(end - pos);
        return pos;
    }
}

static inline void binance_json_extract_str(const char *json, const char *key,
                                             char *out, int out_size) {
    int len;
    const char *val = binance_json_extract(json, key, &len);
    if (val && len > 0 && len < out_size) {
        memcpy(out, val, len);
        out[len] = '\0';
    } else {
        out[0] = '\0';
    }
}

static inline double binance_json_extract_double(const char *json, const char *key) {
    int len;
    const char *val = binance_json_extract(json, key, &len);
    if (!val || len == 0) return 0.0;
    char buf[64];
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, val, len);
    buf[len] = '\0';
    return atof(buf);
}

//======================================================================================================
// [TCP + TLS] — same pattern as BinanceCrypto.hpp
//======================================================================================================
static inline int binance_rest_tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "[REST] getaddrinfo failed: %s\n", gai_strerror(err));
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);
    if (sockfd == -1)
        fprintf(stderr, "[REST] TCP connect failed to %s:%s\n", host, port);
    return sockfd;
}

static inline int binance_rest_tls_setup(BinanceOrderAPI *api) {
    api->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!api->ssl_ctx) return 0;

    api->ssl = SSL_new(api->ssl_ctx);
    if (!api->ssl) {
        SSL_CTX_free(api->ssl_ctx);
        api->ssl_ctx = NULL;
        return 0;
    }

    SSL_set_fd(api->ssl, api->sockfd);
    SSL_set_tlsext_host_name(api->ssl, api->host);

    int ret = SSL_connect(api->ssl);
    if (ret != 1) {
        fprintf(stderr, "[REST] SSL_connect failed: %d\n", SSL_get_error(api->ssl, ret));
        SSL_free(api->ssl);
        SSL_CTX_free(api->ssl_ctx);
        api->ssl = NULL;
        api->ssl_ctx = NULL;
        return 0;
    }
    return 1;
}

//======================================================================================================
// [HTTP REQUEST]
//======================================================================================================
// sends an HTTP/1.1 request over SSL and reads the response
// returns HTTP status code (200, 400, etc.) or -1 on error
// response body written to response_buf
//======================================================================================================
static inline int binance_rest_request(BinanceOrderAPI *api,
                                        const char *method,
                                        const char *path,
                                        const char *query,
                                        char *response_buf, int buf_size) {
    // reconnect if needed (REST connections may be closed between requests)
    if (!api->connected || !api->ssl) {
        if (api->ssl) { SSL_free(api->ssl); api->ssl = NULL; }
        if (api->ssl_ctx) { SSL_CTX_free(api->ssl_ctx); api->ssl_ctx = NULL; }
        if (api->sockfd >= 0) { close(api->sockfd); api->sockfd = -1; }

        api->sockfd = binance_rest_tcp_connect(api->host, "443");
        if (api->sockfd < 0) return -1;
        if (!binance_rest_tls_setup(api)) return -1;
        api->connected = 1;
    }

    // build request
    char request[4096];
    int req_len;

    if (strcmp(method, "POST") == 0) {
        req_len = snprintf(request, sizeof(request),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "X-MBX-APIKEY: %s\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %d\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "%s",
            method, path, api->host, api->api_key, (int)strlen(query), query);
    } else {
        // GET or DELETE — query goes in URL
        req_len = snprintf(request, sizeof(request),
            "%s %s?%s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "X-MBX-APIKEY: %s\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            method, path, query, api->host, api->api_key);
    }

    int written = SSL_write(api->ssl, request, req_len);
    if (written <= 0) {
        fprintf(stderr, "[REST] SSL_write failed\n");
        api->connected = 0;
        return -1;
    }

    // read response
    char raw[8192];
    int total = 0;
    // read headers + body (simple: assume response fits in one SSL_read for REST)
    // Binance REST responses are typically < 2KB
    int n = SSL_read(api->ssl, raw, sizeof(raw) - 1);
    if (n <= 0) {
        fprintf(stderr, "[REST] SSL_read failed\n");
        api->connected = 0;
        return -1;
    }
    total = n;
    raw[total] = '\0';

    // parse HTTP status
    int status = 0;
    if (strncmp(raw, "HTTP/1.1 ", 9) == 0)
        status = atoi(raw + 9);

    // find body (after \r\n\r\n)
    const char *body = strstr(raw, "\r\n\r\n");
    if (body) {
        body += 4;
        int body_len = total - (int)(body - raw);
        if (body_len >= buf_size) body_len = buf_size - 1;
        memcpy(response_buf, body, body_len);
        response_buf[body_len] = '\0';
    } else {
        response_buf[0] = '\0';
    }

    return status;
}

//======================================================================================================
// [SIGN + SEND HELPERS]
//======================================================================================================
static inline int binance_signed_request(BinanceOrderAPI *api,
                                          const char *method, const char *path,
                                          const char *params,
                                          char *response_buf, int buf_size) {
    // add timestamp and signature
    char query[2048];
    int64_t ts = binance_current_ms() + api->time_offset_ms;
    snprintf(query, sizeof(query), "%s&timestamp=%lld", params, (long long)ts);

    char signature[128];
    binance_hmac_sha256(api->api_secret, query, signature);

    char signed_query[2048];
    snprintf(signed_query, sizeof(signed_query), "%s&signature=%s", query, signature);

    return binance_rest_request(api, method, path, signed_query, response_buf, buf_size);
}

//======================================================================================================
// [PUBLIC API]
//======================================================================================================

// init: connect to REST endpoint, calibrate clock
static inline int BinanceOrderAPI_Init(BinanceOrderAPI *api, const char *host,
                                        const char *api_key, const char *api_secret,
                                        const char *symbol) {
    memset(api, 0, sizeof(*api));
    api->sockfd = -1;
    strncpy(api->host, host, sizeof(api->host) - 1);
    strncpy(api->api_key, api_key, sizeof(api->api_key) - 1);
    strncpy(api->api_secret, api_secret, sizeof(api->api_secret) - 1);

    // uppercase symbol for REST API
    for (int i = 0; symbol[i] && i < (int)sizeof(api->symbol) - 1; i++)
        api->symbol[i] = (symbol[i] >= 'a' && symbol[i] <= 'z')
            ? symbol[i] - 32 : symbol[i];

    // connect
    api->sockfd = binance_rest_tcp_connect(host, "443");
    if (api->sockfd < 0) return 0;
    if (!binance_rest_tls_setup(api)) return 0;
    api->connected = 1;

    // calibrate clock offset
    char body[512];
    int status = binance_rest_request(api, "GET", "/api/v3/time", "", body, sizeof(body));
    if (status == 200) {
        double server_time = binance_json_extract_double(body, "serverTime");
        int64_t local_time = binance_current_ms();
        api->time_offset_ms = (int64_t)server_time - local_time;
        fprintf(stderr, "[REST] clock offset: %lldms\n", (long long)api->time_offset_ms);
    } else {
        fprintf(stderr, "[REST] warning: failed to get server time (status %d)\n", status);
        api->time_offset_ms = 0;
    }

    return 1;
}

static inline void BinanceOrderAPI_Cleanup(BinanceOrderAPI *api) {
    if (api->ssl) { SSL_shutdown(api->ssl); SSL_free(api->ssl); api->ssl = NULL; }
    if (api->ssl_ctx) { SSL_CTX_free(api->ssl_ctx); api->ssl_ctx = NULL; }
    if (api->sockfd >= 0) { close(api->sockfd); api->sockfd = -1; }
    api->connected = 0;
}

// place a market buy order — returns 1 on success, 0 on failure
// order_id_out receives the Binance order ID as a string
static inline int BinanceOrderAPI_MarketBuy(BinanceOrderAPI *api,
                                             double quantity,
                                             char *order_id_out) {
    char params[256];
    snprintf(params, sizeof(params),
             "symbol=%s&side=BUY&type=MARKET&quantity=%.8f",
             api->symbol, quantity);

    char body[2048];
    int status = binance_signed_request(api, "POST", "/api/v3/order", params,
                                         body, sizeof(body));

    if (status == 200) {
        binance_json_extract_str(body, "orderId", order_id_out, 32);
        fprintf(stderr, "[REST] BUY order placed: id=%s qty=%.8f\n",
                order_id_out, quantity);
        return 1;
    } else {
        fprintf(stderr, "[REST] BUY failed (status %d): %s\n", status, body);
        order_id_out[0] = '\0';
        return 0;
    }
}

// place a market sell order
static inline int BinanceOrderAPI_MarketSell(BinanceOrderAPI *api,
                                              double quantity,
                                              char *order_id_out) {
    char params[256];
    snprintf(params, sizeof(params),
             "symbol=%s&side=SELL&type=MARKET&quantity=%.8f",
             api->symbol, quantity);

    char body[2048];
    int status = binance_signed_request(api, "POST", "/api/v3/order", params,
                                         body, sizeof(body));

    if (status == 200) {
        binance_json_extract_str(body, "orderId", order_id_out, 32);
        fprintf(stderr, "[REST] SELL order placed: id=%s qty=%.8f\n",
                order_id_out, quantity);
        return 1;
    } else {
        fprintf(stderr, "[REST] SELL failed (status %d): %s\n", status, body);
        order_id_out[0] = '\0';
        return 0;
    }
}

// check order status — returns ORDER_STATUS_* constant
// fills filled_qty and avg_price on success
static inline int BinanceOrderAPI_GetStatus(BinanceOrderAPI *api,
                                             const char *order_id,
                                             double *filled_qty,
                                             double *avg_price) {
    char params[256];
    snprintf(params, sizeof(params), "symbol=%s&orderId=%s", api->symbol, order_id);

    char body[2048];
    int status = binance_signed_request(api, "GET", "/api/v3/order", params,
                                         body, sizeof(body));

    if (status != 200) {
        fprintf(stderr, "[REST] status check failed (HTTP %d): %s\n", status, body);
        return ORDER_STATUS_UNKNOWN;
    }

    // extract status string
    char status_str[32];
    binance_json_extract_str(body, "status", status_str, sizeof(status_str));

    if (filled_qty)
        *filled_qty = binance_json_extract_double(body, "executedQty");
    if (avg_price) {
        // Binance returns cummulativeQuoteQty / executedQty = avg price
        double cumulative = binance_json_extract_double(body, "cummulativeQuoteQty");
        double executed = binance_json_extract_double(body, "executedQty");
        *avg_price = (executed > 0) ? cumulative / executed : 0.0;
    }

    if (strcmp(status_str, "NEW") == 0)              return ORDER_STATUS_NEW;
    if (strcmp(status_str, "PARTIALLY_FILLED") == 0) return ORDER_STATUS_PARTIALLY_FILLED;
    if (strcmp(status_str, "FILLED") == 0)           return ORDER_STATUS_FILLED;
    if (strcmp(status_str, "CANCELED") == 0)          return ORDER_STATUS_CANCELED;
    if (strcmp(status_str, "REJECTED") == 0)          return ORDER_STATUS_REJECTED;
    if (strcmp(status_str, "EXPIRED") == 0)            return ORDER_STATUS_EXPIRED;
    return ORDER_STATUS_UNKNOWN;
}

// get server time (milliseconds) — for clock calibration
static inline int64_t BinanceOrderAPI_ServerTime(BinanceOrderAPI *api) {
    char body[256];
    int status = binance_rest_request(api, "GET", "/api/v3/time", "", body, sizeof(body));
    if (status == 200)
        return (int64_t)binance_json_extract_double(body, "serverTime");
    return 0;
}

//======================================================================================================
// [SECRETS LOADER]
//======================================================================================================
// loads api_key and api_secret from a key=value file (same format as engine.cfg)
// returns 1 if both found, 0 if file missing or keys empty
//======================================================================================================
static inline int LoadSecrets(const char *filepath, char *key_out, char *secret_out) {
    key_out[0] = '\0';
    secret_out[0] = '\0';

    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "[SECRETS] file not found: %s\n", filepath);
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        char *val = eq + 1;

        // strip inline comments
        char *comment = strchr(val, '#');
        if (comment) *comment = '\0';
        int vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t')) val[--vlen] = '\0';

        if (strcmp(key, "api_key") == 0)
            strncpy(key_out, val, 127);
        else if (strcmp(key, "api_secret") == 0)
            strncpy(secret_out, val, 127);
    }

    fclose(f);

    if (key_out[0] == '\0' || secret_out[0] == '\0') {
        fprintf(stderr, "[SECRETS] api_key or api_secret not found in %s\n", filepath);
        return 0;
    }
    return 1;
}

#endif // BINANCE_ORDER_API_HPP
