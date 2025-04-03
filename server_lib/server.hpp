#ifndef KCP_SERVER_HPP
#define KCP_SERVER_HPP

#include <boost/asio.hpp>
#include <string>
#include <memory>
#include <boost/noncopyable.hpp>
#include "kcp_typedef.hpp"

namespace kcp_svr {

class connection_manager;
class UdpMulticastManager;


// The way of using kcp_svr::server is Reactor mode.
// Setting a event_callback func. kcp_svr will call back this func when some event finished (connect, disconnect, lag, msgrcved, .etc).
//   event_call(2342, eConnect, "user_id from client") will be called when a client connected kcp_svr successfully.
//       And this client's conv is 2342. And the user_id is given as msg.
//   event_call(2342, eRcvMsg, "text12345678") will be called when server recved a msg "text12345678" from client with conv 2342.
//   event_call(2342, eDisconnect, "") will be called when server lose connect to client with conv 2342.
//       -- default timeout time is 10 seconds. Please configure your timeout time by ASIO_KCP_CONNECTION_TIMEOUT_TIME in kcp_typedef.hpp.
//   todo: event_call(2342, eLagNotify, "") will be called when none msg recved within some milliseconds.
//       -- You can let other player in same game room show waiting UI if you want to add this logic in your handle function.
//
// Calling send_msg func if you want to send some msg.
//
// Usage detail:
//   You need provide asio::io_service. And user need call io_service.run().
//   then kcp_server will call event_callback func in loop of io_service.
//     asio:io_service io_service;
//     kcp_svr(io_service);
//     io_service.run();
//
// the developer who using kcp_svr::server must known about asio.
//   If you do not want to study the asio, you can use kcp_svr::server_asio_wrapped.
class server
  : private boost::noncopyable
{
public:
    /// Construct the server to listen on the specified TCP address and port
    explicit server(boost::asio::io_service& io_service, const std::string& address, const std::string& port);
    // ~server(); // checking the stop() function called already.

    void set_callback(const std::function<event_callback_t>& func);

    // eLagNotify return when none msg recved within mtime milliseconds.
    // eLagNotify will be not returned if you do not set this or set this 0.
    //  void set_lag_notify_time(uint32_t mtime);

    int send_msg(const kcp_conv_t& conv, std::shared_ptr<std::string> msg);
    //  int send_msg(const std::vector< kcp_conv_t conv >& /*convs*/, std::shared_ptr<std::string> msg);
    //  int send_msg_to_all();
    void force_disconnect(const kcp_conv_t& conv);

    // you must call stop before the destory of io_service or calling io_service.stop
    void stop();

    // UDP组播功能
    // 创建一个组播组，返回组ID
    uint32_t create_multicast_group(const std::string& multicast_addr = "", uint16_t port = 0);
    
    // 删除一个组播组
    bool delete_multicast_group(uint32_t group_id);
    
    // 发送消息到组播组（不可靠，无确认）
    void send_msg_to_multicast_group(uint32_t group_id, std::shared_ptr<std::string> msg);
    
    // 发送可靠消息到组播组（带序列号和确认）
    void send_reliable_msg_to_multicast_group(uint32_t group_id, std::shared_ptr<std::string> msg);
    
    // 获取组播组信息，包括地址和端口
    std::string get_multicast_group_info(uint32_t group_id);

private:
    /// The io_service used to perform asynchronous operations.
    boost::asio::io_service& io_service_; // -known

    /// The connection manager which owns all live connections.
    std::shared_ptr<connection_manager> connection_manager_ptr_;
    
    /// UDP组播管理器
    std::shared_ptr<UdpMulticastManager> multicast_manager_ptr_;
};

} // namespace kcp_svr

#endif // KCP_SERVER_HPP
