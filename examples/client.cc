/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <cstdlib>
#include <cassert>
#include <cerrno>
#include <iostream>
#include <algorithm>
#include <memory>
#include <fstream>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/mman.h>

#include <openssl/bio.h>
#include <openssl/err.h>

#include <http-parser/http_parser.h>

#include <chrono>

#include "client.h"
#include "network.h"
#include "debug.h"
#include "util.h"
#include "shared.h"
#include "template.h"
#include "keylog.h"

using namespace ngtcp2;
using namespace std::literals;

namespace {
auto randgen = util::make_mt19937();
} // namespace

namespace {
Config config{};
} // namespace

Buffer::Buffer(const uint8_t *data, size_t datalen)
    : buf{data, data + datalen}, tail(buf.data() + datalen) {}
Buffer::Buffer(size_t datalen) : buf(datalen), tail(buf.data()) {}

Stream::Stream(const Request &req, int64_t stream_id)
    : req(req), stream_id(stream_id), fd(-1) {}

Stream::~Stream() {
  if (fd != -1) {
    close(fd);
  }
}
// -------------------------------------------------------------------------
// BBB video, SD = 1s
std::vector<int> minh_rate_set_1s = {47, 92, 135, 182, 226, 270, 353, 425, 538, 621, 808, 1100, 1300, 1700, 2200, 2600, 3300, 3800, 4200, 4700}; // BBB video, SD = 1s

// BBB video, SD = 2s
std::vector<int> minh_rate_set_2s = {46, 89, 131, 178, 222, 263, 334, 396, 522, 595, 791, 1000, 1200, 1500, 2100, 2500, 3100, 3500, 3800, 4200}; // BBB video, SD = 2s

// BBB video, SD = 4s
std::vector<int> minh_rate_set_4s = {45, 89, 129, 177, 218, 256, 323, 378, 509, 578, 783, 1000, 1200, 1500, 2100, 2400, 2900, 3300, 3600, 3900}; //BBB video, SD = 4s

// BBB video, SD = 6s
std::vector<int> minh_rate_set_6s = {46, 89, 128, 177, 218, 255, 321, 374, 506, 573, 780, 1000, 1200, 1500, 2100, 2400, 2900, 3300, 3600, 3900}; // BBB video, SD = 6s

int minh_seg_idx = 1;

// Minh: for clocks
auto minh_req_sent = std::chrono::high_resolution_clock::now();;
int  minh_download_time = 0;

// Minh: for parameters
const int minh_sd = 1000; //ms
const int minh_MAX_SEGMENT = 25;//596000/minh_sd + 1;

double  minh_last_thrp = 0;
double  minh_buffer = 0;

int     minh_seg_length = 0; // in Bytes

std::vector<double> minh_thrp_recorder;
std::vector<double> minh_buffer_recorder;
std::vector<int>    minh_bitrate_recorder;

// Minh: for ABR
enum ABR {AGG, SARA, BBA};
// -------------------------------------------------------------------------
int Stream::open_file(const std::string_view &path) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  assert(fd == -1);

  auto it = std::find(std::rbegin(path), std::rend(path), '/').base();
  if (it == std::end(path)) {
    std::cerr << "No file name found: " << path << std::endl;
    return -1;
  }
  auto b = std::string_view{it, static_cast<size_t>(std::end(path) - it)};
  if (b == ".." || b == ".") {
    std::cerr << "Invalid file name: " << b << std::endl;
    return -1;
  }

  auto fname = std::string{config.download};
  fname += '/';
  fname += b;

  fd = open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (fd == -1) {
    std::cerr << "open: Could not open file " << fname << ": "
              << strerror(errno) << std::endl;
    return -1;
  }

  return 0;
}

namespace {
int write_transport_params(const char *path,
                           const ngtcp2_transport_params *params) {
  auto f = std::ofstream(path);
  if (!f) {
    return -1;
  }

  f << "initial_max_streams_bidi=" << params->initial_max_streams_bidi << '\n'
    << "initial_max_streams_uni=" << params->initial_max_streams_uni << '\n'
    << "initial_max_stream_data_bidi_local="
    << params->initial_max_stream_data_bidi_local << '\n'
    << "initial_max_stream_data_bidi_remote="
    << params->initial_max_stream_data_bidi_remote << '\n'
    << "initial_max_stream_data_uni=" << params->initial_max_stream_data_uni
    << '\n'
    << "initial_max_data=" << params->initial_max_data << '\n';

  f.close();
  if (!f) {
    return -1;
  }

  return 0;
}
} // namespace

namespace {
int read_transport_params(const char *path, ngtcp2_transport_params *params) {
  auto f = std::ifstream(path);
  if (!f) {
    return -1;
  }

  for (std::string line; std::getline(f, line);) {
    if (util::istarts_with_l(line, "initial_max_streams_bidi=")) {
      params->initial_max_streams_bidi = strtoul(
          line.c_str() + str_size("initial_max_streams_bidi="), nullptr, 10);
    } else if (util::istarts_with_l(line, "initial_max_streams_uni=")) {
      params->initial_max_streams_uni = strtoul(
          line.c_str() + str_size("initial_max_streams_uni="), nullptr, 10);
    } else if (util::istarts_with_l(line,
                                    "initial_max_stream_data_bidi_local=")) {
      params->initial_max_stream_data_bidi_local = strtoul(
          line.c_str() + str_size("initial_max_stream_data_bidi_local="),
          nullptr, 10);
    } else if (util::istarts_with_l(line,
                                    "initial_max_stream_data_bidi_remote=")) {
      params->initial_max_stream_data_bidi_remote = strtoul(
          line.c_str() + str_size("initial_max_stream_data_bidi_remote="),
          nullptr, 10);
    } else if (util::istarts_with_l(line, "initial_max_stream_data_uni=")) {
      params->initial_max_stream_data_uni = strtoul(
          line.c_str() + str_size("initial_max_stream_data_uni="), nullptr, 10);
    } else if (util::istarts_with_l(line, "initial_max_data=")) {
      params->initial_max_data =
          strtoul(line.c_str() + str_size("initial_max_data="), nullptr, 10);
    }
  }

  return 0;
}
} // namespace

int Client::on_key(ngtcp2_crypto_level level, const uint8_t *rx_secret,
                   const uint8_t *tx_secret, size_t secretlen) {
  std::array<uint8_t, 64> rx_key, rx_iv, rx_hp_key, tx_key, tx_iv, tx_hp_key;

  if (level != NGTCP2_CRYPTO_LEVEL_EARLY &&
      ngtcp2_crypto_derive_and_install_rx_key(
          conn_, ssl_, rx_key.data(), rx_iv.data(), rx_hp_key.data(), level,
          rx_secret, secretlen) != 0) {
    return -1;
  }
  if (ngtcp2_crypto_derive_and_install_tx_key(
          conn_, ssl_, tx_key.data(), tx_iv.data(), tx_hp_key.data(), level,
          tx_secret, secretlen) != 0) {
    return -1;
  }

  auto crypto_ctx = ngtcp2_conn_get_crypto_ctx(conn_);
  auto aead = &crypto_ctx->aead;
  auto keylen = ngtcp2_crypto_aead_keylen(aead);
  auto ivlen = ngtcp2_crypto_packet_protection_ivlen(aead);

  const char *title = nullptr;
  switch (level) {
  case NGTCP2_CRYPTO_LEVEL_EARLY:
    title = "early_traffic";
    keylog::log_secret(ssl_, keylog::QUIC_CLIENT_EARLY_TRAFFIC_SECRET,
                       tx_secret, secretlen);
    break;
  case NGTCP2_CRYPTO_LEVEL_HANDSHAKE:
    title = "handshake_traffic";
    keylog::log_secret(ssl_, keylog::QUIC_SERVER_HANDSHAKE_TRAFFIC_SECRET,
                       rx_secret, secretlen);
    keylog::log_secret(ssl_, keylog::QUIC_CLIENT_HANDSHAKE_TRAFFIC_SECRET,
                       tx_secret, secretlen);
    break;
  case NGTCP2_CRYPTO_LEVEL_APP:
    title = "application_traffic";
    keylog::log_secret(ssl_, keylog::QUIC_SERVER_TRAFFIC_SECRET_0, rx_secret,
                       secretlen);
    keylog::log_secret(ssl_, keylog::QUIC_CLIENT_TRAFFIC_SECRET_0, tx_secret,
                       secretlen);
    break;
  default:
    assert(0);
  }

  if (!config.quiet && config.show_secret) {
    if (rx_secret) {
      std::cerr << title << " rx secret" << std::endl;
      debug::print_secrets(rx_secret, secretlen, rx_key.data(), keylen,
                           rx_iv.data(), ivlen, rx_hp_key.data(), keylen);
    }
    std::cerr << title << " tx secret" << std::endl;
    debug::print_secrets(tx_secret, secretlen, tx_key.data(), keylen,
                         tx_iv.data(), ivlen, tx_hp_key.data(), keylen);
  }

  if (level == NGTCP2_CRYPTO_LEVEL_APP) {
    if (config.tp_file) {
      ngtcp2_transport_params params;

      ngtcp2_conn_get_remote_transport_params(conn_, &params);

      if (write_transport_params(config.tp_file, &params) != 0) {
        std::cerr << "Could not write transport parameters in "
                  << config.tp_file << std::endl;
      }
    }

    if (setup_httpconn() != 0) {
      return -1;
    }
  }

  return 0;
}

namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  ev_io_stop(loop, w);

  auto c = static_cast<Client *>(w->data);

  auto rv = c->on_write();
  switch (rv) {
  case 0:
    return;
  case NETWORK_ERR_SEND_BLOCKED:
    c->start_wev();
    return;
  }
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (c->on_read() != 0) {
    return;
  }
  auto rv = c->on_write();
  switch (rv) {
  case 0:
    return;
  case NETWORK_ERR_SEND_BLOCKED:
    c->start_wev();
    return;
  }
}
} // namespace

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (!config.quiet) {
    std::cerr << "Timeout" << std::endl;
  }

  c->disconnect();
}
} // namespace

namespace {
void retransmitcb(struct ev_loop *loop, ev_timer *w, int revents) {
  int rv;
  auto c = static_cast<Client *>(w->data);

  rv = c->handle_expiry();
  if (rv != 0) {
    goto fail;
  }

  rv = c->on_write();
  if (rv != 0) {
    goto fail;
  }

  return;

fail:
  switch (rv) {
  case NETWORK_ERR_SEND_BLOCKED:
    c->start_wev();
    return;
  default:
    c->disconnect();
    return;
  }
}
} // namespace

namespace {
void change_local_addrcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  c->change_local_addr();
}
} // namespace

namespace {
void key_updatecb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (c->initiate_key_update() != 0) {
    c->disconnect();
  }
}
} // namespace

namespace {
void delay_streamcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  ev_timer_stop(loop, w);
  c->on_extend_max_streams();

  auto rv = c->on_write();
  switch (rv) {
  case 0:
    return;
  case NETWORK_ERR_SEND_BLOCKED:
    c->start_wev();
    return;
  }
}
} // namespace

namespace {
void siginthandler(struct ev_loop *loop, ev_signal *w, int revents) {
  ev_break(loop, EVBREAK_ALL);
}
} // namespace

Client::Client(struct ev_loop *loop, SSL_CTX *ssl_ctx)
    : local_addr_{},
      remote_addr_{},
      max_pktlen_(0),
      loop_(loop),
      ssl_ctx_(ssl_ctx),
      ssl_(nullptr),
      fd_(-1),
      crypto_{},
      qlog_(nullptr),
      conn_(nullptr),
      httpconn_(nullptr),
      addr_(nullptr),
      port_(nullptr),
      last_error_{QUICErrorType::Transport, 0},
      sendbuf_{NGTCP2_MAX_PKTLEN_IPV4},
      nstreams_done_(0),
      nkey_update_(0),
      version_(0),
      early_data_(false),
      should_exit_(false) {
  ev_io_init(&wev_, writecb, 0, EV_WRITE);
  ev_io_init(&rev_, readcb, 0, EV_READ);
  wev_.data = this;
  rev_.data = this;
  ev_timer_init(&timer_, timeoutcb, 0.,
                static_cast<double>(config.timeout) / NGTCP2_SECONDS);
  timer_.data = this;
  ev_timer_init(&rttimer_, retransmitcb, 0., 0.);
  rttimer_.data = this;
  ev_timer_init(&change_local_addr_timer_, change_local_addrcb,
                static_cast<double>(config.change_local_addr) / NGTCP2_SECONDS,
                0.);
  change_local_addr_timer_.data = this;
  ev_timer_init(&key_update_timer_, key_updatecb,
                static_cast<double>(config.key_update) / NGTCP2_SECONDS, 0.);
  key_update_timer_.data = this;
  ev_timer_init(&delay_stream_timer_, delay_streamcb,
                static_cast<double>(config.delay_stream) / NGTCP2_SECONDS, 0.);
  delay_stream_timer_.data = this;
  ev_signal_init(&sigintev_, siginthandler, SIGINT);
}

Client::~Client() {
  disconnect();
  close();
}

void Client::disconnect() {
  handle_error();

  config.tx_loss_prob = 0;

  ev_timer_stop(loop_, &delay_stream_timer_);
  ev_timer_stop(loop_, &key_update_timer_);
  ev_timer_stop(loop_, &change_local_addr_timer_);
  ev_timer_stop(loop_, &rttimer_);
  ev_timer_stop(loop_, &timer_);

  ev_io_stop(loop_, &rev_);

  ev_signal_stop(loop_, &sigintev_);
}

void Client::close() {
  ev_io_stop(loop_, &wev_);

  if (httpconn_) {
    nghttp3_conn_del(httpconn_);
    httpconn_ = nullptr;
  }

  if (conn_) {
    ngtcp2_conn_del(conn_);
    conn_ = nullptr;
  }

  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }

  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }

  if (qlog_) {
    fclose(qlog_);
    qlog_ = nullptr;
  }
}

namespace {
int client_initial(ngtcp2_conn *conn, void *user_data) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  auto c = static_cast<Client *>(user_data);

  if (c->recv_crypto_data(NGTCP2_CRYPTO_LEVEL_INITIAL, nullptr, 0) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int recv_crypto_data(ngtcp2_conn *conn, ngtcp2_crypto_level crypto_level,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data) {
  if (!config.quiet && !config.no_quic_dump) {
    debug::print_crypto_data(crypto_level, data, datalen);
  }

  auto c = static_cast<Client *>(user_data);

  if (c->recv_crypto_data(crypto_level, data, datalen) != 0) {
    if (auto err = ngtcp2_conn_get_tls_error(conn); err) {
      return err;
    }
    return NGTCP2_ERR_CRYPTO;
  }

  return 0;
}
} // namespace

namespace {
int recv_stream_data(ngtcp2_conn *conn, int64_t stream_id, int fin,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data, void *stream_user_data) {
  if (!config.quiet && !config.no_quic_dump) {
    debug::print_stream_data(stream_id, data, datalen);
  }

  auto c = static_cast<Client *>(user_data);

  if (c->recv_stream_data(stream_id, fin, data, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int acked_crypto_offset(ngtcp2_conn *conn, ngtcp2_crypto_level crypto_level,
                        uint64_t offset, size_t datalen, void *user_data) {
  auto c = static_cast<Client *>(user_data);
  c->remove_tx_crypto_data(crypto_level, offset, datalen);

  return 0;
}
} // namespace

namespace {
int acked_stream_data_offset(ngtcp2_conn *conn, int64_t stream_id,
                             uint64_t offset, size_t datalen, void *user_data,
                             void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->acked_stream_data_offset(stream_id, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

namespace {
int handshake_completed(ngtcp2_conn *conn, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (!config.quiet) {
    debug::handshake_completed(conn, user_data);
  }

  if (c->handshake_completed() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Client::handshake_completed() {
  if (!config.quiet) {
    // SSL_get_early_data_status works after handshake completes.
    if (early_data_ &&
        SSL_get_early_data_status(ssl_) != SSL_EARLY_DATA_ACCEPTED) {
      std::cerr << "Early data was rejected by server" << std::endl;

      if (auto rv = ngtcp2_conn_early_data_rejected(conn_); rv != 0) {
        std::cerr << "ngtcp2_conn_early_data_rejected: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
      }
    }

    std::cerr << "Negotiated cipher suite is " << SSL_get_cipher_name(ssl_)
              << std::endl;

    const unsigned char *alpn = nullptr;
    unsigned int alpnlen;

    SSL_get0_alpn_selected(ssl_, &alpn, &alpnlen);
    if (alpn) {
      std::cerr << "Negotiated ALPN is ";
      std::cerr.write(reinterpret_cast<const char *>(alpn), alpnlen);
      std::cerr << std::endl;
    }
  }

  if (config.change_local_addr) {
    start_change_local_addr_timer();
  }
  if (config.key_update) {
    start_key_update_timer();
  }
  if (config.delay_stream) {
    start_delay_stream_timer();
  }

  return 0;
}

namespace {
int recv_retry(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd,
               const ngtcp2_pkt_retry *retry, void *user_data) {
  // Re-generate handshake secrets here because connection ID might
  // change.
  auto c = static_cast<Client *>(user_data);

  c->on_recv_retry();

  return 0;
}
} // namespace

// MINH 18.02.20 [implement ABR] ADD-S
void Client::minh_req_rate(int m_bitrate, int m_seg_idx){
  if (nstreams_done_ >= config.nstreams) {
      std::cout << "\t nstreams_done_ > config.nstreams. RETURN"<<  std::endl;    
    return;
  }

  int64_t stream_id;// = 2*nstreams_done_;  

  if (auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr); // assign stream_id value ???
      rv != 0) {
    std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
    assert(NGTCP2_ERR_STREAM_ID_BLOCKED == rv);
    return;
  }
  config.requests[0].path = "/minh_videos/BBB/1000ms/"+
                            std::to_string(m_bitrate)+
                            "/BigBuckBunny_1s"+
                            std::to_string(m_seg_idx) + ".m4s";
  auto stream = std::make_unique<Stream>(
      config.requests[0], stream_id);

  std::cout << "\n\t[MINH] Send a request for seg: " << m_seg_idx
            << " with bitrate: " << m_bitrate
            << " in stream_id: " << std::hex << stream_id << std::dec << std::endl;
  std::cout << "\tRequests[0].path: " << config.requests[0].path << std::endl;

  minh_req_sent = std::chrono::high_resolution_clock::now();
  if (submit_http_request(stream.get()) != 0) {
    std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__  << "Submit request ERROR " << std::endl;
    return;
  }

  if (!config.download.empty()) {
    stream->open_file(stream->req.path);
  }
  streams_.emplace(stream_id, std::move(stream));

  ++nstreams_done_;
}
// MINH 18.02.20 [implement ABR] ADD-E

// MINH 19.02.20 [parameter measurement] ADD-S

// MINH 19.02.20 [parameter measurement] ADD-E
// 
namespace {
int stream_close(ngtcp2_conn *conn, int64_t stream_id, uint64_t app_error_code,
                 void *user_data, void *stream_user_data) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  std::cout << "-------------------------------------------------------------------------" << '\n'
            << "--------------------------- CLOSE STREAM " << stream_id << " -----------------------------" << '\n'
            << "-------------------------------------------------------------------------" << std::endl;
  auto c = static_cast<Client *>(user_data);

  if (c->on_stream_close(stream_id, app_error_code) != 0) { // Minh to check if there's any error in stream
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
// MINH 18.02.20 [implement ABR] ADD-S
  // 2. invoke para_measurement
  minh_download_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::high_resolution_clock::now() - minh_req_sent).count();
  minh_last_thrp = minh_seg_length*7.8125/minh_download_time;// 7.8125 = 8/1024*1000

  std::cout << "\n\t[MINH] INFO: datalen: " << minh_seg_length*8/1024 << " Kbps" << std::endl;
  std::cout << "\tdownload time: " << minh_download_time << "(ms)\t" 
            << "throughput: " << minh_last_thrp << "Kbps" <<  std::endl;  
  // 1. test-S
  std::cout << "\tconfig.nstreams: " << config.nstreams << std::endl;;
  if (minh_seg_idx <= minh_MAX_SEGMENT){
    ++config.nstreams;
    minh_seg_idx ++;
    c->minh_req_rate(4700, minh_seg_idx);
  }
  // 1. test-E


  // 3. invoke ABR

// MINH 18.02.20 [implement ABR] ADD-E
  return 0;
}
} // namespace

namespace {
int stream_reset(ngtcp2_conn *conn, int64_t stream_id, uint64_t final_size,
                 uint64_t app_error_code, void *user_data,
                 void *stream_user_data) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  auto c = static_cast<Client *>(user_data);

  if (c->on_stream_reset(stream_id) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int extend_max_streams_bidi(ngtcp2_conn *conn, uint64_t max_streams,
                            void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->on_extend_max_streams() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int rand(ngtcp2_conn *conn, uint8_t *dest, size_t destlen, ngtcp2_rand_ctx ctx,
         void *user_data) {
  auto dis = std::uniform_int_distribution<uint8_t>(0, 255);
  std::generate(dest, dest + destlen, [&dis]() { return dis(randgen); });
  return 0;
}
} // namespace

namespace {
int get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
                          size_t cidlen, void *user_data) {
  auto dis = std::uniform_int_distribution<uint8_t>(0, 255);
  auto f = [&dis]() { return dis(randgen); };

  std::generate_n(cid->data, cidlen, f);
  cid->datalen = cidlen;
  auto md = ngtcp2_crypto_md{const_cast<EVP_MD *>(EVP_sha256())};
  if (ngtcp2_crypto_generate_stateless_reset_token(
          token, &md, config.static_secret.data(), config.static_secret.size(),
          cid) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int remove_connection_id(ngtcp2_conn *conn, const ngtcp2_cid *cid,
                         void *user_data) {
  return 0;
}
} // namespace

namespace {
int do_hp_mask(uint8_t *dest, const ngtcp2_crypto_cipher *hp,
               const uint8_t *hp_key, const uint8_t *sample) {
  if (ngtcp2_crypto_hp_mask(dest, hp, hp_key, sample) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  if (!config.quiet && config.show_secret) {
    debug::print_hp_mask(dest, NGTCP2_HP_MASKLEN, sample, NGTCP2_HP_SAMPLELEN);
  }

  return 0;
}
} // namespace

namespace {
int update_key(ngtcp2_conn *conn, uint8_t *rx_secret, uint8_t *tx_secret,
               uint8_t *rx_key, uint8_t *rx_iv, uint8_t *tx_key, uint8_t *tx_iv,
               const uint8_t *current_rx_secret,
               const uint8_t *current_tx_secret, size_t secretlen,
               void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->update_key(rx_secret, tx_secret, rx_key, rx_iv, tx_key, tx_iv,
                    current_rx_secret, current_tx_secret, secretlen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int path_validation(ngtcp2_conn *conn, const ngtcp2_path *path,
                    ngtcp2_path_validation_result res, void *user_data) {
  if (!config.quiet) {
    debug::path_validation(path, res);
  }
  return 0;
}
} // namespace

namespace {
int select_preferred_address(ngtcp2_conn *conn, ngtcp2_addr *dest,
                             const ngtcp2_preferred_addr *paddr,
                             void *user_data) {
  auto c = static_cast<Client *>(user_data);
  Address addr;

  if (config.no_preferred_addr) {
    return 0;
  }

  if (c->select_preferred_address(addr, paddr) != 0) {
    dest->addrlen = 0;
    return 0;
  }

  dest->addrlen = addr.len;
  memcpy(dest->addr, &addr.su, dest->addrlen);

  return 0;
}
} // namespace

namespace {
int extend_max_stream_data(ngtcp2_conn *conn, int64_t stream_id,
                           uint64_t max_data, void *user_data,
                           void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->extend_max_stream_data(stream_id, max_data) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::extend_max_stream_data(int64_t stream_id, uint64_t max_data) {
  if (auto rv = nghttp3_conn_unblock_stream(httpconn_, stream_id); rv != 0) {
    std::cerr << "nghttp3_conn_unblock_stream: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }
  return 0;
}

int Client::init_ssl() {
  if (ssl_) {
    SSL_free(ssl_);
  }

  ssl_ = SSL_new(ssl_ctx_);
  SSL_set_app_data(ssl_, this);
  SSL_set_connect_state(ssl_);

  const uint8_t *alpn = nullptr;
  size_t alpnlen;

  switch (version_) {
  case NGTCP2_PROTO_VER:
    alpn = reinterpret_cast<const uint8_t *>(NGTCP2_ALPN_H3);
    alpnlen = str_size(NGTCP2_ALPN_H3);
    break;
  }
  if (alpn) {
    SSL_set_alpn_protos(ssl_, alpn, alpnlen);
  }

  if (util::numeric_host(addr_)) {
    // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
    // If remote host is numeric address, just send "localhost" as SNI
    // for now.
    SSL_set_tlsext_host_name(ssl_, "localhost");
  } else {
    // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
    SSL_set_tlsext_host_name(ssl_, addr_);
  }

  ngtcp2_transport_params params;
  ngtcp2_conn_get_local_transport_params(conn_, &params);

  std::array<uint8_t, 64> buf;

  auto nwrite = ngtcp2_encode_transport_params(
      buf.data(), buf.size(), NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO,
      &params);
  if (nwrite < 0) {
    std::cerr << "ngtcp2_encode_transport_params: " << ngtcp2_strerror(nwrite)
              << std::endl;
    return -1;
  }

  if (SSL_set_quic_transport_params(ssl_, buf.data(), nwrite) != 1) {
    std::cerr << "SSL_set_quic_transport_params failed" << std::endl;
    return -1;
  }

  if (config.session_file) {
    auto f = BIO_new_file(config.session_file, "r");
    if (f == nullptr) {
      std::cerr << "Could not read TLS session file " << config.session_file
                << std::endl;
    } else {
      auto session = PEM_read_bio_SSL_SESSION(f, nullptr, 0, nullptr);
      BIO_free(f);
      if (session == nullptr) {
        std::cerr << "Could not read TLS session file " << config.session_file
                  << std::endl;
      } else {
        if (!SSL_set_session(ssl_, session)) {
          std::cerr << "Could not set session" << std::endl;
        } else if (!config.disable_early_data &&
                   SSL_SESSION_get_max_early_data(session)) {
          early_data_ = true;
          SSL_set_quic_early_data_enabled(ssl_, 1);
        }
        SSL_SESSION_free(session);
      }
    }
  }

  return 0;
}

namespace {
void write_qlog(void *user_data, const void *data, size_t datalen) {
  auto c = static_cast<Client *>(user_data);
  c->write_qlog(data, datalen);
}
} // namespace

void Client::write_qlog(const void *data, size_t datalen) {
  assert(qlog_);
  fwrite(data, 1, datalen, qlog_);
}

int Client::init(int fd, const Address &local_addr, const Address &remote_addr,
                 const char *addr, const char *port, uint32_t version) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  local_addr_ = local_addr;
  remote_addr_ = remote_addr;
  fd_ = fd;
  addr_ = addr;
  port_ = port;
  version_ = version;

  switch (remote_addr_.su.storage.ss_family) {
  case AF_INET:
    max_pktlen_ = NGTCP2_MAX_PKTLEN_IPV4;
    break;
  case AF_INET6:
    max_pktlen_ = NGTCP2_MAX_PKTLEN_IPV6;
    break;
  default:
    return -1;
  }

  auto callbacks = ngtcp2_conn_callbacks{
      client_initial,
      nullptr, // recv_client_initial
      ::recv_crypto_data,
      ::handshake_completed,
      nullptr, // recv_version_negotiation
      ngtcp2_crypto_encrypt_cb,
      ngtcp2_crypto_decrypt_cb,
      do_hp_mask,
      ::recv_stream_data,
      acked_crypto_offset,
      ::acked_stream_data_offset,
      nullptr, // stream_open
      stream_close,
      nullptr, // recv_stateless_reset
      recv_retry,
      extend_max_streams_bidi,
      nullptr, // extend_max_streams_uni
      rand,
      get_new_connection_id,
      remove_connection_id,
      ::update_key,
      path_validation,
      ::select_preferred_address,
      stream_reset,
      nullptr, // extend_max_remote_streams_bidi,
      nullptr, // extend_max_remote_streams_uni,
      ::extend_max_stream_data,
  };

  auto dis = std::uniform_int_distribution<uint8_t>(
      0, std::numeric_limits<uint8_t>::max());
  auto generate_cid = [&dis](ngtcp2_cid &cid, size_t len) {
    cid.datalen = len;
    std::generate(std::begin(cid.data), std::begin(cid.data) + cid.datalen,
                  [&dis]() { return dis(randgen); });
  };

  ngtcp2_cid scid, dcid;
  generate_cid(scid, 17);
  if (config.dcid.datalen == 0) {
    generate_cid(dcid, 18);
  } else {
    dcid = config.dcid;
  }

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.log_printf = config.quiet ? nullptr : debug::log_printf;
  if (config.qlog_file) {
    qlog_ = fopen(config.qlog_file, "w");
    if (qlog_ == nullptr) {
      std::cerr << "Could not open qlog file " << config.qlog_file << ": "
                << strerror(errno) << std::endl;
      return -1;
    }
    settings.qlog.write = ::write_qlog;
    settings.qlog.odcid = dcid;
  }
  settings.initial_ts = util::timestamp(loop_);
  auto &params = settings.transport_params;
  params.initial_max_stream_data_bidi_local = config.max_stream_data_bidi_local;
  params.initial_max_stream_data_bidi_remote =
      config.max_stream_data_bidi_remote;
  params.initial_max_stream_data_uni = config.max_stream_data_uni;
  params.initial_max_data = config.max_data;
  params.initial_max_streams_bidi = config.max_streams_bidi;
  params.initial_max_streams_uni = config.max_streams_uni;
  params.max_idle_timeout = config.timeout;
  params.active_connection_id_limit = 7;

  auto path = ngtcp2_path{
      {local_addr.len, const_cast<uint8_t *>(
                           reinterpret_cast<const uint8_t *>(&local_addr.su))},
      {remote_addr.len, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(
                            &remote_addr.su))}};
  if (auto rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path, version,
                                       &callbacks, &settings, nullptr, this);
      rv != 0) {
    std::cerr << "ngtcp2_conn_client_new: " << ngtcp2_strerror(rv) << std::endl;
    return -1;
  }

  if (init_ssl() != 0) {
    return -1;
  }

  if (setup_initial_crypto_context() != 0) {
    return -1;
  }

  if (early_data_ && config.tp_file) {
    ngtcp2_transport_params params;
    if (read_transport_params(config.tp_file, &params) != 0) {
      std::cerr << "Could not read transport parameters from " << config.tp_file
                << std::endl;
      early_data_ = false;
    } else {
      ngtcp2_conn_set_early_remote_transport_params(conn_, &params);
      make_stream_early();
    }
  }

  ev_io_set(&wev_, fd_, EV_WRITE);
  ev_io_set(&rev_, fd_, EV_READ);

  ev_io_start(loop_, &rev_);
  ev_timer_again(loop_, &timer_);

  ev_signal_start(loop_, &sigintev_);

  return 0;
}

int Client::setup_initial_crypto_context() {
  std::array<uint8_t, NGTCP2_CRYPTO_INITIAL_SECRETLEN> initial_secret,
      rx_secret, tx_secret;
  std::array<uint8_t, NGTCP2_CRYPTO_INITIAL_KEYLEN> rx_key, rx_hp_key, tx_key,
      tx_hp_key;
  std::array<uint8_t, NGTCP2_CRYPTO_INITIAL_IVLEN> rx_iv, tx_iv;

  auto dcid = ngtcp2_conn_get_dcid(conn_);

  if (ngtcp2_crypto_derive_and_install_initial_key(
          conn_, rx_secret.data(), tx_secret.data(), initial_secret.data(),
          rx_key.data(), rx_iv.data(), rx_hp_key.data(), tx_key.data(),
          tx_iv.data(), tx_hp_key.data(), dcid) != 0) {
    std::cerr << "ngtcp2_crypto_derive_and_install_initial_key() failed"
              << std::endl;
    return -1;
  }

  if (!config.quiet && config.show_secret) {
    debug::print_initial_secret(initial_secret.data(), initial_secret.size());

    std::cerr << "initial rx secret" << std::endl;
    debug::print_secrets(rx_secret.data(), rx_secret.size(), rx_key.data(),
                         rx_key.size(), rx_iv.data(), rx_iv.size(),
                         rx_hp_key.data(), rx_hp_key.size());
    std::cerr << "initial tx secret" << std::endl;
    debug::print_secrets(tx_secret.data(), tx_secret.size(), tx_key.data(),
                         tx_key.size(), tx_iv.data(), tx_iv.size(),
                         tx_hp_key.data(), tx_hp_key.size());
  }

  return 0;
}

int Client::feed_data(const sockaddr *sa, socklen_t salen, uint8_t *data,
                      size_t datalen) {
  auto path = ngtcp2_path{
      {local_addr_.len, reinterpret_cast<uint8_t *>(&local_addr_.su)},
      {salen, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(sa))}};
  if (auto rv = ngtcp2_conn_read_pkt(conn_, &path, data, datalen,
                                     util::timestamp(loop_));
      rv != 0) {
    std::cerr << "ngtcp2_conn_read_pkt: " << ngtcp2_strerror(rv) << std::endl;
    switch (rv) {
    case NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM:
    case NGTCP2_ERR_MALFORMED_TRANSPORT_PARAM:
    case NGTCP2_ERR_TRANSPORT_PARAM:
      // If rv indicates transport_parameters related error, we should
      // send TRANSPORT_PARAMETER_ERROR even if last_error_.code is
      // already set.  This is because OpenSSL might set Alert.
      last_error_ = quic_err_transport(rv);
      break;
    default:
      if (!last_error_.code) {
        last_error_ = quic_err_transport(rv);
      }
    }
    disconnect();
    return -1;
  }
  return 0;
}

int Client::on_read() {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  std::array<uint8_t, 65536> buf;
  sockaddr_union su;
  socklen_t addrlen;

  for (;;) {
    addrlen = sizeof(su);
    auto nread =
        recvfrom(fd_, buf.data(), buf.size(), MSG_DONTWAIT, &su.sa, &addrlen);

    if (nread == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "recvfrom: " << strerror(errno) << std::endl;
      }
      break;
    }

    if (!config.quiet) {
      std::cerr << "Received packet: local="
                << util::straddr(&local_addr_.su.sa, local_addr_.len)
                << " remote=" << util::straddr(&su.sa, addrlen) << " " << nread
                << " bytes" << std::endl;
    }

    if (debug::packet_lost(config.rx_loss_prob)) {
      if (!config.quiet) {
        std::cerr << "** Simulated incoming packet loss **" << std::endl;
      }
      break;
    }

    if (feed_data(&su.sa, addrlen, buf.data(), nread) != 0) {
      return -1;
    }
  }

  if (should_exit_) {
    last_error_ = quic_err_app(0);
    disconnect();
    return -1;
  }

  reset_idle_timer();

  return 0;
}

void Client::reset_idle_timer() {
  auto now = util::timestamp(loop_);
  auto idle_expiry = ngtcp2_conn_get_idle_expiry(conn_);
  timer_.repeat =
      idle_expiry > now
          ? static_cast<ev_tstamp>(idle_expiry - now) / NGTCP2_SECONDS
          : 1e-9;

  if (!config.quiet) {
    std::cerr << "Set idle timer=" << std::fixed << timer_.repeat << "s"
              << std::defaultfloat << std::endl;
  }

  ev_timer_again(loop_, &timer_);
}

int Client::handle_expiry() {
  auto now = util::timestamp(loop_);
  if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0) {
    std::cerr << "ngtcp2_conn_handle_expiry: " << ngtcp2_strerror(rv)
              << std::endl;
    last_error_ = quic_err_transport(NGTCP2_ERR_INTERNAL);
    disconnect();
    return -1;
  }

  return 0;
}

int Client::on_write() {
  if (sendbuf_.size() > 0) {
    if (auto rv = send_packet(); rv != NETWORK_ERR_OK) {
      if (rv != NETWORK_ERR_SEND_BLOCKED) {
        last_error_ = quic_err_transport(NGTCP2_ERR_INTERNAL);
        disconnect();
      }
      return rv;
    }
  }

  assert(sendbuf_.left() >= max_pktlen_);

  if (auto rv = write_streams(); rv != 0) {
    if (rv == NETWORK_ERR_SEND_BLOCKED) {
      schedule_retransmit();
    }
    return rv;
  }

  if (should_exit_) {
    last_error_ = quic_err_app(0);
    disconnect();
    return -1;
  }

  schedule_retransmit();
  return 0;
}

int Client::write_streams() {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  std::array<nghttp3_vec, 16> vec;
  PathStorage path;
  size_t pktcnt = 0;

  for (;;) {
    int64_t stream_id = -1;
    int fin = 0;
    nghttp3_ssize sveccnt = 0;

    if (httpconn_ && ngtcp2_conn_get_max_data_left(conn_)) {
      sveccnt = nghttp3_conn_writev_stream(httpconn_, &stream_id, &fin,
                                           vec.data(), vec.size());
      if (sveccnt < 0) {
        std::cerr << "nghttp3_conn_writev_stream: " << nghttp3_strerror(sveccnt)
                  << std::endl;
        last_error_ = quic_err_app(sveccnt);
        disconnect();
        return -1;
      }
    }

    ngtcp2_ssize ndatalen;
    auto v = vec.data();
    auto vcnt = static_cast<size_t>(sveccnt);

    auto nwrite = ngtcp2_conn_writev_stream(
        conn_, &path.path, sendbuf_.wpos(), max_pktlen_, &ndatalen,
        NGTCP2_WRITE_STREAM_FLAG_MORE, stream_id, fin,
        reinterpret_cast<const ngtcp2_vec *>(v), vcnt, util::timestamp(loop_));
    if (nwrite < 0) {
      switch (nwrite) {
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
      case NGTCP2_ERR_STREAM_SHUT_WR:
        if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED &&
            ngtcp2_conn_get_max_data_left(conn_) == 0) {
          /* Call ngtcp2_conn_writev_stream to ensure that a complete
             packet is written to the buffer. */
          nwrite = ngtcp2_conn_writev_stream(
              conn_, &path.path, sendbuf_.wpos(), max_pktlen_, nullptr,
              NGTCP2_WRITE_STREAM_FLAG_NONE, /* stream_id = */ 0, /* fin = */ 0,
              nullptr, 0, util::timestamp(loop_));
          break;
        }

        if (auto rv = nghttp3_conn_block_stream(httpconn_, stream_id);
            rv != 0) {
          std::cerr << "nghttp3_conn_block_stream: " << nghttp3_strerror(rv)
                    << std::endl;
          last_error_ = quic_err_app(rv);
          disconnect();
          return -1;
        }
        continue;
      case NGTCP2_ERR_WRITE_STREAM_MORE:
        assert(ndatalen > 0);
        if (auto rv =
                nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
            rv != 0) {
          std::cerr << "nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv)
                    << std::endl;
          last_error_ = quic_err_app(rv);
          disconnect();
          return -1;
        }
        continue;
      }

      if (nwrite < 0) {
        std::cerr << "ngtcp2_conn_write_stream: " << ngtcp2_strerror(nwrite)
                  << std::endl;
        last_error_ = quic_err_transport(nwrite);
        disconnect();
        return -1;
      }
    }

    if (nwrite == 0) {
      // We are congestion limited.
      return 0;
    }

    sendbuf_.push(nwrite);

    if (ndatalen >= 0) {
      if (auto rv =
              nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
          rv != 0) {
        std::cerr << "nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv)
                  << std::endl;
        last_error_ = quic_err_app(rv);
        disconnect();
        return -1;
      }
    }

    update_remote_addr(&path.path.remote);
    reset_idle_timer();

    if (auto rv = send_packet(); rv != NETWORK_ERR_OK) {
      if (rv != NETWORK_ERR_SEND_BLOCKED) {
        last_error_ = quic_err_transport(NGTCP2_ERR_INTERNAL);
        disconnect();
      }
      return rv;
    }

    if (++pktcnt == 10) {
      ev_io_start(loop_, &wev_);
      return 0;
    }
  }
}

void Client::schedule_retransmit() {
  auto expiry = ngtcp2_conn_get_expiry(conn_);
  auto now = util::timestamp(loop_);
  auto t = expiry < now ? 1e-9
                        : static_cast<ev_tstamp>(expiry - now) / NGTCP2_SECONDS;
  rttimer_.repeat = t;
  ev_timer_again(loop_, &rttimer_);
}

void Client::write_client_handshake(ngtcp2_crypto_level level,
                                    const uint8_t *data, size_t datalen) {
  auto &crypto = crypto_[level];
  crypto.data.emplace_back(data, datalen);

  auto &buf = crypto.data.back();

  ngtcp2_conn_submit_crypto_data(conn_, level, buf.rpos(), buf.size());
}

int Client::recv_crypto_data(ngtcp2_crypto_level crypto_level,
                             const uint8_t *data, size_t datalen) {
  return ngtcp2_crypto_read_write_crypto_data(conn_, ssl_, crypto_level, data,
                                              datalen);
}

void Client::on_recv_retry() { setup_initial_crypto_context(); }

namespace {
int bind_addr(Address &local_addr, int fd, int family) {
  addrinfo hints{};
  addrinfo *res, *rp;

  hints.ai_family = family;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  if (auto rv = getaddrinfo(nullptr, "0", &hints, &res); rv != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  for (rp = res; rp; rp = rp->ai_next) {
    if (bind(fd, rp->ai_addr, rp->ai_addrlen) != -1) {
      break;
    }
  }

  if (!rp) {
    std::cerr << "Could not bind" << std::endl;
    return -1;
  }

  socklen_t len = sizeof(local_addr.su.storage);
  if (getsockname(fd, &local_addr.su.sa, &len) == -1) {
    std::cerr << "getsockname: " << strerror(errno) << std::endl;
    return -1;
  }
  local_addr.len = len;

  return 0;
}
} // namespace

namespace {
int create_sock(Address &remote_addr, const char *addr, const char *port) {
  addrinfo hints{};
  addrinfo *res, *rp;

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  if (auto rv = getaddrinfo(addr, port, &hints, &res); rv != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  int fd = -1;

  for (rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) {
      continue;
    }

    break;
  }

  if (!rp) {
    std::cerr << "Could not connect" << std::endl;
    return -1;
  }

  auto val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                 static_cast<socklen_t>(sizeof(val))) == -1) {
    close(fd);
    return -1;
  }

  remote_addr.len = rp->ai_addrlen;
  memcpy(&remote_addr.su, rp->ai_addr, rp->ai_addrlen);

  return fd;
}
} // namespace

ngtcp2_conn *Client::conn() const { return conn_; }

void Client::start_change_local_addr_timer() {
  ev_timer_start(loop_, &change_local_addr_timer_);
}

int Client::change_local_addr() {
  Address remote_addr, local_addr;

  if (!config.quiet) {
    std::cerr << "Changing local address" << std::endl;
  }

  auto nfd = create_sock(remote_addr, addr_, port_);
  if (nfd == -1) {
    return -1;
  }

  if (bind_addr(local_addr, nfd, remote_addr.su.sa.sa_family) != 0) {
    ::close(nfd);
    return -1;
  }

  ::close(fd_);

  fd_ = nfd;
  local_addr_ = local_addr;
  remote_addr_ = remote_addr;

  if (config.nat_rebinding) {
    ngtcp2_addr addr;
    ngtcp2_conn_set_local_addr(
        conn_,
        ngtcp2_addr_init(&addr, &local_addr.su, local_addr.len, nullptr));
  } else {
    auto path = ngtcp2_path{
        {local_addr.len, reinterpret_cast<uint8_t *>(&local_addr.su)},
        {remote_addr.len, reinterpret_cast<uint8_t *>(&remote_addr.su)}};
    if (auto rv = ngtcp2_conn_initiate_migration(conn_, &path,
                                                 util::timestamp(loop_));
        rv != 0) {
      std::cerr << "ngtcp2_conn_initiate_migration: " << ngtcp2_strerror(rv)
                << std::endl;
    }
  }

  auto wev_active = ev_is_active(&wev_);

  ev_io_stop(loop_, &wev_);
  ev_io_stop(loop_, &rev_);
  ev_io_set(&wev_, fd_, EV_WRITE);
  ev_io_set(&rev_, fd_, EV_READ);
  if (wev_active) {
    ev_io_start(loop_, &wev_);
  }
  ev_io_start(loop_, &rev_);

  return 0;
}

void Client::start_key_update_timer() {
  ev_timer_start(loop_, &key_update_timer_);
}

int Client::update_key(uint8_t *rx_secret, uint8_t *tx_secret, uint8_t *rx_key,
                       uint8_t *rx_iv, uint8_t *tx_key, uint8_t *tx_iv,
                       const uint8_t *current_rx_secret,
                       const uint8_t *current_tx_secret, size_t secretlen) {
  if (!config.quiet) {
    std::cerr << "Updating traffic key" << std::endl;
  }

  auto crypto_ctx = ngtcp2_conn_get_crypto_ctx(conn_);
  auto aead = &crypto_ctx->aead;
  auto keylen = ngtcp2_crypto_aead_keylen(aead);
  auto ivlen = ngtcp2_crypto_packet_protection_ivlen(aead);

  ++nkey_update_;

  if (ngtcp2_crypto_update_key(conn_, rx_secret, tx_secret, rx_key, rx_iv,
                               tx_key, tx_iv, current_rx_secret,
                               current_tx_secret, secretlen) != 0) {
    return -1;
  }

  if (!config.quiet && config.show_secret) {
    std::cerr << "application_traffic rx secret " << nkey_update_ << std::endl;
    debug::print_secrets(rx_secret, secretlen, rx_key, keylen, rx_iv, ivlen);
    std::cerr << "application_traffic tx secret " << nkey_update_ << std::endl;
    debug::print_secrets(tx_secret, secretlen, tx_key, keylen, tx_iv, ivlen);
  }

  return 0;
}

int Client::initiate_key_update() {
  if (auto rv = ngtcp2_conn_initiate_key_update(conn_, util::timestamp(loop_));
      rv != 0) {
    std::cerr << "ngtcp2_conn_initiate_key_update: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

void Client::start_delay_stream_timer() {
  ev_timer_start(loop_, &delay_stream_timer_);
}

void Client::update_remote_addr(const ngtcp2_addr *addr) {
  remote_addr_.len = addr->addrlen;
  memcpy(&remote_addr_.su, addr->addr, addr->addrlen);
}

int Client::send_packet() {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  if (debug::packet_lost(config.tx_loss_prob)) {
    if (!config.quiet) {
      std::cerr << "** Simulated outgoing packet loss **" << std::endl;
    }
    sendbuf_.reset();
    return NETWORK_ERR_OK;
  }

  ssize_t nwrite = 0;

  do {
    nwrite = sendto(fd_, sendbuf_.rpos(), sendbuf_.size(), MSG_DONTWAIT,
                    &remote_addr_.su.sa, remote_addr_.len);
  } while (nwrite == -1 && errno == EINTR);

  if (nwrite == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return NETWORK_ERR_SEND_BLOCKED;
    }
    std::cerr << "sendto: " << strerror(errno) << std::endl;
    return NETWORK_ERR_FATAL;
  }

  assert(static_cast<size_t>(nwrite) == sendbuf_.size());
  sendbuf_.reset();

  if (!config.quiet) {
    std::cerr << "Sent packet: local="
              << util::straddr(&local_addr_.su.sa, local_addr_.len)
              << " remote="
              << util::straddr(&remote_addr_.su.sa, remote_addr_.len) << " "
              << nwrite << " bytes" << std::endl;
  }

  return NETWORK_ERR_OK;
}

int Client::handle_error() {
  if (!conn_ || ngtcp2_conn_is_in_closing_period(conn_)) {
    return 0;
  }

  sendbuf_.reset();
  assert(sendbuf_.left() >= max_pktlen_);

  if (last_error_.type == QUICErrorType::TransportVersionNegotiation) {
    return 0;
  }

  PathStorage path;
  if (last_error_.type == QUICErrorType::Transport) {
    auto n = ngtcp2_conn_write_connection_close(
        conn_, &path.path, sendbuf_.wpos(), max_pktlen_, last_error_.code,
        util::timestamp(loop_));
    if (n < 0) {
      std::cerr << "ngtcp2_conn_write_connection_close: " << ngtcp2_strerror(n)
                << std::endl;
      return -1;
    }
    sendbuf_.push(n);
  } else {
    auto n = ngtcp2_conn_write_application_close(
        conn_, &path.path, sendbuf_.wpos(), max_pktlen_, last_error_.code,
        util::timestamp(loop_));
    if (n < 0) {
      std::cerr << "ngtcp2_conn_write_application_close: " << ngtcp2_strerror(n)
                << std::endl;
      return -1;
    }
    sendbuf_.push(n);
  }

  update_remote_addr(&path.path.remote);

  return send_packet();
}

namespace {
size_t remove_tx_stream_data(std::deque<Buffer> &d, uint64_t &tx_offset,
                             uint64_t offset) {
  size_t len = 0;
  for (; !d.empty() && tx_offset + d.front().size() <= offset;) {
    tx_offset += d.front().size();
    len += d.front().size();
    d.pop_front();
  }
  return len;
}
} // namespace

void Client::remove_tx_crypto_data(ngtcp2_crypto_level crypto_level,
                                   uint64_t offset, size_t datalen) {
  auto &crypto = crypto_[crypto_level];
  ::remove_tx_stream_data(crypto.data, crypto.acked_offset, offset + datalen);
}

int Client::on_stream_close(int64_t stream_id, uint64_t app_error_code) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  if (httpconn_) {
    if (app_error_code == 0) {
      app_error_code = NGHTTP3_H3_NO_ERROR;
    }
    auto rv = nghttp3_conn_close_stream(httpconn_, stream_id, app_error_code);
    switch (rv) {
    case 0:
      break;
    case NGHTTP3_ERR_STREAM_NOT_FOUND:
      // We have to handle the case when stream opened but no data is
      // transferred.  In this case, nghttp3_conn_close_stream might
      // return error.
      if (!ngtcp2_is_bidi_stream(stream_id)) {
        assert(!ngtcp2_conn_is_local_stream(conn_, stream_id));
        ngtcp2_conn_extend_max_streams_uni(conn_, 1);
      }
      break;
    default:
      std::cerr << "nghttp3_conn_close_stream: " << nghttp3_strerror(rv)
                << std::endl;
      last_error_ = quic_err_app(rv);
      return -1;
    }
  }

  return 0;
}

int Client::on_stream_reset(int64_t stream_id) {
  if (httpconn_) {
    if (auto rv = nghttp3_conn_reset_stream(httpconn_, stream_id); rv != 0) {
      std::cerr << "nghttp3_conn_reset_stream: " << nghttp3_strerror(rv)
                << std::endl;
      return -1;
    }
  }
  return 0;
}

void Client::make_stream_early() {
  if (nstreams_done_ >= config.nstreams) {
    return;
  }

  int64_t stream_id;
  if (auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr);
      rv != 0) {
    std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return;
  }

  // TODO Handle error
  if (setup_httpconn() != 0) {
    return;
  }

  auto stream = std::make_unique<Stream>(
      config.requests[nstreams_done_ % config.requests.size()], stream_id);
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  if (submit_http_request(stream.get()) != 0) {
    return;
  }

  if (!config.download.empty()) {
    stream->open_file(stream->req.path);
  }
  streams_.emplace(stream_id, std::move(stream));

  ++nstreams_done_;
}

int Client::on_extend_max_streams() {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  int64_t stream_id;
  if (ev_is_active(&delay_stream_timer_)) {
    return 0;
  }

  // std::cout << "\n\t[MINH] INFO: config.nstreams: " << config.nstreams << std::endl;
  for (; nstreams_done_ < config.nstreams; ++nstreams_done_) {
    // std::cout << "\n\t[MINH] INFO: nstreams_done_: " << nstreams_done_ << std::endl;
    if (auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr);
        rv != 0) {
      assert(NGTCP2_ERR_STREAM_ID_BLOCKED == rv);
      break;
    }

    auto stream = std::make_unique<Stream>(
        config.requests[nstreams_done_ % config.requests.size()], stream_id);
    std::cout << "\tnstreams_done_: " << nstreams_done_ << '\n'
              << "\tconfig.requests.size(): " << config.requests.size() << '\n'
              << "\tstream_id: " << stream_id << std::endl;
    if (submit_http_request(stream.get()) != 0) {
      break;
    }

    if (!config.download.empty()) {
      // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
      stream->open_file(stream->req.path);
    }
    streams_.emplace(stream_id, std::move(stream));
  }
  return 0;
}

namespace {
nghttp3_ssize read_data(nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec,
                        size_t veccnt, uint32_t *pflags, void *user_data,
                        void *stream_user_data) {
  vec[0].base = config.data;
  vec[0].len = config.datalen;
  *pflags |= NGHTTP3_DATA_FLAG_EOF;

  return 1;
}
} // namespace

int Client::submit_http_request(const Stream *stream) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  std::string content_length_str;

  const auto &req = stream->req;

  std::array<nghttp3_nv, 6> nva{
      util::make_nv(":method", config.http_method),
      util::make_nv(":scheme", req.scheme),
      util::make_nv(":authority", req.authority),
      util::make_nv(":path", req.path),
      util::make_nv("user-agent", "nghttp3/ngtcp2 client"),
  };
  size_t nvlen = 5;
  if (config.fd != -1) {
    content_length_str = std::to_string(config.datalen);
    nva[nvlen++] = util::make_nv("content-length", content_length_str);
  }

  if (!config.quiet) {
    debug::print_http_request_headers(stream->stream_id, nva.data(), nvlen);
  }

  nghttp3_data_reader dr{};
  dr.read_data = read_data;

  if (auto rv = nghttp3_conn_submit_request(
          httpconn_, stream->stream_id, nva.data(), nvlen,
          config.fd == -1 ? nullptr : &dr, nullptr);
      rv != 0) {
    std::cerr << "nghttp3_conn_submit_request: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

int Client::recv_stream_data(int64_t stream_id, int fin, const uint8_t *data,
                             size_t datalen) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  auto nconsumed =
      nghttp3_conn_read_stream(httpconn_, stream_id, data, datalen, fin);
  if (nconsumed < 0) {
    std::cerr << "nghttp3_conn_read_stream: " << nghttp3_strerror(nconsumed)
              << std::endl;
    last_error_ = quic_err_app(nconsumed);
    return -1;
  }

  ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(conn_, nconsumed);

  return 0;
}

int Client::acked_stream_data_offset(int64_t stream_id, size_t datalen) {
  if (auto rv = nghttp3_conn_add_ack_offset(httpconn_, stream_id, datalen);
      rv != 0) {
    std::cerr << "nghttp3_conn_add_ack_offset: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

int Client::select_preferred_address(Address &selected_addr,
                                     const ngtcp2_preferred_addr *paddr) {
  int af;
  const uint8_t *binaddr;
  uint16_t port;
  constexpr uint8_t empty_addr[] = {0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0};
  if (local_addr_.su.sa.sa_family == AF_INET &&
      memcmp(empty_addr, paddr->ipv4_addr, sizeof(paddr->ipv4_addr)) != 0) {
    af = AF_INET;
    binaddr = paddr->ipv4_addr;
    port = paddr->ipv4_port;
  } else if (local_addr_.su.sa.sa_family == AF_INET6 &&
             memcmp(empty_addr, paddr->ipv6_addr, sizeof(paddr->ipv6_addr)) !=
                 0) {
    af = AF_INET6;
    binaddr = paddr->ipv6_addr;
    port = paddr->ipv6_port;
  } else {
    return -1;
  }

  char host[NI_MAXHOST];
  if (inet_ntop(af, binaddr, host, sizeof(host)) == nullptr) {
    std::cerr << "inet_ntop: " << strerror(errno) << std::endl;
    return -1;
  }

  if (!config.quiet) {
    std::cerr << "selected server preferred_address is [" << host
              << "]:" << port << std::endl;
  }

  addrinfo hints{};
  addrinfo *res;

  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  hints.ai_family = af;
  hints.ai_socktype = SOCK_DGRAM;

  if (auto rv = getaddrinfo(host, std::to_string(port).c_str(), &hints, &res);
      rv != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  assert(res);

  selected_addr.len = res->ai_addrlen;
  memcpy(&selected_addr.su, res->ai_addr, res->ai_addrlen);

  freeaddrinfo(res);

  return 0;
}

void Client::start_wev() { ev_io_start(loop_, &wev_); }

void Client::set_tls_alert(uint8_t alert) { last_error_ = quic_err_tls(alert); }

namespace {
int http_acked_stream_data(nghttp3_conn *conn, int64_t stream_id,
                           size_t datalen, void *user_data,
                           void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->http_acked_stream_data(stream_id, datalen) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::http_acked_stream_data(int64_t stream_id, size_t datalen) {
  return 0;
}

namespace {
int http_recv_data(nghttp3_conn *conn, int64_t stream_id, const uint8_t *data,
                   size_t datalen, void *user_data, void *stream_user_data) {
  if (!config.quiet && !config.no_http_dump) {
    // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
    debug::print_http_data(stream_id, data, datalen);
  }
  auto c = static_cast<Client *>(user_data);
  c->http_consume(stream_id, datalen);
  c->http_write_data(stream_id, data, datalen);
  return 0;
}
} // namespace

namespace {
int http_deferred_consume(nghttp3_conn *conn, int64_t stream_id,
                          size_t nconsumed, void *user_data,
                          void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  c->http_consume(stream_id, nconsumed);
  return 0;
}
} // namespace

void Client::http_consume(int64_t stream_id, size_t nconsumed) {
  ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(conn_, nconsumed);
}

void Client::http_write_data(int64_t stream_id, const uint8_t *data,
                             size_t datalen) {
  auto it = streams_.find(stream_id);
  if (it == std::end(streams_)) {
    return;
  }

  auto &stream = (*it).second;

  if (stream->fd == -1) {
    return;
  }

  ssize_t nwrite;
  do {
    nwrite = write(stream->fd, data, datalen);
  } while (nwrite == -1 && errno == EINTR);
}

namespace {
int http_begin_headers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                       void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_begin_response_headers(stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_recv_header(nghttp3_conn *conn, int64_t stream_id, int32_t token,
                     nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                     void *user_data, void *stream_user_data) {
  if (!config.quiet) {
    // std::cout << "\n\t[MINH] INFO: value: " << value << " " << __FILE__ << ": " << __func__<< "(): " << __LINE__ << std::endl;

  auto namebuf = nghttp3_rcbuf_get_buf(name); // namebuf.base == "content-length" ==> valuebuf.base = data-length
  auto valuebuf = nghttp3_rcbuf_get_buf(value);
  if (strncmp((char*)namebuf.base, "content-length", 14)== 0){
    minh_seg_length = std::atoi((char*)valuebuf.base);
    std::cout << "\t[MINH] CLIENT CONTENT-LENGTH: " << valuebuf.base << std::endl;
  }
    debug::print_http_header(stream_id, name, value, flags);
  }
  return 0;
}
} // namespace

namespace {
int http_end_headers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                     void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_end_headers(stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_begin_trailers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                        void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_begin_trailers(stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_recv_trailer(nghttp3_conn *conn, int64_t stream_id, int32_t token,
                      nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                      void *user_data, void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_header(stream_id, name, value, flags);
  }
  return 0;
}
} // namespace

namespace {
int http_end_trailers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                      void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_end_trailers(stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_begin_push_promise(nghttp3_conn *conn, int64_t stream_id,
                            int64_t push_id, void *user_data,
                            void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_begin_push_promise(stream_id, push_id);
  }
  return 0;
}
} // namespace

namespace {
int http_recv_push_promise(nghttp3_conn *conn, int64_t stream_id,
                           int64_t push_id, int32_t token, nghttp3_rcbuf *name,
                           nghttp3_rcbuf *value, uint8_t flags, void *user_data,
                           void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_push_promise(stream_id, push_id, name, value, flags);
  }
  return 0;
}
} // namespace

namespace {
int http_end_push_promise(nghttp3_conn *conn, int64_t stream_id,
                          int64_t push_id, void *user_data,
                          void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_end_push_promise(stream_id, push_id);
  }
  return 0;
}
} // namespace

namespace {
int http_send_stop_sending(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t app_error_code, void *user_data,
                           void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->send_stop_sending(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::send_stop_sending(int64_t stream_id, uint64_t app_error_code) {
  if (auto rv =
          ngtcp2_conn_shutdown_stream_read(conn_, stream_id, app_error_code);
      rv != 0) {
    std::cerr << "ngtcp2_conn_shutdown_stream_read: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }
  return 0;
}

namespace {
int http_cancel_push(nghttp3_conn *conn, int64_t push_id, int64_t stream_id,
                     void *user_data, void *stream_user_data) {
  if (!config.quiet) {
    debug::cancel_push(push_id, stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_push_stream(nghttp3_conn *conn, int64_t push_id, int64_t stream_id,
                     void *user_data) {
  if (!config.quiet) {
    debug::push_stream(push_id, stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_stream_close(nghttp3_conn *conn, int64_t stream_id,
                      uint64_t app_error_code, void *conn_user_data,
                      void *stream_user_data) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  auto c = static_cast<Client *>(conn_user_data);
  if (c->http_stream_close(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::http_stream_close(int64_t stream_id, uint64_t app_error_code) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  if (config.exit_on_first_stream_close) {
    should_exit_ = true;
  }

  if (!ngtcp2_is_bidi_stream(stream_id)) {
    assert(!ngtcp2_conn_is_local_stream(conn_, stream_id));
    ngtcp2_conn_extend_max_streams_uni(conn_, 1);
  }

  if (auto it = streams_.find(stream_id); it != std::end(streams_)) {
    if (!config.quiet) {
      std::cerr << "HTTP stream " << stream_id << " closed with error code "
                << app_error_code << std::endl;
    }
    streams_.erase(it);
  }

  return 0;
}

int Client::setup_httpconn() {
  if (httpconn_) {
    return 0;
  }

  if (ngtcp2_conn_get_max_local_streams_uni(conn_) < 3) {
    std::cerr << "peer does not allow at least 3 unidirectional streams."
              << std::endl;
    return -1;
  }

  nghttp3_conn_callbacks callbacks{
      ::http_acked_stream_data,  ::http_stream_close,
      ::http_recv_data,          ::http_deferred_consume,
      ::http_begin_headers,      ::http_recv_header,
      ::http_end_headers,        ::http_begin_trailers,
      ::http_recv_trailer,       ::http_end_trailers,
      ::http_begin_push_promise, ::http_recv_push_promise,
      ::http_end_push_promise,   ::http_cancel_push,
      ::http_send_stop_sending,  ::http_push_stream,
  };
  nghttp3_conn_settings settings;
  nghttp3_conn_settings_default(&settings);
  settings.qpack_max_table_capacity = 4096;
  settings.qpack_blocked_streams = 100;
  settings.max_pushes = 100;

  auto mem = nghttp3_mem_default();

  if (auto rv =
          nghttp3_conn_client_new(&httpconn_, &callbacks, &settings, mem, this);
      rv != 0) {
    std::cerr << "nghttp3_conn_client_new: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  int64_t ctrl_stream_id;

  if (auto rv = ngtcp2_conn_open_uni_stream(conn_, &ctrl_stream_id, nullptr);
      rv != 0) {
    std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv = nghttp3_conn_bind_control_stream(httpconn_, ctrl_stream_id);
      rv != 0) {
    std::cerr << "nghttp3_conn_bind_control_stream: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  if (!config.quiet) {
    fprintf(stderr, "http: control stream=%" PRIx64 "\n", ctrl_stream_id);
  }

  int64_t qpack_enc_stream_id, qpack_dec_stream_id;

  if (auto rv =
          ngtcp2_conn_open_uni_stream(conn_, &qpack_enc_stream_id, nullptr);
      rv != 0) {
    std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv =
          ngtcp2_conn_open_uni_stream(conn_, &qpack_dec_stream_id, nullptr);
      rv != 0) {
    std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv = nghttp3_conn_bind_qpack_streams(httpconn_, qpack_enc_stream_id,
                                                qpack_dec_stream_id);
      rv != 0) {
    std::cerr << "nghttp3_conn_bind_qpack_streams: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  if (!config.quiet) {
    fprintf(stderr,
            "http: QPACK streams encoder=%" PRIx64 " decoder=%" PRIx64 "\n",
            qpack_enc_stream_id, qpack_dec_stream_id);
  }

  return 0;
}

namespace {
int new_session_cb(SSL *ssl, SSL_SESSION *session) {
  if (SSL_SESSION_get_max_early_data(session) !=
      std::numeric_limits<uint32_t>::max()) {
    std::cerr << "max_early_data_size is not 0xffffffff" << std::endl;
  }
  auto f = BIO_new_file(config.session_file, "w");
  if (f == nullptr) {
    std::cerr << "Could not write TLS session in " << config.session_file
              << std::endl;
    return 0;
  }

  PEM_write_bio_SSL_SESSION(f, session);
  BIO_free(f);

  return 0;
}
} // namespace

namespace {
int set_encryption_secrets(SSL *ssl, OSSL_ENCRYPTION_LEVEL ossl_level,
                           const uint8_t *read_secret,
                           const uint8_t *write_secret, size_t secret_len) {
  auto c = static_cast<Client *>(SSL_get_app_data(ssl));

  if (c->on_key(util::from_ossl_level(ossl_level), read_secret, write_secret,
                secret_len) != 0) {
    return 0;
  }

  return 1;
}
} // namespace

namespace {
int add_handshake_data(SSL *ssl, OSSL_ENCRYPTION_LEVEL ossl_level,
                       const uint8_t *data, size_t len) {
  auto c = static_cast<Client *>(SSL_get_app_data(ssl));
  c->write_client_handshake(util::from_ossl_level(ossl_level), data, len);
  return 1;
}
} // namespace

namespace {
int flush_flight(SSL *ssl) { return 1; }
} // namespace

namespace {
int send_alert(SSL *ssl, enum ssl_encryption_level_t level, uint8_t alert) {
  auto c = static_cast<Client *>(SSL_get_app_data(ssl));
  c->set_tls_alert(alert);
  return 1;
}
} // namespace

namespace {
auto quic_method = SSL_QUIC_METHOD{
    set_encryption_secrets,
    add_handshake_data,
    flush_flight,
    send_alert,
};
} // namespace

namespace {
SSL_CTX *create_ssl_ctx(const char *private_key_file, const char *cert_file) {
  auto ssl_ctx = SSL_CTX_new(TLS_client_method());

  SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);

  SSL_CTX_set_default_verify_paths(ssl_ctx);

  if (SSL_CTX_set_ciphersuites(ssl_ctx, config.ciphers) != 1) {
    std::cerr << "SSL_CTX_set_ciphersuites: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    exit(EXIT_FAILURE);
  }

  if (SSL_CTX_set1_groups_list(ssl_ctx, config.groups) != 1) {
    std::cerr << "SSL_CTX_set1_groups_list failed" << std::endl;
    exit(EXIT_FAILURE);
  }

  if (private_key_file && cert_file) {
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, private_key_file,
                                    SSL_FILETYPE_PEM) != 1) {
      std::cerr << "SSL_CTX_use_PrivateKey_file: "
                << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
      exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file) != 1) {
      std::cerr << "SSL_CTX_use_certificate_file: "
                << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  SSL_CTX_set_quic_method(ssl_ctx, &quic_method);

  if (config.session_file) {
    SSL_CTX_set_session_cache_mode(
        ssl_ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(ssl_ctx, new_session_cb);
  }

  return ssl_ctx;
}
} // namespace

namespace {
int run(Client &c, const char *addr, const char *port) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  Address remote_addr, local_addr;

  auto fd = create_sock(remote_addr, addr, port);
  if (fd == -1) {
    return -1;
  }

  if (bind_addr(local_addr, fd, remote_addr.su.sa.sa_family) != 0) {
    close(fd);
    return -1;
  }

  if (c.init(fd, local_addr, remote_addr, addr, port, config.version) != 0) {
    return -1;
  }

  // TODO Do we need this ?
  if (auto rv = c.on_write(); rv != 0) {
    return rv;
  }

  ev_run(EV_DEFAULT, 0);

  return 0;
}
} // namespace

namespace {
std::string_view get_string(const char *uri, const http_parser_url &u,
                            http_parser_url_fields f) {
  auto p = &u.field_data[f];
  return {uri + p->off, p->len};
}
} // namespace

namespace {
int parse_uri(Request &req, const char *uri) {
  std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  std::cout << "\n\t[MINH] Request: " << &req << '\t'
            << "\t *uri: " << *uri << " uri: " << &uri << std::endl;
  http_parser_url u;

  http_parser_url_init(&u);
  if (http_parser_parse_url(uri, strlen(uri), /* is_connect = */ 0, &u) != 0) {
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
    return -1;
  }

  if (!(u.field_set & (1 << UF_SCHEMA)) || !(u.field_set & (1 << UF_HOST))) {
    // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
    return -1;
  }

  req.scheme = get_string(uri, u, UF_SCHEMA);

  req.authority = get_string(uri, u, UF_HOST);
  if (util::numeric_host(req.authority.c_str())) {
    req.authority = '[' + req.authority + ']';
  }
  if (u.field_set & (1 << UF_PORT)) {
    req.authority += ':';
    req.authority += get_string(uri, u, UF_PORT);
  }

  if (u.field_set & (1 << UF_PATH)) {
    req.path = get_string(uri, u, UF_PATH);
  } else {
    req.path = "/";
  }

  if (u.field_set & (1 << UF_QUERY)) {
    req.path += '?';
    req.path += get_string(uri, u, UF_QUERY);
  }

  return 0;
}
} // namespace

namespace {
int parse_requests(char **argv, size_t argvlen) {
  std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  for (size_t i = 0; i < argvlen; ++i) {
    auto uri = argv[i];
    Request req;
    if (parse_uri(req, uri) != 0) {
      std::cerr << "Could not parse URI: " << uri << std::endl;
      return -1;
    }
    config.requests.emplace_back(std::move(req));
  }
  return 0;
}
} // namespace

namespace {
std::ofstream keylog_file;
void keylog_callback(const SSL *ssl, const char *line) {
  keylog_file.write(line, strlen(line));
  keylog_file.put('\n');
  keylog_file.flush();
}
} // namespace

namespace {
void print_usage() {
  std::cerr << "Usage: client [OPTIONS] <ADDR> <PORT> [<URI>...]" << std::endl;
}
} // namespace

namespace {
void config_set_default(Config &config) {
  config = Config{};
  config.tx_loss_prob = 0.;
  config.rx_loss_prob = 0.;
  config.fd = -1;
  config.ciphers = "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_"
                   "POLY1305_SHA256:TLS_AES_128_CCM_SHA256";
  config.groups = "P-256:X25519:P-384:P-521";
  config.nstreams = 0;
  config.data = nullptr;
  config.datalen = 0;
  config.version = NGTCP2_PROTO_VER;
  config.timeout = 30 * NGTCP2_SECONDS;
  config.http_method = "GET"sv;
  config.max_data = 1_m;
  config.max_stream_data_bidi_local = 256_k;
  config.max_stream_data_bidi_remote = 256_k;
  config.max_stream_data_uni = 256_k;
  config.max_streams_bidi = 1;
  config.max_streams_uni = 100;
}
} // namespace

namespace {
void print_help() {
  print_usage();

  config_set_default(config);

  std::cout << R"(
  <ADDR>      Remote server address
  <PORT>      Remote server port
  <URI>       Remote URI
Options:
  -t, --tx-loss=<P>
              The probability of losing outgoing packets.  <P> must be
              [0.0, 1.0],  inclusive.  0.0 means no  packet loss.  1.0
              means 100% packet loss.
  -r, --rx-loss=<P>
              The probability of losing incoming packets.  <P> must be
              [0.0, 1.0],  inclusive.  0.0 means no  packet loss.  1.0
              means 100% packet loss.
  -d, --data=<PATH>
              Read data from <PATH>, and send them as STREAM data.
  -n, --nstreams=<N>
              The number of requests.  <URI>s are used in the order of
              appearance in the command-line.   If the number of <URI>
              list  is  less than  <N>,  <URI>  list is  wrapped.   It
              defaults to 0 which means the number of <URI> specified.
  -v, --version=<HEX>
              Specify QUIC version to use in hex string.
              Default: )"
            << std::hex << "0x" << config.version << std::dec << R"(
  -q, --quiet Suppress debug output.
  -s, --show-secret
              Print out secrets unless --quiet is used.
  --timeout=<DURATION>
              Specify idle timeout.
              Default: )"
            << util::format_duration(config.timeout) << R"(
  --ciphers=<CIPHERS>
              Specify the cipher suite list to enable.
              Default: )"
            << config.ciphers << R"(
  --groups=<GROUPS>
              Specify the supported groups.
              Default: )"
            << config.groups << R"(
  --session-file=<PATH>
              Read/write  TLS session  from/to  <PATH>.   To resume  a
              session, the previous session must be supplied with this
              option.
  --tp-file=<PATH>
              Read/write QUIC transport parameters from/to <PATH>.  To
              send 0-RTT data, the  transport parameters received from
              the previous session must be supplied with this option.
  --dcid=<DCID>
              Specify  initial  DCID.   <DCID> is  hex  string.   When
              decoded as binary, it should be  at least 8 bytes and at
              most 18 bytes long.
  --change-local-addr=<DURATION>
              Client  changes  local  address when  <DURATION>  elapse
              after handshake completes.
  --nat-rebinding
              When   used  with   --change-local-addr,  simulate   NAT
              rebinding.   In   other  words,  client   changes  local
              address, but it does not start path validation.
  --key-update=<DURATION>
              Client initiates key update when <DURATION> elapse after
              handshake completes.
  -m, --http-method=<METHOD>
              Specify HTTP method.  Default: )"
            << config.http_method << R"(
  --delay-stream=<DURATION>
              Delay sending STREAM data  in 1-RTT for <DURATION> after
              handshake completes.
  --no-preferred-addr
              Do not try to use preferred address offered by server.
  --key=<PATH>
              The path to client private key PEM file.
  --cert=<PATH>
              The path to client certificate PEM file.
  --download=<PATH>
              The path to the directory  to save a downloaded content.
              It is  undefined if 2  concurrent requests write  to the
              same file.
  --no-quic-dump
              Disables printing QUIC STREAM and CRYPTO frame data out.
  --no-http-dump
              Disables printing HTTP response body out.
  --qlog-file=<PATH>
              The path to write qlog.
  --max-data=<SIZE>
              The initial connection-level flow control window.
              Default: )"
            << util::format_uint_iec(config.max_data) << R"(
  --max-stream-data-bidi-local=<SIZE>
              The  initial  stream-level  flow control  window  for  a
              bidirectional stream that the local endpoint initiates.
              Default: )"
            << util::format_uint_iec(config.max_stream_data_bidi_local) << R"(
  --max-stream-data-bidi-remote=<SIZE>
              The  initial  stream-level  flow control  window  for  a
              bidirectional stream that the remote endpoint initiates.
              Default: )"
            << util::format_uint_iec(config.max_stream_data_bidi_remote) << R"(
  --max-stream-data-uni=<SIZE>
              The  initial  stream-level  flow control  window  for  a
              unidirectional stream.
              Default: )"
            << util::format_uint_iec(config.max_stream_data_uni) << R"(
  --max-streams-bidi=<N>
              The number of the concurrent bidirectional streams.
              Default: )"
            << config.max_streams_bidi << R"(
  --max-streams-uni=<N>
              The number of the concurrent unidirectional streams.
              Default: )"
            << config.max_streams_uni << R"(
  --exit-on-first-stream-close
              Exit when a first HTTP stream is closed.
  --disable-early-data
              Disable early data.
  -h, --help  Display this help and exit.

---

  The <SIZE> argument is an integer and an optional unit (e.g., 10K is
  10 * 1024).  Units are K, M and G (powers of 1024).

  The <DURATION> argument is an integer and an optional unit (e.g., 1s
  is 1 second and 500ms is 500  milliseconds).  Units are h, m, s, ms,
  us, or ns (hours,  minutes, seconds, milliseconds, microseconds, and
  nanoseconds respectively).  If  a unit is omitted, a  second is used
  as unit.)" << std::endl;
}
} // namespace

int main(int argc, char **argv) {
  // MINH ADD-S
  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  // std::cout << "\targc: " << argc << '\n'
  //           << " \tsize of argv: " << sizeof(argv) << std::endl;

  // for (int i = 0; i < sizeof(argv); i ++){
  //   std::cout << "\tMINH argv[" << i << "]: " << argv[i] << std::endl;
  // }
  // MINH ADD-E            
  config_set_default(config);
  char *data_path = nullptr;
  const char *private_key_file = nullptr;
  const char *cert_file = nullptr;

  for (;;) {
    static int flag = 0;
    constexpr static option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"tx-loss", required_argument, nullptr, 't'},
        {"rx-loss", required_argument, nullptr, 'r'},
        {"data", required_argument, nullptr, 'd'},
        {"http-method", required_argument, nullptr, 'm'},
        {"nstreams", required_argument, nullptr, 'n'},
        {"version", required_argument, nullptr, 'v'},
        {"quiet", no_argument, nullptr, 'q'},
        {"show-secret", no_argument, nullptr, 's'},
        {"ciphers", required_argument, &flag, 1},
        {"groups", required_argument, &flag, 2},
        {"timeout", required_argument, &flag, 3},
        {"session-file", required_argument, &flag, 4},
        {"tp-file", required_argument, &flag, 5},
        {"dcid", required_argument, &flag, 6},
        {"change-local-addr", required_argument, &flag, 7},
        {"key-update", required_argument, &flag, 8},
        {"nat-rebinding", no_argument, &flag, 9},
        {"delay-stream", required_argument, &flag, 10},
        {"no-preferred-addr", no_argument, &flag, 11},
        {"key", required_argument, &flag, 12},
        {"cert", required_argument, &flag, 13},
        {"download", required_argument, &flag, 14},
        {"no-quic-dump", no_argument, &flag, 15},
        {"no-http-dump", no_argument, &flag, 16},
        {"qlog-file", required_argument, &flag, 17},
        {"max-data", required_argument, &flag, 18},
        {"max-stream-data-bidi-local", required_argument, &flag, 19},
        {"max-stream-data-bidi-remote", required_argument, &flag, 20},
        {"max-stream-data-uni", required_argument, &flag, 21},
        {"max-streams-bidi", required_argument, &flag, 22},
        {"max-streams-uni", required_argument, &flag, 23},
        {"exit-on-first-stream-close", no_argument, &flag, 24},
        {"disable-early-data", no_argument, &flag, 25},
        {nullptr, 0, nullptr, 0},
    };

    auto optidx = 0;
    auto c = getopt_long(argc, argv, "d:him:n:qr:st:v:", long_opts, &optidx);
    if (c == -1) {
      break;
    }
    switch (c) {
    case 'd':
      // --data
      data_path = optarg;
      break;
    case 'h':
      // --help
      print_help();
      exit(EXIT_SUCCESS);
    case 'm':
      // --http-method
      config.http_method = optarg;
      break;
    case 'n':
      // --streams
      config.nstreams = strtol(optarg, nullptr, 10);
      break;
    case 'q':
      // --quiet
      config.quiet = true;
      break;
    case 'r':
      // --rx-loss
      config.rx_loss_prob = strtod(optarg, nullptr);
      break;
    case 's':
      // --show-secret
      config.show_secret = true;
      break;
    case 't':
      // --tx-loss
      config.tx_loss_prob = strtod(optarg, nullptr);
      break;
    case 'v':
      // --version
      config.version = strtol(optarg, nullptr, 16);
      break;
    case '?':
      print_usage();
      exit(EXIT_FAILURE);
    case 0:
      switch (flag) {
      case 1:
        // --ciphers
        config.ciphers = optarg;
        break;
      case 2:
        // --groups
        config.groups = optarg;
        break;
      case 3:
        // --timeout
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "timeout: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.timeout = t;
        }
        break;
      case 4:
        // --session-file
        config.session_file = optarg;
        break;
      case 5:
        // --tp-file
        config.tp_file = optarg;
        break;
      case 6: {
        // --dcid
        auto dcidlen2 = strlen(optarg);
        if (dcidlen2 % 2 || dcidlen2 / 2 < 8 || dcidlen2 / 2 > 18) {
          std::cerr << "dcid: wrong length" << std::endl;
          exit(EXIT_FAILURE);
        }
        auto dcid = util::decode_hex(optarg);
        ngtcp2_cid_init(&config.dcid,
                        reinterpret_cast<const uint8_t *>(dcid.c_str()),
                        dcid.size());
        break;
      }
      case 7:
        // --change-local-addr
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "change-local-addr: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.change_local_addr = t;
        }
        break;
      case 8:
        // --key-update
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "key-update: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.key_update = t;
        }
        break;
      case 9:
        // --nat-rebinding
        config.nat_rebinding = true;
        break;
      case 10:
        // --delay-stream
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "delay-stream: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.delay_stream = t;
        }
        break;
      case 11:
        // --no-preferred-addr
        config.no_preferred_addr = true;
        break;
      case 12:
        // --key
        private_key_file = optarg;
        break;
      case 13:
        // --cert
        cert_file = optarg;
        break;
      case 14:
        // --download
        config.download = optarg;
        break;
      case 15:
        // --no-quic-dump
        config.no_quic_dump = true;
        break;
      case 16:
        // --no-http-dump
        config.no_http_dump = true;
        break;
      case 17:
        // --qlog-file
        config.qlog_file = optarg;
        break;
      case 18:
        // --max-data
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-data: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_data = n;
        }
        break;
      case 19:
        // --max-stream-data-bidi-local
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-stream-data-bidi-local: invalid argument"
                    << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_stream_data_bidi_local = n;
        }
        break;
      case 20:
        // --max-stream-data-bidi-remote
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-stream-data-bidi-remote: invalid argument"
                    << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_stream_data_bidi_remote = n;
        }
        break;
      case 21:
        // --max-stream-data-uni
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-stream-data-uni: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_stream_data_uni = n;
        }
        break;
      case 22:
        // --max-streams-bidi
        config.max_streams_bidi = strtoull(optarg, nullptr, 10);
        break;
      case 23:
        // --max-streams-uni
        config.max_streams_uni = strtoull(optarg, nullptr, 10);
        break;
      case 24:
        // --exit-on-first-stream-close
        config.exit_on_first_stream_close = true;
        break;
      case 25:
        // --disable-early-data
        config.disable_early_data = true;
        break;
      }
      break;
    default:
      break;
    };
  }

  // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  std::cout << "\targc: " << argc << " optind: " << optind << std::endl;
  // MINH 18.02.20 [run dummynet to control network] ADD-S

  // MINH 18.02.20 [run dummynet to control network] ADD-S
  if (argc - optind < 2) {
    std::cerr << "Too few arguments" << std::endl;
    print_usage();
    exit(EXIT_FAILURE);
  }

  if (data_path) {
    auto fd = open(data_path, O_RDONLY);
    if (fd == -1) {
      // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
      std::cerr << "data: Could not open file " << data_path << ": "
                << strerror(errno) << std::endl;
      exit(EXIT_FAILURE);
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
      std::cerr << "data: Could not stat file " << data_path << ": "
                << strerror(errno) << std::endl;
      exit(EXIT_FAILURE);
    }
    config.fd = fd;
    config.datalen = st.st_size;
    auto addr = mmap(nullptr, config.datalen, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
      std::cerr << "data: Could not mmap file " << data_path << ": "
                << strerror(errno) << std::endl;
      exit(EXIT_FAILURE);
    }
    config.data = static_cast<uint8_t *>(addr);
  }

  auto addr = argv[optind++];
  auto port = argv[optind++];

  std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
  std::cout << "\taddr: " << addr << " port: " << port << std::endl;

  if (parse_requests(&argv[optind], argc - optind) != 0) {
    exit(EXIT_FAILURE);
  }

  if (config.nstreams == 0) {
    config.nstreams = config.requests.size();
  }

  auto ssl_ctx = create_ssl_ctx(private_key_file, cert_file);
  auto ssl_ctx_d = defer(SSL_CTX_free, ssl_ctx);

  auto ev_loop_d = defer(ev_loop_destroy, EV_DEFAULT);

  auto keylog_filename = getenv("SSLKEYLOGFILE");
  if (keylog_filename) {
    keylog_file.open(keylog_filename, std::ios_base::app);
    if (keylog_file) {
      SSL_CTX_set_keylog_callback(ssl_ctx, keylog_callback);
    }
  }

  if (util::generate_secret(config.static_secret.data(),
                            config.static_secret.size()) != 0) {
    std::cerr << "Unable to generate static secret" << std::endl;
    exit(EXIT_FAILURE);
  }
    // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;
    std::cout << "\taddr: " << addr << std::endl;
  Client c(EV_DEFAULT, ssl_ctx);
    // std::cout << "\n\t[MINH] INFO: " << __FILE__ << ": " << __func__<< "(): " << std::dec << __LINE__ << std::endl;

  if (run(c, addr, port) != 0) {
    exit(EXIT_FAILURE);
  }

  return EXIT_SUCCESS;
}
