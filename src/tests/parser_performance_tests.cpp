#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include "../fastnetmon_packet_parser.h"

/*
    gcc ../fastnetmon_packet_parser.c -o fastnetmon_packet_parser.o -c
    gcc parser_performance_tests.cpp fastnetmon_packet_parser.o -lpthread
*/

void call_parser(void* ptr, int length);
uint64_t received_packets = 0;

void* speed_printer(void* ptr) {
    while (1) {
        uint64_t packets_before = received_packets;
    
        sleep(1);
    
        uint64_t packets_after = received_packets;
        uint64_t pps = packets_after - packets_before;
 
        printf("We process: %llu pps\n", pps);
    }   
}


// We could pring any payload with this function and use it for tests
void print_packet_payload_in_c_form(unsigned char* data, int length) {
    int i = 0;
    printf("unsigned char payload[] = { ");
    for (i = 0; i < length; i++) {
        printf("0x%02X", (unsigned char)data[i]);
        if (i != length -1) {
            printf(",");
        }   
    }   

    printf(" }\n");
}

int main() {
    pthread_t thread;
    pthread_create(&thread, NULL, speed_printer, NULL);

    pthread_detach(thread);
     
    unsigned char payload1[] = { 0x90,0xE2,0xBA,0x83,0x3F,0x25,0x90,0xE2,0xBA,0x2C,0xCB,0x02,0x08,0x00,0x45,0x00,0x00,0x2E,0x00,0x00,0x00,0x00,0x40,0x06,0x69,0xDC,0x0A,0x84,0xF1,0x83,0x0A,0x0A,0x0A,0xDD,0x04,0x01,0x00,0x50,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x50,0x02,0x00,0x0A,0x9A,0x92,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };

    unsigned char payload2[] = { 0x90,0xE2,0xBA,0x83,0x3F,0x25,0x90,0xE2,0xBA,0x2C,0xCB,0x02,0x08,0x00,0x45,0x00,0x00,0x2E,0x00,0x00,0x00,0x00,0x40,0x06,0x69,0xDB,0x0A,0x84,0xF1,0x84,0x0A,0x0A,0x0A,0xDD,0x04,0x01,0x00,0x50,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x50,0x02,0x00,0x0A,0x9A,0x91,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };

    while (1) {
        call_parser(payload1, sizeof(payload1));
        call_parser(payload2, sizeof(payload2));
    }
}

void call_parser(void* ptr, int length) {
    __sync_fetch_and_add(&received_packets, 1);

    struct pfring_pkthdr packet_header;
    memset(&packet_header, 0, sizeof(struct pfring_pkthdr));

    packet_header.len = length;
    packet_header.caplen = length;

    u_int8_t timestamp = 0;
    u_int8_t add_hash = 0;

    fastnetmon_parse_pkt((u_char*)ptr, &packet_header, 4, 0, 0);

/*
    char print_buffer[512];
    fastnetmon_print_parsed_pkt(print_buffer, 512, (u_char*)ptr, &packet_header);
    printf("packet: %s\n", print_buffer);
*/
}
