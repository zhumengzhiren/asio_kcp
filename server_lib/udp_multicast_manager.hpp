#pragma once

#include <string>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <boost/asio.hpp>
#include "kcp_typedef.hpp"

namespace kcp_svr
{
    class UdpMulticastManager
    {
    public:
        // 创建组播管理器，io_service用于异步操作
        UdpMulticastManager(boost::asio::io_service& io_service);
        ~UdpMulticastManager();

        // 创建一个组播组，返回组ID
        uint32_t create_group(const std::string& multicast_addr = "", uint16_t port = 0);
        
        // 删除一个组播组
        bool delete_group(uint32_t group_id);
        
        // 发送消息到组播组
        void send_to_group(uint32_t group_id, const std::string& msg);
        
        // 发送消息到组播组，并维护可靠性（使用序列号、重传等机制）
        void send_reliable_to_group(uint32_t group_id, const std::string& msg);
        
        // 获取组播组信息
        std::string get_group_info(uint32_t group_id) const;
        
        // 停止所有组播操作
        void stop();
        
    private:
        // 组播组结构
        struct MulticastGroup {
            boost::asio::ip::udp::endpoint endpoint;      // 组播地址和端口
            boost::asio::ip::udp::socket socket;          // 用于发送的socket
            uint32_t next_seq;                            // 下一个序列号
            std::map<uint32_t, std::string> sent_msgs;    // 已发送但未确认的消息
            boost::asio::deadline_timer retransmit_timer; // 重传定时器
            
            MulticastGroup(boost::asio::io_service& io_service) 
                : socket(io_service), next_seq(0), retransmit_timer(io_service) {}
        };
        
        // 初始化组播socket
        bool init_group_socket(MulticastGroup& group, const std::string& multicast_addr, uint16_t port);
        
        // 处理重传
        void handle_retransmit(uint32_t group_id);
        
        // 处理确认接收
        void handle_ack(uint32_t group_id, uint32_t seq);
        
        // 生成一个随机未使用的组播地址和端口
        std::pair<std::string, uint16_t> generate_multicast_address();
        
    private:
        boost::asio::io_service& io_service_;
        std::map<uint32_t, std::shared_ptr<MulticastGroup>> groups_;
        uint32_t next_group_id_;
        std::mutex mutex_;
        
        // 组播地址范围
        static const std::string MULTICAST_PREFIX;
        static const uint16_t MULTICAST_PORT_MIN;
        static const uint16_t MULTICAST_PORT_MAX;
    };
} 