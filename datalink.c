#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"
#include "lprintf.c"

#define DATA_TIMER  2000
#define MAX_SEQ 7
#define NR_BUFS 4
#define PACKET char*


struct FRAME { 
    unsigned char kind; /* FRAME_DATA */
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN]; //packet info
    unsigned int  padding;
}typedef FRAME;

static unsigned char frame_nr = 0, buffer[PKT_LEN], nbuffered;
static unsigned char frame_expected = 0;
static int phl_ready = 0;
static int oldest_frame = 8;

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    s.ack = 1 - frame_expected;
    memcpy(s.data, buffer, PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

static void send_ack_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_ACK;
    s.ack = 1 - frame_expected;

    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}
void inc(int* seq)
{
	if (*seq < MAX_SEQ) (*seq)++;
	else (*seq) = 0;
}
void protocol6(void)
{
	int ack_expected = 0;         //发送者窗口下沿
	int next_frame_to_send = 0;   //发送者窗口上沿+1
	int frame_expected = 0;       //接收者窗口下沿
	int too_far = NR_BUFS;        //接收者窗口上沿+1
	int i;
	FRAME r;
	PACKET out_buf[NR_BUFS];       //输出流的缓冲区
	PACKET in_buf[NR_BUFS];        //输入流的缓冲区
	bool arrived[NR_BUFS] = { false };         //输入窗口的到达情况
	int nbuffered = 0;             //用到了多少输出窗口
	int event;                     //用到的事件，见protocol.h
	enable_network_layer();
	while (true)
	{
		wait_for_event(&event);
		switch (event)
		{
		case NETWORK_LAYER_READY:
			nbuffered++;
			get_packet(&out_buf[next_frame_to_send % NR_BUFS]);
			send_data_frame();
			inc(&next_frame_to_send);
			break;
		case FRAME_RECEIVED:
			

		default:
			break;
		}
	}



}

int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;

    protocol_init(argc, argv); 
    lprintf("Designed by Qi Xiaoyue and Wang Ruojing, build: " __DATE__"  "__TIME__"\n");

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        case NETWORK_LAYER_READY:
            get_packet(buffer);
            nbuffered++;
            send_data_frame();
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED: 
            len = recv_frame((unsigned char *)&f, sizeof f);
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }
            if (f.kind == FRAME_ACK) 
                dbg_frame("Recv ACK  %d\n", f.ack);
            if (f.kind == FRAME_DATA) {
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
                if (f.seq == frame_expected) {
                    put_packet(f.data, len - 7);
                    frame_expected = 1 - frame_expected;
                }
                send_ack_frame();
            } 
            if (f.ack == frame_nr) {
                stop_timer(frame_nr);
                nbuffered--;
                frame_nr = 1 - frame_nr;
            }
            break; 

        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg); 
            send_data_frame();
            break;
        }

        if (nbuffered < 1 && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
	system("pause");
}
