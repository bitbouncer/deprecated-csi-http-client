//
// http_client.h
// ~~~~~~~~~~
// Copyright 2014 Svante Karlsson CSI AB (svante.karlsson at csi dot se)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <chrono>
#include <sstream>
#include <curl/curl.h>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/function.hpp>
#include <csi_http_common/csi_http.h>
#include <csi_http_common/spinlock.h>
#include <csi_http_common/header.h>
#pragma once

namespace csi {
class http_client
{
  public:
  
  // dummy implementetation
  class buffer
  {
    public:
    buffer() { _data.reserve(32*1024);  }
    void reserve(size_t sz) { _data.reserve(sz); }
    void append(const uint8_t* p, size_t sz) { _data.insert(_data.end(), p, p+sz); }
    void append(uint8_t s) { _data.push_back(s); }
    const uint8_t* data() const { return &_data[0]; }
    size_t size() const { return _data.size(); }
    void pop_back() { _data.resize(_data.size() - 1); }

    private:
    std::vector<uint8_t> _data;
  };

  class call_context
  {
    friend http_client;
    public:
    typedef boost::function <void(std::shared_ptr<call_context>)> callback;
    typedef std::shared_ptr<call_context>                         handle;

    call_context(csi::http::method_t method, const std::string& uri, const std::vector<std::string>& headers, const std::chrono::milliseconds& timeout, bool verbose = false) :
      _method(method),
      _uri(uri),
      _curl_verbose(verbose),
      _timeoutX(timeout),
      _http_result(csi::http::undefined),
      _tx_headers(headers),
      _curl_easy(NULL),
      _curl_headerlist(NULL),
      _curl_done(false) {
        {
          csi::spinlock::scoped_lock xx(s_spinlock);
          s_context_count++;
        }
        _curl_easy = curl_easy_init();
    }

    ~call_context() {
      {
        csi::spinlock::scoped_lock xx(s_spinlock);
        s_context_count--;
      }

      if (_curl_easy)
        curl_easy_cleanup(_curl_easy);
      if (_curl_headerlist)
        curl_slist_free_all(_curl_headerlist);
    }

    public:
    static size_t connection_count() { csi::spinlock::scoped_lock xx(s_spinlock); return s_context_count; }
    inline void curl_start(call_context::handle h) { _curl_shared = h; }
    inline void curl_stop() { _curl_shared.reset(); }
    inline call_context::handle curl_handle() { return _curl_shared; }
    inline int64_t milliseconds() const { std::chrono::milliseconds duration = std::chrono::duration_cast<std::chrono::milliseconds>(_end_ts - _start_ts); return duration.count(); }
    inline int64_t microseconds() const { std::chrono::microseconds duration = std::chrono::duration_cast<std::chrono::microseconds>(_end_ts - _start_ts); return duration.count(); }
    std::string tx_content() const { return _tx_stream.str(); }
    const char* rx_content() const { return _rx_buffer.size() ? (const char*) _rx_buffer.data() : ""; }
    inline size_t tx_content_length() const { return _tx_stream.str().size(); }
    inline size_t rx_content_length() const { return _rx_buffer.size(); }
    inline int    rx_kb_per_sec() const { auto sz = rx_content_length(); int64_t ms = milliseconds();  return (int) (ms == 0 ? 0 : sz / ms);  }
    inline void set_verbose(bool state) { _curl_verbose = state; }
    inline const std::string& uri() const { return _uri; }
    inline csi::http::status_type http_result() const { return _http_result; }
    inline bool transport_result() const { return _transport_ok; }
    inline bool ok() const { return _transport_ok && (_http_result >= 200) && (_http_result < 300); }
    std::string get_rx_header(const std::string& header) const;
    private:
    csi::http::method_t                   _method;
    std::string                           _uri;
    std::vector<std::string>              _tx_headers;
    std::vector<csi::http::header_t>      _rx_headers;
    std::chrono::steady_clock::time_point _start_ts;
    std::chrono::steady_clock::time_point _end_ts;
    std::chrono::milliseconds             _timeoutX;
    callback                              _callback;

    //TX
    std::stringstream                     _tx_stream; // temporary for test...
    //RX
    buffer                                _rx_buffer;

    csi::http::status_type                _http_result;
    bool                                  _transport_ok;

    //curl stuff
    CURL*                                 _curl_easy;
    curl_slist*                           _curl_headerlist;
    bool                                  _curl_done;
    handle                                _curl_shared; // used to keep object alive when only curl knows about the context
    bool                                  _curl_verbose;

    static csi::spinlock                  s_spinlock;
    static size_t                         s_context_count;
  };

  http_client(boost::asio::io_service& io_service);
  ~http_client();
  void close();
  bool done(); 


  void perform_async(call_context::handle, call_context::callback cb);
  call_context::handle perform(call_context::handle, bool verbose = false);

  protected:
  void _perform(call_context::handle);       // will be called in context of worker thread
  void _poll_remove(call_context::handle); // will be called in context of worker thread

  // CURL CALLBACKS
  curl_socket_t opensocket_cb(curlsocktype purpose, struct curl_sockaddr *address);
  int           closesocket_cb(curl_socket_t item);
  int           multi_timer_cb(CURLM *multi, long timeout_ms);
  int           sock_cb(CURL *e, curl_socket_t s, int what, void* per_socket_user_data);

  //BOOST EVENTS
  void socket_rx_cb(const boost::system::error_code& e, boost::asio::ip::tcp::socket * tcp_socket, call_context::handle context);
  void socket_tx_cb(const boost::system::error_code& e, boost::asio::ip::tcp::socket * tcp_socket, call_context::handle context);
  void timer_cb(const boost::system::error_code & error);
  void keepalivetimer_cb(const boost::system::error_code & error);
  void _asio_closesocket_cb(curl_socket_t item);

  //curl callbacks
  static int            _multi_timer_cb(CURLM *multi, long timeout_ms, void *userp);
  static int            _sock_cb(CURL *e, curl_socket_t s, int what, void *user_data, void *per_socket_user_data);
  static size_t         _write_cb(void *ptr, size_t size, size_t nmemb, void *data);
  static curl_socket_t  _opensocket_cb(void* user_data, curlsocktype purpose, struct curl_sockaddr *address);
  static int            _closesocket_cb(void* user_data, curl_socket_t item);

  void check_completed();

  boost::asio::io_service& _io_service;
  csi::spinlock            _spinlock;

  // check all platforms before change
  // boost::asio::steady_timer can compile on linux for now 1.54 ubuntu 13.10
  // using old template instead
  //typedef boost::asio::basic_waitable_timer<std::chrono::steady_clock> timer;
  //timer                                                   _keepalive_timer;
  //timer                                                   _timer;

  boost::asio::steady_timer                               _keepalive_timer;
  boost::asio::steady_timer                               _timer;

  std::map<curl_socket_t, boost::asio::ip::tcp::socket *> _socket_map;
  CURLM*                                                  _multi;
  int                                                     _still_running;
};

inline std::shared_ptr<http_client::call_context> create_http_request(csi::http::method_t method, const std::string& uri, const std::vector<std::string>& headers, const std::chrono::milliseconds& timeout, bool verbose = false) {
  std::shared_ptr<http_client::call_context> p(new http_client::call_context(method, uri, headers, timeout, verbose));
  return p;
}
}; // namespace 
