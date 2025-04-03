#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>
#include <mutex>
#include <condition_variable>
#include "../client_lib/kcp_client.hpp"
#include "../client_lib/kcp_multicast_client.hpp"

// 性能统计
struct PerfStats {
    std::atomic<uint64_t> sent_msgs{0};
    std::atomic<uint64_t> sent_bytes{0};
    std::atomic<uint64_t> recv_msgs{0};
    std::atomic<uint64_t> recv_bytes{0};
    std::atomic<uint64_t> multicast_recv_msgs{0};
    std::atomic<uint64_t> multicast_recv_bytes{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_report_time;
    
    void reset() {
        sent_msgs = 0;
        sent_bytes = 0;
        recv_msgs = 0;
        recv_bytes = 0;
        multicast_recv_msgs = 0;
        multicast_recv_bytes = 0;
        start_time = std::chrono::steady_clock::now();
        last_report_time = start_time;
    }
    
    void report() {
        auto now = std::chrono::steady_clock::now();
        double seconds = std::chrono::duration<double>(now - last_report_time).count();
        
        uint64_t s_msgs = sent_msgs.load();
        uint64_t s_bytes = sent_bytes.load();
        uint64_t r_msgs = recv_msgs.load();
        uint64_t r_bytes = recv_bytes.load();
        uint64_t mc_msgs = multicast_recv_msgs.load();
        uint64_t mc_bytes = multicast_recv_bytes.load();
        
        double send_rate = s_msgs / seconds;
        double send_bw = (s_bytes * 8) / (seconds * 1000000); // Mbps
        double recv_rate = r_msgs / seconds;
        double recv_bw = (r_bytes * 8) / (seconds * 1000000); // Mbps
        double mc_recv_rate = mc_msgs / seconds;
        double mc_recv_bw = (mc_bytes * 8) / (seconds * 1000000); // Mbps
        
        std::cout << "Sent: " << s_msgs << " msgs (" << send_rate << " msgs/s, " << send_bw << " Mbps), "
                  << "Recv: " << r_msgs << " msgs (" << recv_rate << " msgs/s, " << recv_bw << " Mbps), "
                  << "Multicast Recv: " << mc_msgs << " msgs (" << mc_recv_rate << " msgs/s, " << mc_recv_bw << " Mbps)"
                  << std::endl;
        
        sent_msgs = 0;
        sent_bytes = 0;
        recv_msgs = 0;
        recv_bytes = 0;
        multicast_recv_msgs = 0;
        multicast_recv_bytes = 0;
        last_report_time = now;
    }
};

// 全局变量
PerfStats g_perf_stats;
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_connected = false;
bool g_running = true;
int g_msg_size = 1024; // 默认消息大小：1KB
int g_send_interval_ms = 100; // 默认发送间隔：100ms

// 组播相关信息
std::string g_multicast_addr;
uint16_t g_multicast_port = 0;
uint32_t g_multicast_group_id = 0;
std::unique_ptr<asio_kcp::kcp_multicast_client> g_multicast_client;

// 生成随机消息
std::string generate_random_message(int size) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);
    
    std::string msg = "echo:"; // 添加前缀用于触发服务器的组播回显
    
    msg.reserve(size);
    for (int i = 0; i < size - 5; ++i) {
        msg += alphanum[dis(gen)];
    }
    
    return msg;
}

// 组播消息回调
void multicast_message_callback(uint32_t group_id, const std::string& msg) {
    if (group_id == g_multicast_group_id) {
        g_perf_stats.multicast_recv_msgs++;
        g_perf_stats.multicast_recv_bytes += msg.size();
    }
}

// 事件回调
void client_event_callback(kcp_conv_t conv, asio_kcp::eEventType event_type, const std::string& msg, void* var) {
    asio_kcp::kcp_client* client = static_cast<asio_kcp::kcp_client*>(var);
    
    switch (event_type) {
        case asio_kcp::eConnect:
            std::cout << "Connected to server, conv: " << conv << std::endl;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_connected = true;
            }
            g_cv.notify_all();
            break;
            
        case asio_kcp::eConnectFailed:
            std::cout << "Failed to connect: " << msg << std::endl;
            break;
            
        case asio_kcp::eDisconnect:
            std::cout << "Disconnected from server" << std::endl;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_connected = false;
                g_running = false;
            }
            g_cv.notify_all();
            break;
            
        case asio_kcp::eRcvMsg:
            // 接收到消息
            g_perf_stats.recv_msgs++;
            g_perf_stats.recv_bytes += msg.size();
            
            // 检查是否是组播信息通知
            if (msg.find("MULTICAST:") == 0) {
                // 格式: MULTICAST:addr:port:group_id
                size_t pos1 = 10; // "MULTICAST:" 的长度
                size_t pos2 = msg.find(':', pos1);
                size_t pos3 = msg.find(':', pos2 + 1);
                
                if (pos2 != std::string::npos && pos3 != std::string::npos) {
                    g_multicast_addr = msg.substr(pos1, pos2 - pos1);
                    g_multicast_port = std::stoi(msg.substr(pos2 + 1, pos3 - pos2 - 1));
                    g_multicast_group_id = std::stoi(msg.substr(pos3 + 1));
                    
                    std::cout << "Received multicast info: "
                              << "addr=" << g_multicast_addr
                              << ", port=" << g_multicast_port
                              << ", group_id=" << g_multicast_group_id << std::endl;
                    
                    // 创建组播客户端
                    if (!g_multicast_client) {
                        g_multicast_client = std::make_unique<asio_kcp::kcp_multicast_client>();
                        g_multicast_client->set_message_callback(multicast_message_callback);
                        
                        if (g_multicast_client->join_group(g_multicast_addr, g_multicast_port, g_multicast_group_id) == 0) {
                            g_multicast_client->start();
                            std::cout << "Joined multicast group " << g_multicast_group_id << std::endl;
                        } else {
                            std::cerr << "Failed to join multicast group" << std::endl;
                        }
                    }
                }
            }
            break;
            
        default:
            break;
    }
}

void print_usage() {
    std::cout << "Usage: multicast_client <local_port> <server_ip> <server_port> [msg_size_bytes] [send_interval_ms]\n";
    std::cout << "Example: multicast_client 23456 127.0.0.1 12345 1024 100\n";
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        print_usage();
        return 1;
    }
    
    int local_port = std::stoi(argv[1]);
    std::string server_ip = argv[2];
    int server_port = std::stoi(argv[3]);
    
    if (argc > 4) {
        g_msg_size = std::stoi(argv[4]);
    }
    
    if (argc > 5) {
        g_send_interval_ms = std::stoi(argv[5]);
    }
    
    std::cout << "Starting client on port " << local_port 
              << ", connecting to " << server_ip << ":" << server_port
              << ", message size: " << g_msg_size << " bytes"
              << ", send interval: " << g_send_interval_ms << "ms"
              << std::endl;
    
    // 创建KCP客户端
    asio_kcp::kcp_client client;
    
    // 设置事件回调
    client.set_event_callback(client_event_callback, &client);
    
    // 开始连接
    std::string client_id = "client_" + std::to_string(local_port);
    int ret = client.connect_async(local_port, server_ip, server_port);
    if (ret < 0) {
        std::cerr << "Failed to initiate connection, error: " << ret << std::endl;
        return 1;
    }
    
    // 等待连接成功
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_cv.wait_for(lock, std::chrono::seconds(5), []{ return g_connected; })) {
            std::cerr << "Connection timeout" << std::endl;
            return 1;
        }
    }
    
    // 重置性能统计
    g_perf_stats.reset();
    
    // 创建定时报告线程
    std::thread report_thread([]() {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            g_perf_stats.report();
        }
    });
    
    // 主循环：生成并发送消息
    while (g_running) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            if (!g_connected) {
                break;
            }
        }
        
        // 生成随机消息并发送
        std::string msg = generate_random_message(g_msg_size);
        client.send_msg(msg);
        
        // 更新统计信息
        g_perf_stats.sent_msgs++;
        g_perf_stats.sent_bytes += msg.size();
        
        // 按指定间隔发送
        std::this_thread::sleep_for(std::chrono::milliseconds(g_send_interval_ms));
        
        // 更新客户端，处理接收和发送
        client.update();
    }
    
    // 停止客户端
    client.stop();
    
    // 停止组播客户端
    if (g_multicast_client) {
        g_multicast_client->stop();
    }
    
    if (report_thread.joinable()) {
        report_thread.join();
    }
    
    return 0;
} 