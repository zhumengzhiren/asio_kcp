#include "udp_multicast_manager.hpp"
#include "asio_kcp_log.hpp"
#include <random>
#include <sstream>
#include <boost/bind.hpp>

namespace kcp_svr
{
    // 组播地址范围 (239.255.0.0 - 239.255.255.255 是本地管理的组播地址)
    const std::string UdpMulticastManager::MULTICAST_PREFIX = "239.255.";
    const uint16_t UdpMulticastManager::MULTICAST_PORT_MIN = 30000;
    const uint16_t UdpMulticastManager::MULTICAST_PORT_MAX = 40000;

    UdpMulticastManager::UdpMulticastManager(boost::asio::io_service& io_service)
        : io_service_(io_service), next_group_id_(1)
    {
        KCP_LOG_INFO("UDP Multicast Manager initialized");
    }

    UdpMulticastManager::~UdpMulticastManager()
    {
        stop();
    }

    uint32_t UdpMulticastManager::create_group(const std::string& multicast_addr, uint16_t port)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 生成组ID
        uint32_t group_id = next_group_id_++;
        
        // 创建组播组
        auto group = std::make_shared<MulticastGroup>(io_service_);
        
        // 如果没有指定组播地址和端口，则生成一个
        std::string mc_addr = multicast_addr;
        uint16_t mc_port = port;
        
        if (mc_addr.empty() || mc_port == 0)
        {
            auto addr_port = generate_multicast_address();
            mc_addr = addr_port.first;
            mc_port = addr_port.second;
        }
        
        // 初始化组播socket
        if (!init_group_socket(*group, mc_addr, mc_port))
        {
            KCP_LOG_ERROR("Failed to initialize multicast socket for group " << group_id);
            return 0; // 返回0表示失败
        }
        
        // 保存组
        groups_[group_id] = group;
        
        KCP_LOG_INFO("Created multicast group " << group_id << " with address " << mc_addr << ":" << mc_port);
        return group_id;
    }

    bool UdpMulticastManager::delete_group(uint32_t group_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = groups_.find(group_id);
        if (it == groups_.end())
        {
            KCP_LOG_INFO("Group " << group_id << " not found when deleting");
            return false;
        }
        
        // 停止重传定时器
        try
        {
            it->second->retransmit_timer.cancel();
        }
        catch (const std::exception& e)
        {
            KCP_LOG_ERROR("Error canceling retransmit timer: " << e.what());
        }
        
        // 关闭socket
        try
        {
            it->second->socket.close();
        }
        catch (const std::exception& e)
        {
            KCP_LOG_ERROR("Error closing socket: " << e.what());
        }
        
        // 删除组
        groups_.erase(it);
        
        KCP_LOG_INFO("Deleted multicast group " << group_id);
        return true;
    }

    void UdpMulticastManager::send_to_group(uint32_t group_id, const std::string& msg)
    {
        std::shared_ptr<MulticastGroup> group;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = groups_.find(group_id);
            if (it == groups_.end())
            {
                KCP_LOG_INFO("Group " << group_id << " not found when sending message");
                return;
            }
            group = it->second;
        }
        
        try
        {
            // 直接发送消息到组播地址
            group->socket.send_to(boost::asio::buffer(msg), group->endpoint);
            KCP_LOG_INFO("Sent " << msg.size() << " bytes to multicast group " << group_id);
        }
        catch (const std::exception& e)
        {
            KCP_LOG_ERROR("Error sending to multicast group " << group_id << ": " << e.what());
        }
    }

    void UdpMulticastManager::send_reliable_to_group(uint32_t group_id, const std::string& msg)
    {
        std::shared_ptr<MulticastGroup> group;
        uint32_t seq = 0;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = groups_.find(group_id);
            if (it == groups_.end())
            {
                KCP_LOG_INFO("Group " << group_id << " not found when sending reliable message");
                return;
            }
            group = it->second;
            
            // 获取并增加序列号
            seq = group->next_seq++;
            
            // 保存消息以便重传
            group->sent_msgs[seq] = msg;
        }
        
        try
        {
            // 构建带序列号的消息
            // 格式: [SEQ:4字节][MSG:余下字节]
            std::string seq_msg;
            seq_msg.resize(4 + msg.size());
            
            // 将序列号编码为网络字节序
            uint32_t net_seq = htonl(seq);
            std::memcpy(&seq_msg[0], &net_seq, 4);
            
            // 复制消息内容
            std::memcpy(&seq_msg[4], msg.data(), msg.size());
            
            // 发送到组播地址
            group->socket.send_to(boost::asio::buffer(seq_msg), group->endpoint);
            
            KCP_LOG_INFO("Sent reliable message (seq=" << seq << ") of " << msg.size() 
                        << " bytes to multicast group " << group_id);
            
            // 设置重传定时器
            group->retransmit_timer.expires_from_now(boost::posix_time::milliseconds(500)); // 500ms后重传
            group->retransmit_timer.async_wait(boost::bind(&UdpMulticastManager::handle_retransmit, this, group_id));
        }
        catch (const std::exception& e)
        {
            KCP_LOG_ERROR("Error sending reliable message to multicast group " << group_id << ": " << e.what());
        }
    }

    void UdpMulticastManager::handle_retransmit(uint32_t group_id)
    {
        std::shared_ptr<MulticastGroup> group;
        std::map<uint32_t, std::string> msgs_to_retransmit;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = groups_.find(group_id);
            if (it == groups_.end())
            {
                return; // 组已被删除
            }
            group = it->second;
            
            // 复制需要重传的消息
            msgs_to_retransmit = group->sent_msgs;
            
            // 如果没有消息需要重传，直接返回
            if (msgs_to_retransmit.empty())
            {
                return;
            }
        }
        
        // 重传所有未确认的消息
        for (const auto& kv : msgs_to_retransmit)
        {
            uint32_t seq = kv.first;
            const std::string& msg = kv.second;
            
            try
            {
                // 构建带序列号的消息
                std::string seq_msg;
                seq_msg.resize(4 + msg.size());
                
                uint32_t net_seq = htonl(seq);
                std::memcpy(&seq_msg[0], &net_seq, 4);
                std::memcpy(&seq_msg[4], msg.data(), msg.size());
                
                // 重传
                group->socket.send_to(boost::asio::buffer(seq_msg), group->endpoint);
                
                KCP_LOG_INFO("Retransmitted message (seq=" << seq << ") to multicast group " << group_id);
            }
            catch (const std::exception& e)
            {
                KCP_LOG_ERROR("Error retransmitting to multicast group " << group_id << ": " << e.what());
            }
        }
        
        // 重新设置定时器
        group->retransmit_timer.expires_from_now(boost::posix_time::milliseconds(500));
        group->retransmit_timer.async_wait(boost::bind(&UdpMulticastManager::handle_retransmit, this, group_id));
    }

    void UdpMulticastManager::handle_ack(uint32_t group_id, uint32_t seq)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = groups_.find(group_id);
        if (it == groups_.end())
        {
            return; // 组已被删除
        }
        
        // 从sent_msgs中移除已确认的消息
        it->second->sent_msgs.erase(seq);
        
        KCP_LOG_INFO("Acknowledged message (seq=" << seq << ") for multicast group " << group_id);
    }

    std::string UdpMulticastManager::get_group_info(uint32_t group_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = groups_.find(group_id);
        if (it == groups_.end())
        {
            return "Group not found";
        }
        
        std::ostringstream oss;
        oss << "Group ID: " << group_id << "\n"
            << "Multicast Address: " << it->second->endpoint.address().to_string() << "\n"
            << "Port: " << it->second->endpoint.port() << "\n"
            << "Pending Messages: " << it->second->sent_msgs.size();
        
        return oss.str();
    }

    void UdpMulticastManager::stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (auto& kv : groups_)
        {
            try
            {
                kv.second->retransmit_timer.cancel();
                kv.second->socket.close();
            }
            catch (const std::exception& e)
            {
                KCP_LOG_ERROR("Error stopping group " << kv.first << ": " << e.what());
            }
        }
        
        groups_.clear();
        KCP_LOG_INFO("UDP Multicast Manager stopped");
    }

    bool UdpMulticastManager::init_group_socket(MulticastGroup& group, const std::string& multicast_addr, uint16_t port)
    {
        try
        {
            // 解析组播地址
            boost::asio::ip::address multicast_address = boost::asio::ip::address::from_string(multicast_addr);
            
            // 创建组播endpoint
            group.endpoint = boost::asio::ip::udp::endpoint(multicast_address, port);
            
            // 打开socket
            group.socket.open(group.endpoint.protocol());
            
            // 设置socket选项
            group.socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));
            
            // 设置TTL (存活时间)
            group.socket.set_option(boost::asio::ip::multicast::hops(1));
            
            // 禁用本地回环
            group.socket.set_option(boost::asio::ip::multicast::enable_loopback(false));
            
            return true;
        }
        catch (const std::exception& e)
        {
            KCP_LOG_ERROR("Error initializing multicast socket: " << e.what());
            return false;
        }
    }

    std::pair<std::string, uint16_t> UdpMulticastManager::generate_multicast_address()
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> addr_dis(0, 255);
        static std::uniform_int_distribution<> port_dis(MULTICAST_PORT_MIN, MULTICAST_PORT_MAX);
        
        // 生成组播地址 239.255.X.Y
        std::string addr = MULTICAST_PREFIX + std::to_string(addr_dis(gen)) + "." + std::to_string(addr_dis(gen));
        
        // 生成端口
        uint16_t port = port_dis(gen);
        
        return std::make_pair(addr, port);
    }
    
} 