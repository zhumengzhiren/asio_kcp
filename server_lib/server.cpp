#include "server.hpp"
#include <boost/bind.hpp>
#include <signal.h>
#include <cstdlib>

#include "../essential/utility/strutil.h"
#include "connection_manager.hpp"
#include "udp_multicast_manager.hpp"


namespace kcp_svr {

server::server(boost::asio::io_service& io_service, const std::string& address, const std::string& port)
  : io_service_(io_service),
    connection_manager_ptr_(new connection_manager(io_service_, address, std::atoi(port.c_str()))),
    multicast_manager_ptr_(new UdpMulticastManager(io_service_))
{
}

void server::stop()
{
    // 停止组播管理器
    if (multicast_manager_ptr_)
    {
        multicast_manager_ptr_->stop();
    }

    // The server is stopped by cancelling all outstanding asynchronous
    // operations. Once all operations have finished the io_service::run() call
    // will exit.
    connection_manager_ptr_->stop_all();

    // todo: when running UdpPacketHandler in work thread pool
    // change to this:
    //     connection_manager_.stop_recv();
    //     connection_manager_.stop_packet_handler_workthread();
    //     connection_manager_.close();
}

void server::set_callback(const std::function<event_callback_t>& func)
{
    connection_manager_ptr_->set_callback(func);
}

void server::force_disconnect(const kcp_conv_t& conv)
{
    connection_manager_ptr_->force_disconnect(conv);
}


int server::send_msg(const kcp_conv_t& conv, std::shared_ptr<std::string> msg)
{
    return connection_manager_ptr_->send_msg(conv, msg);
}

// UDP组播功能实现
uint32_t server::create_multicast_group(const std::string& multicast_addr, uint16_t port)
{
    return multicast_manager_ptr_->create_group(multicast_addr, port);
}

bool server::delete_multicast_group(uint32_t group_id)
{
    return multicast_manager_ptr_->delete_group(group_id);
}

void server::send_msg_to_multicast_group(uint32_t group_id, std::shared_ptr<std::string> msg)
{
    if (msg && !msg->empty())
    {
        multicast_manager_ptr_->send_to_group(group_id, *msg);
    }
}

void server::send_reliable_msg_to_multicast_group(uint32_t group_id, std::shared_ptr<std::string> msg)
{
    if (msg && !msg->empty())
    {
        multicast_manager_ptr_->send_reliable_to_group(group_id, *msg);
    }
}

std::string server::get_multicast_group_info(uint32_t group_id)
{
    return multicast_manager_ptr_->get_group_info(group_id);
}

} // namespace kcp_svr
