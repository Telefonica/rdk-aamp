/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include <PacketSender.hpp>
#include <TtmlPacket.hpp>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>

using DataBuffer = std::vector<uint8_t>;

void serverThread(const char *socket_path)
{
    int epoll = ::epoll_create(10);
    int serversock = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    if (NULL != socket_path)
        (void) std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
    else
        (void) std::strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
    if (-1 == ::bind(serversock, (struct sockaddr*)&addr, sizeof(addr)))
    {
        printf("Couldn't bind server at %s\n", addr.sun_path);
        return;
    }
    listen(serversock, 1);
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = serversock;
    epoll_ctl(epoll, EPOLL_CTL_ADD, serversock, &event);
    
    while(1)
    {
        struct epoll_event ev;
        printf("Waiting for client message. Epoll %d serversock %d\n", epoll, serversock);
        int nfd = epoll_wait(epoll, &ev, 1, 10000);
        if (nfd > 0 && event.data.fd == serversock)
        {
            printf("Message received nfd %d fd %d serversock %d\n", nfd, ev.data.fd, serversock);
            std::uint32_t type, counter, size;
            std::uint32_t data[1024];
            int rd;
            do {
                rd = recv(ev.data.fd, data, sizeof(data), 0);
                printf("Read data %d\n", rd);
            } while (false);
            type = data[0];
            counter = data[1];
            size = data[2];
            printf("Packet received: type %d counter %d size %d\n", type, counter, size);
        }
    }
}

int main(int argc, char *argv[])
{    
    int opt;
    bool test_thread = false, ret, send_reset = false;
    std::thread th;
    const char *path = NULL;
    
    while ((opt = getopt(argc, argv, "s:tr")) != -1)
    {
        switch(opt) {
        case 't':
            test_thread = true;
            break;
        case 's':
            path = optarg;
            printf("path is %s\n", path);
            break;
        case 'r':
            send_reset = true;
            break;
        default:
            break;    
        }
    }
    

    if (test_thread)
    {
        if (path)
            remove(path);
        else
            remove(SOCKET_PATH);

        printf("Starting thread\n");
        th = std::thread(serverThread, path);
        sleep(2);        
    }
    
    TtmlChannel *channel = new TtmlChannel();
    std::vector<uint8_t> data = {'a', 'b', 'c'};
    PacketSender *sender = PacketSender::Instance();

    ret = sender->Init();
	if (ret)
	{
		sender->AddPacket(channel->generateResetAllPacket());
		sender->SendPackets();
        if (!send_reset)
        {
            sender->AddPacket(channel->generateResetChannelPacket());
            sender->AddPacket(channel->generateSelectionPacket(1280, 720));
            sender->AddPacket(channel->generateDataPacket(data));
            sleep(1);
            sender->SendPackets();
            sleep(5);            
        }
	}
    
    if (test_thread) th.join();
    
    return 0;
}