#include "gateway/soupbintcp/session.hpp"

#include "core/async_logger.hpp"
#include "gateway/soupbintcp/wire.hpp"
#include "net/tcp_client.hpp"

#include <system_error>

namespace ttt::soup {

void Session::try_connect() {
  try {
    sock_.emplace(net::TcpClient::connect(cfg_.host, cfg_.port));
  } catch (const std::system_error&) {
    schedule_retry();
    return;
  }
  rx_.reset();
  tx_.reset();
  last_rx_ns_ = last_tx_ns_ = now_ns_;
  ++stats_.connects;
  // ask for everything after the last message we applied
  put(kLoginRequest, kLoginRequestLen,
      [&](std::byte* p) { encode_login(p, cfg_.user, cfg_.password, session_, next_seq_); });
  tx_.flush(*sock_);
  state_ = SoupState::LoginSent;
  logger_.log(core::LogLevel::Info, "login sent, requesting seq {}", next_seq_);
}

} // namespace ttt::soup
