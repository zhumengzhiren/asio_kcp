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

// 性能统计
struct PerfStats {
    std::atomic<uint64_t> sent_msgs{0};
    std::atomic<uint64_t> sent_bytes{0};
    std::atomic<uint64_t> recv_msgs{0};
    std::atomic<uint64_t> recv_bytes{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_report_time;
    
    void reset() {
        sent_msgs = 0;
        sent_bytes = 0;
        recv_msgs = 0;
        recv_bytes = 0;
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
        
        double send_rate = s_msgs / seconds;
        double send_bw = (s_bytes * 8) / (seconds * 1000000); // Mbps
        double recv_rate = r_msgs / seconds;
        double recv_bw = (r_bytes * 8) / (seconds * 1000000); // Mbps
        
        std::cout << "Sent: " << s_msgs << " msgs (" << send_rate << " msgs/s, " << send_bw << " Mbps), "
                  << "Recv: " << r_msgs << " msgs (" << recv_rate << " msgs/s, " << recv_bw << " Mbps)"
                  << std::endl;
        
        sent_msgs = 0;
        sent_bytes = 0;
        recv_msgs = 0;
        recv_bytes = 0;
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
            // 接收到消息（组播消息）
            g_perf_stats.recv_msgs++;
            g_perf_stats.recv_bytes += msg.size();
            break;
            
        default:
            break;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: multicast_client <local_port> <server_ip> <server_port> [msg_size_bytes] [send_interval_ms]\n";
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
    
    if (report_thread.joinable()) {
        report_thread.join();
    }
    
    return 0;
} 