
/*
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_quic_transport.h>
#include <ngx_event_quic_protection.h>


/*  0-RTT and 1-RTT data exist in the same packet number space,
 *  so we have 3 packet number spaces:
 *
 *  0 - Initial
 *  1 - Handshake
 *  2 - 0-RTT and 1-RTT
 */
#define ngx_quic_get_send_ctx(qc, level)                                      \
    ((level) == ssl_encryption_initial) ? &((qc)->send_ctx[0])                \
        : (((level) == ssl_encryption_handshake) ? &((qc)->send_ctx[1])       \
                                                 : &((qc)->send_ctx[2]))

#define NGX_QUIC_SEND_CTX_LAST  (NGX_QUIC_ENCRYPTION_LAST - 1)

#define NGX_QUIC_STREAMS_INC     16
#define NGX_QUIC_STREAMS_LIMIT   (1ULL < 60)

/*
 * 7.4.  Cryptographic Message Buffering
 *       Implementations MUST support buffering at least 4096 bytes of data
 */
#define NGX_QUIC_MAX_BUFFERED    65535

#define NGX_QUIC_STREAM_GONE     (void *) -1

#define NGX_QUIC_UNSET_PN        (uint64_t) -1

/*
 * Endpoints MUST discard packets that are too small to be valid QUIC
 * packets.  With the set of AEAD functions defined in [QUIC-TLS],
 * packets that are smaller than 21 bytes are never valid.
 */
#define NGX_QUIC_MIN_PKT_LEN     21

#define NGX_QUIC_MIN_SR_PACKET   43 /* 5 random + 16 srt + 22 padding */
#define NGX_QUIC_MAX_SR_PACKET   1200

#define NGX_QUIC_MAX_ACK_GAP     2

#define ngx_quic_level_name(lvl)                                              \
    (lvl == ssl_encryption_application) ? "app"                               \
        : (lvl == ssl_encryption_initial) ? "init"                            \
            : (lvl == ssl_encryption_handshake) ? "hs" : "early"


typedef struct {
    ngx_rbtree_t                      tree;
    ngx_rbtree_node_t                 sentinel;

    uint64_t                          received;
    uint64_t                          sent;
    uint64_t                          recv_max_data;
    uint64_t                          send_max_data;

    uint64_t                          server_max_streams_uni;
    uint64_t                          server_max_streams_bidi;
    uint64_t                          server_streams_uni;
    uint64_t                          server_streams_bidi;

    uint64_t                          client_max_streams_uni;
    uint64_t                          client_max_streams_bidi;
    uint64_t                          client_streams_uni;
    uint64_t                          client_streams_bidi;
} ngx_quic_streams_t;


typedef struct {
    size_t                            in_flight;
    size_t                            window;
    size_t                            ssthresh;
    ngx_msec_t                        recovery_start;
} ngx_quic_congestion_t;


/*
 * 12.3.  Packet Numbers
 *
 *  Conceptually, a packet number space is the context in which a packet
 *  can be processed and acknowledged.  Initial packets can only be sent
 *  with Initial packet protection keys and acknowledged in packets which
 *  are also Initial packets.
*/
typedef struct {
    enum ssl_encryption_level_t       level;

    uint64_t                          pnum;        /* to be sent */
    uint64_t                          largest_ack; /* received from peer */
    uint64_t                          largest_pn;  /* received from peer */

    ngx_queue_t                       frames;
    ngx_queue_t                       sent;

    uint64_t                          pending_ack; /* non sent ack-eliciting */
    uint64_t                          largest_range;
    uint64_t                          first_range;
    ngx_msec_t                        largest_received;
    ngx_msec_t                        ack_delay_start;
    ngx_uint_t                        nranges;
    ngx_quic_ack_range_t              ranges[NGX_QUIC_MAX_RANGES];
    ngx_uint_t                        send_ack;
} ngx_quic_send_ctx_t;


struct ngx_quic_connection_s {
    uint32_t                          version;
    ngx_str_t                         scid;  /* initial client ID */
    ngx_str_t                         dcid;  /* server (our own) ID */
    ngx_str_t                         odcid; /* original server ID */
    ngx_str_t                         token;

    ngx_queue_t                       client_ids;
    ngx_queue_t                       free_client_ids;
    ngx_uint_t                        nclient_ids;
    uint64_t                          max_retired_seqnum;
    uint64_t                          curr_seqnum;

    ngx_uint_t                        client_tp_done;
    ngx_quic_tp_t                     tp;
    ngx_quic_tp_t                     ctp;

    ngx_quic_send_ctx_t               send_ctx[NGX_QUIC_SEND_CTX_LAST];

    ngx_quic_frames_stream_t          crypto[NGX_QUIC_ENCRYPTION_LAST];

    ngx_quic_keys_t                  *keys;

    ngx_quic_conf_t                  *conf;

    ngx_event_t                       push;
    ngx_event_t                       pto;
    ngx_event_t                       close;
    ngx_queue_t                       free_frames;
    ngx_msec_t                        last_cc;

    ngx_msec_t                        latest_rtt;
    ngx_msec_t                        avg_rtt;
    ngx_msec_t                        min_rtt;
    ngx_msec_t                        rttvar;

    ngx_uint_t                        pto_count;

#if (NGX_DEBUG)
    ngx_uint_t                        nframes;
#endif

    ngx_quic_streams_t                streams;
    ngx_quic_congestion_t             congestion;
    size_t                            received;

    ngx_uint_t                        error;
    enum ssl_encryption_level_t       error_level;
    ngx_uint_t                        error_ftype;
    const char                       *error_reason;

    unsigned                          error_app:1;
    unsigned                          send_timer_set:1;
    unsigned                          closing:1;
    unsigned                          draining:1;
    unsigned                          key_phase:1;
    unsigned                          in_retry:1;
    unsigned                          initialized:1;
    unsigned                          validated:1;
};


typedef struct {
    ngx_queue_t                       queue;
    uint64_t                          seqnum;
    size_t                            len;
    u_char                            id[NGX_QUIC_CID_LEN_MAX];
    u_char                            sr_token[NGX_QUIC_SR_TOKEN_LEN];
} ngx_quic_client_id_t;


typedef ngx_int_t (*ngx_quic_frame_handler_pt)(ngx_connection_t *c,
    ngx_quic_frame_t *frame, void *data);


#if BORINGSSL_API_VERSION >= 10
static int ngx_quic_set_read_secret(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, const SSL_CIPHER *cipher,
    const uint8_t *secret, size_t secret_len);
static int ngx_quic_set_write_secret(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, const SSL_CIPHER *cipher,
    const uint8_t *secret, size_t secret_len);
#else
static int ngx_quic_set_encryption_secrets(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, const uint8_t *read_secret,
    const uint8_t *write_secret, size_t secret_len);
#endif

static int ngx_quic_add_handshake_data(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, const uint8_t *data, size_t len);
static int ngx_quic_flush_flight(ngx_ssl_conn_t *ssl_conn);
static int ngx_quic_send_alert(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, uint8_t alert);


static ngx_quic_connection_t *ngx_quic_new_connection(ngx_connection_t *c,
    ngx_quic_conf_t *conf, ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_send_stateless_reset(ngx_connection_t *c,
    ngx_quic_conf_t *conf, ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_process_stateless_reset(ngx_connection_t *c,
    ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_negotiate_version(ngx_connection_t *c,
    ngx_quic_header_t *inpkt);
static ngx_int_t ngx_quic_new_dcid(ngx_connection_t *c,
    ngx_quic_connection_t *qc, ngx_str_t *odcid);
static ngx_int_t ngx_quic_send_retry(ngx_connection_t *c);
static ngx_int_t ngx_quic_new_token(ngx_connection_t *c, ngx_str_t *token);
static ngx_int_t ngx_quic_validate_token(ngx_connection_t *c,
    ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_init_connection(ngx_connection_t *c);
static ngx_inline size_t ngx_quic_max_udp_payload(ngx_connection_t *c);
static void ngx_quic_input_handler(ngx_event_t *rev);

static void ngx_quic_close_connection(ngx_connection_t *c, ngx_int_t rc);
static ngx_int_t ngx_quic_close_quic(ngx_connection_t *c, ngx_int_t rc);
static void ngx_quic_close_timer_handler(ngx_event_t *ev);
static ngx_int_t ngx_quic_close_streams(ngx_connection_t *c,
    ngx_quic_connection_t *qc);

static ngx_int_t ngx_quic_input(ngx_connection_t *c, ngx_buf_t *b,
    ngx_quic_conf_t *conf);
static ngx_int_t ngx_quic_process_packet(ngx_connection_t *c,
    ngx_quic_conf_t *conf, ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_init_secrets(ngx_connection_t *c);
static void ngx_quic_discard_ctx(ngx_connection_t *c,
    enum ssl_encryption_level_t level);
static ngx_int_t ngx_quic_check_peer(ngx_quic_connection_t *qc,
    ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_payload_handler(ngx_connection_t *c,
    ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_ack_packet(ngx_connection_t *c,
    ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_send_ack_range(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx, uint64_t smallest, uint64_t largest);
static void ngx_quic_drop_ack_ranges(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx, uint64_t pn);
static ngx_int_t ngx_quic_send_ack(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx);
static ngx_int_t ngx_quic_send_cc(ngx_connection_t *c);
static ngx_int_t ngx_quic_send_new_token(ngx_connection_t *c);

static ngx_int_t ngx_quic_handle_ack_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_ack_frame_t *f);
static ngx_int_t ngx_quic_handle_ack_frame_range(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx, uint64_t min, uint64_t max,
    ngx_msec_t *send_time);
static void ngx_quic_rtt_sample(ngx_connection_t *c, ngx_quic_ack_frame_t *ack,
    enum ssl_encryption_level_t level, ngx_msec_t send_time);
static ngx_inline ngx_msec_t ngx_quic_pto(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx);
static void ngx_quic_handle_stream_ack(ngx_connection_t *c,
    ngx_quic_frame_t *f);

static ngx_int_t ngx_quic_handle_ordered_frame(ngx_connection_t *c,
    ngx_quic_frames_stream_t *fs, ngx_quic_frame_t *frame,
    ngx_quic_frame_handler_pt handler, void *data);
static ngx_int_t ngx_quic_adjust_frame_offset(ngx_connection_t *c,
    ngx_quic_frame_t *f, uint64_t offset_in);
static ngx_int_t ngx_quic_buffer_frame(ngx_connection_t *c,
    ngx_quic_frames_stream_t *stream, ngx_quic_frame_t *f);

static ngx_int_t ngx_quic_handle_crypto_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_frame_t *frame);
static ngx_int_t ngx_quic_crypto_input(ngx_connection_t *c,
    ngx_quic_frame_t *frame, void *data);
static ngx_int_t ngx_quic_handle_stream_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_frame_t *frame);
static ngx_int_t ngx_quic_stream_input(ngx_connection_t *c,
    ngx_quic_frame_t *frame, void *data);

static ngx_int_t ngx_quic_handle_max_data_frame(ngx_connection_t *c,
    ngx_quic_max_data_frame_t *f);
static ngx_int_t ngx_quic_handle_streams_blocked_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_streams_blocked_frame_t *f);
static ngx_int_t ngx_quic_handle_stream_data_blocked_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_stream_data_blocked_frame_t *f);
static ngx_int_t ngx_quic_handle_max_stream_data_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_max_stream_data_frame_t *f);
static ngx_int_t ngx_quic_handle_reset_stream_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_reset_stream_frame_t *f);
static ngx_int_t ngx_quic_handle_stop_sending_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_stop_sending_frame_t *f);
static ngx_int_t ngx_quic_handle_max_streams_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_max_streams_frame_t *f);
static ngx_int_t ngx_quic_handle_path_challenge_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_path_challenge_frame_t *f);
static ngx_int_t ngx_quic_handle_new_connection_id_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_new_conn_id_frame_t *f);
static ngx_int_t ngx_quic_retire_connection_id(ngx_connection_t *c,
    enum ssl_encryption_level_t level, uint64_t seqnum);
static ngx_quic_client_id_t *ngx_quic_alloc_connection_id(ngx_connection_t *c,
    ngx_quic_connection_t *qc);

static void ngx_quic_queue_frame(ngx_quic_connection_t *qc,
    ngx_quic_frame_t *frame);

static ngx_int_t ngx_quic_output(ngx_connection_t *c);
static ngx_int_t ngx_quic_output_frames(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx);
static void ngx_quic_free_frames(ngx_connection_t *c, ngx_queue_t *frames);
static ngx_int_t ngx_quic_send_frames(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx, ngx_queue_t *frames);

static void ngx_quic_set_packet_number(ngx_quic_header_t *pkt,
    ngx_quic_send_ctx_t *ctx);
static void ngx_quic_pto_handler(ngx_event_t *ev);
static void ngx_quic_lost_handler(ngx_event_t *ev);
static ngx_int_t ngx_quic_detect_lost(ngx_connection_t *c);
static void ngx_quic_resend_frames(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx);
static void ngx_quic_push_handler(ngx_event_t *ev);

static void ngx_quic_rbtree_insert_stream(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_quic_stream_t *ngx_quic_find_stream(ngx_rbtree_t *rbtree,
    uint64_t id);
static ngx_quic_stream_t *ngx_quic_create_client_stream(ngx_connection_t *c,
    uint64_t id);
static ngx_quic_stream_t *ngx_quic_create_stream(ngx_connection_t *c,
    uint64_t id, size_t rcvbuf_size);
static ssize_t ngx_quic_stream_recv(ngx_connection_t *c, u_char *buf,
    size_t size);
static ssize_t ngx_quic_stream_send(ngx_connection_t *c, u_char *buf,
    size_t size);
static ngx_chain_t *ngx_quic_stream_send_chain(ngx_connection_t *c,
    ngx_chain_t *in, off_t limit);
static size_t ngx_quic_max_stream_frame(ngx_quic_connection_t *qc);
static size_t ngx_quic_max_stream_flow(ngx_connection_t *c);
static void ngx_quic_stream_cleanup_handler(void *data);
static ngx_quic_frame_t *ngx_quic_alloc_frame(ngx_connection_t *c, size_t size);
static void ngx_quic_free_frame(ngx_connection_t *c, ngx_quic_frame_t *frame);

static void ngx_quic_congestion_ack(ngx_connection_t *c,
    ngx_quic_frame_t *frame);
static void ngx_quic_congestion_lost(ngx_connection_t *c,
    ngx_quic_frame_t *frame);


static SSL_QUIC_METHOD quic_method = {
#if BORINGSSL_API_VERSION >= 10
    ngx_quic_set_read_secret,
    ngx_quic_set_write_secret,
#else
    ngx_quic_set_encryption_secrets,
#endif
    ngx_quic_add_handshake_data,
    ngx_quic_flush_flight,
    ngx_quic_send_alert,
};


#if (NGX_DEBUG)

static void
ngx_quic_log_frame(ngx_log_t *log, ngx_quic_frame_t *f, ngx_uint_t tx)
{
    u_char      *p, *last, *pos, *end;
    ssize_t      n;
    uint64_t     gap, range, largest, smallest;
    ngx_uint_t   i;
    u_char       buf[NGX_MAX_ERROR_STR];

    p = buf;
    last = buf + sizeof(buf);

    switch (f->type) {

    case NGX_QUIC_FT_CRYPTO:
        p = ngx_slprintf(p, last, "CRYPTO len:%uL off:%uL",
                         f->u.crypto.length, f->u.crypto.offset);
        break;

    case NGX_QUIC_FT_PADDING:
        p = ngx_slprintf(p, last, "PADDING");
        break;

    case NGX_QUIC_FT_ACK:
    case NGX_QUIC_FT_ACK_ECN:

        p = ngx_slprintf(p, last, "ACK n:%ui delay:%uL ",
                         f->u.ack.range_count, f->u.ack.delay);

        pos = f->u.ack.ranges_start;
        end = f->u.ack.ranges_end;

        largest = f->u.ack.largest;
        smallest = f->u.ack.largest - f->u.ack.first_range;

        if (largest == smallest) {
            p = ngx_slprintf(p, last, "%uL", largest);

        } else {
            p = ngx_slprintf(p, last, "%uL-%uL", largest, smallest);
        }

        for (i = 0; i < f->u.ack.range_count; i++) {
            n = ngx_quic_parse_ack_range(log, pos, end, &gap, &range);
            if (n == NGX_ERROR) {
                break;
            }

            pos += n;

            largest = smallest - gap - 2;
            smallest = largest - range;

            if (largest == smallest) {
                p = ngx_slprintf(p, last, " %uL", largest);

            } else {
                p = ngx_slprintf(p, last, " %uL-%uL", largest, smallest);
            }
        }

        if (f->type == NGX_QUIC_FT_ACK_ECN) {
            p = ngx_slprintf(p, last, " ECN counters ect0:%uL ect1:%uL ce:%uL",
                             f->u.ack.ect0, f->u.ack.ect1, f->u.ack.ce);
        }
        break;

    case NGX_QUIC_FT_PING:
        p = ngx_slprintf(p, last, "PING");
        break;

    case NGX_QUIC_FT_NEW_CONNECTION_ID:
        p = ngx_slprintf(p, last, "NCID seq:%uL retire:%uL len:%ud",
                         f->u.ncid.seqnum, f->u.ncid.retire, f->u.ncid.len);
        break;

    case NGX_QUIC_FT_RETIRE_CONNECTION_ID:
        p = ngx_slprintf(p, last, "RETIRE_CONNECTION_ID seqnum:%uL",
                         f->u.retire_cid.sequence_number);
        break;

    case NGX_QUIC_FT_CONNECTION_CLOSE:
    case NGX_QUIC_FT_CONNECTION_CLOSE_APP:
        p = ngx_slprintf(p, last, "CONNECTION_CLOSE%s err:%ui",
                         f->u.close.app ? "_APP" : "", f->u.close.error_code);

        if (f->u.close.reason.len) {
            p = ngx_slprintf(p, last, " %V", &f->u.close.reason);
        }

        if (f->type == NGX_QUIC_FT_CONNECTION_CLOSE) {
            p = ngx_slprintf(p, last, " ft:%ui", f->u.close.frame_type);
        }


        break;

    case NGX_QUIC_FT_STREAM0:
    case NGX_QUIC_FT_STREAM1:
    case NGX_QUIC_FT_STREAM2:
    case NGX_QUIC_FT_STREAM3:
    case NGX_QUIC_FT_STREAM4:
    case NGX_QUIC_FT_STREAM5:
    case NGX_QUIC_FT_STREAM6:
    case NGX_QUIC_FT_STREAM7:

        p = ngx_slprintf(p, last, "STREAM id:0x%xL", f->u.stream.stream_id);

        if (f->u.stream.off) {
            p = ngx_slprintf(p, last, " off:%uL", f->u.stream.offset);
        }

        if (f->u.stream.len) {
            p = ngx_slprintf(p, last, " len:%uL", f->u.stream.length);
        }

        if (f->u.stream.fin) {
            p = ngx_slprintf(p, last, " fin:1");
        }

        break;

    case NGX_QUIC_FT_MAX_DATA:
        p = ngx_slprintf(p, last, "MAX_DATA max_data:%uL on recv",
                         f->u.max_data.max_data);
        break;

    case NGX_QUIC_FT_RESET_STREAM:
       p = ngx_slprintf(p, last, "RESET_STREAM"
                        " id:0x%xL error_code:0x%xL final_size:0x%xL",
                        f->u.reset_stream.id, f->u.reset_stream.error_code,
                        f->u.reset_stream.final_size);
        break;

    case NGX_QUIC_FT_STOP_SENDING:
        p = ngx_slprintf(p, last, "STOP_SENDING id:0x%xL err:0x%xL",
                         f->u.stop_sending.id, f->u.stop_sending.error_code);
        break;

    case NGX_QUIC_FT_STREAMS_BLOCKED:
    case NGX_QUIC_FT_STREAMS_BLOCKED2:
        p = ngx_slprintf(p, last, "STREAMS_BLOCKED limit:%uL bidi:%ui",
                         f->u.streams_blocked.limit, f->u.streams_blocked.bidi);
        break;

    case NGX_QUIC_FT_MAX_STREAMS:
    case NGX_QUIC_FT_MAX_STREAMS2:
        p = ngx_slprintf(p, last, "MAX_STREAMS limit:%uL bidi:%ui",
                         f->u.max_streams.limit, f->u.max_streams.bidi);
        break;

    case NGX_QUIC_FT_MAX_STREAM_DATA:
        p = ngx_slprintf(p, last, "MAX_STREAM_DATA id:0x%xL limit:%uL",
                         f->u.max_stream_data.id, f->u.max_stream_data.limit);
        break;


    case NGX_QUIC_FT_DATA_BLOCKED:
        p = ngx_slprintf(p, last, "DATA_BLOCKED limit:%uL",
                         f->u.data_blocked.limit);
        break;

    case NGX_QUIC_FT_STREAM_DATA_BLOCKED:
        p = ngx_slprintf(p, last, "STREAM_DATA_BLOCKED id:0x%xL limit:%uL",
                         f->u.stream_data_blocked.id,
                         f->u.stream_data_blocked.limit);
        break;

    case NGX_QUIC_FT_PATH_CHALLENGE:
        p = ngx_slprintf(p, last, "PATH_CHALLENGE data:0x%xL",
                         *(uint64_t *) &f->u.path_challenge.data);
        break;

    case NGX_QUIC_FT_PATH_RESPONSE:
        p = ngx_slprintf(p, last, "PATH_RESPONSE data:0x%xL",
                         f->u.path_response);
        break;

    case NGX_QUIC_FT_NEW_TOKEN:
        p = ngx_slprintf(p, last, "NEW_TOKEN");
        break;

    case NGX_QUIC_FT_HANDSHAKE_DONE:
        p = ngx_slprintf(p, last, "HANDSHAKE DONE");
        break;

    default:
        p = ngx_slprintf(p, last, "unknown type 0x%xi", f->type);
        break;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, log, 0, "quic frame %s %s %*s",
                   tx ? "tx" : "rx", ngx_quic_level_name(f->level),
                   p - buf, buf);
}


static void
ngx_quic_connstate_dbg(ngx_connection_t *c)
{
    u_char                 *p, *last;
    ngx_quic_connection_t  *qc;
    u_char                  buf[NGX_MAX_ERROR_STR];

    p = buf;
    last = p + sizeof(buf);

    qc = c->quic;

    p = ngx_slprintf(p, last, "state:");

    if (qc) {

        if (qc->error) {
            p = ngx_slprintf(p, last, "%s", qc->error_app ? " app" : "");
            p = ngx_slprintf(p, last, " error:%ui", qc->error);

            if (qc->error_reason) {
                p = ngx_slprintf(p, last, " \"%s\"", qc->error_reason);
            }
        }

        p = ngx_slprintf(p, last, "%s", qc->closing ? " closing" : "");
        p = ngx_slprintf(p, last, "%s", qc->draining ? " draining" : "");
        p = ngx_slprintf(p, last, "%s", qc->key_phase ? " kp" : "");
        p = ngx_slprintf(p, last, "%s", qc->in_retry ? " retry" : "");
        p = ngx_slprintf(p, last, "%s", qc->validated? " valid" : "");

    } else {
        p = ngx_slprintf(p, last, " early");
    }

    if (c->read->timer_set) {
        p = ngx_slprintf(p, last,
                         qc && qc->send_timer_set ? " send:%M" : " read:%M",
                         c->read->timer.key - ngx_current_msec);
    }

    if (qc) {

        if (qc->push.timer_set) {
            p = ngx_slprintf(p, last, " push:%M",
                             qc->push.timer.key - ngx_current_msec);
        }

        if (qc->pto.timer_set) {
            p = ngx_slprintf(p, last, " pto:%M",
                             qc->pto.timer.key - ngx_current_msec);
        }

        if (qc->close.timer_set) {
            p = ngx_slprintf(p, last, " close:%M",
                             qc->close.timer.key - ngx_current_msec);
        }
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic %*s", p - buf, buf);
}

#else

#define ngx_quic_log_frame(log, f, tx)
#define ngx_quic_connstate_dbg(c)

#endif


#if BORINGSSL_API_VERSION >= 10

static int
ngx_quic_set_read_secret(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, const SSL_CIPHER *cipher,
    const uint8_t *rsecret, size_t secret_len)
{
    ngx_connection_t  *c;

    c = ngx_ssl_get_connection((ngx_ssl_conn_t *) ssl_conn);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_set_read_secret() level:%d", level);
#ifdef NGX_QUIC_DEBUG_CRYPTO
    ngx_quic_hexdump(c->log, "quic read secret", rsecret, secret_len);
#endif

    return ngx_quic_keys_set_encryption_secret(c->pool, 0, c->quic->keys, level,
                                               cipher, rsecret, secret_len);
}


static int
ngx_quic_set_write_secret(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, const SSL_CIPHER *cipher,
    const uint8_t *wsecret, size_t secret_len)
{
    ngx_connection_t  *c;

    c = ngx_ssl_get_connection((ngx_ssl_conn_t *) ssl_conn);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_set_write_secret() level:%d", level);
#ifdef NGX_QUIC_DEBUG_CRYPTO
    ngx_quic_hexdump(c->log, "quic write secret", wsecret, secret_len);
#endif

    return ngx_quic_keys_set_encryption_secret(c->pool, 1, c->quic->keys, level,
                                               cipher, wsecret, secret_len);
}

#else

static int
ngx_quic_set_encryption_secrets(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, const uint8_t *rsecret,
    const uint8_t *wsecret, size_t secret_len)
{
    ngx_connection_t  *c;
    const SSL_CIPHER  *cipher;

    c = ngx_ssl_get_connection((ngx_ssl_conn_t *) ssl_conn);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_set_encryption_secrets() level:%d", level);
#ifdef NGX_QUIC_DEBUG_CRYPTO
    ngx_quic_hexdump(c->log, "quic read secret", rsecret, secret_len);
#endif

    cipher = SSL_get_current_cipher(ssl_conn);

    if (ngx_quic_keys_set_encryption_secret(c->pool, 0, c->quic->keys, level,
                                            cipher, rsecret, secret_len)
        != 1)
    {
        return 0;
    }

    if (level == ssl_encryption_early_data) {
        return 1;
    }

#ifdef NGX_QUIC_DEBUG_CRYPTO
    ngx_quic_hexdump(c->log, "quic write secret", wsecret, secret_len);
#endif

    return ngx_quic_keys_set_encryption_secret(c->pool, 1, c->quic->keys, level,
                                               cipher, wsecret, secret_len);
}

#endif


static int
ngx_quic_add_handshake_data(ngx_ssl_conn_t *ssl_conn,
    enum ssl_encryption_level_t level, const uint8_t *data, size_t len)
{
    u_char                    *p, *end;
    size_t                     client_params_len, fsize, limit;
    const uint8_t             *client_params;
    ngx_quic_frame_t          *frame;
    ngx_connection_t          *c;
    ngx_quic_connection_t     *qc;
    ngx_quic_frames_stream_t  *fs;

    c = ngx_ssl_get_connection((ngx_ssl_conn_t *) ssl_conn);
    qc = c->quic;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_add_handshake_data");

    if (!qc->client_tp_done) {
        /*
         * things to do once during handshake: check ALPN and transport
         * parameters; we want to break handshake if something is wrong
         * here;
         */

#if defined(TLSEXT_TYPE_application_layer_protocol_negotiation)
        if (qc->conf->require_alpn) {
            unsigned int          len;
            const unsigned char  *data;

            SSL_get0_alpn_selected(ssl_conn, &data, &len);

            if (len == 0) {
                qc->error = 0x100 + SSL_AD_NO_APPLICATION_PROTOCOL;
                qc->error_reason = "unsupported protocol in ALPN extension";

                ngx_log_error(NGX_LOG_INFO, c->log, 0,
                              "quic unsupported protocol in ALPN extension");
                return 0;
            }
        }
#endif

        SSL_get_peer_quic_transport_params(ssl_conn, &client_params,
                                           &client_params_len);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic SSL_get_peer_quic_transport_params():"
                       " params_len:%ui", client_params_len);

        if (client_params_len == 0) {
            /* quic-tls 8.2 */
            qc->error = NGX_QUIC_ERR_CRYPTO(SSL_AD_MISSING_EXTENSION);
            qc->error_reason = "missing transport parameters";

            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                          "missing transport parameters");
            return 0;
        }

        p = (u_char *) client_params;
        end = p + client_params_len;

        if (ngx_quic_parse_transport_params(p, end, &qc->ctp, c->log)
            != NGX_OK)
        {
            qc->error = NGX_QUIC_ERR_TRANSPORT_PARAMETER_ERROR;
            qc->error_reason = "failed to process transport parameters";

            return 0;
        }

        if (qc->ctp.max_idle_timeout > 0
            && qc->ctp.max_idle_timeout < qc->tp.max_idle_timeout)
        {
            qc->tp.max_idle_timeout = qc->ctp.max_idle_timeout;
        }

        if (qc->ctp.max_udp_payload_size < NGX_QUIC_MIN_INITIAL_SIZE
            || qc->ctp.max_udp_payload_size > NGX_QUIC_MAX_UDP_PAYLOAD_SIZE)
        {
            qc->error = NGX_QUIC_ERR_TRANSPORT_PARAMETER_ERROR;
            qc->error_reason = "invalid maximum packet size";

            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                          "quic maximum packet size is invalid");
            return 0;
        }

        if (qc->ctp.max_udp_payload_size > ngx_quic_max_udp_payload(c)) {
            qc->ctp.max_udp_payload_size = ngx_quic_max_udp_payload(c);
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                          "quic client maximum packet size truncated");
        }

#if (NGX_QUIC_DRAFT_VERSION >= 28)
        if (qc->scid.len != qc->ctp.initial_scid.len
            || ngx_memcmp(qc->scid.data, qc->ctp.initial_scid.data,
                          qc->scid.len) != 0)
        {
            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                          "quic client initial_source_connection_id "
                          "mismatch");
            return 0;
        }
#endif

        qc->streams.server_max_streams_bidi = qc->ctp.initial_max_streams_bidi;
        qc->streams.server_max_streams_uni = qc->ctp.initial_max_streams_uni;

        qc->client_tp_done = 1;
    }

    /*
     * we need to fit at least 1 frame into a packet, thus account head/tail;
     * 17 = 1 + 8x2 is max header for CRYPTO frame, with 1 byte for frame type
     */
    limit = qc->ctp.max_udp_payload_size - NGX_QUIC_MAX_LONG_HEADER - 17
            - EVP_GCM_TLS_TAG_LEN;

    fs = &qc->crypto[level];

    p = (u_char *) data;
    end = (u_char *) data + len;

    while (p < end) {

        fsize = ngx_min(limit, (size_t) (end - p));

        frame = ngx_quic_alloc_frame(c, fsize);
        if (frame == NULL) {
            return 0;
        }

        ngx_memcpy(frame->data, p, fsize);

        frame->level = level;
        frame->type = NGX_QUIC_FT_CRYPTO;
        frame->u.crypto.offset = fs->sent;
        frame->u.crypto.length = fsize;
        frame->u.crypto.data = frame->data;

        fs->sent += fsize;
        p += fsize;

        ngx_quic_queue_frame(qc, frame);
    }

    return 1;
}


static int
ngx_quic_flush_flight(ngx_ssl_conn_t *ssl_conn)
{
#if (NGX_DEBUG)
    ngx_connection_t  *c;

    c = ngx_ssl_get_connection((ngx_ssl_conn_t *) ssl_conn);

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_flush_flight()");
#endif
    return 1;
}


static int
ngx_quic_send_alert(ngx_ssl_conn_t *ssl_conn, enum ssl_encryption_level_t level,
    uint8_t alert)
{
    ngx_connection_t       *c;
    ngx_quic_connection_t  *qc;

    c = ngx_ssl_get_connection((ngx_ssl_conn_t *) ssl_conn);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_send_alert() lvl:%d  alert:%d",
                   (int) level, (int) alert);

    qc = c->quic;
    if (qc == NULL) {
        return 1;
    }

    qc->error_level = level;
    qc->error = NGX_QUIC_ERR_CRYPTO(alert);
    qc->error_reason = "TLS alert";
    qc->error_app = 0;
    qc->error_ftype = 0;

    if (ngx_quic_send_cc(c) != NGX_OK) {
        return 0;
    }

    return 1;
}


void
ngx_quic_run(ngx_connection_t *c, ngx_quic_conf_t *conf)
{
    ngx_int_t  rc;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0, "quic run");

    rc = ngx_quic_input(c, c->buffer, conf);
    if (rc != NGX_OK) {
        ngx_quic_close_connection(c, rc == NGX_DECLINED ? NGX_DONE : NGX_ERROR);
        return;
    }

    ngx_add_timer(c->read, c->quic->in_retry ? NGX_QUIC_RETRY_TIMEOUT
                                             : c->quic->tp.max_idle_timeout);

    c->read->handler = ngx_quic_input_handler;

    ngx_quic_connstate_dbg(c);
    return;
}


static ngx_quic_connection_t *
ngx_quic_new_connection(ngx_connection_t *c, ngx_quic_conf_t *conf,
    ngx_quic_header_t *pkt)
{
    ngx_uint_t              i;
    ngx_quic_tp_t          *ctp;
    ngx_quic_client_id_t   *cid;
    ngx_quic_connection_t  *qc;

    qc = ngx_pcalloc(c->pool, sizeof(ngx_quic_connection_t));
    if (qc == NULL) {
        return NULL;
    }

    qc->keys = ngx_quic_keys_new(c->pool);
    if (qc->keys == NULL) {
        return NULL;
    }

    qc->version = pkt->version;

    ngx_rbtree_init(&qc->streams.tree, &qc->streams.sentinel,
                    ngx_quic_rbtree_insert_stream);

    for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {
        ngx_queue_init(&qc->send_ctx[i].frames);
        ngx_queue_init(&qc->send_ctx[i].sent);
        qc->send_ctx[i].largest_pn = NGX_QUIC_UNSET_PN;
        qc->send_ctx[i].largest_ack = NGX_QUIC_UNSET_PN;
        qc->send_ctx[i].largest_range = NGX_QUIC_UNSET_PN;
        qc->send_ctx[i].pending_ack = NGX_QUIC_UNSET_PN;
    }

    qc->send_ctx[0].level = ssl_encryption_initial;
    qc->send_ctx[1].level = ssl_encryption_handshake;
    qc->send_ctx[2].level = ssl_encryption_application;

    for (i = 0; i < NGX_QUIC_ENCRYPTION_LAST; i++) {
        ngx_queue_init(&qc->crypto[i].frames);
    }

    ngx_queue_init(&qc->free_frames);
    ngx_queue_init(&qc->client_ids);
    ngx_queue_init(&qc->free_client_ids);

    qc->avg_rtt = NGX_QUIC_INITIAL_RTT;
    qc->rttvar = NGX_QUIC_INITIAL_RTT / 2;
    qc->min_rtt = NGX_TIMER_INFINITE;

    /*
     * qc->latest_rtt = 0
     * qc->nclient_ids = 0
     * qc->max_retired_seqnum = 0
     */

    qc->received = pkt->raw->last - pkt->raw->start;

    qc->pto.log = c->log;
    qc->pto.data = c;
    qc->pto.handler = ngx_quic_pto_handler;
    qc->pto.cancelable = 1;

    qc->push.log = c->log;
    qc->push.data = c;
    qc->push.handler = ngx_quic_push_handler;
    qc->push.cancelable = 1;

    qc->conf = conf;
    qc->tp = conf->tp;

    ctp = &qc->ctp;
    ctp->max_udp_payload_size = ngx_quic_max_udp_payload(c);
    ctp->ack_delay_exponent = NGX_QUIC_DEFAULT_ACK_DELAY_EXPONENT;
    ctp->max_ack_delay = NGX_QUIC_DEFAULT_MAX_ACK_DELAY;

    qc->streams.recv_max_data = qc->tp.initial_max_data;

    qc->streams.client_max_streams_uni = qc->tp.initial_max_streams_uni;
    qc->streams.client_max_streams_bidi = qc->tp.initial_max_streams_bidi;

    qc->congestion.window = ngx_min(10 * qc->tp.max_udp_payload_size,
                                    ngx_max(2 * qc->tp.max_udp_payload_size,
                                            14720));
    qc->congestion.ssthresh = (size_t) -1;
    qc->congestion.recovery_start = ngx_current_msec;

    if (ngx_quic_new_dcid(c, qc, &pkt->dcid) != NGX_OK) {
        return NULL;
    }

#if (NGX_QUIC_DRAFT_VERSION >= 28)
    qc->tp.original_dcid = qc->odcid;
#endif
    qc->tp.initial_scid = qc->dcid;

    qc->scid.len = pkt->scid.len;
    qc->scid.data = ngx_pnalloc(c->pool, qc->scid.len);
    if (qc->scid.data == NULL) {
        return NULL;
    }
    ngx_memcpy(qc->scid.data, pkt->scid.data, qc->scid.len);

    cid = ngx_quic_alloc_connection_id(c, qc);
    if (cid == NULL) {
        return NULL;
    }

    cid->seqnum = 0;
    cid->len = pkt->scid.len;
    ngx_memcpy(cid->id, pkt->scid.data, pkt->scid.len);

    ngx_queue_insert_tail(&qc->client_ids, &cid->queue);
    qc->nclient_ids++;
    qc->curr_seqnum = 0;

    return qc;
}


static ngx_int_t
ngx_quic_send_stateless_reset(ngx_connection_t *c, ngx_quic_conf_t *conf,
    ngx_quic_header_t *pkt)
{
    u_char    *token;
    size_t     len, max;
    uint16_t   rndbytes;
    u_char     buf[NGX_QUIC_MAX_SR_PACKET];

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic handle stateless reset output");

    if (conf->sr_token_key.len == 0) {
        return NGX_DECLINED;
    }

    if (pkt->len <= NGX_QUIC_MIN_PKT_LEN) {
        return NGX_DECLINED;
    }

    if (pkt->len <= NGX_QUIC_MIN_SR_PACKET) {
        len = pkt->len - 1;

    } else {
        max = ngx_min(NGX_QUIC_MAX_SR_PACKET, pkt->len * 3);

        if (RAND_bytes((u_char *) &rndbytes, sizeof(rndbytes)) != 1) {
            return NGX_ERROR;
        }

        len = (rndbytes % (max - NGX_QUIC_MIN_SR_PACKET + 1))
              + NGX_QUIC_MIN_SR_PACKET;
    }

    if (RAND_bytes(buf, len - NGX_QUIC_SR_TOKEN_LEN) != 1) {
        return NGX_ERROR;
    }

    buf[0] &= ~NGX_QUIC_PKT_LONG;
    buf[0] |= NGX_QUIC_PKT_FIXED_BIT;

    token = &buf[len - NGX_QUIC_SR_TOKEN_LEN];

    if (ngx_quic_new_sr_token(c, &pkt->dcid, &conf->sr_token_key, token)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    (void) c->send(c, buf, len);

    return NGX_DECLINED;
}


static ngx_int_t
ngx_quic_process_stateless_reset(ngx_connection_t *c, ngx_quic_header_t *pkt)
{
    u_char                 *tail, ch;
    ngx_uint_t              i;
    ngx_queue_t            *q;
    ngx_quic_client_id_t   *cid;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    /* A stateless reset uses an entire UDP datagram */
    if (pkt->raw->start != pkt->data) {
        return NGX_DECLINED;
    }

    tail = pkt->raw->last - NGX_QUIC_SR_TOKEN_LEN;

    for (q = ngx_queue_head(&qc->client_ids);
         q != ngx_queue_sentinel(&qc->client_ids);
         q = ngx_queue_next(q))
    {
        cid = ngx_queue_data(q, ngx_quic_client_id_t, queue);

        if (cid->seqnum == 0) {
            /* no stateless reset token in initial connection id */
            continue;
        }

        /* constant time comparison */

        for (ch = 0, i = 0; i < NGX_QUIC_SR_TOKEN_LEN; i++) {
            ch |= tail[i] ^ cid->sr_token[i];
        }

        if (ch == 0) {
            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_quic_negotiate_version(ngx_connection_t *c, ngx_quic_header_t *inpkt)
{
    size_t             len;
    ngx_quic_header_t  pkt;
    static u_char      buf[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "sending version negotiation packet");

    pkt.log = c->log;
    pkt.flags = NGX_QUIC_PKT_LONG | NGX_QUIC_PKT_FIXED_BIT;
    pkt.dcid = inpkt->scid;
    pkt.scid = inpkt->dcid;

    len = ngx_quic_create_version_negotiation(&pkt, buf);

#ifdef NGX_QUIC_DEBUG_PACKETS
    ngx_quic_hexdump(c->log, "quic vnego packet to send", buf, len);
#endif

    (void) c->send(c, buf, len);

    return NGX_ERROR;
}


static ngx_int_t
ngx_quic_new_dcid(ngx_connection_t *c, ngx_quic_connection_t *qc,
    ngx_str_t *odcid)
{
    qc->dcid.len = NGX_QUIC_SERVER_CID_LEN;
    qc->dcid.data = ngx_pnalloc(c->pool, NGX_QUIC_SERVER_CID_LEN);
    if (qc->dcid.data == NULL) {
        return NGX_ERROR;
    }

    if (RAND_bytes(qc->dcid.data, NGX_QUIC_SERVER_CID_LEN) != 1) {
        return NGX_ERROR;
    }

    ngx_quic_hexdump(c->log, "quic server CID", qc->dcid.data, qc->dcid.len);

    qc->odcid.len = odcid->len;
    qc->odcid.data = ngx_pstrdup(c->pool, odcid);
    if (qc->odcid.data == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_send_retry(ngx_connection_t *c)
{
    ssize_t            len;
    ngx_str_t          res, token;
    ngx_quic_header_t  pkt;
    u_char             buf[NGX_QUIC_RETRY_BUFFER_SIZE];

    if (ngx_quic_new_token(c, &token) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_memzero(&pkt, sizeof(ngx_quic_header_t));
    pkt.flags = NGX_QUIC_PKT_FIXED_BIT | NGX_QUIC_PKT_LONG | NGX_QUIC_PKT_RETRY;
    pkt.version = c->quic->version;
    pkt.log = c->log;
    pkt.odcid = c->quic->odcid;
    pkt.dcid = c->quic->scid;
    pkt.scid = c->quic->dcid;
    pkt.token = token;

    res.data = buf;

    if (ngx_quic_encrypt(&pkt, &res) != NGX_OK) {
        return NGX_ERROR;
    }

#ifdef NGX_QUIC_DEBUG_PACKETS
    ngx_quic_hexdump(c->log, "quic packet to send", res.data, res.len);
#endif

    len = c->send(c, res.data, res.len);
    if (len == NGX_ERROR || (size_t) len != res.len) {
        return NGX_ERROR;
    }

    c->quic->token = token;
#if (NGX_QUIC_DRAFT_VERSION < 28)
    c->quic->tp.original_dcid = c->quic->odcid;
#endif
    c->quic->tp.retry_scid = c->quic->dcid;
    c->quic->in_retry = 1;

    return NGX_OK;
}


static ngx_int_t
ngx_quic_new_token(ngx_connection_t *c, ngx_str_t *token)
{
    int                   len, iv_len;
    u_char               *data, *p, *key, *iv;
    ngx_msec_t            now;
    EVP_CIPHER_CTX       *ctx;
    const EVP_CIPHER     *cipher;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif
    u_char                in[NGX_QUIC_MAX_TOKEN_SIZE];

    switch (c->sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:
        sin6 = (struct sockaddr_in6 *) c->sockaddr;

        len = sizeof(struct in6_addr);
        data = sin6->sin6_addr.s6_addr;

        break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
    case AF_UNIX:

        len = ngx_min(c->addr_text.len, NGX_QUIC_MAX_TOKEN_SIZE - sizeof(now));
        data = c->addr_text.data;

        break;
#endif

    default: /* AF_INET */
        sin = (struct sockaddr_in *) c->sockaddr;

        len = sizeof(in_addr_t);
        data = (u_char *) &sin->sin_addr;

        break;
    }

    p = ngx_cpymem(in, data, len);

    now = ngx_current_msec;
    len += sizeof(now);
    ngx_memcpy(p, &now, sizeof(now));

    cipher = EVP_aes_256_cbc();
    iv_len = EVP_CIPHER_iv_length(cipher);

    token->len = iv_len + len + EVP_CIPHER_block_size(cipher);
    token->data = ngx_pnalloc(c->pool, token->len);
    if (token->data == NULL) {
        return NGX_ERROR;
    }

    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    key = c->quic->conf->token_key;
    iv = token->data;

    if (RAND_bytes(iv, iv_len) <= 0
        || !EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv))
    {
        EVP_CIPHER_CTX_free(ctx);
        return NGX_ERROR;
    }

    token->len = iv_len;

    if (EVP_EncryptUpdate(ctx, token->data + token->len, &len, in, len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return NGX_ERROR;
    }

    token->len += len;

    if (EVP_EncryptFinal_ex(ctx, token->data + token->len, &len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        return NGX_ERROR;
    }

    token->len += len;

    EVP_CIPHER_CTX_free(ctx);

#ifdef NGX_QUIC_DEBUG_PACKETS
    ngx_quic_hexdump(c->log, "quic new token", token->data, token->len);
#endif

    return NGX_OK;
}


static ngx_int_t
ngx_quic_validate_token(ngx_connection_t *c, ngx_quic_header_t *pkt)
{
    int                     len, tlen, iv_len;
    u_char                 *key, *iv, *p, *data;
    ngx_msec_t              msec;
    EVP_CIPHER_CTX         *ctx;
    const EVP_CIPHER       *cipher;
    struct sockaddr_in     *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6    *sin6;
#endif
    ngx_quic_connection_t  *qc;
    u_char                  tdec[NGX_QUIC_MAX_TOKEN_SIZE];

    qc = c->quic;

    /* Retry token */

    if (qc->token.len) {
        if (pkt->token.len != qc->token.len) {
            goto bad_token;
        }

        if (ngx_memcmp(pkt->token.data, qc->token.data, pkt->token.len) != 0) {
            goto bad_token;
        }

        return NGX_OK;
    }

    /* NEW_TOKEN in a previous connection */

    cipher = EVP_aes_256_cbc();
    key = c->quic->conf->token_key;
    iv = pkt->token.data;
    iv_len = EVP_CIPHER_iv_length(cipher);

    /* sanity checks */

    if (pkt->token.len < (size_t) iv_len + EVP_CIPHER_block_size(cipher)) {
        goto bad_token;
    }

    if (pkt->token.len > (size_t) iv_len + NGX_QUIC_MAX_TOKEN_SIZE) {
        goto bad_token;
    }

    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    if (!EVP_DecryptInit_ex(ctx, cipher, NULL, key, iv)) {
        EVP_CIPHER_CTX_free(ctx);
        return NGX_ERROR;
    }

    p = pkt->token.data + iv_len;
    len = pkt->token.len - iv_len;

    if (EVP_DecryptUpdate(ctx, tdec, &len, p, len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        goto bad_token;
    }

    if (EVP_DecryptFinal_ex(ctx, tdec + len, &tlen) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        goto bad_token;
    }

    EVP_CIPHER_CTX_free(ctx);

    switch (c->sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:
        sin6 = (struct sockaddr_in6 *) c->sockaddr;

        len = sizeof(struct in6_addr);
        data = sin6->sin6_addr.s6_addr;

        break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
    case AF_UNIX:

        len = ngx_min(c->addr_text.len, NGX_QUIC_MAX_TOKEN_SIZE - sizeof(msec));
        data = c->addr_text.data;

        break;
#endif

    default: /* AF_INET */
        sin = (struct sockaddr_in *) c->sockaddr;

        len = sizeof(in_addr_t);
        data = (u_char *) &sin->sin_addr;

        break;
    }

    if (ngx_memcmp(tdec, data, len) != 0) {
        goto bad_token;
    }

    ngx_memcpy(&msec, tdec + len, sizeof(msec));

    if (ngx_current_msec - msec > NGX_QUIC_RETRY_LIFETIME) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "quic expired token");
        return NGX_DECLINED;
    }

    return NGX_OK;

bad_token:

    ngx_log_error(NGX_LOG_INFO, c->log, 0, "quic invalid token");

    qc->error = NGX_QUIC_ERR_INVALID_TOKEN;
    qc->error_reason = "invalid_token";

    return NGX_DECLINED;
}


static ngx_int_t
ngx_quic_init_connection(ngx_connection_t *c)
{
    u_char                 *p;
    size_t                  clen;
    ssize_t                 len;
    ngx_ssl_conn_t         *ssl_conn;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (ngx_ssl_create_connection(qc->conf->ssl, c, NGX_SSL_BUFFER) != NGX_OK) {
        return NGX_ERROR;
    }

    ssl_conn = c->ssl->connection;

    if (SSL_set_quic_method(ssl_conn, &quic_method) == 0) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "quic SSL_set_quic_method() failed");
        return NGX_ERROR;
    }

#ifdef SSL_READ_EARLY_DATA_SUCCESS
    if (SSL_CTX_get_max_early_data(qc->conf->ssl->ctx)) {
        SSL_set_quic_early_data_enabled(ssl_conn, 1);
    }
#endif

    if (qc->conf->sr_token_key.len) {
        qc->tp.sr_enabled = 1;

        if (ngx_quic_new_sr_token(c, &qc->dcid, &qc->conf->sr_token_key,
                                  qc->tp.sr_token)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        ngx_quic_hexdump(c->log, "quic stateless reset token",
                         qc->tp.sr_token, (size_t) NGX_QUIC_SR_TOKEN_LEN);
    }

    len = ngx_quic_create_transport_params(NULL, NULL, &qc->tp, &clen);
    /* always succeeds */

    p = ngx_pnalloc(c->pool, len);
    if (p == NULL) {
        return NGX_ERROR;
    }

    len = ngx_quic_create_transport_params(p, p + len, &qc->tp, NULL);
    if (len < 0) {
        return NGX_ERROR;
    }

#ifdef NGX_QUIC_DEBUG_PACKETS
    ngx_quic_hexdump(c->log, "quic transport parameters", p, len);
#endif

    if (SSL_set_quic_transport_params(ssl_conn, p, len) == 0) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "quic SSL_set_quic_transport_params() failed");
        return NGX_ERROR;
    }

#if NGX_OPENSSL_QUIC_ZRTT_CTX
    if (SSL_set_quic_early_data_context(ssl_conn, p, clen) == 0) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "quic SSL_set_quic_early_data_context() failed");
        return NGX_ERROR;
    }
#endif

    return NGX_OK;
}


static ngx_inline size_t
ngx_quic_max_udp_payload(ngx_connection_t *c)
{
    /* TODO: path MTU discovery */

#if (NGX_HAVE_INET6)
    if (c->sockaddr->sa_family == AF_INET6) {
        return NGX_QUIC_MAX_UDP_PAYLOAD_OUT6;
    }
#endif

    return NGX_QUIC_MAX_UDP_PAYLOAD_OUT;
}


static void
ngx_quic_input_handler(ngx_event_t *rev)
{
    ssize_t                 n;
    ngx_int_t               rc;
    ngx_buf_t               b;
    ngx_connection_t       *c;
    ngx_quic_connection_t  *qc;
    static u_char           buf[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, rev->log, 0, "quic input handler");

    ngx_memzero(&b, sizeof(ngx_buf_t));
    b.start = buf;
    b.end = buf + sizeof(buf);
    b.pos = b.last = b.start;
    b.memory = 1;

    c = rev->data;
    qc = c->quic;

    c->log->action = "handling quic input";

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "quic client timed out");
        ngx_quic_close_connection(c, NGX_DONE);
        return;
    }

    if (c->close) {
        qc->error_reason = "graceful shutdown";
        ngx_quic_close_connection(c, NGX_OK);
        return;
    }

    n = c->recv(c, b.start, b.end - b.start);

    if (n == NGX_AGAIN) {
        if (qc->closing) {
            ngx_quic_close_connection(c, NGX_OK);
        }
        return;
    }

    if (n == NGX_ERROR) {
        c->read->eof = 1;
        ngx_quic_close_connection(c, NGX_ERROR);
        return;
    }

    b.last += n;
    qc->received += n;

    rc = ngx_quic_input(c, &b, NULL);

    if (rc == NGX_ERROR) {
        ngx_quic_close_connection(c, NGX_ERROR);
        return;
    }

    if (rc == NGX_DECLINED) {
        return;
    }

    /* rc == NGX_OK */

    qc->send_timer_set = 0;
    ngx_add_timer(rev, qc->tp.max_idle_timeout);

    ngx_quic_connstate_dbg(c);
}


static void
ngx_quic_close_connection(ngx_connection_t *c, ngx_int_t rc)
{
    ngx_pool_t  *pool;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_close_connection rc:%i", rc);

    if (!c->quic) {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                      "quic close connection early error");

    } else if (ngx_quic_close_quic(c, rc) == NGX_AGAIN) {
        return;
    }

    if (c->ssl) {
        (void) ngx_ssl_shutdown(c);
    }

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_active, -1);
#endif

    c->destroyed = 1;

    pool = c->pool;

    ngx_close_connection(c);

    ngx_destroy_pool(pool);
}


static ngx_int_t
ngx_quic_close_quic(ngx_connection_t *c, ngx_int_t rc)
{
    ngx_uint_t              i;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (!qc->closing) {

        /* drop packets from retransmit queues, no ack is expected */
        for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {
            ctx = ngx_quic_get_send_ctx(qc, i);
            ngx_quic_free_frames(c, &ctx->sent);
        }

        if (rc == NGX_DONE) {

            /*
             *  10.2.  Idle Timeout
             *
             *  If the idle timeout is enabled by either peer, a connection is
             *  silently closed and its state is discarded when it remains idle
             */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic closing %s connection",
                           qc->draining ? "drained" : "idle");

        } else {

            /*
             * 10.3.  Immediate Close
             *
             *  An endpoint sends a CONNECTION_CLOSE frame (Section 19.19)
             *  to terminate the connection immediately.
             */

            qc->error_level = c->ssl ? SSL_quic_read_level(c->ssl->connection)
                                     : ssl_encryption_initial;

            if (rc == NGX_OK) {
                 ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                                "quic immediate close drain:%d",
                                qc->draining);

                qc->close.log = c->log;
                qc->close.data = c;
                qc->close.handler = ngx_quic_close_timer_handler;
                qc->close.cancelable = 1;

                ctx = ngx_quic_get_send_ctx(qc, qc->error_level);

                ngx_add_timer(&qc->close, 3 * ngx_quic_pto(c, ctx));

                qc->error = NGX_QUIC_ERR_NO_ERROR;

            } else {
                if (qc->error == 0 && !qc->error_app) {
                    qc->error = NGX_QUIC_ERR_INTERNAL_ERROR;
                }

                ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                               "quic immediate close due to %s error: %ui %s",
                               qc->error_app ? "app " : "", qc->error,
                               qc->error_reason ? qc->error_reason : "");
            }

            (void) ngx_quic_send_cc(c);

            if (qc->error_level == ssl_encryption_handshake) {
                /* for clients that might not have handshake keys */
                qc->error_level = ssl_encryption_initial;
                (void) ngx_quic_send_cc(c);
            }
        }

        qc->closing = 1;
    }

    if (rc == NGX_ERROR && qc->close.timer_set) {
        /* do not wait for timer in case of fatal error */
        ngx_del_timer(&qc->close);
    }

    if (ngx_quic_close_streams(c, qc) == NGX_AGAIN) {
        return NGX_AGAIN;
    }

    if (qc->push.timer_set) {
        ngx_del_timer(&qc->push);
    }

    if (qc->pto.timer_set) {
        ngx_del_timer(&qc->pto);
    }

    if (qc->push.posted) {
        ngx_delete_posted_event(&qc->push);
    }

    for (i = 0; i < NGX_QUIC_ENCRYPTION_LAST; i++) {
        ngx_quic_free_frames(c, &qc->crypto[i].frames);
    }

    for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {
        ngx_quic_free_frames(c, &qc->send_ctx[i].frames);
        ngx_quic_free_frames(c, &qc->send_ctx[i].sent);
    }

    if (qc->close.timer_set) {
        return NGX_AGAIN;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic part of connection is terminated");

    /* may be tested from SSL callback during SSL shutdown */
    c->quic = NULL;

    return NGX_OK;
}


void
ngx_quic_finalize_connection(ngx_connection_t *c, ngx_uint_t err,
    const char *reason)
{
    ngx_quic_connection_t  *qc;

    qc = c->quic;
    qc->error = err;
    qc->error_reason = reason;
    qc->error_app = 1;
    qc->error_ftype = 0;

    ngx_quic_close_connection(c, NGX_ERROR);
}


static void
ngx_quic_close_timer_handler(ngx_event_t *ev)
{
    ngx_connection_t  *c;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "quic close timer");

    c = ev->data;
    ngx_quic_close_connection(c, NGX_DONE);
}


static ngx_int_t
ngx_quic_close_streams(ngx_connection_t *c, ngx_quic_connection_t *qc)
{
    ngx_event_t        *rev, *wev;
    ngx_rbtree_t       *tree;
    ngx_rbtree_node_t  *node;
    ngx_quic_stream_t  *qs;

#if (NGX_DEBUG)
    ngx_uint_t          ns;
#endif

    tree = &qc->streams.tree;

    if (tree->root == tree->sentinel) {
        return NGX_OK;
    }

#if (NGX_DEBUG)
    ns = 0;
#endif

    for (node = ngx_rbtree_min(tree->root, tree->sentinel);
         node;
         node = ngx_rbtree_next(tree, node))
    {
        qs = (ngx_quic_stream_t *) node;

        rev = qs->c->read;
        rev->error = 1;
        rev->ready = 1;

        wev = qs->c->write;
        wev->error = 1;
        wev->ready = 1;

        ngx_post_event(rev, &ngx_posted_events);

        if (rev->timer_set) {
            ngx_del_timer(rev);
        }

#if (NGX_DEBUG)
        ns++;
#endif
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic connection has %ui active streams", ns);

    return NGX_AGAIN;
}


static ngx_int_t
ngx_quic_input(ngx_connection_t *c, ngx_buf_t *b, ngx_quic_conf_t *conf)
{
    u_char             *p;
    ngx_int_t           rc;
    ngx_uint_t          good;
    ngx_quic_header_t   pkt;

    good = 0;

    p = b->pos;

    while (p < b->last) {

        ngx_memzero(&pkt, sizeof(ngx_quic_header_t));
        pkt.raw = b;
        pkt.data = p;
        pkt.len = b->last - p;
        pkt.log = c->log;
        pkt.flags = p[0];
        pkt.raw->pos++;

        if (c->quic) {
            c->quic->error = 0;
            c->quic->error_reason = 0;
        }

        rc = ngx_quic_process_packet(c, conf, &pkt);

#if (NGX_DEBUG)
        if (pkt.parsed) {
            ngx_log_debug5(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic packet %s done decr:%d pn:%L perr:%ui rc:%i",
                           ngx_quic_level_name(pkt.level), pkt.decrypted,
                           pkt.pn, pkt.error, rc);
        } else {
            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic packet done parse failed rc:%i", rc);
        }
#endif

        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (rc == NGX_OK) {
            good = 1;
        }

        /* NGX_OK || NGX_DECLINED */

        /*
         * we get NGX_DECLINED when there are no keys [yet] available
         * to decrypt packet.
         * Instead of queueing it, we ignore it and rely on the sender's
         * retransmission:
         *
         * 12.2.  Coalescing Packets:
         *
         * For example, if decryption fails (because the keys are
         * not available or any other reason), the receiver MAY either
         * discard or buffer the packet for later processing and MUST
         * attempt to process the remaining packets.
         *
         * We also skip packets that don't match connection state
         * or cannot be parsed properly.
         */

        /* b->pos is at header end, adjust by actual packet length */
        b->pos = pkt.data + pkt.len;

        /* firefox workaround: skip zero padding at the end of quic packet */
        while (b->pos < b->last && *(b->pos) == 0) {
            b->pos++;
        }

        p = b->pos;
    }

    return good ? NGX_OK : NGX_DECLINED;
}


static ngx_int_t
ngx_quic_process_packet(ngx_connection_t *c, ngx_quic_conf_t *conf,
    ngx_quic_header_t *pkt)
{
    ngx_int_t               rc;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    static u_char           buf[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    c->log->action = "parsing quic packet";

    rc = ngx_quic_parse_packet(pkt);

    if (rc == NGX_DECLINED || rc == NGX_ERROR) {
        return rc;
    }

    pkt->parsed = 1;

    c->log->action = "processing quic packet";

    qc = c->quic;

#if (NGX_DEBUG)
    ngx_quic_hexdump(c->log, "quic packet rx dcid",
                     pkt->dcid.data, pkt->dcid.len);

    if (pkt->level != ssl_encryption_application) {
        ngx_quic_hexdump(c->log, "quic packet rx scid", pkt->scid.data,
                         pkt->scid.len);
    }
#endif

    if (qc) {

        if (rc == NGX_ABORT) {
            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                          "quic unsupported version: 0x%xD", pkt->version);
            return NGX_DECLINED;
        }

        if (pkt->level != ssl_encryption_application) {
            if (pkt->version != qc->version) {
                ngx_log_error(NGX_LOG_INFO, c->log, 0,
                              "quic version mismatch: 0x%xD", pkt->version);
                return NGX_DECLINED;
            }
        }

        if (ngx_quic_check_peer(qc, pkt) != NGX_OK) {

            if (pkt->level == ssl_encryption_application) {
                if (ngx_quic_process_stateless_reset(c, pkt) == NGX_OK) {
                    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                                  "quic stateless reset packet detected");

                    qc->draining = 1;
                    ngx_quic_close_connection(c, NGX_OK);

                    return NGX_OK;
                }

                return ngx_quic_send_stateless_reset(c, qc->conf, pkt);
            }

            return NGX_DECLINED;
        }

        if (qc->in_retry) {

            c->log->action = "retrying quic connection";

            if (pkt->level != ssl_encryption_initial) {
                ngx_log_error(NGX_LOG_INFO, c->log, 0,
                              "quic discard late retry packet");
                return NGX_DECLINED;
            }

            if (!pkt->token.len) {
                ngx_log_error(NGX_LOG_INFO, c->log, 0,
                              "quic discard retry packet without token");
                return NGX_DECLINED;
            }

            if (ngx_quic_new_dcid(c, qc, &pkt->dcid) != NGX_OK) {
                return NGX_ERROR;
            }

            qc->tp.initial_scid = qc->dcid;
            qc->in_retry = 0;

            if (ngx_quic_init_secrets(c) != NGX_OK) {
                return NGX_ERROR;
            }

            if (ngx_quic_validate_token(c, pkt) != NGX_OK) {
                return NGX_ERROR;
            }

            qc->validated = 1;
        }

    } else {

        if (rc == NGX_ABORT) {
            return ngx_quic_negotiate_version(c, pkt);
        }

        if (pkt->level == ssl_encryption_initial) {

            c->log->action = "creating quic connection";

            if (pkt->dcid.len < NGX_QUIC_CID_LEN_MIN) {
                /* 7.2.  Negotiating Connection IDs */
                ngx_log_error(NGX_LOG_INFO, c->log, 0,
                              "quic too short dcid in initial"
                              " packet: len:%i", pkt->dcid.len);
                return NGX_ERROR;
            }

            qc = ngx_quic_new_connection(c, conf, pkt);
            if (qc == NULL) {
                return NGX_ERROR;
            }

            c->quic = qc;

            if (ngx_terminate || ngx_exiting) {
                qc->error = NGX_QUIC_ERR_CONNECTION_REFUSED;
                return NGX_ERROR;
            }

            if (pkt->token.len) {
                rc = ngx_quic_validate_token(c, pkt);

                if (rc == NGX_OK) {
                    qc->validated = 1;

                } else if (rc == NGX_ERROR) {
                    return NGX_ERROR;

                } else {
                    /* NGX_DECLINED */
                    if (conf->retry) {
                        return ngx_quic_send_retry(c);
                    }
                }

            } else if (conf->retry) {
                return ngx_quic_send_retry(c);
            }

            if (ngx_quic_init_secrets(c) != NGX_OK) {
                return NGX_ERROR;
            }

        } else if (pkt->level == ssl_encryption_application) {
            return ngx_quic_send_stateless_reset(c, conf, pkt);

        } else {
            return NGX_ERROR;
        }
    }

    c->log->action = "decrypting packet";

    if (!ngx_quic_keys_available(qc->keys, pkt->level)) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "quic no level %d keys yet, ignoring packet", pkt->level);
        return NGX_DECLINED;
    }

    pkt->keys = qc->keys;
    pkt->key_phase = qc->key_phase;
    pkt->plaintext = buf;

    ctx = ngx_quic_get_send_ctx(qc, pkt->level);

    rc = ngx_quic_decrypt(pkt, &ctx->largest_pn);
    if (rc != NGX_OK) {
        qc->error = pkt->error;
        qc->error_reason = "failed to decrypt packet";
        return rc;
    }

    pkt->decrypted = 1;

    if (c->ssl == NULL) {
        if (ngx_quic_init_connection(c) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (pkt->level == ssl_encryption_handshake) {
        /*
         * 4.10.1. The successful use of Handshake packets indicates
         * that no more Initial packets need to be exchanged
         */
        ngx_quic_discard_ctx(c, ssl_encryption_initial);

        if (qc->validated == 0) {
            qc->validated = 1;
            ngx_post_event(&c->quic->push, &ngx_posted_events);
        }
    }

    pkt->received = ngx_current_msec;

    c->log->action = "handling payload";

    if (pkt->level != ssl_encryption_application) {
        return ngx_quic_payload_handler(c, pkt);
    }

    if (!pkt->key_update) {
        return ngx_quic_payload_handler(c, pkt);
    }

    /* switch keys and generate next on Key Phase change */

    qc->key_phase ^= 1;
    ngx_quic_keys_switch(c, qc->keys);

    rc = ngx_quic_payload_handler(c, pkt);
    if (rc != NGX_OK) {
        return rc;
    }

    return ngx_quic_keys_update(c, qc->keys);
}


static ngx_int_t
ngx_quic_init_secrets(ngx_connection_t *c)
{
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (ngx_quic_keys_set_initial_secret(c->pool, qc->keys, &qc->odcid)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    qc->initialized = 1;

    return NGX_OK;
}


static void
ngx_quic_discard_ctx(ngx_connection_t *c, enum ssl_encryption_level_t level)
{
    ngx_queue_t            *q;
    ngx_quic_frame_t       *f;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (!ngx_quic_keys_available(qc->keys, level)) {
        return;
    }

    ngx_quic_keys_discard(qc->keys, level);

    qc->pto_count = 0;

    ctx = ngx_quic_get_send_ctx(qc, level);

    while (!ngx_queue_empty(&ctx->sent)) {
        q = ngx_queue_head(&ctx->sent);
        ngx_queue_remove(q);

        f = ngx_queue_data(q, ngx_quic_frame_t, queue);
        ngx_quic_congestion_ack(c, f);
        ngx_quic_free_frame(c, f);
    }

    while (!ngx_queue_empty(&ctx->frames)) {
        q = ngx_queue_head(&ctx->frames);
        ngx_queue_remove(q);

        f = ngx_queue_data(q, ngx_quic_frame_t, queue);
        ngx_quic_congestion_ack(c, f);
        ngx_quic_free_frame(c, f);
    }

    ctx->send_ack = 0;
}


static ngx_int_t
ngx_quic_check_peer(ngx_quic_connection_t *qc, ngx_quic_header_t *pkt)
{
    ngx_str_t             *dcid;
    ngx_queue_t           *q;
    ngx_quic_send_ctx_t   *ctx;
    ngx_quic_client_id_t  *cid;

    dcid = (pkt->level == ssl_encryption_early_data) ? &qc->odcid : &qc->dcid;

    if (pkt->dcid.len == dcid->len
        && ngx_memcmp(pkt->dcid.data, dcid->data, dcid->len) == 0)
    {
        if (pkt->level == ssl_encryption_application) {
            return NGX_OK;
        }

        goto found;
    }

    /*
     * a packet sent in response to an initial client packet might be lost,
     * thus check also for old dcid
     */
    ctx = ngx_quic_get_send_ctx(qc, ssl_encryption_initial);

    if (pkt->level == ssl_encryption_initial
        && ctx->largest_ack == NGX_QUIC_UNSET_PN)
    {
        if (pkt->dcid.len == qc->odcid.len
            && ngx_memcmp(pkt->dcid.data, qc->odcid.data, qc->odcid.len) == 0)
        {
            goto found;
        }
    }

    ngx_log_error(NGX_LOG_INFO, pkt->log, 0, "quic unexpected quic dcid");
    return NGX_ERROR;

found:

    for (q = ngx_queue_head(&qc->client_ids);
         q != ngx_queue_sentinel(&qc->client_ids);
         q = ngx_queue_next(q))
    {
        cid = ngx_queue_data(q, ngx_quic_client_id_t, queue);

        if (pkt->scid.len == cid->len
            && ngx_memcmp(pkt->scid.data, cid->id, cid->len) == 0)
        {
            return NGX_OK;
        }
    }

    ngx_log_error(NGX_LOG_INFO, pkt->log, 0, "quic unexpected quic scid");
    return NGX_ERROR;
}


static ngx_int_t
ngx_quic_payload_handler(ngx_connection_t *c, ngx_quic_header_t *pkt)
{
    u_char                 *end, *p;
    ssize_t                 len;
    ngx_uint_t              do_close;
    ngx_quic_frame_t        frame;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (qc->closing) {
        /*
         * 10.1  Closing and Draining Connection States
         * ... delayed or reordered packets are properly discarded.
         *
         *  An endpoint retains only enough information to generate
         *  a packet containing a CONNECTION_CLOSE frame and to identify
         *  packets as belonging to the connection.
         */

        qc->error_level = pkt->level;
        qc->error = NGX_QUIC_ERR_NO_ERROR;
        qc->error_reason = "connection is closing, packet discarded";
        qc->error_ftype = 0;
        qc->error_app = 0;

        return ngx_quic_send_cc(c);
    }

    p = pkt->payload.data;
    end = p + pkt->payload.len;

    do_close = 0;

    while (p < end) {

        c->log->action = "parsing frames";

        len = ngx_quic_parse_frame(pkt, p, end, &frame);

        if (len < 0) {
            qc->error = pkt->error;
            return NGX_ERROR;
        }

        ngx_quic_log_frame(c->log, &frame, 0);

        c->log->action = "handling frames";

        p += len;

        switch (frame.type) {

        case NGX_QUIC_FT_ACK:
            if (ngx_quic_handle_ack_frame(c, pkt, &frame.u.ack) != NGX_OK) {
                return NGX_ERROR;
            }

            continue;

        case NGX_QUIC_FT_PADDING:
            /* no action required */
            continue;

        case NGX_QUIC_FT_CONNECTION_CLOSE:
        case NGX_QUIC_FT_CONNECTION_CLOSE_APP:
            do_close = 1;
            continue;
        }

        /* got there with ack-eliciting packet */
        pkt->need_ack = 1;

        switch (frame.type) {

        case NGX_QUIC_FT_CRYPTO:

            if (ngx_quic_handle_crypto_frame(c, pkt, &frame) != NGX_OK) {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_PING:
            break;

        case NGX_QUIC_FT_STREAM0:
        case NGX_QUIC_FT_STREAM1:
        case NGX_QUIC_FT_STREAM2:
        case NGX_QUIC_FT_STREAM3:
        case NGX_QUIC_FT_STREAM4:
        case NGX_QUIC_FT_STREAM5:
        case NGX_QUIC_FT_STREAM6:
        case NGX_QUIC_FT_STREAM7:

            if (ngx_quic_handle_stream_frame(c, pkt, &frame) != NGX_OK) {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_MAX_DATA:

            if (ngx_quic_handle_max_data_frame(c, &frame.u.max_data) != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_STREAMS_BLOCKED:
        case NGX_QUIC_FT_STREAMS_BLOCKED2:

            if (ngx_quic_handle_streams_blocked_frame(c, pkt,
                                                      &frame.u.streams_blocked)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_STREAM_DATA_BLOCKED:

            if (ngx_quic_handle_stream_data_blocked_frame(c, pkt,
                                                  &frame.u.stream_data_blocked)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_MAX_STREAM_DATA:

            if (ngx_quic_handle_max_stream_data_frame(c, pkt,
                                                      &frame.u.max_stream_data)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_RESET_STREAM:

            if (ngx_quic_handle_reset_stream_frame(c, pkt,
                                                   &frame.u.reset_stream)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_STOP_SENDING:

            if (ngx_quic_handle_stop_sending_frame(c, pkt,
                                                   &frame.u.stop_sending)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_MAX_STREAMS:
        case NGX_QUIC_FT_MAX_STREAMS2:

            if (ngx_quic_handle_max_streams_frame(c, pkt, &frame.u.max_streams)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_PATH_CHALLENGE:

            if (ngx_quic_handle_path_challenge_frame(c, pkt,
                                                     &frame.u.path_challenge)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_NEW_CONNECTION_ID:

            if (ngx_quic_handle_new_connection_id_frame(c, pkt, &frame.u.ncid)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case NGX_QUIC_FT_RETIRE_CONNECTION_ID:
        case NGX_QUIC_FT_PATH_RESPONSE:

            /* TODO: handle */
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic frame handler not implemented");
            break;

        default:
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic missing frame handler");
            return NGX_ERROR;
        }
    }

    if (p != end) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "quic trailing garbage in payload:%ui bytes", end - p);

        qc->error = NGX_QUIC_ERR_FRAME_ENCODING_ERROR;
        return NGX_ERROR;
    }

    if (do_close) {
        qc->draining = 1;
        ngx_quic_close_connection(c, NGX_OK);
    }

    if (ngx_quic_ack_packet(c, pkt) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_ack_packet(ngx_connection_t *c, ngx_quic_header_t *pkt)
{
    uint64_t               base, largest, smallest, gs, ge, gap, range, pn;
    uint64_t               prev_pending;
    ngx_uint_t             i, nr;
    ngx_quic_send_ctx_t   *ctx;
    ngx_quic_ack_range_t  *r;

    c->log->action = "preparing ack";

    ctx = ngx_quic_get_send_ctx(c->quic, pkt->level);

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_ack_packet pn:%uL largest %L fr:%uL"
                   " nranges:%ui", pkt->pn, (int64_t) ctx->largest_range,
                   ctx->first_range, ctx->nranges);

    prev_pending = ctx->pending_ack;

    if (pkt->need_ack) {

        ngx_post_event(&c->quic->push, &ngx_posted_events);

        if (ctx->send_ack == 0) {
            ctx->ack_delay_start = ngx_current_msec;
        }

        ctx->send_ack++;

        if (ctx->pending_ack == NGX_QUIC_UNSET_PN
            || ctx->pending_ack < pkt->pn)
        {
            ctx->pending_ack = pkt->pn;
        }
    }

    base = ctx->largest_range;
    pn = pkt->pn;

    if (base == NGX_QUIC_UNSET_PN) {
        ctx->largest_range = pn;
        ctx->largest_received = pkt->received;
        return NGX_OK;
    }

    if (base == pn) {
        return NGX_OK;
    }

    largest = base;
    smallest = largest - ctx->first_range;

    if (pn > base) {

        if (pn - base == 1) {
            ctx->first_range++;
            ctx->largest_range = pn;
            ctx->largest_received = pkt->received;

            return NGX_OK;

        } else {
            /* new gap in front of current largest */

            /* no place for new range, send current range as is */
            if (ctx->nranges == NGX_QUIC_MAX_RANGES) {

                if (prev_pending != NGX_QUIC_UNSET_PN) {
                    if (ngx_quic_send_ack(c, ctx) != NGX_OK) {
                        return NGX_ERROR;
                    }
                }

                if (prev_pending == ctx->pending_ack || !pkt->need_ack) {
                    ctx->pending_ack = NGX_QUIC_UNSET_PN;
                }
            }

            gap = pn - base - 2;
            range = ctx->first_range;

            ctx->first_range = 0;
            ctx->largest_range = pn;
            ctx->largest_received = pkt->received;

            /* packet is out of order, force send */
            if (pkt->need_ack) {
                ctx->send_ack = NGX_QUIC_MAX_ACK_GAP;
            }

            i = 0;

            goto insert;
        }
    }

    /*  pn < base, perform lookup in existing ranges */

    /* packet is out of order */
    if (pkt->need_ack) {
        ctx->send_ack = NGX_QUIC_MAX_ACK_GAP;
    }

    if (pn >= smallest && pn <= largest) {
        return NGX_OK;
    }

#if (NGX_SUPPRESS_WARN)
    r = NULL;
#endif

    for (i = 0; i < ctx->nranges; i++) {
        r = &ctx->ranges[i];

        ge = smallest - 1;
        gs = ge - r->gap;

        if (pn >= gs && pn <= ge) {

            if (gs == ge) {
                /* gap size is exactly one packet, now filled */

                /* data moves to previous range, current is removed */

                if (i == 0) {
                    ctx->first_range += r->range + 2;

                } else {
                    ctx->ranges[i - 1].range += r->range + 2;
                }

                nr = ctx->nranges - i - 1;
                if (nr) {
                    ngx_memmove(&ctx->ranges[i], &ctx->ranges[i + 1],
                                sizeof(ngx_quic_ack_range_t) * nr);
                }

                ctx->nranges--;

            } else if (pn == gs) {
                /* current gap shrinks from tail (current range grows) */
                r->gap--;
                r->range++;

            } else if (pn == ge) {
                /* current gap shrinks from head (previous range grows) */
                r->gap--;

                if (i == 0) {
                    ctx->first_range++;

                } else {
                    ctx->ranges[i - 1].range++;
                }

            } else {
                /* current gap is split into two parts */

                gap = ge - pn - 1;
                range = 0;

                if (ctx->nranges == NGX_QUIC_MAX_RANGES) {
                    if (prev_pending != NGX_QUIC_UNSET_PN) {
                        if (ngx_quic_send_ack(c, ctx) != NGX_OK) {
                            return NGX_ERROR;
                        }
                    }

                    if (prev_pending == ctx->pending_ack || !pkt->need_ack) {
                        ctx->pending_ack = NGX_QUIC_UNSET_PN;
                    }
                }

                r->gap = pn - gs - 1;
                goto insert;
            }

            return NGX_OK;
        }

        largest = smallest - r->gap - 2;
        smallest = largest - r->range;

        if (pn >= smallest && pn <= largest) {
            /* this packet number is already known */
            return NGX_OK;
        }

    }

    if (pn == smallest - 1) {
        /* extend first or last range */

        if (i == 0) {
            ctx->first_range++;

        } else {
            r->range++;
        }

        return NGX_OK;
    }

    /* nothing found, add new range at the tail  */

    if (ctx->nranges == NGX_QUIC_MAX_RANGES) {
        /* packet is too old to keep it */

        if (pkt->need_ack) {
            return ngx_quic_send_ack_range(c, ctx, pn, pn);
        }

        return NGX_OK;
    }

    gap = smallest - 2 - pn;
    range = 0;

insert:

    if (ctx->nranges < NGX_QUIC_MAX_RANGES) {
        ctx->nranges++;
    }

    ngx_memmove(&ctx->ranges[i + 1], &ctx->ranges[i],
                sizeof(ngx_quic_ack_range_t) * (ctx->nranges - i - 1));

    ctx->ranges[i].gap = gap;
    ctx->ranges[i].range = range;

    return NGX_OK;
}


static ngx_int_t
ngx_quic_send_ack_range(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    uint64_t smallest, uint64_t largest)
{
    ngx_quic_frame_t  *frame;

    frame = ngx_quic_alloc_frame(c, 0);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = ctx->level;
    frame->type = NGX_QUIC_FT_ACK;
    frame->u.ack.largest = largest;
    frame->u.ack.delay = 0;
    frame->u.ack.range_count = 0;
    frame->u.ack.first_range = largest - smallest;

    return NGX_OK;
}


static void
ngx_quic_drop_ack_ranges(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    uint64_t pn)
{
    uint64_t               base;
    ngx_uint_t             i, smallest, largest;
    ngx_quic_ack_range_t  *r;

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_drop_ack_ranges pn:%uL largest:%uL"
                   " fr:%uL nranges:%ui", pn, ctx->largest_range,
                   ctx->first_range, ctx->nranges);

    base = ctx->largest_range;

    if (base == NGX_QUIC_UNSET_PN) {
        return;
    }

    if (ctx->pending_ack != NGX_QUIC_UNSET_PN && pn >= ctx->pending_ack) {
        ctx->pending_ack = NGX_QUIC_UNSET_PN;
    }

    largest = base;
    smallest = largest - ctx->first_range;

    if (pn >= largest) {
        ctx->largest_range = NGX_QUIC_UNSET_PN;
        ctx->first_range = 0;
        ctx->nranges = 0;
        return;
    }

    if (pn >= smallest) {
        ctx->first_range = largest - pn - 1;
        ctx->nranges = 0;
        return;
    }

    for (i = 0; i < ctx->nranges; i++) {
        r = &ctx->ranges[i];

        largest = smallest - r->gap - 2;
        smallest = largest - r->range;

        if (pn >= largest) {
            ctx->nranges = i;
            return;
        }
        if (pn >= smallest) {
            r->range = largest - pn - 1;
            ctx->nranges = i + 1;
            return;
        }
    }
}


static ngx_int_t
ngx_quic_send_ack(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx)
{
    u_char            *p;
    size_t             ranges_len;
    uint64_t           ack_delay;
    ngx_uint_t         i;
    ngx_quic_frame_t  *frame;

    if (ctx->level == ssl_encryption_application) {
        ack_delay = ngx_current_msec - ctx->largest_received;
        ack_delay *= 1000;
        ack_delay >>= c->quic->ctp.ack_delay_exponent;

    } else {
        ack_delay = 0;
    }

    ranges_len = 0;

    for (i = 0; i < ctx->nranges; i++) {
        ranges_len += ngx_quic_create_ack_range(NULL, ctx->ranges[i].gap,
                                                ctx->ranges[i].range);
    }

    frame = ngx_quic_alloc_frame(c, ranges_len);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    p = frame->data;

    for (i = 0; i < ctx->nranges; i++) {
        p += ngx_quic_create_ack_range(p, ctx->ranges[i].gap,
                                       ctx->ranges[i].range);
    }

    frame->level = ctx->level;
    frame->type = NGX_QUIC_FT_ACK;
    frame->u.ack.largest = ctx->largest_range;
    frame->u.ack.delay = ack_delay;
    frame->u.ack.range_count = ctx->nranges;
    frame->u.ack.first_range = ctx->first_range;
    frame->u.ack.ranges_start = frame->data;
    frame->u.ack.ranges_end = frame->data + ranges_len;

    ngx_quic_queue_frame(c->quic, frame);

    return NGX_OK;
}


static ngx_int_t
ngx_quic_send_cc(ngx_connection_t *c)
{
    ngx_quic_frame_t       *frame;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (qc->draining) {
        return NGX_OK;
    }

    if (!qc->initialized) {
        /* try to initialize secrets to send an early error */
        if (ngx_quic_init_secrets(c) != NGX_OK) {
            return NGX_OK;
        }
    }

    if (qc->closing
        && ngx_current_msec - qc->last_cc < NGX_QUIC_CC_MIN_INTERVAL)
    {
        /* dot not send CC too often */
        return NGX_OK;
    }

    frame = ngx_quic_alloc_frame(c, 0);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = qc->error_level;
    frame->type = NGX_QUIC_FT_CONNECTION_CLOSE;
    frame->u.close.error_code = qc->error;
    frame->u.close.frame_type = qc->error_ftype;
    frame->u.close.app = qc->error_app;

    if (qc->error_reason) {
        frame->u.close.reason.len = ngx_strlen(qc->error_reason);
        frame->u.close.reason.data = (u_char *) qc->error_reason;
    }

    ngx_quic_queue_frame(c->quic, frame);

    qc->last_cc = ngx_current_msec;

    return ngx_quic_output(c);
}


static ngx_int_t
ngx_quic_send_new_token(ngx_connection_t *c)
{
    ngx_str_t          token;
    ngx_quic_frame_t  *frame;

    if (!c->quic->conf->retry) {
        return NGX_OK;
    }

    if (ngx_quic_new_token(c, &token) != NGX_OK) {
        return NGX_ERROR;
    }

    frame = ngx_quic_alloc_frame(c, 0);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = ssl_encryption_application;
    frame->type = NGX_QUIC_FT_NEW_TOKEN;
    frame->u.token.length = token.len;
    frame->u.token.data = token.data;

    ngx_quic_queue_frame(c->quic, frame);

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_ack_frame(ngx_connection_t *c, ngx_quic_header_t *pkt,
    ngx_quic_ack_frame_t *ack)
{
    ssize_t                 n;
    u_char                 *pos, *end;
    uint64_t                min, max, gap, range;
    ngx_msec_t              send_time;
    ngx_uint_t              i;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    ctx = ngx_quic_get_send_ctx(qc, pkt->level);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_handle_ack_frame level:%d", pkt->level);

    /*
     *  If any computed packet number is negative, an endpoint MUST
     *  generate a connection error of type FRAME_ENCODING_ERROR.
     *  (19.3.1)
     */

    if (ack->first_range > ack->largest) {
        qc->error = NGX_QUIC_ERR_FRAME_ENCODING_ERROR;
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "quic invalid first range in ack frame");
        return NGX_ERROR;
    }

    min = ack->largest - ack->first_range;
    max = ack->largest;

    if (ngx_quic_handle_ack_frame_range(c, ctx, min, max, &send_time)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    /* 13.2.3.  Receiver Tracking of ACK Frames */
    if (ctx->largest_ack < max || ctx->largest_ack == NGX_QUIC_UNSET_PN) {
        ctx->largest_ack = max;
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic updated largest received ack:%uL", max);

        /*
         *  An endpoint generates an RTT sample on receiving an
         *  ACK frame that meets the following two conditions:
         *
         *  - the largest acknowledged packet number is newly acknowledged
         *  - at least one of the newly acknowledged packets was ack-eliciting.
         */

        if (send_time != NGX_TIMER_INFINITE) {
            ngx_quic_rtt_sample(c, ack, pkt->level, send_time);
        }
    }

    pos = ack->ranges_start;
    end = ack->ranges_end;

    for (i = 0; i < ack->range_count; i++) {

        n = ngx_quic_parse_ack_range(pkt->log, pos, end, &gap, &range);
        if (n == NGX_ERROR) {
            return NGX_ERROR;
        }
        pos += n;

        if (gap + 2 > min) {
            qc->error = NGX_QUIC_ERR_FRAME_ENCODING_ERROR;
            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                         "quic invalid range:%ui in ack frame", i);
            return NGX_ERROR;
        }

        max = min - gap - 2;

        if (range > max) {
            qc->error = NGX_QUIC_ERR_FRAME_ENCODING_ERROR;
            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                         "quic invalid range:%ui in ack frame", i);
            return NGX_ERROR;
        }

        min = max - range;

        if (ngx_quic_handle_ack_frame_range(c, ctx, min, max, &send_time)
            != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    return ngx_quic_detect_lost(c);
}


static ngx_int_t
ngx_quic_handle_ack_frame_range(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    uint64_t min, uint64_t max, ngx_msec_t *send_time)
{
    uint64_t                found_num;
    ngx_uint_t              found;
    ngx_queue_t            *q;
    ngx_quic_frame_t       *f;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    *send_time = NGX_TIMER_INFINITE;
    found = 0;
    found_num = 0;

    q = ngx_queue_last(&ctx->sent);

    while (q != ngx_queue_sentinel(&ctx->sent)) {

        f = ngx_queue_data(q, ngx_quic_frame_t, queue);
        q = ngx_queue_prev(q);

        if (f->pnum >= min && f->pnum <= max) {
            ngx_quic_congestion_ack(c, f);

            switch (f->type) {
            case NGX_QUIC_FT_ACK:
            case NGX_QUIC_FT_ACK_ECN:
                ngx_quic_drop_ack_ranges(c, ctx, f->u.ack.largest);
                break;

            case NGX_QUIC_FT_STREAM0:
            case NGX_QUIC_FT_STREAM1:
            case NGX_QUIC_FT_STREAM2:
            case NGX_QUIC_FT_STREAM3:
            case NGX_QUIC_FT_STREAM4:
            case NGX_QUIC_FT_STREAM5:
            case NGX_QUIC_FT_STREAM6:
            case NGX_QUIC_FT_STREAM7:
                ngx_quic_handle_stream_ack(c, f);
                break;
            }

            if (f->pnum > found_num || !found) {
                *send_time = f->last;
                found_num = f->pnum;
            }

            ngx_queue_remove(&f->queue);
            ngx_quic_free_frame(c, f);
            found = 1;
        }
    }

    if (!found) {

        if (max < ctx->pnum) {
            /* duplicate ACK or ACK for non-ack-eliciting frame */
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "quic ACK for the packet not sent");

        qc->error = NGX_QUIC_ERR_PROTOCOL_VIOLATION;
        qc->error_ftype = NGX_QUIC_FT_ACK;
        qc->error_reason = "unknown packet number";

        return NGX_ERROR;
    }

    if (!qc->push.timer_set) {
        ngx_post_event(&qc->push, &ngx_posted_events);
    }

    qc->pto_count = 0;

    return NGX_OK;
}


static void
ngx_quic_rtt_sample(ngx_connection_t *c, ngx_quic_ack_frame_t *ack,
    enum ssl_encryption_level_t level, ngx_msec_t send_time)
{
    ngx_msec_t              latest_rtt, ack_delay, adjusted_rtt, rttvar_sample;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    latest_rtt = ngx_current_msec - send_time;
    qc->latest_rtt = latest_rtt;

    if (qc->min_rtt == NGX_TIMER_INFINITE) {
        qc->min_rtt = latest_rtt;
        qc->avg_rtt = latest_rtt;
        qc->rttvar = latest_rtt / 2;

    } else {
        qc->min_rtt = ngx_min(qc->min_rtt, latest_rtt);


        if (level == ssl_encryption_application) {
            ack_delay = ack->delay * (1 << qc->ctp.ack_delay_exponent) / 1000;
            ack_delay = ngx_min(ack_delay, qc->ctp.max_ack_delay);

        } else {
            ack_delay = 0;
        }

        adjusted_rtt = latest_rtt;

        if (qc->min_rtt + ack_delay < latest_rtt) {
            adjusted_rtt -= ack_delay;
        }

        qc->avg_rtt = 0.875 * qc->avg_rtt + 0.125 * adjusted_rtt;
        rttvar_sample = ngx_abs((ngx_msec_int_t) (qc->avg_rtt - adjusted_rtt));
        qc->rttvar = 0.75 * qc->rttvar + 0.25 * rttvar_sample;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic rtt sample latest:%M min:%M avg:%M var:%M",
                   latest_rtt, qc->min_rtt, qc->avg_rtt, qc->rttvar);
}


static ngx_inline ngx_msec_t
ngx_quic_pto(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx)
{
    ngx_msec_t              duration;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    /* PTO calculation: quic-recovery, Appendix 8 */
    duration = qc->avg_rtt;

    duration += ngx_max(4 * qc->rttvar, NGX_QUIC_TIME_GRANULARITY);
    duration <<= qc->pto_count;

    if (qc->congestion.in_flight == 0) { /* no in-flight packets */
        return duration;
    }

    if (ctx == &qc->send_ctx[2] && c->ssl->handshaked) {
        /* application send space */

        duration += qc->tp.max_ack_delay << qc->pto_count;
    }

    return duration;
}


static void
ngx_quic_handle_stream_ack(ngx_connection_t *c, ngx_quic_frame_t *f)
{
    uint64_t                sent, unacked;
    ngx_event_t            *wev;
    ngx_quic_stream_t      *sn;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    sn = ngx_quic_find_stream(&qc->streams.tree, f->u.stream.stream_id);
    if (sn == NULL) {
        return;
    }

    wev = sn->c->write;
    sent = sn->c->sent;
    unacked = sent - sn->acked;

    if (unacked >= NGX_QUIC_STREAM_BUFSIZE && wev->active) {
        wev->ready = 1;
        ngx_post_event(wev, &ngx_posted_events);
    }

    sn->acked += f->u.stream.length;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, sn->c->log, 0,
                   "quic stream ack len:%uL acked:%uL unacked:%uL",
                   f->u.stream.length, sn->acked, sent - sn->acked);
}


static ngx_int_t
ngx_quic_handle_ordered_frame(ngx_connection_t *c, ngx_quic_frames_stream_t *fs,
    ngx_quic_frame_t *frame, ngx_quic_frame_handler_pt handler, void *data)
{
    size_t                     full_len;
    ngx_int_t                  rc;
    ngx_queue_t               *q;
    ngx_quic_ordered_frame_t  *f;

    f = &frame->u.ord;

    if (f->offset > fs->received) {
        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic out-of-order frame: expecting:%uL got:%uL",
                       fs->received, f->offset);

        return ngx_quic_buffer_frame(c, fs, frame);
    }

    if (f->offset < fs->received) {

        if (ngx_quic_adjust_frame_offset(c, frame, fs->received)
            == NGX_DONE)
        {
            /* old/duplicate data range */
            return handler == ngx_quic_crypto_input ? NGX_DECLINED : NGX_OK;
        }

        /* intersecting data range, frame modified */
    }

    /* f->offset == fs->received */

    rc = handler(c, frame, data);
    if (rc == NGX_ERROR) {
        return NGX_ERROR;

    } else if (rc == NGX_DONE) {
        /* handler destroyed stream, queue no longer exists */
        return NGX_OK;
    }

    /* rc == NGX_OK */

    fs->received += f->length;

    /* now check the queue if we can continue with buffered frames */

    do {
        q = ngx_queue_head(&fs->frames);
        if (q == ngx_queue_sentinel(&fs->frames)) {
            break;
        }

        frame = ngx_queue_data(q, ngx_quic_frame_t, queue);
        f = &frame->u.ord;

        if (f->offset > fs->received) {
            /* gap found, nothing more to do */
            break;
        }

        full_len = f->length;

        if (f->offset < fs->received) {

            if (ngx_quic_adjust_frame_offset(c, frame, fs->received)
                == NGX_DONE)
            {
                /* old/duplicate data range */
                ngx_queue_remove(q);
                fs->total -= f->length;

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                              "quic skipped buffered frame, total:%ui",
                              fs->total);
                ngx_quic_free_frame(c, frame);
                continue;
            }

            /* frame was adjusted, proceed to input */
        }

        /* f->offset == fs->received */

        rc = handler(c, frame, data);

        if (rc == NGX_ERROR) {
            return NGX_ERROR;

        } else if (rc == NGX_DONE) {
            /* handler destroyed stream, queue no longer exists */
            return NGX_OK;
        }

        fs->received += f->length;
        fs->total -= full_len;

        ngx_queue_remove(q);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                      "quic consumed buffered frame, total:%ui", fs->total);

        ngx_quic_free_frame(c, frame);

    } while (1);

    return NGX_OK;
}


static ngx_int_t
ngx_quic_adjust_frame_offset(ngx_connection_t *c, ngx_quic_frame_t *frame,
    uint64_t offset_in)
{
    size_t                     tail;
    ngx_quic_ordered_frame_t  *f;

    f = &frame->u.ord;

    tail = offset_in - f->offset;

    if (tail >= f->length) {
        /* range preceeding already received data or duplicate, ignore */

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic old or duplicate data in ordered frame, ignored");
        return NGX_DONE;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic adjusted ordered frame data start to expected offset");

    /* intersecting range: adjust data size */

    f->offset += tail;
    f->data += tail;
    f->length -= tail;

    return NGX_OK;
}


static ngx_int_t
ngx_quic_buffer_frame(ngx_connection_t *c, ngx_quic_frames_stream_t *fs,
    ngx_quic_frame_t *frame)
{
    u_char                    *data;
    ngx_queue_t               *q;
    ngx_quic_frame_t          *dst, *item;
    ngx_quic_ordered_frame_t  *f, *df;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_buffer_frame");

    f = &frame->u.ord;

    /* frame start offset is in the future, buffer it */

    dst = ngx_quic_alloc_frame(c, f->length);
    if (dst == NULL) {
        return NGX_ERROR;
    }

    data = dst->data;
    ngx_memcpy(dst, frame, sizeof(ngx_quic_frame_t));
    dst->data = data;

    ngx_memcpy(dst->data, f->data, f->length);

    df = &dst->u.ord;
    df->data = dst->data;

    fs->total += f->length;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                  "quic ordered frame with unexpected offset:"
                  " buffered total:%ui", fs->total);

    if (ngx_queue_empty(&fs->frames)) {
        ngx_queue_insert_after(&fs->frames, &dst->queue);
        return NGX_OK;
    }

    for (q = ngx_queue_last(&fs->frames);
         q != ngx_queue_sentinel(&fs->frames);
         q = ngx_queue_prev(q))
    {
        item = ngx_queue_data(q, ngx_quic_frame_t, queue);
        f = &item->u.ord;

        if (f->offset < df->offset) {
            ngx_queue_insert_after(q, &dst->queue);
            return NGX_OK;
        }
    }

    ngx_queue_insert_after(&fs->frames, &dst->queue);

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_crypto_frame(ngx_connection_t *c, ngx_quic_header_t *pkt,
    ngx_quic_frame_t *frame)
{
    uint64_t                   last;
    ngx_int_t                  rc;
    ngx_quic_send_ctx_t       *ctx;
    ngx_quic_connection_t     *qc;
    ngx_quic_crypto_frame_t   *f;
    ngx_quic_frames_stream_t  *fs;

    qc = c->quic;
    fs = &qc->crypto[pkt->level];
    f = &frame->u.crypto;

    /* no overflow since both values are 62-bit */
    last = f->offset + f->length;

    if (last > fs->received && last - fs->received > NGX_QUIC_MAX_BUFFERED) {
        c->quic->error = NGX_QUIC_ERR_CRYPTO_BUFFER_EXCEEDED;
        return NGX_ERROR;
    }

    rc = ngx_quic_handle_ordered_frame(c, fs, frame, ngx_quic_crypto_input,
                                       NULL);
    if (rc != NGX_DECLINED) {
        return rc;
    }

    /* speeding up handshake completion */

    if (pkt->level == ssl_encryption_initial) {
        ctx = ngx_quic_get_send_ctx(qc, pkt->level);

        if (!ngx_queue_empty(&ctx->sent)) {
            ngx_quic_resend_frames(c, ctx);
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_crypto_input(ngx_connection_t *c, ngx_quic_frame_t *frame, void *data)
{
    int                       n, sslerr;
    ngx_ssl_conn_t           *ssl_conn;
    ngx_quic_crypto_frame_t  *f;

    f = &frame->u.crypto;

    ssl_conn = c->ssl->connection;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic SSL_quic_read_level:%d SSL_quic_write_level:%d",
                   (int) SSL_quic_read_level(ssl_conn),
                   (int) SSL_quic_write_level(ssl_conn));

    if (!SSL_provide_quic_data(ssl_conn, SSL_quic_read_level(ssl_conn),
                               f->data, f->length))
    {
        ngx_ssl_error(NGX_LOG_INFO, c->log, 0,
                      "SSL_provide_quic_data() failed");
        return NGX_ERROR;
    }

    n = SSL_do_handshake(ssl_conn);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic SSL_quic_read_level:%d SSL_quic_write_level:%d",
                   (int) SSL_quic_read_level(ssl_conn),
                   (int) SSL_quic_write_level(ssl_conn));

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "SSL_do_handshake: %d", n);

    if (n <= 0) {
        sslerr = SSL_get_error(ssl_conn, n);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "SSL_get_error: %d",
                       sslerr);

        if (sslerr != SSL_ERROR_WANT_READ) {
            ngx_ssl_error(NGX_LOG_ERR, c->log, 0, "SSL_do_handshake() failed");
            return NGX_ERROR;
        }

        return NGX_OK;
    }

    if (SSL_in_init(ssl_conn)) {
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ssl cipher:%s", SSL_get_cipher(ssl_conn));

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic handshake completed successfully");

    c->ssl->handshaked = 1;
    c->ssl->no_wait_shutdown = 1;

    frame = ngx_quic_alloc_frame(c, 0);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    /* 12.4 Frames and frame types, figure 8 */
    frame->level = ssl_encryption_application;
    frame->type = NGX_QUIC_FT_HANDSHAKE_DONE;
    ngx_quic_queue_frame(c->quic, frame);

    if (ngx_quic_send_new_token(c) != NGX_OK) {
        return NGX_ERROR;
    }

    /*
     * Generating next keys before a key update is received.
     * See quic-tls 9.4 Header Protection Timing Side-Channels.
     */

    if (ngx_quic_keys_update(c, c->quic->keys) != NGX_OK) {
        return NGX_ERROR;
    }

    /*
     * 4.10.2 An endpoint MUST discard its handshake keys
     * when the TLS handshake is confirmed
     */
    ngx_quic_discard_ctx(c, ssl_encryption_handshake);

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_stream_frame(ngx_connection_t *c, ngx_quic_header_t *pkt,
    ngx_quic_frame_t *frame)
{
    size_t                     window;
    uint64_t                   last;
    ngx_buf_t                 *b;
    ngx_pool_t                *pool;
    ngx_connection_t          *sc;
    ngx_quic_stream_t         *sn;
    ngx_quic_connection_t     *qc;
    ngx_quic_stream_frame_t   *f;
    ngx_quic_frames_stream_t  *fs;

    qc = c->quic;
    f = &frame->u.stream;

    if ((f->stream_id & NGX_QUIC_STREAM_UNIDIRECTIONAL)
        && (f->stream_id & NGX_QUIC_STREAM_SERVER_INITIATED))
    {
        qc->error = NGX_QUIC_ERR_STREAM_STATE_ERROR;
        return NGX_ERROR;
    }

    /* no overflow since both values are 62-bit */
    last = f->offset + f->length;

    sn = ngx_quic_find_stream(&qc->streams.tree, f->stream_id);

    if (sn == NULL) {
        sn = ngx_quic_create_client_stream(c, f->stream_id);

        if (sn == NULL) {
            return NGX_ERROR;
        }

        if (sn == NGX_QUIC_STREAM_GONE) {
            return NGX_OK;
        }

        sc = sn->c;
        fs = &sn->fs;
        b = sn->b;
        window = b->end - b->last;

        if (last > window) {
            c->quic->error = NGX_QUIC_ERR_FLOW_CONTROL_ERROR;
            goto cleanup;
        }

        if (ngx_quic_handle_ordered_frame(c, fs, frame, ngx_quic_stream_input,
                                          sn)
            != NGX_OK)
        {
            goto cleanup;
        }

        sc->listening->handler(sc);

        return NGX_OK;
    }

    fs = &sn->fs;
    b = sn->b;
    window = (b->pos - b->start) + (b->end - b->last);

    if (last > fs->received && last - fs->received > window) {
        c->quic->error = NGX_QUIC_ERR_FLOW_CONTROL_ERROR;
        return NGX_ERROR;
    }

    return ngx_quic_handle_ordered_frame(c, fs, frame, ngx_quic_stream_input,
                                         sn);

cleanup:

    pool = sc->pool;

    ngx_close_connection(sc);
    ngx_destroy_pool(pool);

    return NGX_ERROR;
}


static ngx_int_t
ngx_quic_stream_input(ngx_connection_t *c, ngx_quic_frame_t *frame, void *data)
{
    uint64_t                  id;
    ngx_buf_t                *b;
    ngx_event_t              *rev;
    ngx_quic_stream_t        *sn;
    ngx_quic_connection_t    *qc;
    ngx_quic_stream_frame_t  *f;

    qc = c->quic;
    sn = data;

    f = &frame->u.stream;
    id = f->stream_id;

    b = sn->b;

    if ((size_t) ((b->pos - b->start) + (b->end - b->last)) < f->length) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "quic no space in stream buffer");
        return NGX_ERROR;
    }

    if ((size_t) (b->end - b->last) < f->length) {
        b->last = ngx_movemem(b->start, b->pos, b->last - b->pos);
        b->pos = b->start;
    }

    b->last = ngx_cpymem(b->last, f->data, f->length);

    rev = sn->c->read;
    rev->ready = 1;

    if (f->fin) {
        rev->pending_eof = 1;
    }

    if (rev->active) {
        rev->handler(rev);
    }

    /* check if stream was destroyed by handler */
    if (ngx_quic_find_stream(&qc->streams.tree, id) == NULL) {
        return NGX_DONE;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_max_data_frame(ngx_connection_t *c,
    ngx_quic_max_data_frame_t *f)
{
    ngx_event_t            *wev;
    ngx_rbtree_t           *tree;
    ngx_rbtree_node_t      *node;
    ngx_quic_stream_t      *qs;
    ngx_quic_connection_t  *qc;

    qc = c->quic;
    tree = &qc->streams.tree;

    if (f->max_data <= qc->streams.send_max_data) {
        return NGX_OK;
    }

    if (qc->streams.sent >= qc->streams.send_max_data) {

        for (node = ngx_rbtree_min(tree->root, tree->sentinel);
             node;
             node = ngx_rbtree_next(tree, node))
        {
            qs = (ngx_quic_stream_t *) node;
            wev = qs->c->write;

            if (wev->active) {
                wev->ready = 1;
                ngx_post_event(wev, &ngx_posted_events);
            }
        }
    }

    qc->streams.send_max_data = f->max_data;

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_streams_blocked_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_streams_blocked_frame_t *f)
{
    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_stream_data_blocked_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_stream_data_blocked_frame_t *f)
{
    size_t                  n;
    ngx_buf_t              *b;
    ngx_quic_frame_t       *frame;
    ngx_quic_stream_t      *sn;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if ((f->id & NGX_QUIC_STREAM_UNIDIRECTIONAL)
        && (f->id & NGX_QUIC_STREAM_SERVER_INITIATED))
    {
        qc->error = NGX_QUIC_ERR_STREAM_STATE_ERROR;
        return NGX_ERROR;
    }

    sn = ngx_quic_find_stream(&qc->streams.tree, f->id);

    if (sn == NULL) {
        sn = ngx_quic_create_client_stream(c, f->id);

        if (sn == NULL) {
            return NGX_ERROR;
        }

        if (sn == NGX_QUIC_STREAM_GONE) {
            return NGX_OK;
        }

        b = sn->b;
        n = b->end - b->last;

        sn->c->listening->handler(sn->c);

    } else {
        b = sn->b;
        n = sn->fs.received + (b->pos - b->start) + (b->end - b->last);
    }

    frame = ngx_quic_alloc_frame(c, 0);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = pkt->level;
    frame->type = NGX_QUIC_FT_MAX_STREAM_DATA;
    frame->u.max_stream_data.id = f->id;
    frame->u.max_stream_data.limit = n;

    ngx_quic_queue_frame(c->quic, frame);

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_max_stream_data_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_max_stream_data_frame_t *f)
{
    uint64_t                sent;
    ngx_event_t            *wev;
    ngx_quic_stream_t      *sn;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if ((f->id & NGX_QUIC_STREAM_UNIDIRECTIONAL)
        && (f->id & NGX_QUIC_STREAM_SERVER_INITIATED) == 0)
    {
        qc->error = NGX_QUIC_ERR_STREAM_STATE_ERROR;
        return NGX_ERROR;
    }

    sn = ngx_quic_find_stream(&qc->streams.tree, f->id);

    if (sn == NULL) {
        sn = ngx_quic_create_client_stream(c, f->id);

        if (sn == NULL) {
            return NGX_ERROR;
        }

        if (sn == NGX_QUIC_STREAM_GONE) {
            return NGX_OK;
        }

        if (f->limit > sn->send_max_data) {
            sn->send_max_data = f->limit;
        }

        sn->c->listening->handler(sn->c);

        return NGX_OK;
    }

    if (f->limit <= sn->send_max_data) {
        return NGX_OK;
    }

    sent = sn->c->sent;

    if (sent >= sn->send_max_data) {
        wev = sn->c->write;

        if (wev->active) {
            wev->ready = 1;
            ngx_post_event(wev, &ngx_posted_events);
        }
    }

    sn->send_max_data = f->limit;

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_reset_stream_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_reset_stream_frame_t *f)
{
    ngx_event_t            *rev;
    ngx_connection_t       *sc;
    ngx_quic_stream_t      *sn;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if ((f->id & NGX_QUIC_STREAM_UNIDIRECTIONAL)
        && (f->id & NGX_QUIC_STREAM_SERVER_INITIATED))
    {
        qc->error = NGX_QUIC_ERR_STREAM_STATE_ERROR;
        return NGX_ERROR;
    }

    sn = ngx_quic_find_stream(&qc->streams.tree, f->id);

    if (sn == NULL) {
        sn = ngx_quic_create_client_stream(c, f->id);

        if (sn == NULL) {
            return NGX_ERROR;
        }

        if (sn == NGX_QUIC_STREAM_GONE) {
            return NGX_OK;
        }

        sc = sn->c;

        rev = sc->read;
        rev->error = 1;
        rev->ready = 1;

        sc->listening->handler(sc);

        return NGX_OK;
    }

    rev = sn->c->read;
    rev->error = 1;
    rev->ready = 1;

    if (rev->active) {
        rev->handler(rev);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_stop_sending_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_stop_sending_frame_t *f)
{
    ngx_event_t            *wev;
    ngx_connection_t       *sc;
    ngx_quic_stream_t      *sn;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if ((f->id & NGX_QUIC_STREAM_UNIDIRECTIONAL)
        && (f->id & NGX_QUIC_STREAM_SERVER_INITIATED) == 0)
    {
        qc->error = NGX_QUIC_ERR_STREAM_STATE_ERROR;
        return NGX_ERROR;
    }

    sn = ngx_quic_find_stream(&qc->streams.tree, f->id);

    if (sn == NULL) {
        sn = ngx_quic_create_client_stream(c, f->id);

        if (sn == NULL) {
            return NGX_ERROR;
        }

        if (sn == NGX_QUIC_STREAM_GONE) {
            return NGX_OK;
        }

        sc = sn->c;

        wev = sc->write;
        wev->error = 1;
        wev->ready = 1;

        sc->listening->handler(sc);

        return NGX_OK;
    }

    wev = sn->c->write;
    wev->error = 1;
    wev->ready = 1;

    if (wev->active) {
        wev->handler(wev);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_max_streams_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_max_streams_frame_t *f)
{
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (f->bidi) {
        if (qc->streams.server_max_streams_bidi < f->limit) {
            qc->streams.server_max_streams_bidi = f->limit;

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic max_streams_bidi:%uL", f->limit);
        }

    } else {
        if (qc->streams.server_max_streams_uni < f->limit) {
            qc->streams.server_max_streams_uni = f->limit;

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic max_streams_uni:%uL", f->limit);
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_path_challenge_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_path_challenge_frame_t *f)
{
    ngx_quic_frame_t  *frame;

    frame = ngx_quic_alloc_frame(c, 0);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = pkt->level;
    frame->type = NGX_QUIC_FT_PATH_RESPONSE;
    frame->u.path_response = *f;

    ngx_quic_queue_frame(c->quic, frame);

    return NGX_OK;
}


static ngx_int_t
ngx_quic_handle_new_connection_id_frame(ngx_connection_t *c,
    ngx_quic_header_t *pkt, ngx_quic_new_conn_id_frame_t *f)
{
    ngx_queue_t            *q;
    ngx_quic_client_id_t   *cid, *item;
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (f->seqnum < qc->max_retired_seqnum) {
        /*
         *  An endpoint that receives a NEW_CONNECTION_ID frame with
         *  a sequence number smaller than the Retire Prior To field
         *  of a previously received NEW_CONNECTION_ID frame MUST send
         *  a corresponding RETIRE_CONNECTION_ID frame that retires
         *  the newly received connection  ID, unless it has already
         *  done so for that sequence number.
         */

        if (ngx_quic_retire_connection_id(c, pkt->level, f->seqnum) != NGX_OK) {
            return NGX_ERROR;
        }

        goto retire;
    }

    cid = NULL;

    for (q = ngx_queue_head(&qc->client_ids);
         q != ngx_queue_sentinel(&qc->client_ids);
         q = ngx_queue_next(q))
    {
        item = ngx_queue_data(q, ngx_quic_client_id_t, queue);

        if (item->seqnum == f->seqnum) {
            cid = item;
            break;
        }
    }

    if (cid) {
        /*
         * Transmission errors, timeouts and retransmissions might cause the
         * same NEW_CONNECTION_ID frame to be received multiple times
         */

        if (cid->len != f->len
            || ngx_strncmp(cid->id, f->cid, f->len) != 0
            || ngx_strncmp(cid->sr_token, f->srt, NGX_QUIC_SR_TOKEN_LEN) != 0)
        {
            /*
             * ..a sequence number is used for different connection IDs,
             * the endpoint MAY treat that receipt as a connection error
             * of type PROTOCOL_VIOLATION.
             */
            qc->error = NGX_QUIC_ERR_PROTOCOL_VIOLATION;
            qc->error_reason = "seqnum refers to different connection id/token";
            return NGX_ERROR;
        }

    } else {

        cid = ngx_quic_alloc_connection_id(c, qc);
        if (cid == NULL) {
            return NGX_ERROR;
        }

        cid->seqnum = f->seqnum;
        cid->len = f->len;
        ngx_memcpy(cid->id, f->cid, f->len);

        ngx_memcpy(cid->sr_token, f->srt, NGX_QUIC_SR_TOKEN_LEN);

        ngx_queue_insert_tail(&qc->client_ids, &cid->queue);
        qc->nclient_ids++;

        /* always use latest available connection id */
        if (f->seqnum > qc->curr_seqnum) {
            qc->scid.len = cid->len;
            qc->scid.data = cid->id;
            qc->curr_seqnum = f->seqnum;
        }
    }

retire:

    if (qc->max_retired_seqnum && f->retire <= qc->max_retired_seqnum) {
        /*
         * Once a sender indicates a Retire Prior To value, smaller values sent
         * in subsequent NEW_CONNECTION_ID frames have no effect.  A receiver
         * MUST ignore any Retire Prior To fields that do not increase the
         * largest received Retire Prior To value.
         */
        goto done;
    }

    qc->max_retired_seqnum = f->retire;

    q = ngx_queue_head(&qc->client_ids);

    while (q != ngx_queue_sentinel(&qc->client_ids)) {

        cid = ngx_queue_data(q, ngx_quic_client_id_t, queue);
        q = ngx_queue_next(q);

        if (cid->seqnum >= f->retire) {
            continue;
        }

        /* this connection id must be retired */

        if (ngx_quic_retire_connection_id(c, pkt->level, cid->seqnum)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        ngx_queue_remove(&cid->queue);
        ngx_queue_insert_head(&qc->free_client_ids, &cid->queue);
        qc->nclient_ids--;
    }

done:

    if (qc->nclient_ids > qc->tp.active_connection_id_limit) {
        /*
         * After processing a NEW_CONNECTION_ID frame and
         * adding and retiring active connection IDs, if the number of active
         * connection IDs exceeds the value advertised in its
         * active_connection_id_limit transport parameter, an endpoint MUST
         * close the connection with an error of type CONNECTION_ID_LIMIT_ERROR.
         */
        qc->error = NGX_QUIC_ERR_CONNECTION_ID_LIMIT_ERROR;
        qc->error_reason = "too many connection ids received";
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_retire_connection_id(ngx_connection_t *c,
    enum ssl_encryption_level_t level, uint64_t seqnum)
{
    ngx_quic_frame_t  *frame;

    frame = ngx_quic_alloc_frame(c, 0);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = level;
    frame->type = NGX_QUIC_FT_RETIRE_CONNECTION_ID;
    frame->u.retire_cid.sequence_number = seqnum;

    ngx_quic_queue_frame(c->quic, frame);

    return NGX_OK;
}


static ngx_quic_client_id_t *
ngx_quic_alloc_connection_id(ngx_connection_t *c, ngx_quic_connection_t *qc)
{
    ngx_queue_t           *q;
    ngx_quic_client_id_t  *cid;

    if (!ngx_queue_empty(&qc->free_client_ids)) {

        q = ngx_queue_head(&qc->free_client_ids);
        cid = ngx_queue_data(q, ngx_quic_client_id_t, queue);

        ngx_queue_remove(&cid->queue);

        ngx_memzero(cid, sizeof(ngx_quic_client_id_t));

    } else {

        cid = ngx_pcalloc(c->pool, sizeof(ngx_quic_client_id_t));
        if (cid == NULL) {
            return NULL;
        }
    }

    return cid;
}


static void
ngx_quic_queue_frame(ngx_quic_connection_t *qc, ngx_quic_frame_t *frame)
{
    ngx_quic_send_ctx_t  *ctx;

    ctx = ngx_quic_get_send_ctx(qc, frame->level);

    ngx_queue_insert_tail(&ctx->frames, &frame->queue);

    frame->len = ngx_quic_create_frame(NULL, frame);
    /* always succeeds */

    if (qc->closing) {
        return;
    }

    ngx_post_event(&qc->push, &ngx_posted_events);
}


static ngx_int_t
ngx_quic_output(ngx_connection_t *c)
{
    ngx_uint_t              i;
    ngx_msec_t              delay;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    c->log->action = "sending frames";

    qc = c->quic;

    for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {

        ctx = &qc->send_ctx[i];

        if (ctx->send_ack) {

            if (ctx->level == ssl_encryption_application)  {

                delay = ngx_current_msec - ctx->ack_delay_start;

                if (ctx->send_ack < NGX_QUIC_MAX_ACK_GAP
                    && delay < qc->tp.max_ack_delay)
                {
                    if (!qc->push.timer_set && !qc->closing) {
                        ngx_add_timer(&qc->push, qc->tp.max_ack_delay - delay);
                    }

                    goto output;
                }
            }

            if (ngx_quic_send_ack(c, ctx) != NGX_OK) {
                return NGX_ERROR;
            }
            ctx->send_ack = 0;
        }

    output:

        if (ngx_quic_output_frames(c, ctx) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (!qc->send_timer_set && !qc->closing) {
        qc->send_timer_set = 1;
        ngx_add_timer(c->read, qc->tp.max_idle_timeout);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_output_frames(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx)
{
    size_t                  len, hlen, cutoff;
    ngx_uint_t              need_ack;
    ngx_queue_t            *q, range;
    ngx_quic_frame_t       *f;
    ngx_quic_congestion_t  *cg;
    ngx_quic_connection_t  *qc;

    qc = c->quic;
    cg = &qc->congestion;

    if (ngx_queue_empty(&ctx->frames)) {
        return NGX_OK;
    }

    q = ngx_queue_head(&ctx->frames);
    f = ngx_queue_data(q, ngx_quic_frame_t, queue);

    /* all frames in same send_ctx share same level */
    hlen = (f->level == ssl_encryption_application) ? NGX_QUIC_MAX_SHORT_HEADER
                                                    : NGX_QUIC_MAX_LONG_HEADER;
    hlen += EVP_GCM_TLS_TAG_LEN;
    hlen -= NGX_QUIC_MAX_CID_LEN - qc->scid.len;

    do {
        len = 0;
        need_ack = 0;
        ngx_queue_init(&range);

        do {
            /* process group of frames that fits into packet */
            f = ngx_queue_data(q, ngx_quic_frame_t, queue);

            if (len && hlen + len + f->len > qc->ctp.max_udp_payload_size) {
                break;
            }

            if (f->need_ack) {
                need_ack = 1;
            }

            if (need_ack && cg->in_flight + len + f->len > cg->window) {
                break;
            }

            if (!qc->validated) {
                /*
                 * Prior to validation, endpoints are limited in what they
                 * are able to send.  During the handshake, a server cannot
                 * send more than three times the data it receives;
                 */

                if (f->level == ssl_encryption_initial) {
                    cutoff = (c->sent + NGX_QUIC_MIN_INITIAL_SIZE) / 3;

                } else {
                    cutoff = (c->sent + hlen + len + f->len) / 3;
                }

                if (cutoff > qc->received) {
                    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                                   "quic hit amplification limit"
                                   " received:%uz sent:%O",
                                   qc->received, c->sent);
                    break;
                }
            }

            q = ngx_queue_next(q);

            f->first = ngx_current_msec;

            ngx_queue_remove(&f->queue);
            ngx_queue_insert_tail(&range, &f->queue);

            len += f->len;

        } while (q != ngx_queue_sentinel(&ctx->frames));

        if (ngx_queue_empty(&range)) {
            break;
        }

        if (ngx_quic_send_frames(c, ctx, &range) != NGX_OK) {
            return NGX_ERROR;
        }

    } while (q != ngx_queue_sentinel(&ctx->frames));

    return NGX_OK;
}


static void
ngx_quic_free_frames(ngx_connection_t *c, ngx_queue_t *frames)
{
    ngx_queue_t       *q;
    ngx_quic_frame_t  *f;

    do {
        q = ngx_queue_head(frames);

        if (q == ngx_queue_sentinel(frames)) {
            break;
        }

        ngx_queue_remove(q);

        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        ngx_quic_free_frame(c, f);
    } while (1);
}


static ngx_int_t
ngx_quic_send_frames(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    ngx_queue_t *frames)
{
    u_char                 *p;
    size_t                  pad_len;
    ssize_t                 len;
    ngx_str_t               out, res;
    ngx_msec_t              now;
    ngx_queue_t            *q;
    ngx_quic_frame_t       *f, *start;
    ngx_quic_header_t       pkt;
    ngx_quic_connection_t  *qc;
    static u_char           src[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];
    static u_char           dst[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic ngx_quic_send_frames");

    q = ngx_queue_head(frames);
    start = ngx_queue_data(q, ngx_quic_frame_t, queue);

    ngx_memzero(&pkt, sizeof(ngx_quic_header_t));

    now = ngx_current_msec;

    p = src;
    out.data = src;

    for (q = ngx_queue_head(frames);
         q != ngx_queue_sentinel(frames);
         q = ngx_queue_next(q))
    {
        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        ngx_quic_log_frame(c->log, f, 1);

        len = ngx_quic_create_frame(p, f);
        if (len == -1) {
            ngx_quic_free_frames(c, frames);
            return NGX_ERROR;
        }

        if (f->need_ack) {
            pkt.need_ack = 1;
        }

        p += len;
        f->pnum = ctx->pnum;
        f->last = now;
        f->plen = 0;
    }

    out.len = p - out.data;

    qc = c->quic;

    pkt.keys = qc->keys;

    pkt.flags = NGX_QUIC_PKT_FIXED_BIT;

    if (start->level == ssl_encryption_initial) {
        pkt.flags |= NGX_QUIC_PKT_LONG | NGX_QUIC_PKT_INITIAL;

    } else if (start->level == ssl_encryption_handshake) {
        pkt.flags |= NGX_QUIC_PKT_LONG | NGX_QUIC_PKT_HANDSHAKE;

    } else {
        if (c->quic->key_phase) {
            pkt.flags |= NGX_QUIC_PKT_KPHASE;
        }
    }

    ngx_quic_set_packet_number(&pkt, ctx);

    pkt.version = qc->version;
    pkt.log = c->log;
    pkt.level = start->level;
    pkt.dcid = qc->scid;
    pkt.scid = qc->dcid;

    if (start->level == ssl_encryption_initial && pkt.need_ack) {
        pad_len = NGX_QUIC_MIN_INITIAL_SIZE - EVP_GCM_TLS_TAG_LEN
                  - ngx_quic_create_long_header(&pkt, NULL, out.len, NULL);
        pad_len = ngx_min(pad_len, NGX_QUIC_MIN_INITIAL_SIZE);

    } else {
        pad_len = 4;
    }

    if (out.len < pad_len) {
        ngx_memset(p, NGX_QUIC_FT_PADDING, pad_len - out.len);
        out.len = pad_len;
    }

    pkt.payload = out;

    res.data = dst;

    ngx_log_debug6(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic packet tx %s bytes:%ui"
                   " need_ack:%d number:%L encoded nl:%d trunc:0x%xD",
                   ngx_quic_level_name(start->level), out.len, pkt.need_ack,
                   pkt.number, pkt.num_len, pkt.trunc);

    if (ngx_quic_encrypt(&pkt, &res) != NGX_OK) {
        ngx_quic_free_frames(c, frames);
        return NGX_ERROR;
    }

    len = c->send(c, res.data, res.len);
    if (len == NGX_ERROR || (size_t) len != res.len) {
        ngx_quic_free_frames(c, frames);
        return NGX_ERROR;
    }

    /* len == NGX_OK || NGX_AGAIN */
    ctx->pnum++;

    if (pkt.need_ack) {
        /* move frames into the sent queue to wait for ack */

        if (qc->closing) {
            /* if we are closing, any ack will be discarded */
            ngx_quic_free_frames(c, frames);

        } else {
            ngx_queue_add(&ctx->sent, frames);
            if (qc->pto.timer_set) {
                ngx_del_timer(&qc->pto);
            }
            ngx_add_timer(&qc->pto, ngx_quic_pto(c, ctx));

            start->plen = len;
        }

        qc->congestion.in_flight += len;

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion send if:%uz",
                       qc->congestion.in_flight);
    } else {
        /* no ack is expected for this frames, so we can free them */
        ngx_quic_free_frames(c, frames);
    }

    return NGX_OK;
}


static void
ngx_quic_set_packet_number(ngx_quic_header_t *pkt, ngx_quic_send_ctx_t *ctx)
{
    uint64_t  delta;

    delta = ctx->pnum - ctx->largest_ack;
    pkt->number = ctx->pnum;

    if (delta <= 0x7F) {
        pkt->num_len = 1;
        pkt->trunc = ctx->pnum & 0xff;

    } else if (delta <= 0x7FFF) {
        pkt->num_len = 2;
        pkt->flags |= 0x1;
        pkt->trunc = ctx->pnum & 0xffff;

    } else if (delta <= 0x7FFFFF) {
        pkt->num_len = 3;
        pkt->flags |= 0x2;
        pkt->trunc = ctx->pnum & 0xffffff;

    } else {
        pkt->num_len = 4;
        pkt->flags |= 0x3;
        pkt->trunc = ctx->pnum & 0xffffffff;
    }
}


static void
ngx_quic_pto_handler(ngx_event_t *ev)
{
    ngx_uint_t              i;
    ngx_queue_t            *q;
    ngx_connection_t       *c;
    ngx_quic_frame_t       *start;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "quic pto timer");

    c = ev->data;
    qc = c->quic;

    qc->pto_count++;

    for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {

        ctx = &qc->send_ctx[i];

        if (ngx_queue_empty(&ctx->sent)) {
            continue;
        }

        q = ngx_queue_head(&ctx->sent);
        start = ngx_queue_data(q, ngx_quic_frame_t, queue);

        if (start->pnum <= ctx->largest_ack
            && ctx->largest_ack != NGX_QUIC_UNSET_PN)
        {
            continue;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic pto pnum:%uL pto_count:%ui level:%d",
                       start->pnum, c->quic->pto_count, start->level);

        ngx_quic_resend_frames(c, ctx);
    }

    ngx_quic_connstate_dbg(c);
}


static void
ngx_quic_push_handler(ngx_event_t *ev)
{
    ngx_connection_t  *c;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "quic push timer");

    c = ev->data;

    if (ngx_quic_output(c) != NGX_OK) {
        ngx_quic_close_connection(c, NGX_ERROR);
        return;
    }

    ngx_quic_connstate_dbg(c);
}


static
void ngx_quic_lost_handler(ngx_event_t *ev)
{
    ngx_connection_t  *c;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "quic lost timer");

    c = ev->data;

    if (ngx_quic_detect_lost(c) != NGX_OK) {
        ngx_quic_close_connection(c, NGX_ERROR);
    }

    ngx_quic_connstate_dbg(c);
}


static ngx_int_t
ngx_quic_detect_lost(ngx_connection_t *c)
{
    ngx_uint_t              i;
    ngx_msec_t              now, wait, min_wait, thr;
    ngx_queue_t            *q;
    ngx_quic_frame_t       *start;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    qc = c->quic;
    now = ngx_current_msec;

    min_wait = 0;

    thr = NGX_QUIC_TIME_THR * ngx_max(qc->latest_rtt, qc->avg_rtt);
    thr = ngx_max(thr, NGX_QUIC_TIME_GRANULARITY);

    for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {

        ctx = &qc->send_ctx[i];

        if (ctx->largest_ack == NGX_QUIC_UNSET_PN) {
            continue;
        }

        while (!ngx_queue_empty(&ctx->sent)) {

            q = ngx_queue_head(&ctx->sent);
            start = ngx_queue_data(q, ngx_quic_frame_t, queue);

            if (start->pnum > ctx->largest_ack) {
                break;
            }

            wait = start->last + thr - now;

            ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic detect_lost pnum:%uL thr:%M wait:%i level:%d",
                           start->pnum, thr, (ngx_int_t) wait, start->level);

            if ((ngx_msec_int_t) wait > 0
                && ctx->largest_ack - start->pnum < NGX_QUIC_PKT_THR)
            {

                if (min_wait == 0 || wait < min_wait) {
                    min_wait = wait;
                }

                break;
            }

            ngx_quic_resend_frames(c, ctx);
        }
    }

    /* no more preceeding packets */

    if (min_wait == 0) {
        qc->pto.handler = ngx_quic_pto_handler;
        return NGX_OK;
    }

    qc->pto.handler = ngx_quic_lost_handler;

    if (qc->pto.timer_set) {
        ngx_del_timer(&qc->pto);
    }

    ngx_add_timer(&qc->pto, min_wait);

    return NGX_OK;
}


static void
ngx_quic_resend_frames(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx)
{
    size_t                  n;
    ngx_buf_t              *b;
    ngx_queue_t            *q;
    ngx_quic_frame_t       *f, *start;
    ngx_quic_stream_t      *sn;
    ngx_quic_connection_t  *qc;

    qc = c->quic;
    q = ngx_queue_head(&ctx->sent);
    start = ngx_queue_data(q, ngx_quic_frame_t, queue);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic resend packet pnum:%uL", start->pnum);

    ngx_quic_congestion_lost(c, start);

    do {
        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        if (f->pnum != start->pnum) {
            break;
        }

        q = ngx_queue_next(q);

        ngx_queue_remove(&f->queue);

        switch (f->type) {
        case NGX_QUIC_FT_ACK:
        case NGX_QUIC_FT_ACK_ECN:
            /* force generation of most recent acknowledgment */
            ctx->send_ack = NGX_QUIC_MAX_ACK_GAP;
            ngx_quic_free_frame(c, f);
            break;

        case NGX_QUIC_FT_PING:
        case NGX_QUIC_FT_PATH_RESPONSE:
        case NGX_QUIC_FT_CONNECTION_CLOSE:
            ngx_quic_free_frame(c, f);
            break;

        case NGX_QUIC_FT_MAX_DATA:
            f->u.max_data.max_data = qc->streams.recv_max_data;
            ngx_quic_queue_frame(qc, f);
            break;

        case NGX_QUIC_FT_MAX_STREAMS:
        case NGX_QUIC_FT_MAX_STREAMS2:
            f->u.max_streams.limit = f->u.max_streams.bidi
                                     ? qc->streams.client_max_streams_bidi
                                     : qc->streams.client_max_streams_uni;
            ngx_quic_queue_frame(qc, f);
            break;

        case NGX_QUIC_FT_MAX_STREAM_DATA:
            sn = ngx_quic_find_stream(&qc->streams.tree,
                                      f->u.max_stream_data.id);
            if (sn == NULL) {
                ngx_quic_free_frame(c, f);
                break;
            }

            b = sn->b;
            n = sn->fs.received + (b->pos - b->start) + (b->end - b->last);

            if (f->u.max_stream_data.limit < n) {
                f->u.max_stream_data.limit = n;
            }

            ngx_quic_queue_frame(qc, f);
            break;

        case NGX_QUIC_FT_STREAM0:
        case NGX_QUIC_FT_STREAM1:
        case NGX_QUIC_FT_STREAM2:
        case NGX_QUIC_FT_STREAM3:
        case NGX_QUIC_FT_STREAM4:
        case NGX_QUIC_FT_STREAM5:
        case NGX_QUIC_FT_STREAM6:
        case NGX_QUIC_FT_STREAM7:
            sn = ngx_quic_find_stream(&qc->streams.tree, f->u.stream.stream_id);

            if (sn && sn->c->write->error) {
                /* RESET_STREAM was sent */
                ngx_quic_free_frame(c, f);
                break;
            }

            /* fall through */

        default:
            ngx_queue_insert_tail(&ctx->frames, &f->queue);
        }

    } while (q != ngx_queue_sentinel(&ctx->sent));

    if (qc->closing) {
        return;
    }

    ngx_post_event(&qc->push, &ngx_posted_events);
}


ngx_connection_t *
ngx_quic_open_stream(ngx_connection_t *c, ngx_uint_t bidi)
{
    size_t                  rcvbuf_size;
    uint64_t                id;
    ngx_quic_stream_t      *qs, *sn;
    ngx_quic_connection_t  *qc;

    qs = c->qs;
    qc = qs->parent->quic;

    if (bidi) {
        if (qc->streams.server_streams_bidi
            >= qc->streams.server_max_streams_bidi)
        {
            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic too many server bidi streams:%uL",
                           qc->streams.server_streams_bidi);
            return NULL;
        }

        id = (qc->streams.server_streams_bidi << 2)
             | NGX_QUIC_STREAM_SERVER_INITIATED;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic creating server bidi stream"
                       " streams:%uL max:%uL id:0x%xL",
                       qc->streams.server_streams_bidi,
                       qc->streams.server_max_streams_bidi, id);

        qc->streams.server_streams_bidi++;
        rcvbuf_size = qc->tp.initial_max_stream_data_bidi_local;

    } else {
        if (qc->streams.server_streams_uni
            >= qc->streams.server_max_streams_uni)
        {
            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic too many server uni streams:%uL",
                           qc->streams.server_streams_uni);
            return NULL;
        }

        id = (qc->streams.server_streams_uni << 2)
             | NGX_QUIC_STREAM_SERVER_INITIATED
             | NGX_QUIC_STREAM_UNIDIRECTIONAL;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic creating server uni stream"
                       " streams:%uL max:%uL id:0x%xL",
                       qc->streams.server_streams_uni,
                       qc->streams.server_max_streams_uni, id);

        qc->streams.server_streams_uni++;
        rcvbuf_size = 0;
    }

    sn = ngx_quic_create_stream(qs->parent, id, rcvbuf_size);
    if (sn == NULL) {
        return NULL;
    }

    return sn->c;
}


static void
ngx_quic_rbtree_insert_stream(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t  **p;
    ngx_quic_stream_t   *qn, *qnt;

    for ( ;; ) {
        qn = (ngx_quic_stream_t *) node;
        qnt = (ngx_quic_stream_t *) temp;

        p = (qn->id < qnt->id) ? &temp->left : &temp->right;

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_quic_stream_t *
ngx_quic_find_stream(ngx_rbtree_t *rbtree, uint64_t id)
{
    ngx_rbtree_node_t  *node, *sentinel;
    ngx_quic_stream_t  *qn;

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {
        qn = (ngx_quic_stream_t *) node;

        if (id == qn->id) {
            return qn;
        }

        node = (id < qn->id) ? node->left : node->right;
    }

    return NULL;
}


static ngx_quic_stream_t *
ngx_quic_create_client_stream(ngx_connection_t *c, uint64_t id)
{
    size_t                  n;
    uint64_t                min_id;
    ngx_quic_stream_t      *sn;
    ngx_quic_connection_t  *qc;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic stream id:0x%xL is new", id);

    qc = c->quic;

    if (id & NGX_QUIC_STREAM_UNIDIRECTIONAL) {

        if (id & NGX_QUIC_STREAM_SERVER_INITIATED) {
            if ((id >> 2) < qc->streams.server_streams_uni) {
                return NGX_QUIC_STREAM_GONE;
            }

            qc->error = NGX_QUIC_ERR_STREAM_STATE_ERROR;
            return NULL;
        }

        if ((id >> 2) < qc->streams.client_streams_uni) {
            return NGX_QUIC_STREAM_GONE;
        }

        if ((id >> 2) >= qc->streams.client_max_streams_uni) {
            qc->error = NGX_QUIC_ERR_STREAM_LIMIT_ERROR;
            return NULL;
        }

        min_id = (qc->streams.client_streams_uni << 2)
                 | NGX_QUIC_STREAM_UNIDIRECTIONAL;
        qc->streams.client_streams_uni = (id >> 2) + 1;
        n = qc->tp.initial_max_stream_data_uni;

    } else {

        if (id & NGX_QUIC_STREAM_SERVER_INITIATED) {
            if ((id >> 2) < qc->streams.server_streams_bidi) {
                return NGX_QUIC_STREAM_GONE;
            }

            qc->error = NGX_QUIC_ERR_STREAM_STATE_ERROR;
            return NULL;
        }

        if ((id >> 2) < qc->streams.client_streams_bidi) {
            return NGX_QUIC_STREAM_GONE;
        }

        if ((id >> 2) >= qc->streams.client_max_streams_bidi) {
            qc->error = NGX_QUIC_ERR_STREAM_LIMIT_ERROR;
            return NULL;
        }

        min_id = (qc->streams.client_streams_bidi << 2);
        qc->streams.client_streams_bidi = (id >> 2) + 1;
        n = qc->tp.initial_max_stream_data_bidi_remote;
    }

    if (n < NGX_QUIC_STREAM_BUFSIZE) {
        n = NGX_QUIC_STREAM_BUFSIZE;
    }

    /*
     *   2.1.  Stream Types and Identifiers
     *
     *   Within each type, streams are created with numerically increasing
     *   stream IDs.  A stream ID that is used out of order results in all
     *   streams of that type with lower-numbered stream IDs also being
     *   opened.
     */

    for ( /* void */ ; min_id < id; min_id += 0x04) {

        sn = ngx_quic_create_stream(c, min_id, n);
        if (sn == NULL) {
            return NULL;
        }

        sn->c->listening->handler(sn->c);
    }

    return ngx_quic_create_stream(c, id, n);
}


static ngx_quic_stream_t *
ngx_quic_create_stream(ngx_connection_t *c, uint64_t id, size_t rcvbuf_size)
{
    ngx_log_t              *log;
    ngx_pool_t             *pool;
    ngx_quic_stream_t      *sn;
    ngx_pool_cleanup_t     *cln;
    ngx_quic_connection_t  *qc;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic stream id:0x%xL create", id);

    qc = c->quic;

    pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, c->log);
    if (pool == NULL) {
        return NULL;
    }

    sn = ngx_pcalloc(pool, sizeof(ngx_quic_stream_t));
    if (sn == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    sn->node.key = id;
    sn->parent = c;
    sn->id = id;

    sn->b = ngx_create_temp_buf(pool, rcvbuf_size);
    if (sn->b == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    ngx_queue_init(&sn->fs.frames);

    log = ngx_palloc(pool, sizeof(ngx_log_t));
    if (log == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    *log = *c->log;
    pool->log = log;

    sn->c = ngx_get_connection(-1, log);
    if (sn->c == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    sn->c->qs = sn;
    sn->c->type = SOCK_STREAM;
    sn->c->pool = pool;
    sn->c->ssl = c->ssl;
    sn->c->sockaddr = c->sockaddr;
    sn->c->listening = c->listening;
    sn->c->addr_text = c->addr_text;
    sn->c->local_sockaddr = c->local_sockaddr;
    sn->c->local_socklen = c->local_socklen;
    sn->c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);

    sn->c->recv = ngx_quic_stream_recv;
    sn->c->send = ngx_quic_stream_send;
    sn->c->send_chain = ngx_quic_stream_send_chain;

    sn->c->read->log = log;
    sn->c->write->log = log;

    log->connection = sn->c->number;

    if ((id & NGX_QUIC_STREAM_UNIDIRECTIONAL) == 0
        || (id & NGX_QUIC_STREAM_SERVER_INITIATED))
    {
        sn->c->write->ready = 1;
    }

    if (id & NGX_QUIC_STREAM_UNIDIRECTIONAL) {
        if (id & NGX_QUIC_STREAM_SERVER_INITIATED) {
            sn->send_max_data = qc->ctp.initial_max_stream_data_uni;
        }

    } else {
        if (id & NGX_QUIC_STREAM_SERVER_INITIATED) {
            sn->send_max_data = qc->ctp.initial_max_stream_data_bidi_remote;
        } else {
            sn->send_max_data = qc->ctp.initial_max_stream_data_bidi_local;
        }
    }

    cln = ngx_pool_cleanup_add(pool, 0);
    if (cln == NULL) {
        ngx_close_connection(sn->c);
        ngx_destroy_pool(pool);
        return NULL;
    }

    cln->handler = ngx_quic_stream_cleanup_handler;
    cln->data = sn->c;

    ngx_rbtree_insert(&c->quic->streams.tree, &sn->node);

    return sn;
}


static ssize_t
ngx_quic_stream_recv(ngx_connection_t *c, u_char *buf, size_t size)
{
    ssize_t                 len;
    ngx_buf_t              *b;
    ngx_event_t            *rev;
    ngx_connection_t       *pc;
    ngx_quic_frame_t       *frame;
    ngx_quic_stream_t      *qs;
    ngx_quic_connection_t  *qc;

    qs = c->qs;
    b = qs->b;
    pc = qs->parent;
    qc = pc->quic;
    rev = c->read;

    if (rev->error) {
        return NGX_ERROR;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic stream recv id:0x%xL eof:%d avail:%z",
                   qs->id, rev->pending_eof, b->last - b->pos);

    if (b->pos == b->last) {
        rev->ready = 0;

        if (rev->pending_eof) {
            rev->eof = 1;
            return 0;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic stream id:0x%xL recv() not ready", qs->id);
        return NGX_AGAIN;
    }

    len = ngx_min(b->last - b->pos, (ssize_t) size);

    ngx_memcpy(buf, b->pos, len);

    b->pos += len;
    qc->streams.received += len;

    if (b->pos == b->last) {
        b->pos = b->start;
        b->last = b->start;
        rev->ready = rev->pending_eof;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic stream id:0x%xL recv len:%z of size:%uz",
                   qs->id, len, size);

    if (!rev->pending_eof) {
        frame = ngx_quic_alloc_frame(pc, 0);
        if (frame == NULL) {
            return NGX_ERROR;
        }

        frame->level = ssl_encryption_application;
        frame->type = NGX_QUIC_FT_MAX_STREAM_DATA;
        frame->u.max_stream_data.id = qs->id;
        frame->u.max_stream_data.limit = qs->fs.received + (b->pos - b->start)
                                         + (b->end - b->last);

        ngx_quic_queue_frame(pc->quic, frame);
    }

    if ((qc->streams.recv_max_data / 2) < qc->streams.received) {

        frame = ngx_quic_alloc_frame(pc, 0);

        if (frame == NULL) {
            return NGX_ERROR;
        }

        qc->streams.recv_max_data *= 2;

        frame->level = ssl_encryption_application;
        frame->type = NGX_QUIC_FT_MAX_DATA;
        frame->u.max_data.max_data = qc->streams.recv_max_data;

        ngx_quic_queue_frame(pc->quic, frame);

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic stream id:0x%xL recv: increased max_data:%uL",
                       qs->id, qc->streams.recv_max_data);
    }

    return len;
}


static ssize_t
ngx_quic_stream_send(ngx_connection_t *c, u_char *buf, size_t size)
{
    ngx_buf_t    b;
    ngx_chain_t  cl;

    ngx_memzero(&b, sizeof(ngx_buf_t));

    b.memory = 1;
    b.pos = buf;
    b.last = buf + size;

    cl.buf = &b;
    cl.next = NULL;

    if (ngx_quic_stream_send_chain(c, &cl, 0) == NGX_CHAIN_ERROR) {
        return NGX_ERROR;
    }

    if (b.pos == buf) {
        return NGX_AGAIN;
    }

    return b.pos - buf;
}


static ngx_chain_t *
ngx_quic_stream_send_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    u_char                 *p;
    size_t                  n, max, max_frame, max_flow, max_limit, len;
#if (NGX_DEBUG)
    size_t                  sent;
#endif
    ngx_buf_t              *b;
#if (NGX_DEBUG)
    ngx_uint_t              nframes;
#endif
    ngx_event_t            *wev;
    ngx_chain_t            *cl;
    ngx_connection_t       *pc;
    ngx_quic_frame_t       *frame;
    ngx_quic_stream_t      *qs;
    ngx_quic_connection_t  *qc;

    qs = c->qs;
    pc = qs->parent;
    qc = pc->quic;
    wev = c->write;

    if (wev->error) {
        return NGX_CHAIN_ERROR;
    }

    max_frame = ngx_quic_max_stream_frame(qc);
    max_flow = ngx_quic_max_stream_flow(c);
    max_limit = limit;

#if (NGX_DEBUG)
    sent = 0;
    nframes = 0;
#endif

    for ( ;; ) {
        max = ngx_min(max_frame, max_flow);

        if (limit) {
            max = ngx_min(max, max_limit);
        }

        for (cl = in, n = 0; in; in = in->next) {

            if (!ngx_buf_in_memory(in->buf)) {
                continue;
            }

            n += ngx_buf_size(in->buf);

            if (n > max) {
                n = max;
                break;
            }
        }

        if (n == 0) {
            wev->ready = (max_flow ? 1 : 0);
            break;
        }

        frame = ngx_quic_alloc_frame(pc, n);
        if (frame == NULL) {
            return NGX_CHAIN_ERROR;
        }

        frame->level = ssl_encryption_application;
        frame->type = NGX_QUIC_FT_STREAM6; /* OFF=1 LEN=1 FIN=0 */
        frame->u.stream.off = 1;
        frame->u.stream.len = 1;
        frame->u.stream.fin = 0;

        frame->u.stream.type = frame->type;
        frame->u.stream.stream_id = qs->id;
        frame->u.stream.offset = c->sent;
        frame->u.stream.length = n;
        frame->u.stream.data = frame->data;

        c->sent += n;
        qc->streams.sent += n;
        max_flow -= n;

        if (limit) {
            max_limit -= n;
        }

#if (NGX_DEBUG)
        sent += n;
        nframes++;
#endif

        for (p = frame->data; n > 0; cl = cl->next) {
            b = cl->buf;

            if (!ngx_buf_in_memory(b)) {
                continue;
            }

            len = ngx_min(n, (size_t) (b->last - b->pos));
            p = ngx_cpymem(p, b->pos, len);

            b->pos += len;
            n -= len;
        }

        ngx_quic_queue_frame(qc, frame);
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic send_chain sent:%uz nframes:%ui", sent, nframes);

    return in;
}


static size_t
ngx_quic_max_stream_frame(ngx_quic_connection_t *qc)
{
    /*
     * we need to fit at least 1 frame into a packet, thus account head/tail;
     * 25 = 1 + 8x3 is max header for STREAM frame, with 1 byte for frame type
     */

    return qc->ctp.max_udp_payload_size - NGX_QUIC_MAX_SHORT_HEADER - 25
           - EVP_GCM_TLS_TAG_LEN;
}


static size_t
ngx_quic_max_stream_flow(ngx_connection_t *c)
{
    size_t                  size;
    uint64_t                sent, unacked;
    ngx_quic_stream_t      *qs;
    ngx_quic_connection_t  *qc;

    qs = c->qs;
    qc = qs->parent->quic;

    size = NGX_QUIC_STREAM_BUFSIZE;
    sent = c->sent;
    unacked = sent - qs->acked;

    if (qc->streams.send_max_data == 0) {
        qc->streams.send_max_data = qc->ctp.initial_max_data;
    }

    if (unacked >= NGX_QUIC_STREAM_BUFSIZE) {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic send flow hit buffer size");
        return 0;
    }

    if (unacked + size > NGX_QUIC_STREAM_BUFSIZE) {
        size = NGX_QUIC_STREAM_BUFSIZE - unacked;
    }

    if (qc->streams.sent >= qc->streams.send_max_data) {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic send flow hit MAX_DATA");
        return 0;
    }

    if (qc->streams.sent + size > qc->streams.send_max_data) {
        size = qc->streams.send_max_data - qc->streams.sent;
    }

    if (sent >= qs->send_max_data) {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic send flow hit MAX_STREAM_DATA");
        return 0;
    }

    if (sent + size > qs->send_max_data) {
        size = qs->send_max_data - sent;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic send flow:%uz", size);

    return size;
}


static void
ngx_quic_stream_cleanup_handler(void *data)
{
    ngx_connection_t *c = data;

    ngx_connection_t       *pc;
    ngx_quic_frame_t       *frame;
    ngx_quic_stream_t      *qs;
    ngx_quic_connection_t  *qc;

    qs = c->qs;
    pc = qs->parent;
    qc = pc->quic;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic stream id:0x%xL cleanup", qs->id);

    ngx_rbtree_delete(&qc->streams.tree, &qs->node);
    ngx_quic_free_frames(pc, &qs->fs.frames);

    if (qc->closing) {
        /* schedule handler call to continue ngx_quic_close_connection() */
        ngx_post_event(pc->read, &ngx_posted_events);
        return;
    }

    if ((qs->id & NGX_QUIC_STREAM_SERVER_INITIATED) == 0
        || (qs->id & NGX_QUIC_STREAM_UNIDIRECTIONAL) == 0)
    {
        if (!c->read->pending_eof && !c->read->error) {
            frame = ngx_quic_alloc_frame(pc, 0);
            if (frame == NULL) {
                return;
            }

            frame->level = ssl_encryption_application;
            frame->type = NGX_QUIC_FT_STOP_SENDING;
            frame->u.stop_sending.id = qs->id;
            frame->u.stop_sending.error_code = 0x100; /* HTTP/3 no error */

            ngx_quic_queue_frame(qc, frame);
        }
    }

    if ((qs->id & NGX_QUIC_STREAM_SERVER_INITIATED) == 0) {
        frame = ngx_quic_alloc_frame(pc, 0);
        if (frame == NULL) {
            return;
        }

        frame->level = ssl_encryption_application;
        frame->type = NGX_QUIC_FT_MAX_STREAMS;

        if (qs->id & NGX_QUIC_STREAM_UNIDIRECTIONAL) {
            frame->u.max_streams.limit = ++qc->streams.client_max_streams_uni;
            frame->u.max_streams.bidi = 0;

        } else {
            frame->u.max_streams.limit = ++qc->streams.client_max_streams_bidi;
            frame->u.max_streams.bidi = 1;
        }

        ngx_quic_queue_frame(qc, frame);

        if (qs->id & NGX_QUIC_STREAM_UNIDIRECTIONAL) {
            /* do not send fin for client unidirectional streams */
            return;
        }
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic stream id:0x%xL send fin", qs->id);

    frame = ngx_quic_alloc_frame(pc, 0);
    if (frame == NULL) {
        return;
    }

    frame->level = ssl_encryption_application;
    frame->type = NGX_QUIC_FT_STREAM7; /* OFF=1 LEN=1 FIN=1 */
    frame->u.stream.off = 1;
    frame->u.stream.len = 1;
    frame->u.stream.fin = 1;

    frame->u.stream.type = frame->type;
    frame->u.stream.stream_id = qs->id;
    frame->u.stream.offset = c->sent;
    frame->u.stream.length = 0;
    frame->u.stream.data = NULL;

    ngx_quic_queue_frame(qc, frame);

    (void) ngx_quic_output(pc);
}


static ngx_quic_frame_t *
ngx_quic_alloc_frame(ngx_connection_t *c, size_t size)
{
    u_char                 *p;
    ngx_queue_t            *q;
    ngx_quic_frame_t       *frame;
    ngx_quic_connection_t  *qc;

    if (size) {
        p = ngx_alloc(size, c->log);
        if (p == NULL) {
            return NULL;
        }

    } else {
        p = NULL;
    }

    qc = c->quic;

    if (!ngx_queue_empty(&qc->free_frames)) {

        q = ngx_queue_head(&qc->free_frames);
        frame = ngx_queue_data(q, ngx_quic_frame_t, queue);

        ngx_queue_remove(&frame->queue);

#ifdef NGX_QUIC_DEBUG_FRAMES_ALLOC
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic reuse frame n:%ui", qc->nframes);
#endif

    } else {
        frame = ngx_pcalloc(c->pool, sizeof(ngx_quic_frame_t));
        if (frame == NULL) {
            ngx_free(p);
            return NULL;
        }

#if (NGX_DEBUG)
        ++qc->nframes;
#endif

#ifdef NGX_QUIC_DEBUG_FRAMES_ALLOC
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic alloc frame n:%ui", qc->nframes);
#endif
    }

    ngx_memzero(frame, sizeof(ngx_quic_frame_t));

    frame->data = p;

    return frame;
}


static void
ngx_quic_congestion_ack(ngx_connection_t *c, ngx_quic_frame_t *f)
{
    ngx_msec_t              timer;
    ngx_quic_congestion_t  *cg;
    ngx_quic_connection_t  *qc;

    if (f->plen == 0) {
        return;
    }

    qc = c->quic;
    cg = &qc->congestion;

    cg->in_flight -= f->plen;

    timer = f->last - cg->recovery_start;

    if ((ngx_msec_int_t) timer <= 0) {
        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion ack recovery win:%uz ss:%z if:%uz",
                       cg->window, cg->ssthresh, cg->in_flight);

        return;
    }

    if (cg->window < cg->ssthresh) {
        cg->window += f->plen;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion slow start win:%uz ss:%z if:%uz",
                       cg->window, cg->ssthresh, cg->in_flight);

    } else {
        cg->window += qc->tp.max_udp_payload_size * f->plen / cg->window;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion avoidance win:%uz ss:%z if:%uz",
                       cg->window, cg->ssthresh, cg->in_flight);
    }

    /* prevent recovery_start from wrapping */

    timer = cg->recovery_start - ngx_current_msec + qc->tp.max_idle_timeout * 2;

    if ((ngx_msec_int_t) timer < 0) {
        cg->recovery_start = ngx_current_msec - qc->tp.max_idle_timeout * 2;
    }
}


static void
ngx_quic_congestion_lost(ngx_connection_t *c, ngx_quic_frame_t *f)
{
    ngx_msec_t              timer;
    ngx_quic_congestion_t  *cg;
    ngx_quic_connection_t  *qc;

    if (f->plen == 0) {
        return;
    }

    qc = c->quic;
    cg = &qc->congestion;

    cg->in_flight -= f->plen;
    f->plen = 0;

    timer = f->last - cg->recovery_start;

    if ((ngx_msec_int_t) timer <= 0) {
        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion lost recovery win:%uz ss:%z if:%uz",
                       cg->window, cg->ssthresh, cg->in_flight);

        return;
    }

    cg->recovery_start = ngx_current_msec;
    cg->window /= 2;

    if (cg->window < qc->tp.max_udp_payload_size * 2) {
        cg->window = qc->tp.max_udp_payload_size * 2;
    }

    cg->ssthresh = cg->window;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic congestion lost win:%uz ss:%z if:%uz",
                   cg->window, cg->ssthresh, cg->in_flight);
}


static void
ngx_quic_free_frame(ngx_connection_t *c, ngx_quic_frame_t *frame)
{
    ngx_quic_connection_t  *qc;

    qc = c->quic;

    if (frame->data) {
        ngx_free(frame->data);
        frame->data = NULL;
    }

    ngx_queue_insert_head(&qc->free_frames, &frame->queue);

#ifdef NGX_QUIC_DEBUG_FRAMES_ALLOC
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic free frame n:%ui", qc->nframes);
#endif
}


uint32_t
ngx_quic_version(ngx_connection_t *c)
{
    uint32_t  version;

    version = c->quic->version;

    return (version & 0xff000000) == 0xff000000 ? version & 0xff : version;
}
