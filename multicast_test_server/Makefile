CXX = g++
CXXFLAGS = -std=c++11 -Wall -g -I../server_lib -I../essential -I../third_party/g2log/src -I../third_party/muduo
LDFLAGS = -L../server_lib -pthread -lrt
LIBS = -lasio_kcp_server -lboost_system

# Boost相关设置 - 根据你的环境修改
BOOST_LIB_PATH ?= /usr/lib/x86_64-linux-gnu
BOOST_INC_PATH ?= /usr/include/boost

all: multicast_server

multicast_server: multicast_server.o
	$(CXX) -o $@ $< $(LDFLAGS) -L$(BOOST_LIB_PATH) $(LIBS)

multicast_server.o: multicast_server.cpp
	$(CXX) $(CXXFLAGS) -I$(BOOST_INC_PATH) -c $< -o $@

clean:
	rm -f *.o multicast_server 