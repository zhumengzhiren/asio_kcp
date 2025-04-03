#pragma once

#include <map>
#include <set>
#include <memory>
#include <mutex>
#include "kcp_typedef.hpp"
#include "connection.hpp"

namespace kcp_svr
{
    class MulticastManager
    {
    public:
        MulticastManager();
        ~MulticastManager();

        // 创建一个组播组，返回组ID
        uint32_t create_group();
        
        // 向组添加成员
        bool add_member_to_group(uint32_t group_id, kcp_conv_t conv);
        
        // 从组移除成员
        bool remove_member_from_group(uint32_t group_id, kcp_conv_t conv);
        
        // 删除一个组
        bool delete_group(uint32_t group_id);
        
        // 向组内所有成员发送消息
        void send_to_group(uint32_t group_id, const std::string& msg);
        
        // 设置连接管理器用于查找连接对象
        void set_connection_manager(std::weak_ptr<class ConnectionManager> conn_mgr);
        
    private:
        std::map<uint32_t, std::set<kcp_conv_t>> group_members_; // 组ID -> 成员列表
        uint32_t next_group_id_;
        std::mutex mutex_;
        std::weak_ptr<class ConnectionManager> connection_manager_;
    };
} 