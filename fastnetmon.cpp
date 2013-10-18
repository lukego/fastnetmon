/*
 TODO:
  1) ДОбавить среднюю нагрузку за 30 секунд/минуту/5 минут, хз как ее сделать :)
  2) Добавить проверку на существование конфигам с сетями
  3) Подумать на тему выноса всех параметров в конфиг
  4) Сделать трейсер 100-200 пакетов при бане
*/


#include <pcap.h> 
#include <stdio.h> 
#include <stdlib.h> 
#include <errno.h> 
#include <sys/socket.h> 
#include <netinet/in.h> 
#include <arpa/inet.h> 
#include <netinet/if_ether.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <vector>
#include <utility>
#include <sstream>
#include <time.h>

// for boost split
#include <boost/algorithm/string.hpp>

// ULOG
#include "libipulog.h"

// Мы используем механизмы ULOG2
#define ULOG2

/*

Custom pcap:
  Install PCAP from sources: http://www.stableit.ru/2013/10/lib-pcap-debian-squeeze.html
  g++ sniffer.cpp -lpcap -I/opt/libpcap140/include -L/opt/libpcap140/lib
  g++ sniffer.cpp -Linclude
  LD_LIBRARY_PATH=/opt/libpcap140/lib ./a.out
 Pcap docs:    
   http://www.linuxforu.com/2011/02/capturing-packets-c-program-libpcap/
   http://vichargrave.com/develop-a-packet-sniffer-with-libpcap/ парсер отсюда
*/

using namespace std;
// not a good idea
//using namespace boost;

// main data structure for storing traffic data for all our IPs
typedef map <uint32_t, int> map_for_counters;
map_for_counters PacketsCounterIncoming;
map_for_counters PacketsCounterOutgoing;

map_for_counters TrafficCounterIncoming;
map_for_counters TrafficCounterOutgoing;

enum direction {INCOMING, OUTGOING, INTERNAL, OTHER};

#ifdef ULOG2
// для подсчета числа ошибок буфера при работе по netlink
int netlink_error_counter = 0;
int netlink_packets_counter = 0;
#endif

string get_direction_name(direction direction_value) {
    string direction_name; 

    switch (direction_value) {
        case INCOMING: direction_name = "incoming"; break;
        case OUTGOING: direction_name = "outgoing"; break;
        case INTERNAL: direction_name = "internal"; break;
        case OTHER:    direction_name = "other";    break;
        default:       direction_name = "unknown";  break;
    }
}

// делаем глобальной, так как нам нужно иметь к ней доступ из обработчика сигнала
pcap_t* descr;

int total_count_of_incoming_packets = 0;
int total_count_of_outgoing_packets = 0;
int total_count_of_other_packets = 0;
int total_count_of_internal_packets = 0;

int total_count_of_incoming_bytes = 0;
int total_count_of_outgoing_bytes = 0;
int total_count_of_other_bytes = 0;
int total_count_of_internal_bytes = 0;

map<uint32_t,int> ban_list;

time_t start_time;
int DEBUG = 0;

// Период, через который мы пересчитываем pps/трафик
int check_period = 3;

// Увеличиваем буфер, чтобы минимизировать потери пакетов
int pcap_buffer_size_mbytes = 10; 

// Нас не интересуют запросы IP, у которых менее XXX  pps в секунду
int threshhold = 2000;

// Баним IP, если он превысил данный порог
int ban_threshhold = 10000;

// data structure for storing data in Vector
typedef pair<uint32_t, int> pair_of_map_elements;

/* 
 Тут кроется огромный баго-фич:
  В случае прослушивания any интерфейсов мы ловим фичу-баг, вместо эзернет хидера у нас тип 113, который LINUX SLL, а следовательно размер хидера не 14, а 16 байт! 
  Если мы сниффим один интерфейсе - у нас хидер эзернет, 14 байт, а если ANY, то хидер у нас 16 !!!

 packetptr += 14; // Ethernet
 packetptr += 16; // LINUX SLL, только в случае указания any интерфейса 

 Подробнее:
  https://github.com/the-tcpdump-group/libpcap/issues/324
  http://comments.gmane.org/gmane.network.tcpdump.devel/5043
  http://www.tcpdump.org/linktypes/LINKTYPE_LINUX_SLL.html 
  https://github.com/the-tcpdump-group/libpcap/issues/163

*/

// стандартно у нас смещение для типа DLT_EN10MB, Ethernet
int DATA_SHIFT_VALUE = 14;

typedef pair<uint32_t, uint32_t> subnet;
vector<subnet> our_networks;
vector<subnet> whitelist_networks;

// prototypes
void signal_handler(int signal_number);
uint32_t convert_cidr_to_binary_netmask(int cidr);
bool belongs_to_networks(vector<subnet> networks_list, uint32_t ip);

// Function for sorting Vector of pairs
bool compare_function (pair_of_map_elements a, pair_of_map_elements b) {
    return a.second > b.second;
}

uint32_t convert_ip_as_string_to_uint(string ip) {
    struct in_addr ip_addr;
    inet_aton(ip.c_str(), &ip_addr);

    // in network byte order
    return ip_addr.s_addr;
}

string convert_ip_as_uint_to_string(uint32_t ip_as_string) {
    struct in_addr ip_addr;
    ip_addr.s_addr = ip_as_string;
    return (string)inet_ntoa(ip_addr);
}

vector<string> exec(string cmd) {
    vector<string> output_list;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return output_list;

    char buffer[256];
    std::string result = "";
    while(!feof(pipe)) {
        if(fgets(buffer, 256, pipe) != NULL) {
            size_t newbuflen = strlen(buffer);
            
            // remove newline at the end
            if (buffer[newbuflen - 1] == '\n') {
                buffer[newbuflen - 1] = '\0';
            }

            output_list.push_back(buffer);
        }
    }

    pclose(pipe);
    return output_list;
}

void draw_table(map_for_counters& my_map_packets, map_for_counters& my_map_traffic, string data_direction) {
        std::vector<pair_of_map_elements> vector_for_sort;

        /* Вобщем-то весь код ниже зависит лишь от входных векторов и порядка сортировки данных */
        for( map<uint32_t,int>::iterator ii=my_map_packets.begin(); ii!=my_map_packets.end(); ++ii) {
            // кладем все наши элементы в массив для последующей сортировки при отображении
            pair_of_map_elements current_pair;
            current_pair.first = (*ii).first;
            current_pair.second = (*ii).second;

            vector_for_sort.push_back(current_pair);
        }   
   
        std::sort( vector_for_sort.begin(), vector_for_sort.end(), compare_function);

        for( vector<pair_of_map_elements>::iterator ii=vector_for_sort.begin(); ii!=vector_for_sort.end(); ++ii) {
            int pps = (*ii).second / check_period;
            uint32_t client_ip = (*ii).first;
            string client_ip_as_string = convert_ip_as_uint_to_string((*ii).first);

            if (pps >= threshhold) {
                string pps_as_string;
                std::stringstream out;
                out << pps;
                pps_as_string = out.str();

                // еси клиента еще нету в бан листе
                if (pps > ban_threshhold) {
                    if (belongs_to_networks(whitelist_networks, client_ip)) {
                        // IP в белом списке 
                    } else {
 
                        cout<<"!!!ALARM!!! WE MUST BAN THIS IP!!! ";
                        // add IP to BAN list

                        if (ban_list.count(client_ip) == 0) {
                            ban_list[client_ip] = pps;
                            cout << "*BAN EXECUTED* ";
                            exec("echo 'Please execute reglaments and notify client' | mail -s \"Myflower Guard: IP " + client_ip_as_string  +" was locked, " + pps_as_string  + " pps/" + data_direction + "\" odintsov@fastvps.ru,hohryakov@fastvps.ru,ziltsov@fastvps.ee");
                        } else {
                            cout << "*BAN EXECUTED* ";
                            // already in ban list
                        }
                    } 
                }   

                // determine attack speed
                int bps = my_map_traffic[ (*ii).first ] / check_period;
                // convert to mbps
                int mbps = bps / 1024 / 1024 * 8;
                cout << client_ip_as_string << "\t\t" << pps << " pps " << mbps << " Mbps" << endl;
            }   
        }   
}

#include <boost/assign/std/vector.hpp>

// bring 'operator+=()' into scope
using namespace boost::assign;

bool load_our_networks_list() {
    // вносим в белый список, IP из этой сети мы не баним
    subnet white_subnet = std::make_pair(convert_ip_as_string_to_uint("159.253.17.0"), convert_cidr_to_binary_netmask(24));
    whitelist_networks.push_back(white_subnet);

    vector<string> networks_list_as_string;
    // если мы на openvz ноде, то "свои" IP мы можем получить из спец-файла в /proc
    FILE *detect_openvz_file = fopen("/proc/vz/version", "r");
    string our_networks_netmask;

    if (detect_openvz_file) {
        fclose(detect_openvz_file);
        cout<<"We found OpenVZ"<<endl;
        // тут искусствено добавляем суффикс 32
        networks_list_as_string = exec("cat /proc/vz/veip | awk '{print $1\"/32\"}' |grep -vi version |grep -v ':'");
    } 
    
    vector<string> network_list_from_config = exec("cat /etc/networks_list");
    networks_list_as_string.insert(networks_list_as_string.end(), network_list_from_config.begin(), network_list_from_config.end());

    // если это ложь, то в моих функциях косяк
    assert( convert_ip_as_string_to_uint("255.255.255.0")   == convert_cidr_to_binary_netmask(24) );
    assert( convert_ip_as_string_to_uint("255.255.255.255") == convert_cidr_to_binary_netmask(32) );

    for( vector<string>::iterator ii=networks_list_as_string.begin(); ii!=networks_list_as_string.end(); ++ii) {
        vector<string> subnet_as_string; 
        split( subnet_as_string, *ii, boost::is_any_of("/"), boost::token_compress_on );
        int cidr = atoi(subnet_as_string[1].c_str());

        uint32_t subnet_as_int  = convert_ip_as_string_to_uint(subnet_as_string[0]);
        uint32_t netmask_as_int = convert_cidr_to_binary_netmask(cidr);

        subnet current_subnet = std::make_pair(subnet_as_int, netmask_as_int);
        //current_subnet.first  = subnet_as_int;
        //current_subnet.second = netmask_as_int;

        our_networks.push_back(current_subnet);
    }
 
    return true;
}





uint32_t convert_cidr_to_binary_netmask(int cidr) {
    uint32_t binary_netmask = 0xFFFFFFFF; 
    binary_netmask = binary_netmask << ( 32 - cidr );
    // htonl from host byte order to network
    // ntohl from network byte order to host

    // поидее, на выходе тут нужен network byte order 
    return htonl(binary_netmask);
}

bool belongs_to_networks(vector<subnet> networks_list, uint32_t ip) {
    for( vector<subnet>::iterator ii=networks_list.begin(); ii!=networks_list.end(); ++ii) {

        if ( (ip & (*ii).second) == ((*ii).first & (*ii).second) ) {
            return true; 
        }
    }

    return false;
}

// в случае прямого вызова скрипта колбэка - нужно конст, напрямую в хендлере - конст не нужно
void parse_packet(u_char *user, struct pcap_pkthdr *packethdr, const u_char *packetptr) {
    struct ip* iphdr;
    struct icmphdr* icmphdr;
    struct tcphdr* tcphdr;
    struct udphdr* udphdr;
    char iphdrInfo[256], srcip_char[256], dstip_char[256];
    unsigned short id, seq;
    int packet_length;

    // Skip the datalink layer header and get the IP header fields.
    packetptr += DATA_SHIFT_VALUE;
    iphdr = (struct ip*)packetptr;

    // исходящий/входящий айпи это in_addr, http://man7.org/linux/man-pages/man7/ip.7.html
    strcpy(srcip_char, inet_ntoa(iphdr->ip_src));
    strcpy(dstip_char, inet_ntoa(iphdr->ip_dst));

    uint32_t src_ip = iphdr->ip_src.s_addr;
    uint32_t dst_ip = iphdr->ip_dst.s_addr;

    //cout<<srcip_char<<" > "<<dstip_char<<endl;

    // The ntohs() function converts the unsigned short integer netshort from network byte order to host byte order
    packet_length = ntohs(iphdr->ip_len);  

    direction packet_direction;

    if (belongs_to_networks(our_networks, src_ip) && belongs_to_networks(our_networks, dst_ip)) {
        packet_direction = INTERNAL;

        total_count_of_internal_packets ++;
        total_count_of_internal_bytes += packet_length;

    } else if (belongs_to_networks(our_networks, src_ip)) {
        packet_direction = OUTGOING;

        total_count_of_outgoing_packets ++;
        total_count_of_outgoing_bytes += packet_length;

        PacketsCounterOutgoing[ src_ip ]++; 
        TrafficCounterOutgoing[ src_ip ] += packet_length;
    } else if (belongs_to_networks(our_networks, dst_ip)) {
        packet_direction = INCOMING;
    
        total_count_of_incoming_packets++;
        total_count_of_incoming_bytes += packet_length;

        PacketsCounterIncoming[ dst_ip ]++;
        TrafficCounterIncoming[ dst_ip ] += packet_length;
    } else {
        packet_direction = OTHER;
        total_count_of_other_packets ++;
        total_count_of_other_bytes += packet_length;
    }

    time_t current_time;
    time(&current_time);
    
    if ( difftime(current_time, start_time) >= check_period ) {
        // clean up screen
        system("clear");

        cout<<"Below you can see all clients with more than "<<threshhold<<" pps"<<endl<<endl;

        cout<<"Incoming Traffic"<<"\t"<<total_count_of_incoming_packets/check_period<<" pps "<<total_count_of_incoming_bytes/check_period/1024/1024*8<<" mbps"<<endl;
        draw_table(PacketsCounterIncoming, TrafficCounterIncoming, "incoming");
    
        cout<<endl; 

        cout<<"Outgoing traffic"<<"\t"<<total_count_of_outgoing_packets/check_period<<" pps "<<total_count_of_outgoing_bytes/check_period/1024/1024*8<<" mbps"<<endl;
        draw_table(PacketsCounterOutgoing, TrafficCounterOutgoing, "outgoing"); 

        cout<<endl;

        cout<<"Internal traffic"<<"\t"<<total_count_of_internal_packets/check_period<<" pps"<<endl;    

        cout<<endl;

        cout<<"Other traffic"<<"\t\t"<<total_count_of_other_packets/check_period<<" pps"<<endl;

        cout<<endl;

#ifdef PCAP
        struct pcap_stat current_pcap_stats;
        if (pcap_stats(descr, &current_pcap_stats) == 0) {
            cout<<"PCAP statistics"<<endl<<"Received packets: "<<current_pcap_stats.ps_recv<<endl
                <<"Dropped packets: "<<current_pcap_stats.ps_drop
                <<" ("<<int((double)current_pcap_stats.ps_drop/current_pcap_stats.ps_recv*100)<<"%)"<<endl
                <<"Dropped by driver or interface: "<<current_pcap_stats.ps_ifdrop<<endl;
        }
#endif

#ifdef ULOG2
       cout<<"ULOG buffer errors: "   << netlink_error_counter<<" ("<<int((double)netlink_error_counter/netlink_packets_counter)<<"%)"<<endl; 
       cout<<"ULOG packets received: "<< netlink_packets_counter<<endl;
#endif
 
        if (ban_list.size() > 0) {
            cout<<endl<<"Ban list:"<<endl;  
 
            for( map<uint32_t,int>::iterator ii=ban_list.begin(); ii!=ban_list.end(); ++ii) {
                cout<<convert_ip_as_uint_to_string((*ii).first)<<"/"<<(*ii).second<<" pps"<<endl;
            }
        }
        
        // переустанавливаем время запуска
        time(&start_time);
        // зануляем счетчики пакетов
        PacketsCounterIncoming.clear();
        PacketsCounterOutgoing.clear();
        TrafficCounterIncoming.clear();
        TrafficCounterOutgoing.clear();
              
        /* вот здесь можно сбросить данные в Redis */ 
        total_count_of_incoming_bytes = 0;
        total_count_of_outgoing_bytes = 0;

        total_count_of_other_packets = 0;
        total_count_of_other_bytes   = 0;

        total_count_of_internal_packets = 0;
        total_count_of_internal_bytes = 0;
 
        total_count_of_incoming_packets = 0;
        total_count_of_outgoing_packets = 0;
    }
    
    // Advance to the transport layer header then parse and display
    // the fields based on the type of hearder: tcp, udp or icmp.
    packetptr += 4*iphdr->ip_hl;
    switch (iphdr->ip_p) {
    case IPPROTO_TCP:
        tcphdr = (struct tcphdr*)packetptr;
        if (DEBUG) {
            printf("TCP %s:%d -> %s:%d\n", srcip_char, ntohs(tcphdr->source), dstip_char, ntohs(tcphdr->dest));
        }
        //printf("%s\n", iphdrInfo);
        /*
        printf("%c%c%c%c%c%c Seq: 0x%x Ack: 0x%x Win: 0x%x TcpLen: %d\n",
               (tcphdr->urg ? 'U' : '*'),
               (tcphdr->ack ? 'A' : '*'),
               (tcphdr->psh ? 'P' : '*'),
               (tcphdr->rst ? 'R' : '*'),
               (tcphdr->syn ? 'S' : '*'),
               (tcphdr->fin ? 'F' : '*'),
               ntohl(tcphdr->seq), ntohl(tcphdr->ack_seq),
               ntohs(tcphdr->window), 4*tcphdr->doff);
        */
        break;
 
    case IPPROTO_UDP:
        udphdr = (struct udphdr*)packetptr;
        if (DEBUG) {
            printf("UDP %s:%d -> %s:%d\n", srcip_char, ntohs(udphdr->source), dstip_char, ntohs(udphdr->dest));
        }
        //printf("%s\n", iphdrInfo);
        break;
 
    case IPPROTO_ICMP:
        icmphdr = (struct icmphdr*)packetptr;
        if (DEBUG) {
            printf("ICMP %s -> %s\n", srcip_char, dstip_char);
        }
        break;
    }
}


int main(int argc,char **argv) {
    int i;
    char *dev; 
    char errbuf[PCAP_ERRBUF_SIZE]; 
    const u_char *packet; 
    struct pcap_pkthdr hdr;
    struct ether_header *eptr;    /* net/ethernet.h */
    struct bpf_program fp;        /* hold compiled program */
 
    time(&start_time);
    printf("I need few seconds for collecting data, please wait. Thank you!\n");
  
    if (argc != 2) {
        fprintf(stdout, "Usage: %s \"eth0\" or \"any\"\n", argv[0]);
        cout<< "We must automatically select interface"<<endl;
        /* Now get a device */
        dev = pcap_lookupdev(errbuf);
        
        if(dev == NULL) {
            fprintf(stderr, "%s\n", errbuf);
            exit (1);    
        }

        printf("Automatically selected %s device\n", dev);

    } else { 
        dev = argv[1];
    }

    // загружаем наши сети 
    load_our_networks_list();
 
    /* open device for reading in promiscuous mode */
    int promisc = 1;
    int pcap_read_timeout = -1;


    bpf_u_int32 maskp; /* subnet mask */
    bpf_u_int32 netp;  /* ip */ 

#ifdef PCAP
    cout<<"Start listening on "<<dev<<endl;

    /* Get the network address and mask */
    pcap_lookupnet(dev, &netp, &maskp, errbuf);

    descr = pcap_create(dev, errbuf);

    if (descr == NULL) {
        printf("pcap_create was failed with error: %s", errbuf);
        exit(0);
    }

    int set_buffer_size_res = pcap_set_buffer_size(descr, pcap_buffer_size_mbytes * 1024 * 1024);
    if (set_buffer_size_res != 0 ) { // выставляем буфер в 1 мегабайт
        if (set_buffer_size_res == PCAP_ERROR_ACTIVATED) {
            printf("Can't set buffer size because pcap already activated\n");
            exit(1);
        } else {
            printf("Can't set buffer size due to error %d\n", set_buffer_size_res);
            exit(1);
        }   
    } 

    /*
    Вот через этот спец механизм можно собирать лишь хидеры!
    If you don't need the entire contents of the packet - for example, if you are only interested in the TCP headers of packets - you can set the "snapshot length" for the capture to an appropriate value.
    */
    /*
    if (pcap_set_snaplen(descr, 32 ) != 0 ) {
        printf("Can't set snap len\n");
        exit(1);
    }
    */

    if (pcap_set_promisc(descr, promisc) != 0) {
        printf("Can't activate promisc mode for interface: %s\n", dev);
        exit(1);
    }

    if (pcap_activate(descr) != 0) {
        printf("Call pcap_activate was failed: %s\n", pcap_geterr(descr));
        exit(1);
    }

    /*
    descr = pcap_open_live(dev, BUFSIZ, promisc, pcap_read_timeout, errbuf); 
    if(descr == NULL) {
        printf("pcap_open_live(): %s\n", errbuf);
        exit(1);
    }
    */ 

    // В общем-то можно фильтровать то, что нам падает от PCAP, но в моем случае это совершенно не требуется
    // тут было argv[1], но я убрал фильтрацию
    /* Now we'll compile the filter expression*/
    //if(pcap_compile(descr, &fp, "", 0, netp) == -1) {
    //    fprintf(stderr, "Error calling pcap_compile\n");
    //    exit(1);

    //} 
 
    /* set the filter */
    //if(pcap_setfilter(descr, &fp) == -1) {
    //    fprintf(stderr, "Error setting filter\n");
    //    exit(1);
    //} 
 
    /* loop for callback function */
    // pcap_setnonblock(descr, 1, NULL);

    signal(SIGINT, signal_handler);

    // man pcap-linktype
    int link_layer_header_type = pcap_datalink(descr);

    if (link_layer_header_type == DLT_EN10MB) {
        DATA_SHIFT_VALUE = 14;
    } else if (link_layer_header_type == DLT_LINUX_SLL) {
        DATA_SHIFT_VALUE = 16;
    } else {
        printf("We did not support link type %d\n", link_layer_header_type);
        exit(0);
    }
#endif
   
#ifdef PCAP 
    // пока деактивируем pcap, начинаем интегрировать ULOG
    pcap_loop(descr, -1, (pcap_handler)parse_packet, NULL);
#endif

    /* Size of the socket receive memory.  Should be at least the same size as the 'nlbufsiz' module loadtime parameter of ipt_ULOG.o If you have _big_ in-kernel queues, you may have to increase this number.  (
     * --qthreshold 100 * 1500 bytes/packet = 150kB  */
    int ULOGD_RMEM_DEFAULT = 131071;

    // В загрузке модуля есть параметры: modprobe ipt_ULOG nlbufsiz=131072
    // Увеличиваем размер буфера в ядре, так как стандартно он всего-то 3712    
    // Так задать нельзя, только при запуске модуля ядром
    //exec("echo '131072' > /sys/module/ipt_ULOG/parameters/nlbufsiz");

    /* Size of the receive buffer for the netlink socket.  Should be at least of RMEM_DEFAULT size.  */
    int ULOGD_BUFSIZE_DEFAULT = 150000;
    int ULOGD_NLGROUP_DEFAULT = 1;
    struct ipulog_handle *libulog_h;
    unsigned char *libulog_buf;

    libulog_buf = (unsigned char*)malloc(ULOGD_BUFSIZE_DEFAULT);
    if (!libulog_buf) {
        printf("Can't allocate buffer");
        exit(1);
    }

    libulog_h = ipulog_create_handle(ipulog_group2gmask(ULOGD_NLGROUP_DEFAULT), ULOGD_RMEM_DEFAULT);

    if (!libulog_h) {
        printf("Can't create ipulog handle");
        exit(0);
    }
    
    int len;
    while ( len = ipulog_read(libulog_h, libulog_buf, ULOGD_BUFSIZE_DEFAULT) ) {
        if (len <= 0) {
            if (errno == EAGAIN) {
                break;
            }

            if (errno == 105) {
                // Наш уютный бажик: errno = '105' ('No buffer space available'
                netlink_error_counter++;
                continue;
            }

            // поймали ошибку - зафиксируем ее при расчетах
            printf("ipulog_read = '%d'! "
                "ipulog_errno = '%d' ('%s'), "
                "errno = '%d' ('%s')\n",
                len, ipulog_errno,
                ipulog_strerror(ipulog_errno),
                errno, strerror(errno));

            continue;
        } 

        // успешний прием пакета
        netlink_packets_counter++;

        ulog_packet_msg_t *upkt;
        while ((upkt = ipulog_get_packet(libulog_h, libulog_buf, len))) {
            // вот такой хитрый хак, так как данные начинаются без ethernet хидера
            DATA_SHIFT_VALUE = 0;
            parse_packet(NULL, NULL, upkt->payload);
        }
    }

    free(libulog_buf);

    /*
    Альтернативный парсер, пока не совсем корректно работает, так как возвращает NULL
    const u_char* packetptr;
    struct pcap_pkthdr packethdr;
    while ( (packetptr = pcap_next(descr, &packethdr) ) != NULL) { 
        parse_packet(NULL, &packethdr, packetptr);
    } 
    */ 

    return 0; 
}

// для корректной останвоки программы
void signal_handler(int signal_number) {

#ifdef PCAP
    // останавливаем PCAP цикл
    pcap_breakloop(descr);
#endif

    exit(1); 
}

