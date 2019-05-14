#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "protocol.h"

#define DATA 1
#define NAK  2
#define ACK  3

#define DATA_TIMER  3000
#define ACK_TIMER 1000

#define MAX_SEQ 63
#define NR_BUFS ((MAX_SEQ+1)/2)

bool no_nak = true;
static bool phl_ready = false;

struct FRAME {
	unsigned char kind; 
	unsigned char ack;
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int  padding;
};

static bool between(unsigned char a, unsigned char b, unsigned char c)
{
	//If the package is in the windows
	return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((b < c) && (c < a));
}

static void put_frame(unsigned char* frame, int len)
{
	*(unsigned int*)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = false;
}

static void send_data_frame(unsigned char frame_kind, unsigned char frame_nr, unsigned char frame_expected, unsigned char buffer[NR_BUFS][PKT_LEN])
{
	struct FRAME s;

	s.kind = frame_kind;
	s.seq = frame_nr;
	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);

	if (frame_kind == DATA) {
		memcpy(s.data, buffer[frame_nr % NR_BUFS], PKT_LEN);
		dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short*)s.data);
		put_frame((unsigned char*)& s, 3 + PKT_LEN);
		start_timer(frame_nr % NR_BUFS, DATA_TIMER);
	}
	if (frame_kind == NAK) {
		no_nak = false;
		dbg_frame("Send NAK %d\n", s.ack);
		put_frame((unsigned char*)& s, 3 + PKT_LEN);
	}
	if (frame_kind == ACK) {
		dbg_frame("Send ACK  %d\n", s.ack);
		put_frame((unsigned char*)& s, 3 + PKT_LEN);
	}
	phl_ready = false;
	stop_ack_timer();
}

void main(int argc, char** argv)
{
	int event, arg;
	struct FRAME f;
	int len = 0;
	int i;

	bool arrived[NR_BUFS];
	static unsigned char ack_expected = 0;
	static unsigned char next_frame_to_send = 0;
	static unsigned char frame_expected = 0;
	static unsigned char too_far = NR_BUFS;
	static unsigned char nbuffered = 0;
	static unsigned char out_buf[NR_BUFS][PKT_LEN];
	static unsigned char in_buf[NR_BUFS][PKT_LEN];

	protocol_init(argc, argv); //初始化协议
	lprintf("Designed by CK~~, build: " __DATE__"  "__TIME__"\n");

	for (i = 0; i < NR_BUFS; i++)
		arrived[i] = false;//没有帧到达接收方

	enable_network_layer();//初始化
	while (1) {
		event = wait_for_event(&arg);//等待下一事件
		switch (event) {
		case NETWORK_LAYER_READY://允许网络层发送数据帧
			nbuffered++;//缓冲区数据帧数加一
			get_packet(out_buf[next_frame_to_send % NR_BUFS]);//从网络层接收数据帧
			send_data_frame(DATA, next_frame_to_send, frame_expected, out_buf);//发送数据帧
			next_frame_to_send = (next_frame_to_send + 1) % (MAX_SEQ + 1);//发送窗口上界下移
			break;

		case PHYSICAL_LAYER_READY://物理层空闲
			phl_ready = 1;
			break;

		case FRAME_RECEIVED://接收方收到一个帧
			len = recv_frame((unsigned char*)& f, sizeof f);//帧长
			if (len < 5 || crc32((unsigned char*)& f, len) != 0) {//收到的帧损坏
				if (no_nak)//如果没有发送NAK,则发送NAK要求重传(避免多次请求重发)
					send_data_frame(NAK, 0, frame_expected, out_buf);//相对协议5不同，收到错误要求重传而不是直接abort
				dbg_event("**** Receiver Error, Bad CRC Checksum\n");//打印损坏报告
				break;//跳出switch语句
			}
			if (f.kind == DATA) {//收到一个数据帧
				dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short*)f.data);//打印收到帧完好
				if ((f.seq != frame_expected) && no_nak)//未按照顺序到达 
					send_data_frame(NAK, 0, frame_expected, out_buf);//返回错误帧
				else
					start_ack_timer(ACK_TIMER);//开启辅助计时器
				if (between(frame_expected, f.seq, too_far) && arrived[f.seq % NR_BUFS] == false) {//数据帧落在窗口内
					/*数据帧可能以任何顺序到达 */
					arrived[f.seq % NR_BUFS] = true;
					memcpy(in_buf[f.seq % NR_BUFS], f.data, len - 7);

					while (arrived[frame_expected % NR_BUFS]) {//如果到达的帧落在接收窗上
						/*数据帧通过并移动接收窗口*/
						put_packet(in_buf[frame_expected % NR_BUFS], len - 7);//将到达的数据帧发送到网络层
						no_nak = true;//表示对于这一帧没有发送过否定确认
						arrived[frame_expected % NR_BUFS] = false;//该处缓冲空间重新记为空
						frame_expected = (frame_expected + 1) % (MAX_SEQ + 1);//接收窗口后移一位
						too_far = (too_far + 1) % (MAX_SEQ + 1);
						start_ack_timer(ACK_TIMER);/*查看辅助计时器是否超时，超时则发送独立的确认帧*/
					}
				}
			}//收到的是错误帧
			if ((f.kind == NAK) && between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send)) {//如果收到落在接收窗的错误帧
				dbg_frame("Recv NAK %d\n", f.ack);//打印错误信息
				send_data_frame(DATA, (f.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);//重传
			}
			if ((f.kind == ACK) && between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send)) {//如果收到落在接收窗的确认帧
				dbg_frame("Recv ACK %d\n", f.ack);//打印正确信息
			}

			while (between(ack_expected, f.ack, next_frame_to_send)) {//接收方收到自己作为发送窗口在等待的ack，处理。
					 /*处理捎带确认*/
				nbuffered--;//缓冲区数据帧数量减一
				stop_timer(ack_expected % NR_BUFS);//停止辅助计时器
				ack_expected = (ack_expected + 1) % (MAX_SEQ + 1);
			}
			break;

		case DATA_TIMEOUT://数据帧超时。重发
			dbg_event("---- DATA %d timeout\n", arg);
			send_data_frame(DATA, ack_expected, frame_expected, out_buf);
			break;

		case ACK_TIMEOUT://确认帧超时，发送确认帧
			dbg_event("---- ACK %d timeout\n", arg);
			send_data_frame(ACK, 0, frame_expected, out_buf);
		}

		if (nbuffered < NR_BUFS && phl_ready)
			enable_network_layer();//付过接收方缓冲区未满且物理层空闲，则开启网络层
		else//否则关闭网络层
			disable_network_layer();
	}
}
