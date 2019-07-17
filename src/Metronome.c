#include <stdlib.h>
#include <stdio.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <sys/netmgr.h>
#include <sys/neutrino.h>


#define INFO   0
#define QUIT   1
#define PAUSE  2
#define METRONOME  3

char data[255];
int server_coid;
int beats;
int row = 0;
name_attach_t *attach;
double timing;
dispatch_t* dpp;
struct sigevent         event;
struct itimerspec       itime;
timer_t                 timer_id;

typedef union {
	struct _pulse pulse;
	char msg[255];
} my_message_t;



typedef struct {
	int timeSignatureTop;
	int timeSignatureBottom;
	int timeSignatureInterval;
	char msg[255];

} DataTableRow;


DataTableRow t[] = {
		{2, 4, 4, "|1&2&"},
		{3, 4, 6, "|1&2&3&"},
		{4, 4, 8, "|1&2&3&4&"},
		{5, 4, 10, "|1&2&3&4-5-"},
		{3, 8, 6, "|1-2-3-"},
		{6, 8, 6, "|1&a2&a"},
		{9, 8, 9, "|1&a2&a3&a"},
		{12, 8, 12, "|1&a2&a3&a4&a"}
};


int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{

	int nb;

	if(data == NULL)
		return 0;

	nb = strlen(data);

	//test to see if we have already sent the whole message.
	if (ocb->offset == nb)
		return 0;

	//We will return which ever is smaller the size of our data or the size of the buffer
	nb = min(nb, msg->i.nbytes);

	//Set the number of bytes we will return
	_IO_SET_READ_NBYTES(ctp, nb);

	//Copy data into reply buffer.
	SETIOV(ctp->iov, data, nb);

	//update offset into our data used to determine start position for next read.
	ocb->offset += nb;

	//If we are going to send any bytes update the access timing for this resource.
	if (nb > 0)
		ocb->attr->flags |= IOFUNC_ATTR_ATIME;

	return(_RESMGR_NPARTS(1));
}


int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
	int nb = 0;

	if( msg->i.nbytes == ctp->info.msglen - (ctp->offset + sizeof(*msg) ))
	{
		/* have all the data */
		char *buf;
		char *alert_msg;
		int i, small_integer;
		buf = (char *)(msg+1);
		//change alert to pulse


		if(strstr(buf, "pause") != NULL){
			for(i = 0; i < 2; i++){
				alert_msg = strsep(&buf, " ");
			}
			small_integer = atoi(alert_msg);

			if(small_integer >= 1 && small_integer <= 9){
				MsgSendPulse(server_coid, SchedGet(0,0,NULL), PAUSE, small_integer);
			} else {
				printf("Integer is not between 1 and 9.\n");
			}
		}
		else if(strstr(buf, "info") != NULL){
			MsgSendPulse(server_coid, SchedGet(0,0,NULL), INFO, 0);
		}
		else if(strstr(buf, "quit") != NULL){
			MsgSendPulse(server_coid, SchedGet(0,0,NULL), QUIT, 0);
		}
		else {
			printf("Invalid command");
			strcpy(data, buf);
		}

		nb = msg->i.nbytes;
	}
	_IO_SET_WRITE_NBYTES (ctp, nb);

	if (msg->i.nbytes > 0)
		ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;

	return (_RESMGR_NPARTS (0));
}



int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra)
{
	if ((server_coid = name_open("metronome", 0)) == -1) {
		perror("name_open failed.");
		return EXIT_FAILURE;
	}
	return (iofunc_open_default (ctp, msg, handle, extra));
}

void* metronome_thread(void* arg){

	int printer = 1;
	int rcvid;
	my_message_t msg;
	int normal_time = 1;

	for(;;){
		rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);

		if(rcvid == 0){
			if (normal_time == -1){
				itime.it_value.tv_sec = (int) timing;
				/* 500 million nsecs = .5 secs */
				itime.it_value.tv_nsec = 100000000 * (timing - (int) timing);
				itime.it_interval.tv_sec = (int) timing;
				/* 500 million nsecs = .5 secs */
				itime.it_interval.tv_nsec = 100000000 * (timing - (int) timing);
				timer_settime(timer_id, 0, &itime, NULL);

			}

			switch(msg.pulse.code){

			case METRONOME:
				if(printer == 1){
					printf("|%c", t[row].msg[printer]);
					fflush(stdout);
					printer++;
				}
				else if(printer < t[row].timeSignatureInterval){
					printf("%c", t[row].msg[printer]);
					fflush(stdout);
					printer++;
				}
				else{
					printf("%c\n", t[row].msg[printer]);
					printer = 1;
				}
				break;

			case INFO:
				sprintf(data, " metronome [%d beats/min, timing %d/%d]", beats , t[row].timeSignatureTop, t[row].timeSignatureBottom);
				break;

			case PAUSE:
				itime.it_value.tv_sec = msg.pulse.value.sival_int;
				itime.it_interval.tv_sec = msg.pulse.value.sival_int;
				timer_settime(timer_id, 0, &itime, NULL);
				normal_time = -1;
				break;

			case QUIT:
				name_close(server_coid);
				name_detach(attach, 0);
				dispatch_destroy(dpp);
				exit(EXIT_SUCCESS);
			}
		}else{
			printf("Unable to receive a PULSE");

		}
	}
}





int main(int argc, char* argv[]){

	if(argc!=4){
		printf("Invalid number parameters");
		return EXIT_FAILURE;
	}

	beats = atoi(argv[1]);
	for(int i= 0; i < sizeof(t)/sizeof(t[0]); i++ ){

		if(t[i].timeSignatureTop == atoi(argv[2]) && t[i].timeSignatureBottom == atoi(argv[3]) ){
			row = i;
			break;
		}
	}

	timing = (double) (t[row].timeSignatureTop * 60 )/(atoi(argv[1]) );
	resmgr_io_funcs_t io_funcs;
	resmgr_connect_funcs_t connect_funcs;
	iofunc_attr_t ioattr;
	dispatch_context_t   *ctp;
	int id;

	pthread_attr_t attr;


	dpp = dispatch_create();
	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
	connect_funcs.open = io_open;
	io_funcs.read = io_read;
	io_funcs.write = io_write;

	iofunc_attr_init(&ioattr, S_IFCHR | 0666, NULL, NULL);

	id = resmgr_attach(dpp, NULL, "/dev/local/metronome", _FTYPE_ANY, NULL, &connect_funcs, &io_funcs, &ioattr);

	if ((attach = name_attach(NULL, "metronome", 0)) == NULL) {
		perror("name_attach failed");
		return EXIT_FAILURE;
	}

	pthread_attr_init(&attr);
	pthread_create(NULL, &attr, &metronome_thread, NULL);
	pthread_attr_destroy(&attr);

	event.sigev_notify = SIGEV_PULSE;
	event.sigev_coid =  ConnectAttach(ND_LOCAL_NODE, 0, attach->chid, _NTO_SIDE_CHANNEL, 0);
	event.sigev_priority = SchedGet(0,0,NULL);
	event.sigev_code = METRONOME;
	timer_create(CLOCK_REALTIME, &event, &timer_id);


	itime.it_value.tv_sec = (int) timing;
	/* 500 million nsecs = .5 secs */
	itime.it_value.tv_nsec = 100000000 * (timing - (int) timing);
	itime.it_interval.tv_sec = (int) timing;
	/* 500 million nsecs = .5 secs */
	itime.it_interval.tv_nsec = 100000000 * (timing - (int) timing);
	timer_settime(timer_id, 0, &itime, NULL);

	ctp = dispatch_context_alloc(dpp);

	while(1) {
		ctp = dispatch_block(ctp);
		dispatch_handler(ctp);
	}

	return EXIT_SUCCESS;
}



