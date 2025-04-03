#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <boost/asio.hpp>
#include "../server_lib/server.hpp"

// 用于统计性能
struct PerfStats {
    std::atomic<uint64_t> total_msgs{0};
    std::atomic<uint64_t> total_bytes{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_report_time;
    
    void reset() {
        total_msgs = 0;
        total_bytes = 0;
        start_time = std::chrono::steady_clock::now();
        last_report_time = start_time;
    }
    
    void report() {
        auto now = std::chrono::steady_clock::now();
        double seconds = std::chrono::duration<double>(now - last_report_time).count();
        uint64_t msgs = total_msgs.load();
        uint64_t bytes = total_bytes.load();
        
        double msg_rate = msgs / seconds;
        double bandwidth_mbps = (bytes * 8) / (seconds * 1000000);
        
        std::cout << "Messages: " << msgs << " in " << seconds << "s (" 
                  << msg_rate << " msgs/s), Bandwidth: " << bandwidth_mbps << " Mbps" << std::endl;
        
        total_msgs = 0;
        total_bytes = 0;
        last_report_time = now;
    }
};

// 全局性能统计对象
PerfStats g_perf_stats;

// 全局变量
std::map<kcp_conv_t, std::string> g_client_info;
std::mutex g_client_mutex;
uint32_t g_multicast_group_id = 0;

// 全局服务器对象
std::unique_ptr<kcp_svr::server> g_server;

// 事件回调函数
void event_callback(kcp_conv_t conv, kcp_svr::eEventType event_type, std::shared_ptr<std::string> msg) {
    switch (event_type) {
        case kcp_svr::eConnect: {
            {
                std::lock_guard<std::mutex> lock(g_client_mutex);
                std::string client_id = msg ? *msg : "unknown";
                g_client_info[conv] = client_id;
                std::cout << "Client connected: " << conv << " - " << client_id << std::endl;
                
                // 将新客户端添加到组播组
                if (g_multicast_group_id != 0) {
                    g_server->add_to_multicast_group(g_multicast_group_id, conv);
                    std::cout << "Added client " << conv << " to multicast group " << g_multicast_group_id << std::endl;
                }
            }
            break;
        }
        case kcp_svr::eDisconnect: {
            {
                std::lock_guard<std::mutex> lock(g_client_mutex);
                std::string client_id = g_client_info[conv];
                g_client_info.erase(conv);
                std::cout << "Client disconnected: " << conv << " - " << client_id << std::endl;
                
                // 从组播组移除客户端
                if (g_multicast_group_id != 0) {
                    g_server->remove_from_multicast_group(g_multicast_group_id, conv);
                    std::cout << "Removed client " << conv << " from multicast group " << g_multicast_group_id << std::endl;
                }
            }
            break;
        }
        case kcp_svr::eRcvMsg: {
            if (msg) {
                // 更新性能统计信息
                g_perf_stats.total_msgs++;
                g_perf_stats.total_bytes += msg->size();
                
                // 收到消息后，可选地回显到组播组
                if (g_multicast_group_id != 0 && msg->find("echo:") == 0) {
                    std::string echo_msg = msg->substr(5); // 去掉"echo:"前缀
                    auto reply = std::make_shared<std::string>(echo_msg);
                    g_server->send_msg_to_group(g_multicast_group_id, reply);
                }
            }
            break;
        }
        default:
            break;
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: multicast_server <address> <port>\n";
            return 1;
        }

        std::string address = argv[1];
        std::string port = argv[2];

        // 初始化IO service
        boost::asio::io_service io_service;
        
        // 创建服务器
        g_server = std::make_unique<kcp_svr::server>(io_service, address, port);
        g_server->set_callback(event_callback);
        
        // 创建组播组
        g_multicast_group_id = g_server->create_multicast_group();
        std::cout << "Created multicast group: " << g_multicast_group_id << std::endl;
        
        // 重置性能统计
        g_perf_stats.reset();
        
        // 创建定时报告线程
        std::thread report_thread([&]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                g_perf_stats.report();
            }
        });
        report_thread.detach();
        
        // 启动服务器
        std::cout << "Server started at " << address << ":" << port << std::endl;
        io_service.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
} 