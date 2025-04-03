#include "kcp_multicast_client.hpp"
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>

namespace asio_kcp {

kcp_multicast_client::kcp_multicast_client()
    : running_(false), thread_running_(false)
{
}

kcp_multicast_client::~kcp_multicast_client()
{
    stop();
}

int kcp_multicast_client::join_group(const std::string& multicast_addr, uint16_t port, uint32_t group_id)
{
    asio_kcp::lock_guard lock(mutex_);
    
    // 检查是否已经加入该组
    if (groups_.find(group_id) != groups_.end())
    {
        std::cerr << "Already joined group " << group_id << std::endl;
        return -1;
    }
    
    // 创建UDP套接字
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0)
    {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return -2;
    }
    
    // 设置套接字选项：地址重用
    int reuse = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        std::cerr << "Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        close(sock_fd);
        return -3;
    }
    
    // 设置非阻塞模式
    int flags = fcntl(sock_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        std::cerr << "Failed to set non-blocking mode: " << strerror(errno) << std::endl;
        close(sock_fd);
        return -4;
    }
    
    // 绑定到本地地址和端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "Failed to bind: " << strerror(errno) << std::endl;
        close(sock_fd);
        return -5;
    }
    
    // 加入组播组
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(multicast_addr.c_str());
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    
    if (setsockopt(sock_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        std::cerr << "Failed to join multicast group: " << strerror(errno) << std::endl;
        close(sock_fd);
        return -6;
    }
    
    // 保存组信息
    GroupInfo group_info;
    group_info.multicast_addr = multicast_addr;
    group_info.port = port;
    group_info.socket_fd = sock_fd;
    group_info.last_seq = 0;
    
    groups_[group_id] = group_info;
    
    std::cout << "Joined multicast group " << group_id << " at " << multicast_addr << ":" << port << std::endl;
    
    return 0;
}

int kcp_multicast_client::leave_group(uint32_t group_id)
{
    asio_kcp::lock_guard lock(mutex_);
    
    auto it = groups_.find(group_id);
    if (it == groups_.end())
    {
        std::cerr << "Not in group " << group_id << std::endl;
        return -1;
    }
    
    // 离开组播组
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(it->second.multicast_addr.c_str());
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    
    if (setsockopt(it->second.socket_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        std::cerr << "Failed to leave multicast group: " << strerror(errno) << std::endl;
        // 继续执行，关闭套接字
    }
    
    // 关闭套接字
    close(it->second.socket_fd);
    
    // 移除组信息
    groups_.erase(it);
    
    std::cout << "Left multicast group " << group_id << std::endl;
    
    return 0;
}

void kcp_multicast_client::set_message_callback(const multicast_message_callback_t& cb)
{
    asio_kcp::lock_guard lock(mutex_);
    msg_callback_ = cb;
}

bool kcp_multicast_client::start()
{
    asio_kcp::lock_guard lock(mutex_);
    
    if (running_)
    {
        std::cerr << "Multicast client already running" << std::endl;
        return false;
    }
    
    running_ = true;
    
    // 创建接收线程
    int ret = pthread_create(&receive_thread_, nullptr, 
                            [](void* arg) -> void* {
                                kcp_multicast_client* client = static_cast<kcp_multicast_client*>(arg);
                                client->receive_thread_func();
                                return nullptr;
                            }, this);
    
    if (ret != 0)
    {
        std::cerr << "Failed to create receive thread: " << strerror(ret) << std::endl;
        running_ = false;
        return false;
    }
    
    std::cout << "Multicast client started" << std::endl;
    return true;
}

void kcp_multicast_client::stop()
{
    {
        asio_kcp::lock_guard lock(mutex_);
        if (!running_)
        {
            return;
        }
        
        running_ = false;
    }
    
    // 等待接收线程结束
    if (thread_running_)
    {
        pthread_join(receive_thread_, nullptr);
    }
    
    // 离开所有组播组
    asio_kcp::lock_guard lock(mutex_);
    for (auto it = groups_.begin(); it != groups_.end(); )
    {
        // 离开组播组
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(it->second.multicast_addr.c_str());
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        
        setsockopt(it->second.socket_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        
        // 关闭套接字
        close(it->second.socket_fd);
        
        // 移除组信息
        it = groups_.erase(it);
    }
    
    std::cout << "Multicast client stopped" << std::endl;
}

void kcp_multicast_client::send_ack(uint32_t group_id, uint32_t seq)
{
    asio_kcp::lock_guard lock(mutex_);
    
    auto it = groups_.find(group_id);
    if (it == groups_.end())
    {
        std::cerr << "Not in group " << group_id << " when sending ACK" << std::endl;
        return;
    }
    
    // 创建ACK消息
    // 格式: "ACK:[SEQ]"
    std::string ack_msg = "ACK:" + std::to_string(seq);
    
    // 创建目标地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(it->second.multicast_addr.c_str());
    addr.sin_port = htons(it->second.port);
    
    // 发送ACK
    sendto(it->second.socket_fd, ack_msg.c_str(), ack_msg.size(), 0,
           (struct sockaddr*)&addr, sizeof(addr));
    
    std::cout << "Sent ACK for seq " << seq << " to group " << group_id << std::endl;
}

void kcp_multicast_client::receive_thread_func()
{
    thread_running_ = true;
    
    constexpr size_t MAX_BUFFER_SIZE = 65536;
    char buffer[MAX_BUFFER_SIZE];
    
    // 创建pollfd数组
    std::vector<pollfd> poll_fds;
    std::vector<uint32_t> group_ids;
    
    while (running_)
    {
        // 更新poll_fds
        {
            asio_kcp::lock_guard lock(mutex_);
            
            poll_fds.clear();
            group_ids.clear();
            
            for (const auto& entry : groups_)
            {
                pollfd pfd;
                pfd.fd = entry.second.socket_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                
                poll_fds.push_back(pfd);
                group_ids.push_back(entry.first);
            }
        }
        
        if (poll_fds.empty())
        {
            // 没有组播组，等待一段时间
            usleep(100000); // 100ms
            continue;
        }
        
        // 等待数据
        int ret = poll(poll_fds.data(), poll_fds.size(), 100); // 超时100ms
        
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue; // 被信号中断，重试
            }
            
            std::cerr << "Poll failed: " << strerror(errno) << std::endl;
            break;
        }
        
        if (ret == 0)
        {
            continue; // 超时，继续循环
        }
        
        // 检查每个套接字
        for (size_t i = 0; i < poll_fds.size(); ++i)
        {
            if (!(poll_fds[i].revents & POLLIN))
            {
                continue; // 没有数据可读
            }
            
            uint32_t group_id = group_ids[i];
            int sock_fd = poll_fds[i].fd;
            
            // 接收数据
            struct sockaddr_in src_addr;
            socklen_t addr_len = sizeof(src_addr);
            
            ssize_t recv_len = recvfrom(sock_fd, buffer, MAX_BUFFER_SIZE, 0,
                                        (struct sockaddr*)&src_addr, &addr_len);
            
            if (recv_len < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    continue; // 没有数据，继续
                }
                
                std::cerr << "Recvfrom failed: " << strerror(errno) << std::endl;
                continue;
            }
            
            // 检查是否是ACK消息回复（我们不处理）
            if (recv_len >= 4 && memcmp(buffer, "ACK:", 4) == 0)
            {
                continue;
            }
            
            // 检查是否是带序列号的可靠消息
            if (recv_len >= 4)
            {
                uint32_t net_seq;
                memcpy(&net_seq, buffer, 4);
                uint32_t seq = ntohl(net_seq);
                
                // 处理可靠消息
                handle_reliable_message(group_id, buffer + 4, recv_len - 4);
                
                // 发送ACK确认
                send_ack(group_id, seq);
            }
            else
            {
                // 普通组播消息
                std::string msg(buffer, recv_len);
                handle_multicast_message(group_id, msg);
            }
        }
    }
    
    thread_running_ = false;
}

void kcp_multicast_client::handle_multicast_message(uint32_t group_id, const std::string& msg)
{
    multicast_message_callback_t cb;
    
    {
        asio_kcp::lock_guard lock(mutex_);
        cb = msg_callback_;
    }
    
    if (cb)
    {
        cb(group_id, msg);
    }
}

void kcp_multicast_client::handle_reliable_message(uint32_t group_id, const char* data, size_t len)
{
    std::string msg(data, len);
    
    // 更新最后收到的序列号
    {
        asio_kcp::lock_guard lock(mutex_);
        auto it = groups_.find(group_id);
        if (it != groups_.end())
        {
            // 这里可以添加序列号检查逻辑，丢弃重复的消息
            // ...
        }
    }
    
    // 调用回调
    handle_multicast_message(group_id, msg);
}

} // namespace asio_kcp 