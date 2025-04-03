#include "multicast_manager.hpp"
#include "connection_manager.hpp"
#include "asio_kcp_log.hpp"

namespace kcp_svr
{
    MulticastManager::MulticastManager() : next_group_id_(1)
    {
    }

    MulticastManager::~MulticastManager()
    {
    }

    uint32_t MulticastManager::create_group()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t group_id = next_group_id_++;
        group_members_[group_id] = std::set<kcp_conv_t>();
        return group_id;
    }

    bool MulticastManager::add_member_to_group(uint32_t group_id, kcp_conv_t conv)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = group_members_.find(group_id);
        if (it == group_members_.end())
        {
            KCP_LOG_INFO("Group " << group_id << " not found when adding member " << conv);
            return false;
        }
        
        it->second.insert(conv);
        KCP_LOG_INFO("Added member " << conv << " to group " << group_id);
        return true;
    }

    bool MulticastManager::remove_member_from_group(uint32_t group_id, kcp_conv_t conv)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = group_members_.find(group_id);
        if (it == group_members_.end())
        {
            KCP_LOG_INFO("Group " << group_id << " not found when removing member " << conv);
            return false;
        }
        
        size_t erased = it->second.erase(conv);
        if (erased > 0)
        {
            KCP_LOG_INFO("Removed member " << conv << " from group " << group_id);
            return true;
        }
        
        KCP_LOG_INFO("Member " << conv << " not found in group " << group_id);
        return false;
    }

    bool MulticastManager::delete_group(uint32_t group_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t erased = group_members_.erase(group_id);
        if (erased > 0)
        {
            KCP_LOG_INFO("Deleted group " << group_id);
            return true;
        }
        
        KCP_LOG_INFO("Group " << group_id << " not found when deleting");
        return false;
    }

    void MulticastManager::send_to_group(uint32_t group_id, const std::string& msg)
    {
        // First, get the list of members
        std::set<kcp_conv_t> members;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = group_members_.find(group_id);
            if (it == group_members_.end())
            {
                KCP_LOG_INFO("Group " << group_id << " not found when sending message");
                return;
            }
            members = it->second; // Copy to avoid locking during send
        }
        
        // Then get the connection manager
        auto conn_mgr = connection_manager_.lock();
        if (!conn_mgr)
        {
            KCP_LOG_INFO("Connection manager unavailable when sending to group " << group_id);
            return;
        }
        
        // Send to each member
        size_t sent_count = 0;
        for (auto conv : members)
        {
            auto conn = conn_mgr->get_connection_by_conv(conv);
            if (conn)
            {
                conn->send_msg(msg);
                sent_count++;
            }
            else
            {
                KCP_LOG_INFO("Failed to find connection for conv " << conv << " in group " << group_id);
            }
        }
        
        KCP_LOG_INFO("Sent message to " << sent_count << "/" << members.size() << " members in group " << group_id);
    }

    void MulticastManager::set_connection_manager(std::weak_ptr<ConnectionManager> conn_mgr)
    {
        connection_manager_ = conn_mgr;
    }
} 