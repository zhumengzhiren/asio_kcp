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
std::string g_multicast_address;
uint16_t g_multicast_port = 0;
bool g_reliable_multicast = false;

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
                
                // 告知客户端组播信息
                if (g_multicast_group_id != 0) {
                    std::string multicast_info = "MULTICAST:" + g_multicast_address + ":" + 
                                                std::to_string(g_multicast_port) + ":" + 
                                                std::to_string(g_multicast_group_id);
                    auto info_msg = std::make_shared<std::string>(multicast_info);
                    g_server->send_msg(conv, info_msg);
                    std::cout << "Sent multicast info to client " << conv << std::endl;
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
            }
            break;
        }
        case kcp_svr::eRcvMsg: {
            if (msg) {
                // 更新性能统计信息
                g_perf_stats.total_msgs++;
                g_perf_stats.total_bytes += msg->size();
                
                // 收到消息后，可选地转发到组播组
                if (g_multicast_group_id != 0 && msg->find("echo:") == 0) {
                    std::string echo_msg = msg->substr(5); // 去掉"echo:"前缀
                    auto reply = std::make_shared<std::string>(echo_msg);
                    
                    if (g_reliable_multicast) {
                        g_server->send_reliable_msg_to_multicast_group(g_multicast_group_id, reply);
                        std::cout << "Sent reliable multicast message, size: " << echo_msg.size() << std::endl;
                    } else {
                        g_server->send_msg_to_multicast_group(g_multicast_group_id, reply);
                        std::cout << "Sent multicast message, size: " << echo_msg.size() << std::endl;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void print_usage() {
    std::cout << "Usage: multicast_server <address> <port> [multicast_address] [multicast_port] [reliable=0|1]\n";
    std::cout << "Example: multicast_server 0.0.0.0 12345 239.255.0.1 30000 1\n";
    std::cout << "If multicast_address and multicast_port are not provided, random ones will be assigned.\n";
    std::cout << "reliable=1 means using reliable multicast with acknowledgments and retransmissions.\n";
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            print_usage();
            return 1;
        }

        std::string address = argv[1];
        std::string port = argv[2];
        
        // 可选的组播地址和端口
        std::string multicast_addr;
        uint16_t multicast_port = 0;
        
        if (argc >= 5) {
            multicast_addr = argv[3];
            multicast_port = static_cast<uint16_t>(std::stoi(argv[4]));
        }
        
        // 是否使用可靠组播
        if (argc >= 6) {
            g_reliable_multicast = (std::string(argv[5]) == "1");
        }

        // 初始化IO service
        boost::asio::io_service io_service;
        
        // 创建服务器
        g_server = std::make_unique<kcp_svr::server>(io_service, address, port);
        g_server->set_callback(event_callback);
        
        // 创建组播组
        g_multicast_group_id = g_server->create_multicast_group(multicast_addr, multicast_port);
        
        if (g_multicast_group_id == 0) {
            std::cerr << "Failed to create multicast group\n";
            return 1;
        }
        
        // 获取组播组信息
        std::string group_info = g_server->get_multicast_group_info(g_multicast_group_id);
        std::cout << "Created multicast group:\n" << group_info << std::endl;
        
        // 解析组播地址和端口，用于通知客户端
        size_t addr_pos = group_info.find("Multicast Address: ");
        size_t port_pos = group_info.find("Port: ");
        
        if (addr_pos != std::string::npos && port_pos != std::string::npos) {
            addr_pos += 19; // "Multicast Address: " 的长度
            size_t addr_end = group_info.find("\n", addr_pos);
            g_multicast_address = group_info.substr(addr_pos, addr_end - addr_pos);
            
            port_pos += 6; // "Port: " 的长度
            size_t port_end = group_info.find("\n", port_pos);
            g_multicast_port = std::stoi(group_info.substr(port_pos, port_end - port_pos));
            
            std::cout << "Using multicast address: " << g_multicast_address 
                      << ", port: " << g_multicast_port 
                      << ", group ID: " << g_multicast_group_id
                      << ", reliable: " << (g_reliable_multicast ? "yes" : "no") << std::endl;
        }
        
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