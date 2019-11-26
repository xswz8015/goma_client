// Copyright 2014 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_HTTP_H_
#define DEVTOOLS_GOMA_CLIENT_HTTP_H_

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#else
#include "socket_helper_win.h"
#endif

#include <json/json.h>

#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "basictypes.h"
#include "base/compiler_specific.h"
#include "compress_util.h"
#include "gtest/gtest_prod.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "google/protobuf/io/zero_copy_stream.h"
MSVC_POP_WARNING()
#include "http_util.h"
#include "lockhelper.h"
#include "luci_context.h"
#include "oauth2.h"
#include "scoped_fd.h"
#include "tls_engine.h"
#include "worker_thread_manager.h"

namespace devtools_goma {

class Descriptor;
class Histogram;
class HttpRequest;
class HttpResponse;
class HttpRPCStats;
class OAuth2AccessTokenRefreshTask;
class OneshotClosure;
class SocketFactory;

// HttpClient is a HTTP client.  It sends HttpRequest on Descriptor
// generated by SocketFactory and TLSEngineFactory, and receives
// the response in HttpResponse.
class HttpClient {
 public:
  struct Options {
    std::string dest_host_name;
    int dest_port = 0;
    std::string proxy_host_name;
    int proxy_port = 0;
    std::string extra_params;
    std::string authorization;
    std::string cookie;
    bool capture_response_header = false;
    std::string url_path_prefix;
    bool use_ssl = false;
    std::string ssl_extra_cert;
    std::string ssl_extra_cert_data;
    absl::optional<absl::Duration> ssl_crl_max_valid_duration;
    absl::Duration socket_read_timeout = absl::Seconds(1);
    absl::Duration min_retry_backoff = absl::Milliseconds(500);
    absl::Duration max_retry_backoff = absl::Seconds(5);

    OAuth2Config oauth2_config;
    std::string gce_service_account;
    std::string service_account_json_filename;
    LuciContextAuth luci_context_auth;

    bool fail_fast = false;
    absl::Duration network_error_margin;
    int network_error_threshold_percent = 30;

    // Allows throttling if this is true.
    bool allow_throttle = true;

    bool reuse_connection = true;

    bool InitFromURL(absl::string_view url);

    // Socket{Host,Port} represents where HttpClient connects.
    // If an HTTP proxy is used, the value should be the proxy's host and port.
    std::string SocketHost() const;
    int SocketPort() const;

    // Returns HTTP request-target.
    std::string RequestURL(absl::string_view path) const;

    // Returns "Host" header field.
    std::string Host() const;

    // Returns true if HttpClient is configure to use an HTTP proxy.
    bool UseProxy() const { return !proxy_host_name.empty(); }

    std::string DebugString() const;
    void ClearAuthConfig();
  };

  // Status is used for each HTTP transaction.
  // Caller can specify
  //  - timeout_should_be_http_error
  //  - timeouts.
  // The other fields are filled by HttpClient.
  // Once it is passed to HttpClient, caller should not access
  // all fields, except finished, until finished becomes true.
  struct Status {
    enum State {
      // Running state. If failed in some step, State would be kept as-is.
      // Then, caller of HttpClient can know where HttpClient failed.
      INIT,
      PENDING,
      SENDING_REQUEST,
      REQUEST_SENT,
      RECEIVING_RESPONSE,
      RESPONSE_RECEIVED,
    };
    static absl::string_view StateName(State state);
    Status();

    // HACK: to provide copy constructor of std::atomic<bool>.
    struct AtomicBool {
      std::atomic<bool> value;

      AtomicBool(bool b) : value(b) {}  // NOLINT
      AtomicBool(const AtomicBool& b) : value(b.value.load()) {}
      AtomicBool& operator=(const AtomicBool& b) {
        value = b.value.load();
        return *this;
      }
      AtomicBool& operator=(bool b) {
        value = b;
        return *this;
      }
      operator bool() const {
        return value.load();
      }
    };

    State state;

    // If true, timeout is treated as http error (default).
    bool timeout_should_be_http_error;

    // timeouts from when connection becomes ready to when start receiving
    // response.  Once start receiving response, timeout would be controlled
    // by http_client's options socket_read_timeout.
    std::deque<absl::Duration> timeouts;

    // Whether connect() was successful for this request.
    bool connect_success;

    // Whether RPC was finished or not.
    AtomicBool finished;

    // Result of RPC for CallWithAsync. OK=success, or error code.
    int err;
    std::string err_message;

    // Become false if http is disabled with failnow().
    bool enabled;

    int http_return_code;
    std::string response_header;

    // size of message on http (maybe compressed).
    size_t req_size;
    size_t resp_size;

    // size of serialized message (not compressed).
    // for now, it only for proto messages on HttpRPC.
    // TODO: set this for compressed test message or so.
    size_t raw_req_size;
    size_t raw_resp_size;

    absl::Duration throttle_time;
    absl::Duration pending_time;
    absl::Duration req_build_time;
    absl::Duration req_send_time;
    absl::Duration wait_time;
    absl::Duration resp_recv_time;
    absl::Duration resp_parse_time;

    int num_retry;
    int num_throttled;
    int num_connect_failed;

    std::string trace_id;
    std::string master_trace_id;  // master request in multi http rpc.

    std::string DebugString() const;
  };

  enum ConnectionCloseState {
    NO_CLOSE,
    NORMAL_CLOSE,
    ERROR_CLOSE,
  };

  // NetworkErrorMonitor can be attached to HttpClient.
  // When network error is detected, or network is recovered,
  // corresponding method will be called.
  // These methods will be called with under mu_ is locked
  // to be called in serial.
  class NetworkErrorMonitor {
   public:
    virtual ~NetworkErrorMonitor() {}
    // Called when http request was not succeeded.
    virtual void OnNetworkErrorDetected() = 0;
    // Called when http request was succeeded after network error started.
    virtual void OnNetworkRecovered() = 0;
  };

  // Request is a request of HTTP transaction.
  class Request {
   public:
    Request();
    virtual ~Request();

    void Init(const std::string& method,
              const std::string& path,
              const Options& options);

    void SetMethod(const std::string& method);
    void SetRequestPath(const std::string& path);
    const std::string& request_path() const { return request_path_; }
    void SetHost(const std::string& host);
    void SetContentType(const std::string& content_type);
    void SetAuthorization(const std::string& authorization);
    void SetCookie(const std::string& cookie);
    void AddHeader(const std::string& key, const std::string& value);

    // Clone returns clone of this Request.
    virtual std::unique_ptr<Request> Clone() const = 0;

    // Returns stream of the request message.
    virtual std::unique_ptr<google::protobuf::io::ZeroCopyInputStream>
      NewStream() const = 0;

   protected:
    // CreateHeader creates a header line.
    static std::string CreateHeader(absl::string_view key,
                                    absl::string_view value);

    // BuildHeader creates HTTP request message with additional headers.
    // If content_length >= 0, set Content-Length: header.
    // If content_length < 0, header should include
    // "Transfer-Encoding: chunked" and NewStream should provide
    // chunked-body.
    std::string BuildHeader(const std::vector<std::string>& headers,
                            int content_length) const;

   private:
    std::string method_;
    std::string request_path_;
    std::string host_;
    std::string content_type_;
    std::string authorization_;
    std::string cookie_;
    std::vector<std::string> headers_;

    DISALLOW_ASSIGN(Request);
  };

  // Response is a response of HTTP transaction.
  class Response {
   public:
    // Body receives http response body.
    // Body parses Transfer-Encoding (i.e. chunked),
    // and Content-Encoding (e.g. deflate).
    class Body {
     public:
      enum class State {
        Error = -1,
        Ok = 0,
        Incomplete = 1,
      };
      Body() = default;
      virtual ~Body() = default;

      // Next obtains a buffer into which data can be written.
      // Any data written into this buffer will be parsed accoding to
      // Tarnsfer-Encoding and Content-Encoding.
      // Ownership of buffer remains to the Body, and the buffer remains
      // valid until some other method of Body is called or
      // Body is destroyed.
      // Different from ZeroCopyOutputStream, *body_size never be 0.
      virtual void Next(char** buf, int* buf_size) = 0;

      // Process processes data stored in the buffer returned by the
      // last Next call at most data_size bytes.
      // Data in the buffer after data_size bytes will be ignored,
      // and may be reused in the next Next call (or not).
      // If data_size == 0, it means EOF.
      // If data_size is negative, it means error and must return
      // State::Error.
      virtual State Process(int data_size) = 0;

      // Returns the total number of bytes written.
      virtual size_t ByteCount() const = 0;
    };

    Response();
    virtual ~Response();

    bool HasHeader() const;
    absl::string_view Header() const;

    // HttpClient will use the following methods to receive HTTP response.
    void SetRequestPath(const std::string& path);
    void SetTraceId(const std::string& trace_id);
    void Reset();

    // NextBuffer returns a buffer pointer and buffer's size.
    // Received data should be filled in buf[0..buf_size), and call
    // Recv with number data received in the buffer.
    void NextBuffer(char** buf, int* buf_size);

    // Recv receives r bytes in the buffer specified by NextBuffer().
    // Returns true if all HTTP response is received so ready to parse.
    // Returns false if more data is needed to parse response.
    bool Recv(int r);

    // Parse parses a HTTP response message.
    void Parse();

    // Number of bytes already received.
    size_t total_recv_len() const { return total_recv_len_; }

    // status_code reports HTTP status code.
    int status_code() const { return status_code_; }

    // result reports transaction results. OK or FAIL.
    int result() const { return result_; }
    const std::string& err_message() const { return err_message_; }

    // represents whether response has 'Connection: close' header.
    bool HasConnectionClose() const;

    // returns string of the total response size if Content-Length exists
    // in HTTP header.
    // Otherwise, "unknown".
    std::string TotalResponseSize() const;

   protected:
    // ParseBody parses body.
    // if error occured, updates result_, err_message_.
    virtual void ParseBody() = 0;

    // called to initialize body_.
    // subclass must own Body.  Body should be valid until next NewBody
    // call or Response is destroyed.
    virtual Body* NewBody(
        size_t content_length, bool is_chunked,
        EncodingType encoding_type) = 0;

    int result_;
    std::string err_message_;
    std::string trace_id_;

   private:
    // Buffer is the default buffer used for receiving HTTP response.
    class Buffer {
     public:
      // Next returns a buffer pointer and buffer's size.
      // Received data should be filled in buf[0..buf_size), and call
      // Process with number data received in the buffer.
      void Next(char** buf, int* buf_size);

      // Process make buffer understand |data_size| of the buffer returned
      // by Next function is used.
      void Process(int data_size);

      // Contents returns the buffer contents.
      // Since buffer_ may have invalid area (allocated but not used),
      // we won't return a reference to |buffer_|.
      //
      // Note: returned data will become invalid if destructor or
      // Reset is called.
      absl::string_view Contents() const;

      // Reset the buffer to be used for other input.
      void Reset();

      std::string DebugString() const;

     private:
      std::string buffer_;
      size_t len_ = 0UL;
    };

    // BodyRecv receives r bytes in body.
    // Returns true if no more data needed.
    // Returns false if need more data.
    bool BodyRecv(int r);

    std::string request_path_;

    size_t total_recv_len_ = 0UL;
    size_t body_offset_ = 0UL;  // position to start response body in buffer_
    absl::optional<size_t> content_length_;  // size of the content if known.

    // buffer is the default place to put the received data.
    // when start receiving response body, received data would be written
    // to body_ instead.
    Buffer buffer_;

    // body becomes non nullptr when start receiving response body.
    Body *body_ = nullptr;

    int status_code_ = 0;

    DISALLOW_COPY_AND_ASSIGN(Response);
  };


  static std::unique_ptr<SocketFactory> NewSocketFactoryFromOptions(
      const Options& options);
  static std::unique_ptr<TLSEngineFactory> NewTLSEngineFactoryFromOptions(
      const Options& options);

  // HttpClient is a http client to a specific server.
  // Takes ownership of socket_factory and tls_engine_factory.
  // It doesn't take ownership of wm.
  HttpClient(std::unique_ptr<SocketFactory> socket_factory,
             std::unique_ptr<TLSEngineFactory> tls_engine_factory,
             const Options& options,
             WorkerThreadManager* wm);
  ~HttpClient();

  // Initializes Request for method and path.
  void InitHttpRequest(Request* req,
                       const std::string& method,
                       const std::string& path) const;

  // Do performs a HTTP transaction.
  // Caller have ownership of req, resp and status.
  // This is synchronous call.
  void Do(const Request* req, Response* resp, Status* status)
      LOCKS_EXCLUDED(mu_);

  // DoAsync initiates a HTTP transaction.
  // Caller have ownership of req, resp and status, until callback is called
  // (if callback is not NULL) or status->finished becomes true (if callback
  // is NULL).
  void DoAsync(const Request* req,
               Response* resp,
               Status* status,
               OneshotClosure* callback) LOCKS_EXCLUDED(mu_);

  // Wait waits for a HTTP transaction initiated by DoAsync with callback=NULL.
  void Wait(Status* status);

  // Shutdown the client. all on-the-fly requests will fail.
  void Shutdown() LOCKS_EXCLUDED(mu_);
  bool shutting_down() const LOCKS_EXCLUDED(mu_);

  // ramp_up return [0, 100].
  // ramp_up == 0 means 0% of requests could be sent.
  // ramp_up == 100 means 100% of requests could be sent.
  // when !enabled(), it returns 0.
  // when enabled_from_ == 0, it returns 100.
  int ramp_up() const LOCKS_EXCLUDED(mu_);

  // IsHealthyRecently returns false if more than given percentage
  // (via options_.network_error_threshold_percent) of http requests in
  // last 3 seconds having status code other than 200.
  bool IsHealthyRecently() LOCKS_EXCLUDED(mu_);
  std::string GetHealthStatusMessage() const LOCKS_EXCLUDED(mu_);
  // Prefer to use IsHealthyRecently instead of IsHealthy to judge
  // network is healthy or not.  HTTP status could be temporarily unhealthy,
  // but we prefer to ignore the case.
  bool IsHealthy() const LOCKS_EXCLUDED(mu_);

  // Get email address to login with oauth2.
  std::string GetAccount();
  bool GetOAuth2Config(OAuth2Config* config) const;

  std::string DebugString() const LOCKS_EXCLUDED(mu_);

  void DumpToJson(Json::Value* json) const LOCKS_EXCLUDED(mu_);
  void DumpStatsToProto(HttpRPCStats* stats) const LOCKS_EXCLUDED(mu_);

  // options used to construct this client.
  // Note that oauth2_config might have been updated and differ from this one.
  // Use GetOAuth2Config above.
  const Options& options() const { return options_; }

  // Calculate next backoff duration.
  // prev_backoff_duration must be positive.
  static absl::Duration GetNextBackoff(
      const Options& option, absl::Duration prev_backoff_duration,
      bool in_error);

  // public for HttpRPC ping.
  void IncNumActive() LOCKS_EXCLUDED(mu_);
  void DecNumActive() LOCKS_EXCLUDED(mu_);
  // Provided for test that checks socket_pool status.
  // A test should wait all in-flight tasks land.
  void WaitNoActive() LOCKS_EXCLUDED(mu_);

  int UpdateHealthStatusMessageForPing(
      const Status& status,
      absl::optional<absl::Duration> round_trip_time) LOCKS_EXCLUDED(mu_);

  // NetworkErrorStartedTime return a time network error started.
  // Returns absl::nullopt if no error occurred recently.
  // The time will be set on fatal http error (302, 401, 403) and when
  // no socket in socket pool is available to connect to the host.
  // The time will be cleared when HttpClient get 2xx response.
  absl::optional<absl::Time> NetworkErrorStartedTime() const
      LOCKS_EXCLUDED(mu_);

  // Takes the ownership.
  void SetMonitor(std::unique_ptr<NetworkErrorMonitor> monitor)
      LOCKS_EXCLUDED(mu_);

 private:
  class Task;
  friend class Task;

  struct TrafficStat {
    TrafficStat();
    int read_byte;
    int write_byte;
    int query;
    int http_err;
  };
  typedef std::deque<TrafficStat> TrafficHistory;

  // NetworkErrorStatus checks the network error is continued
  // from the previous error or not.
  // Thread-unsafe, must be guarded by mutex.
  class NetworkErrorStatus {
   public:
    explicit NetworkErrorStatus(absl::Duration margin)
        : error_recover_margin_(margin) {}

    // Returns the network error started time.
    absl::optional<absl::Time> NetworkErrorStartedTime() const {
      return error_started_time_;
    }
    absl::optional<absl::Time> NetworkErrorUntil() const {
      return error_until_;
    }

    // Call this when the network access was error.
    // Returns true if a new network error is detected.
    // This will convert level trigger to edge trigger.
    bool OnNetworkErrorDetected(absl::Time now);

    // Call this when network access was not error.
    // Even this called, we keep the error until |error_until_|.
    // Returns true if the network is really recovered.
    // This will convert level trigger to edge trigger.
    bool OnNetworkRecovered(absl::Time now);

   private:
    // Timeout to recover from error.
    const absl::Duration error_recover_margin_;
    // Unset if network is not in the error state. Otherwise, time when the
    // network error has started.
    absl::optional<absl::Time> error_started_time_;
    // Even we get the 2xx http status, we consider the network is still
    // in the error state until this time.
    absl::optional<absl::Time> error_until_;
  };

  // |may_retry| is provided for initial ping.
  Descriptor* NewDescriptor() LOCKS_EXCLUDED(mu_);
  void ReleaseDescriptor(Descriptor* d, ConnectionCloseState close_state);

  absl::Duration EstimatedRecvTime(size_t bytes) LOCKS_EXCLUDED(mu_);

  std::string GetOAuth2Authorization() const;
  bool ShouldRefreshOAuth2AccessToken() const;
  void RunAfterOAuth2AccessTokenGetReady(
      WorkerThread::ThreadId thread_id,
      OneshotClosure* callback);

  void UpdateBackoffUnlocked(bool in_error) EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Returns time to wait in the queue. If returns 0, no need to wait.
  absl::Duration TryStart() LOCKS_EXCLUDED(mu_);

  void IncNumPending() LOCKS_EXCLUDED(mu_);
  void DecNumPending() LOCKS_EXCLUDED(mu_);

  // Returns randomized duration to wait in the queue on error.
  absl::Duration GetRandomizedBackoff() const;

  // return true if shutting_down or disabled.
  bool failnow() const LOCKS_EXCLUDED(mu_);

  void IncReadByte(int n) LOCKS_EXCLUDED(mu_);
  void IncWriteByte(int n) LOCKS_EXCLUDED(mu_);

  void UpdateStats(const Status& status) LOCKS_EXCLUDED(mu_);

  void UpdateTrafficHistory() LOCKS_EXCLUDED(mu_);

  void NetworkErrorDetectedUnlocked() EXCLUSIVE_LOCKS_REQUIRED(mu_);
  void NetworkRecoveredUnlocked() EXCLUSIVE_LOCKS_REQUIRED(mu_);

  void UpdateStatusCodeHistoryUnlocked() EXCLUSIVE_LOCKS_REQUIRED(mu_);
  void AddStatusCodeHistoryUnlocked(int status_code)
      EXCLUSIVE_LOCKS_REQUIRED(mu_);

  const Options options_;
  const std::unique_ptr<TLSEngineFactory> tls_engine_factory_;
  const std::unique_ptr<SocketFactory> socket_pool_;
  std::unique_ptr<OAuth2AccessTokenRefreshTask> oauth_refresh_task_;

  WorkerThreadManager* const wm_;

  mutable Lock mu_;
  ConditionVariable cond_ GUARDED_BY(mu_);  // signaled when num_active_ is 0.
  std::string health_status_ GUARDED_BY(mu_);
  bool shutting_down_ GUARDED_BY(mu_);
  std::deque<std::pair<absl::Time, int>>
      recent_http_status_code_ GUARDED_BY(mu_);
  size_t bad_status_num_in_recent_http_ GUARDED_BY(mu_);

  std::unique_ptr<NetworkErrorMonitor> monitor_ GUARDED_BY(mu_);
  // Checking network error state. When we get fatal http error
  // defined in IsFatalNetworkErrorCode(), or when no socket in socket pool is
  // available to connect to the host, we consider the network error.
  // When we get 2xx HTTP responses for specified duration, we consider
  // the network is recovered.
  // For the other error, this does not care.
  NetworkErrorStatus network_error_status_ GUARDED_BY(mu_);;

  int num_query_ GUARDED_BY(mu_);
  int num_active_ GUARDED_BY(mu_);
  int total_pending_ GUARDED_BY(mu_);
  int peak_pending_  GUARDED_BY(mu_);
  int num_pending_ GUARDED_BY(mu_);
  int num_http_retry_ GUARDED_BY(mu_);
  int num_http_throttled_ GUARDED_BY(mu_);
  int num_http_connect_failed_ GUARDED_BY(mu_);
  int num_http_timeout_ GUARDED_BY(mu_);
  int num_http_error_ GUARDED_BY(mu_);

  size_t total_write_byte_ GUARDED_BY(mu_);
  size_t total_read_byte_ GUARDED_BY(mu_);
  size_t num_writable_ GUARDED_BY(mu_);
  size_t num_readable_ GUARDED_BY(mu_);
  std::unique_ptr<Histogram> read_size_ GUARDED_BY(mu_);
  std::unique_ptr<Histogram> write_size_ GUARDED_BY(mu_);

  size_t total_resp_byte_ GUARDED_BY(mu_);
  absl::Duration total_resp_time_ GUARDED_BY(mu_);  // msec.

  int ping_http_return_code_ GUARDED_BY(mu_);
  absl::optional<absl::Duration> ping_round_trip_time_ GUARDED_BY(mu_);

  std::map<int, int> num_http_status_code_ GUARDED_BY(mu_);
  TrafficHistory traffic_history_ GUARDED_BY(mu_);
  PeriodicClosureId traffic_history_closure_id_ GUARDED_BY(mu_);
  // TODO: Either wrap |retry_backoff_| inside ThreadSafeVariable
  // or read it under |mu_| in `GetRandomizedBackoff()`.
  absl::Duration retry_backoff_;
  // if enabled_from_ is set:
  //   if now < *enabled_from: it will be disabled,
  //   if enabled_from <= now: it is in ramp up period
  // if enabled_from_ is not set: it is enabled (without checking time()).
  absl::optional<absl::Time> enabled_from_ GUARDED_BY(mu_);

  int num_network_error_ GUARDED_BY(mu_);
  int num_network_recovered_ GUARDED_BY(mu_);

  FRIEND_TEST(NetworkErrorStatusTest, Basic);
  DISALLOW_COPY_AND_ASSIGN(HttpClient);
};

// HttpRequest is a request of HTTP transaction.
class HttpRequest : public HttpClient::Request {
 public:
  HttpRequest();
  ~HttpRequest() override;

  void SetBody(const std::string& body);
  std::unique_ptr<google::protobuf::io::ZeroCopyInputStream>
    NewStream() const override;

  std::unique_ptr<HttpClient::Request> Clone() const override {
    return std::unique_ptr<HttpClient::Request>(new HttpRequest(*this));
  }

 private:
  std::string body_;

  DISALLOW_ASSIGN(HttpRequest);
};

// HttpFileUploadRequest is a request to upload a file.
class HttpFileUploadRequest : public HttpClient::Request {
 public:
  HttpFileUploadRequest() = default;
  ~HttpFileUploadRequest() override = default;

  void operator=(const HttpFileUploadRequest&) = delete;

  void SetBodyFile(std::string filename, size_t size) {
    filename_ = std::move(filename);
    size_ = size;
  }
  std::unique_ptr<google::protobuf::io::ZeroCopyInputStream> NewStream()
      const override;

  std::unique_ptr<HttpClient::Request> Clone() const override;

 private:
  std::string filename_;
  size_t size_ = 0;
};

// HttpResponse is a response of HTTP transaction.
class HttpResponse : public HttpClient::Response {
 public:
  class Body : public HttpClient::Response::Body {
   public:
    Body(size_t content_length, bool is_chunked, EncodingType encoding_type);
    ~Body() override = default;

    void Next(char** buf, int* buf_size) override;
    State Process(int data_size) override;
    size_t ByteCount() const override { return len_; }

    std::unique_ptr<google::protobuf::io::ZeroCopyInputStream>
      ParsedStream() const;

   private:
    const size_t content_length_;
    std::unique_ptr<HttpChunkParser> chunk_parser_;
    const EncodingType encoding_type_;

    // buffer_ holds receiving data.
    // each char[] has kNetworkBufSize.
    // it uses std::unique_ptr<char[]> to avoid relocation of backing array.
    // [0, len_) is processed data, chunks_ would point several areas
    // in this region.
    // [len_, end) is in last char[] in buffer_
    // returned by Next to receive body data.
    std::vector<std::unique_ptr<char[]>> buffer_;
    size_t len_ = 0;

    std::vector<absl::string_view> chunks_;
  };

  HttpResponse();
  ~HttpResponse() override;

  const std::string& parsed_body() const { return parsed_body_; }

  HttpClient::Response::Body* NewBody(
      size_t content_length, bool is_chunked,
      EncodingType encoding_type) override;

 protected:
  // ParseBody parses body.
  // if error occured, updates result_, err_message_.
  void ParseBody() override;

 private:
  std::unique_ptr<Body> response_body_;
  std::string parsed_body_;  // dechunked and uncompressed

  DISALLOW_COPY_AND_ASSIGN(HttpResponse);
};

// HttpFileDownloadResponse is a response of HTTP file downloading.
class HttpFileDownloadResponse : public HttpClient::Response {
 public:
  class Body : public HttpClient::Response::Body {
   public:
    Body(ScopedFd&& fd,
         size_t content_length, bool is_chunked, EncodingType encoding_type);
    ~Body() override = default;

    void Next(char** buf, int* buf_size) override;
    State Process(int data_size) override;
    size_t ByteCount() const override { return len_; }

   private:
    bool Write(absl::string_view data);
    State Close();

    ScopedFd fd_;
    const size_t content_length_;
    std::unique_ptr<HttpChunkParser> chunk_parser_;
    const EncodingType encoding_type_;

    char buf_[kNetworkBufSize] = {};
    size_t len_ = 0;
  };

  HttpFileDownloadResponse(std::string filename, int mode);
  ~HttpFileDownloadResponse() override;

  HttpFileDownloadResponse(const HttpFileDownloadResponse&) = delete;
  HttpFileDownloadResponse(HttpFileDownloadResponse&&) = delete;
  HttpFileDownloadResponse& operator=(const HttpFileDownloadResponse&) = delete;
  HttpFileDownloadResponse& operator=(HttpFileDownloadResponse&&) = delete;

  HttpClient::Response::Body* NewBody(
      size_t content_length, bool is_chunked,
      EncodingType encoding_type) override;

 protected:
  // ParseBody sets result_ to OK.
  void ParseBody() override;

 private:
  const std::string filename_;
  const int mode_;
  std::unique_ptr<Body> response_body_;
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_HTTP_H_
