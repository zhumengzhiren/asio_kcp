#ifndef _ASIO_KCP_MULTICAST_CLIENT_
#define _ASIO_KCP_MULTICAST_CLIENT_

#include <stdint.h>
#include <string>
#include <sys/types.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <map>
#include <memory>
#include <atomic>
#include <functional>
#include "mutex.h"

namespace asio_kcp {

// 组播消息回调函数类型
typedef std::function<void(uint32_t /*group_id*/, const std::string& /*msg*/)> multicast_message_callback_t;

// 组播客户端类，用于接收组播消息
class kcp_multicast_client
{
public:
    kcp_multicast_client();
    ~kcp_multicast_client();

    // 加入组播组
    // multicast_addr: 组播地址 (例如 "239.255.0.1")
    // port: 组播端口
    // group_id: 自定义的组ID，由服务端告知
    // 返回值: 成功返回0，失败返回负数错误码
    int join_group(const std::string& multicast_addr, uint16_t port, uint32_t group_id);

    // 离开组播组
    // 返回值: 成功返回0，失败返回负数错误码
    int leave_group(uint32_t group_id);

    // 设置消息回调，当收到组播消息时调用
    void set_message_callback(const multicast_message_callback_t& cb);

    // 启动接收线程
    bool start();

    // 停止接收
    void stop();

    // 发送ACK确认消息（用于可靠组播）
    void send_ack(uint32_t group_id, uint32_t seq);

private:
    // 组播组信息
    struct GroupInfo
    {
        std::string multicast_addr;   // 组播地址
        uint16_t port;                // 组播端口
        int socket_fd;                // 套接字描述符
        uint32_t last_seq;            // 最后收到的序列号
        
        GroupInfo() : port(0), socket_fd(-1), last_seq(0) {}
    };

    // 接收线程函数
    void receive_thread_func();

    // 处理收到的组播消息
    void handle_multicast_message(uint32_t group_id, const std::string& msg);

    // 处理可靠组播消息
    void handle_reliable_message(uint32_t group_id, const char* data, size_t len);

private:
    std::map<uint32_t, GroupInfo> groups_;    // 组播组信息映射表
    std::atomic<bool> running_;               // 运行标志
    std::atomic<bool> thread_running_;        // 线程运行标志
    pthread_t receive_thread_;                // 接收线程
    multicast_message_callback_t msg_callback_; // 消息回调函数
    asio_kcp::mutex mutex_;                   // 互斥锁
};

} // namespace asio_kcp

#endif // _ASIO_KCP_MULTICAST_CLIENT_ 