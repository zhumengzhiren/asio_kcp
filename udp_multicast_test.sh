#!/bin/bash

# 定义颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印标题
echo -e "${BLUE}===== KCP真实UDP组播性能测试 =====${NC}"

# 确保目录存在
mkdir -p udp_multicast_test_server
mkdir -p udp_multicast_test_client

# 更新Boost路径（根据你的环境修改）
echo -e "${YELLOW}检测Boost路径...${NC}"
BOOST_LIB_PATH=""
BOOST_INC_PATH=""

# 尝试几个常见位置
for lib_path in /usr/lib/x86_64-linux-gnu /usr/local/lib /opt/local/lib /usr/lib; do
    if [ -d "$lib_path" ] && [ -f "$lib_path/libboost_system.so" ] || [ -f "$lib_path/libboost_system.a" ]; then
        BOOST_LIB_PATH=$lib_path
        break
    fi
done

for inc_path in /usr/include/boost /usr/local/include/boost /opt/local/include/boost; do
    if [ -d "$inc_path" ]; then
        BOOST_INC_PATH=${inc_path%"/boost"}
        break
    fi
done

if [ -z "$BOOST_LIB_PATH" ] || [ -z "$BOOST_INC_PATH" ]; then
    echo -e "${RED}无法自动检测Boost路径，请在脚本中手动设置${NC}"
    exit 1
fi

echo -e "${GREEN}找到Boost库路径: $BOOST_LIB_PATH${NC}"
echo -e "${GREEN}找到Boost头文件路径: $BOOST_INC_PATH${NC}"

# 更新allmake.sh中的Boost路径
echo -e "${YELLOW}更新allmake.sh中的Boost路径...${NC}"
sed -i "s|BOOST_LIB_PATH=.*|BOOST_LIB_PATH=$BOOST_LIB_PATH|g" allmake.sh
sed -i "s|BOOST_INC_PATH=.*|BOOST_INC_PATH=$BOOST_INC_PATH|g" allmake.sh

# 编译基础库
echo -e "${YELLOW}编译asio_kcp基础库...${NC}"
. allmake.sh

# 编译客户端组播库
echo -e "${YELLOW}编译客户端组播库...${NC}"
cd client_lib
make kcp_multicast_client.o
ar rcs libkcp_multicast_client.a kcp_multicast_client.o
cd ..

# 编译测试程序
echo -e "${YELLOW}编译UDP组播测试程序...${NC}"
cd udp_multicast_test_server
make clean && make BOOST_LIB_PATH=$BOOST_LIB_PATH BOOST_INC_PATH=$BOOST_INC_PATH
cd ../udp_multicast_test_client
make clean && make
cd ..

echo -e "${GREEN}编译完成!${NC}"
echo ""
echo -e "${BLUE}===== 使用说明 =====${NC}"
echo "1. 首先在一个终端启动服务器："
echo "   cd udp_multicast_test_server && ./multicast_server 0.0.0.0 12345 239.255.0.1 30000 1"
echo ""
echo "2. 然后在多个终端启动客户端："
echo "   cd udp_multicast_test_client && ./multicast_client 23456 127.0.0.1 12345 1024 100"
echo "   cd udp_multicast_test_client && ./multicast_client 23457 127.0.0.1 12345 1024 100"
echo "   cd udp_multicast_test_client && ./multicast_client 23458 127.0.0.1 12345 1024 100"
echo ""
echo "3. 参数说明："
echo "   服务器: <绑定地址> <端口> [组播地址] [组播端口] [可靠模式=0|1]"
echo "   客户端: <本地端口> <服务器IP> <服务器端口> [消息大小(字节)] [发送间隔(毫秒)]"
echo ""
echo "4. 性能测试建议："
echo "   - 增加客户端数量测试多客户端场景"
echo "   - 调整消息大小（如1KB, 4KB, 16KB等）"
echo "   - 调整发送频率（从10ms到500ms）"
echo "   - 对比可靠组播和普通组播的性能差异"
echo "   - 尝试在不同网络环境（LAN, WAN, WiFi, 4G等）测试"
echo ""
echo -e "${GREEN}祝测试愉快!${NC}" 