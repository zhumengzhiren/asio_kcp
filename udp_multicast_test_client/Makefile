CXX = g++
CXXFLAGS = -std=c++11 -Wall -g -I../client_lib -I../essential
LDFLAGS = -L../client_lib -pthread -lrt
LIBS = -lkcp_client -lkcp_multicast_client

all: multicast_client

multicast_client: multicast_client.o
	$(CXX) -o $@ $< $(LDFLAGS) $(LIBS)

multicast_client.o: multicast_client.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o multicast_client 