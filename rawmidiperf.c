/*
 * This file is part of the rawmidiperf project
 *
 * (C) 2020 Andreas Steinmetz, ast@domdv.de
 * The contents of this file is licensed under the GPL version 2 or, at
 * your choice, any later version of this license.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sys/signalfd.h>
#include <sys/signal.h>
#include <poll.h>
#include <time.h>
#include <alsa/asoundlib.h>

struct mididev
{
	snd_rawmidi_t *in;
	snd_rawmidi_t *out;
	int infd;
	int outfd;
	struct timespec t1;
	struct timespec t2;
	int idx;
	int mode;
	int term;
	unsigned long long sum;
	unsigned long long ev;
	char *dev;
};

struct single_serial
{
	struct mididev *ctx;
	int ndev;
	int delay;
	int term;
};

static int opendev(char *dev,int maxfill,struct mididev *ctx)
{
	int outsize;
	snd_rawmidi_params_t *prm;
	struct pollfd p;

	if(snd_rawmidi_open(&ctx->in,NULL,dev,SND_RAWMIDI_NONBLOCK))
		goto err1;
	if(snd_rawmidi_open(NULL,&ctx->out,dev,SND_RAWMIDI_NONBLOCK))
		goto err2;
	if(snd_rawmidi_params_malloc(&prm))goto err3;
	if(snd_rawmidi_params_current(ctx->in,prm))goto err4;
	if(snd_rawmidi_params_set_no_active_sensing(ctx->in,prm,1))goto err4;
	if(snd_rawmidi_params_set_avail_min(ctx->in,prm,1))goto err4;
	if(snd_rawmidi_params(ctx->in,prm))goto err4;
	if(snd_rawmidi_params_current(ctx->out,prm))goto err4;
	outsize=snd_rawmidi_params_get_buffer_size(prm);
	if(maxfill>outsize)goto err4;
	if(snd_rawmidi_params_set_no_active_sensing(ctx->out,prm,1))goto err4;
	if(snd_rawmidi_params_set_avail_min(ctx->out,prm,outsize-maxfill))
		goto err4;
	if(snd_rawmidi_params(ctx->out,prm))goto err4;
	if(snd_rawmidi_poll_descriptors(ctx->in,&p,1)!=1)goto err4;
	ctx->infd=p.fd;
	if(snd_rawmidi_poll_descriptors(ctx->out,&p,1)!=1)goto err4;
	ctx->outfd=p.fd;
	if(snd_rawmidi_drop(ctx->out))goto err4;
	usleep(10000);
	if(snd_rawmidi_drain(ctx->in))goto err4;
	snd_rawmidi_params_free(prm);
	return 0;

err4:	snd_rawmidi_params_free(prm);
err3:	snd_rawmidi_close(ctx->out);
err2:	snd_rawmidi_close(ctx->in);
	ctx->out=NULL;
	ctx->in=NULL;
err1:	return -1;
}

static void closedev(struct mididev *ctx)
{
	if(ctx->out)snd_rawmidi_drop(ctx->out);
	if(ctx->in)snd_rawmidi_drain(ctx->in);
	if(ctx->out)snd_rawmidi_close(ctx->out);
	if(ctx->in)snd_rawmidi_close(ctx->in);
}

static void *serial_single_worker(void *data)
{
	struct single_serial *ss=data;
	struct mididev *ctx=ss->ctx;
	int ndev=ss->ndev;
	int i;
	int j=0;
	int l;
	int type;
	unsigned long long sum[16];
	unsigned long long n=0;
	struct pollfd p[16];
	unsigned char msg[16][3];
	unsigned char bfr[3];

	memset(sum,0,sizeof(sum));

	for(i=0;i<ndev;i++)
	{
		p[i].fd=ctx[i].infd;
		p[i].events=POLLIN|POLLOUT|POLLERR;
	}

	while(!ss->term)
	{
		j^=1;
		n++;
		for(i=0;i<ndev;i++)
		{
			if(ctx[i].mode==2)
			{
				l=1;
				type=(j?0xf9:0xf8);
			}
			else
			{
				l=(ctx[i].mode?2:3);
				type=(j?0x90:0x80)+(ctx[i].mode?0x40:0);
			}
			msg[i][0]=type;
			if(ctx[i].mode<2)msg[i][1]=0x00+i;
			if(!ctx[i].mode)msg[i][2]=0x10+i;
			clock_gettime(CLOCK_MONOTONIC_RAW,&ctx[i].t1);
			if(snd_rawmidi_write(ctx[i].out,msg[i],l)!=l)
			{
				fprintf(stderr,"midi write error\n");
				goto out;
			}
			if(poll(&p[i],1,1000)<1)
			{
				fprintf(stderr,"midi receive timeout\n");
				goto out;
			}
			if(p[i].revents&(POLLHUP|POLLERR))
			{
				fprintf(stderr,"midi receive error\n");
				goto out;
			}
			else if(p[i].revents&POLLIN)
			{
				clock_gettime(CLOCK_MONOTONIC_RAW,&ctx[i].t2);
				if(snd_rawmidi_read(ctx[i].in,bfr,l)!=l)
				{
					fprintf(stderr,"midi receive error\n");
					goto out;
				}
				if(memcmp(bfr,msg[i],l))
				{
					fprintf(stderr,"midi receive error\n");
					goto out;
				}
				__atomic_add_fetch(&ctx[i].ev,1,
					__ATOMIC_SEQ_CST);
			}

			ctx[i].t2.tv_sec-=ctx[i].t1.tv_sec;
			if(ctx[i].t2.tv_nsec<ctx[i].t1.tv_nsec)
			{
				ctx[i].t2.tv_nsec+=1000000000;
				ctx[i].t2.tv_sec--;
			}
			ctx[i].t2.tv_nsec-=ctx[i].t1.tv_nsec;
			if(ctx[i].t2.tv_sec)
			{
				fprintf(stderr,"unexpected timestamp error\n");
				goto out;
			}
			if(ctx[i].t2.tv_nsec<0||ctx[i].t2.tv_nsec>999999999)
			{
				fprintf(stderr,"unexpected timestamp error\n");
				goto out;
			}
			sum[i]+=ctx[i].t2.tv_nsec;

			__atomic_store_n(&ctx[i].sum,sum[i]/(n*l),
				__ATOMIC_SEQ_CST);
			usleep(ss->delay);
		}
	}

out:	pthread_exit(0);
}

static void *parallel_single_worker(void *data)
{
	int i;
	int l;
	int wa=1;
	int head=0;
	int tail=0;
	int pre=200;
	unsigned long long val;
	unsigned long long sum=0;
	unsigned long long n=0;
	struct mididev *ctx=data;
	struct pollfd p[2];
	unsigned char bfr[3];
	struct
	{
		unsigned char msg[3];
		struct timespec t1;
		struct timespec t2;
	} list[8192];

	if(ctx->mode==2)l=1;
	else l=(ctx->mode?2:3);

	for(i=0;i<8192;i++)
	{
		if(ctx->mode==2)list[i].msg[0]=0xf8+(i&1);
		else list[i].msg[0]=((i&1)?0x80:0x90)+ctx->idx+
			(ctx->mode?0x40:0);
		if(ctx->mode<2)list[i].msg[1]=i&0x7f;
		if(!ctx->mode)list[i].msg[2]=0x7f-(i&0x7f);
	}

	p[0].fd=ctx->infd;
	p[0].events=POLLIN|POLLHUP|POLLERR;
	p[1].fd=ctx->outfd;
	p[1].events=POLLOUT|POLLHUP|POLLERR;

	while(!ctx->term)
	{
		if(wa)while(poll(&p[1],1,0)==1)
		{
			if(p[1].revents&(POLLHUP|POLLERR))
			{
				fprintf(stderr,"midi device error\n");
				goto out;
			}
			if(!(p[1].revents&POLLOUT))break;
			clock_gettime(CLOCK_MONOTONIC_RAW,&list[head].t1);
			if(snd_rawmidi_write(ctx->out,list[head].msg,l)!=l)
			{
				fprintf(stderr,"midi write error\n");
				goto out;
			}
			head+=1;
			head&=0x1fff;
			if(!--wa)break;
		}

		if(poll(&p[0],1,1000)<1)
		{
			fprintf(stderr,"midi poll timeout\n");
			goto out;
		}

		clock_gettime(CLOCK_MONOTONIC_RAW,&list[tail].t2);

		if(p[0].revents&(POLLHUP|POLLERR))
		{
			fprintf(stderr,"midi device error\n");
			goto out;
		}

		if(!(p[0].revents&POLLIN))
		{
			fprintf(stderr,"unexpected midi read error\n");
			goto out;
		}

		if(snd_rawmidi_read(ctx->in,bfr,l)!=l)
		{
			fprintf(stderr,"midi read error\n");
			goto out;
		}

		if(memcmp(bfr,list[tail].msg,l))
		{
			fprintf(stderr,"data mismatch error\n");
			goto out;
		}

		__atomic_add_fetch(&ctx->ev,1,__ATOMIC_SEQ_CST);

		list[tail].t2.tv_sec-=list[tail].t1.tv_sec;
		if(list[tail].t2.tv_nsec<list[tail].t1.tv_nsec)
		{
			list[tail].t2.tv_nsec+=1000000000;
			list[tail].t2.tv_sec--;
		}
		list[tail].t2.tv_nsec-=list[tail].t1.tv_nsec;
		if(list[tail].t2.tv_sec)
		{
			fprintf(stderr,"unexpected timestamp error\n");
			goto out;
		}
		if(list[tail].t2.tv_nsec<0||list[tail].t2.tv_nsec>999999999)
		{
			fprintf(stderr,"unexpected timestamp error\n");
			goto out;
		}

		val=list[tail].t2.tv_nsec;
		tail+=1;
		tail&=0x1fff;
		wa++;

		if(pre)
		{
			pre--;
			continue;
		}

		sum+=val;
		n++;
		__atomic_store_n(&ctx->sum,sum/(n*l),__ATOMIC_SEQ_CST);
	}

out:	pthread_exit(NULL);
}

static void *parallel_block_worker(void *data)
{
	int i;
	int l;
	int wa=8;
	int head=0;
	int tail=0;
	int pre=5;
	int prev=0;
	int curr;
	unsigned long long val;
	unsigned long long sum=0;
	unsigned long long n=0;
	struct mididev *ctx=data;
	struct pollfd p[2];
	unsigned char bfr[3];
	struct
	{
		unsigned char msg[3];
		struct timespec t1;
		struct timespec t2;
	} list[8192];

	if(ctx->mode==2)l=1;
	else l=(ctx->mode?2:3);

	for(i=0;i<8192;i++)
	{
		if(ctx->mode==2)list[i].msg[0]=0xf8+(i&1);
		else list[i].msg[0]=((i&1)?0x80:0x90)+ctx->idx+
			(ctx->mode?0x40:0);
		if(ctx->mode<2)list[i].msg[1]=i&0x7f;
		if(!ctx->mode)list[i].msg[2]=0x7f-(i&0x7f);
	}

	p[0].fd=ctx->infd;
	p[0].events=POLLIN|POLLHUP|POLLERR;
	p[1].fd=ctx->outfd;
	p[1].events=POLLOUT|POLLHUP|POLLERR;

	while(!ctx->term)
	{
		if(wa)while(poll(&p[1],1,0)==1)
		{
			if(p[1].revents&(POLLHUP|POLLERR))
			{
				fprintf(stderr,"midi device error\n");
				goto out;
			}
			if(!(p[1].revents&POLLOUT))break;
			if(!(head&0x1ff))clock_gettime(CLOCK_MONOTONIC_RAW,
				&list[head].t1);
			if(snd_rawmidi_write(ctx->out,list[head].msg,l)!=l)
			{
				fprintf(stderr,"midi write error\n");
				goto out;
			}
			head+=1;
			head&=0x1fff;
			if(!--wa)break;
		}

		if(poll(&p[0],1,1000)<1)
		{
			fprintf(stderr,"midi poll timeout\n");
			goto out;
		}

		if(!(tail&0x1ff))clock_gettime(CLOCK_MONOTONIC_RAW,
			&list[tail].t2);

		if(p[0].revents&(POLLHUP|POLLERR))
		{
			fprintf(stderr,"midi device error\n");
			goto out;
		}

		if(!(p[0].revents&POLLIN))
		{
			fprintf(stderr,"unexpected midi read error\n");
			goto out;
		}

		if(snd_rawmidi_read(ctx->in,bfr,l)!=l)
		{
			fprintf(stderr,"midi read error\n");
			goto out;
		}

		if(memcmp(bfr,list[tail].msg,l))
		{
			fprintf(stderr,"data mismatch error (%d)\n",ctx->idx);
			goto out;
		}

		__atomic_add_fetch(&ctx->ev,1,__ATOMIC_SEQ_CST);

		curr=tail;
		tail+=1;
		tail&=0x1fff;
		wa++;

		if(curr&0x1ff)continue;

		if(pre)
		{
			pre--;
			prev=curr;
			continue;
		}

		list[curr].t2.tv_sec-=list[prev].t1.tv_sec;
		if(list[curr].t2.tv_nsec<list[prev].t1.tv_nsec)
		{
			list[curr].t2.tv_nsec+=1000000000;
			list[curr].t2.tv_sec--;
		}
		list[curr].t2.tv_nsec-=list[prev].t1.tv_nsec;
		if(list[curr].t2.tv_sec)
		{
			fprintf(stderr,"unexpected timestamp error\n");
			goto out;
		}
		if(list[curr].t2.tv_nsec<0||list[curr].t2.tv_nsec>999999999)
		{
			fprintf(stderr,"unexpected timestamp error\n");
			goto out;
		}

		val=list[curr].t2.tv_nsec;
		prev=curr;

		sum+=val/512;
		n++;
		__atomic_store_n(&ctx->sum,sum/(n*l),__ATOMIC_SEQ_CST);
	}

out:	pthread_exit(NULL);
}

static void usage(void)
{
	fprintf(stderr, "\n"
		"Usage: rawmidiperf [<options|] -s|-S|-1|-p|-P|-2|-b|-B|-3 "
			"<alsa-device> [...]\n\n"
		"-s <alsa-device>  do serialized single event test with 3 byte "
			"events\n"
		"-S <alsa-device>  do serialized single event test with 2 byte "
			"events\n"
		"-1 <alsa-device>  do serialized single event test with 1 byte "
			"events\n"
		"-p <alsa-device>  do parallel single event test with 3 byte "
			"events\n"
		"-P <alsa-device>  do parallel single event test with 2 byte "
			"events\n"
		"-2 <alsa-device>  do parallel single event test with 1 byte "
			"events\n"
		"-b <alsa-device>  do streaming event test with 3 byte events\n"
		"-B <alsa-device>  do streaming event test with 2 byte events\n"
		"-3 <alsa-device>  do streaming event test with 1 byte events\n"
		"-w <wait-ms>      set wait ms between events for -s|-S|-1 "
			"(1-100)\n"
		"-l <latency-us>   set minimum system latency (0-9999)\n"
		"-r <rt-priority>  set realtime priority (1-99)\n"
		"-c <cpu-affinity> use ony specified cpu (0-1023)\n"
		"-e                show events per second\n\n"
		"The output columns resemble the ALSA devices as specified on\n"
		"the command line. The output value is roundtrip time per\n"
		"byte in ns.\n"
		"If '-e' was specified this value is followed by the number\n"
		"of events per second processed.\n\n"
		"Up to 16 devices per test type can be specified in arbitrary\n"
		"sequence.\n\n"
		"Serialized single event test:\n"
		"-----------------------------\n"
		"For all ALSA devices there is one event in flight, e.g.:\n"
		"1 - - -\n"
		"- 1 - -\n"
		"- - 1 -\n"
		"- - - 1 \n\n"
		"Parallel single event test:\n"
		"---------------------------\n"
		"For every ALSA device there is always one event in flight, "
			"e.g.:\n"
		"1 1 1 1\n"
		"1 1 1 1\n"
		"1 1 1 1\n"
		"1 1 1 1\n\n"
		"Streaming event test:\n"
		"---------------------\n"
		"For every ALSA device there are always 8 events in flight, "
			"e.g.:\n"
		"8 8 8 8\n"
		"8 8 8 8\n"
		"8 8 8 8\n"
		"8 8 8 8\n\n");
	exit(1);
}

int main(int argc,char *argv[])
{
	int c;
	int i;
	int err=1;
	int lat=-1;
	int ssdev=0;
	int psdev=0;
	int pbdev=0;
	int ndev=0;
	int delay=1;
	int rt=0;
	int fd=-1;
	int sfd=.1;
	int ev=0;
	int cpu=-1;
	unsigned long long start;
	unsigned long long now;
	struct pollfd p;
	struct mididev ssctx[16];
	struct mididev psctx[16];
	struct mididev pbctx[16];
	struct single_serial ss;
	pthread_t ssth;
	pthread_t psth[16];
	pthread_t pbth[16];
	struct mididev *ctx[48];
	struct sched_param prm;
	sigset_t set;
	cpu_set_t core;

	memset(ssctx,0,sizeof(ssctx));
	memset(psctx,0,sizeof(psctx));
	memset(pbctx,0,sizeof(pbctx));
	memset(psth,0,sizeof(psth));
	memset(pbth,0,sizeof(pbth));
	ssth=0;

	while((c=getopt(argc,argv,"s:S:p:P:b:B:1:2:3:w:l:r:c:e"))!=-1)switch(c)
	{
	case 's':
		if(ssdev==16)usage();
		ctx[ndev++]=&ssctx[ssdev];
		ssctx[ssdev].mode=0;
		ssctx[ssdev++].dev=optarg;
		break;

	case 'S':
		if(ssdev==16)usage();
		ctx[ndev++]=&ssctx[ssdev];
		ssctx[ssdev].mode=1;
		ssctx[ssdev++].dev=optarg;
		break;

	case '1':
		if(ssdev==16)usage();
		ctx[ndev++]=&ssctx[ssdev];
		ssctx[ssdev].mode=2;
		ssctx[ssdev++].dev=optarg;
		break;

	case 'p':
		if(psdev==16)usage();
		ctx[ndev++]=&psctx[psdev];
		psctx[psdev].mode=0;
		psctx[psdev++].dev=optarg;
		break;

	case 'P':
		if(psdev==16)usage();
		ctx[ndev++]=&psctx[psdev];
		psctx[psdev].mode=1;
		psctx[psdev++].dev=optarg;
		break;

	case '2':
		if(psdev==16)usage();
		ctx[ndev++]=&psctx[psdev];
		psctx[psdev].mode=2;
		psctx[psdev++].dev=optarg;
		break;

	case 'b':
		if(pbdev==16)usage();
		ctx[ndev++]=&pbctx[pbdev];
		pbctx[pbdev].mode=0;
		pbctx[pbdev++].dev=optarg;
		break;

	case 'B':
		if(pbdev==16)usage();
		ctx[ndev++]=&pbctx[pbdev];
		pbctx[pbdev].mode=1;
		pbctx[pbdev++].dev=optarg;
		break;

	case '3':
		if(pbdev==16)usage();
		ctx[ndev++]=&pbctx[pbdev];
		pbctx[pbdev].mode=2;
		pbctx[pbdev++].dev=optarg;
		break;

	case 'w':
		if((delay=atoi(optarg))<1||delay>100)usage();
		break;

	case 'l':
		if((lat=atoi(optarg))<0||lat>9999)usage();
		break;

	case 'r':
		if((rt=atoi(optarg))<1||rt>99)usage();
		break;

	case 'c':
		if((cpu=atoi(optarg))<0||cpu>1023)usage();
		break;

	case 'e':
		ev=1;
		break;

	default:usage();
	}

	if(!ndev)usage();

	sigfillset(&set);
	sigprocmask(SIG_BLOCK,&set,NULL);
	sigemptyset(&set);
	sigaddset(&set,SIGINT);
	sigaddset(&set,SIGHUP);
	sigaddset(&set,SIGQUIT);
	sigaddset(&set,SIGTERM);
	if((sfd=signalfd(-1,&set,SFD_NONBLOCK|SFD_CLOEXEC))==-1)
	{
		perror("signalfd");
		goto err1;
	}
	p.fd=sfd;
	p.events=POLLIN;

	if(cpu!=-1)
	{
		CPU_ZERO(&core);
		CPU_SET(cpu,&core);
		if(sched_setaffinity(0,sizeof(cpu_set_t),&core))
		{
			perror("sched_setaffinity");
			goto err2;
		}
	}

	if(rt)
	{
		prm.sched_priority=rt;
		if(sched_setscheduler(0,SCHED_RR,&prm))
		{
			perror("sched_setscheduler");
			goto err2;
		}
	}

	if(lat!=-1)
	{
		if((fd=open("/dev/cpu_dma_latency",O_WRONLY|O_CLOEXEC))==-1)
		{
			perror("open");
			goto err2;
		}
		if(write(fd,&lat,sizeof(lat))!=sizeof(lat))
		{
			perror("write");
			goto err3;
		}
	}

	for(i=0;i<ssdev;i++)if(opendev(ssctx[i].dev,1,&ssctx[i]))
	{
		fprintf(stderr,"can't open %s\n",ssctx[i].dev);
		goto err4;
	}
	for(i=0;i<psdev;i++)if(opendev(psctx[i].dev,1,&psctx[i]))
	{
		fprintf(stderr,"can't open %s\n",psctx[i].dev);
		goto err5;
	}
	for(i=0;i<pbdev;i++)if(opendev(pbctx[i].dev,1,&pbctx[i]))
	{
		fprintf(stderr,"can't open %s\n",pbctx[i].dev);
		goto err6;
	}

	start=time(NULL);

	if(ssdev)
	{
		ss.ctx=ssctx;
		ss.ndev=ssdev;
		ss.delay=delay*1000;
		ss.term=0;
		if(pthread_create(&ssth,NULL,serial_single_worker,&ss))
		{
			perror("pthread_create");
			goto err6;
		}
	}

	for(i=0;i<psdev;i++)if(pthread_create(&psth[i],NULL,
		parallel_single_worker,&psctx[i]))
	{
		perror("pthread_create");
		goto err7;
	}

	for(i=0;i<pbdev;i++)if(pthread_create(&pbth[i],NULL,
		parallel_block_worker,&pbctx[i]))
	{
		perror("pthread_create");
		goto err8;
	}

	err=0;

	sleep(4);

	while(1)
	{
		if(poll(&p,1,500)!=0)break;

		now=time(NULL)-start;

		if(!ev)for(i=0;i<ndev;i++)printf("%7llu ",
			__atomic_load_n(&ctx[i]->sum,__ATOMIC_SEQ_CST));
		else for(i=0;i<ndev;i++)printf("%7llu/%4llu ",
			__atomic_load_n(&ctx[i]->sum,__ATOMIC_SEQ_CST),
			__atomic_load_n(&ctx[i]->ev,__ATOMIC_SEQ_CST)/now);
		printf("\n");
	}

	printf("\n");

err8:	for(i=0;i<pbdev;i++)pbctx[i].term=1;
	for(i=0;i<pbdev;i++)if(pbth[i])pthread_join(pbth[i],NULL);
err7:	for(i=0;i<psdev;i++)psctx[i].term=1;
	for(i=0;i<psdev;i++)if(psth[i])pthread_join(psth[i],NULL);
	ss.term=1;
	if(ssdev)pthread_join(ssth,NULL);
err6:	for(i=0;i<pbdev;i++)closedev(&pbctx[i]);
err5:	for(i=0;i<psdev;i++)closedev(&psctx[i]);
err4:	for(i=0;i<ssdev;i++)closedev(&ssctx[i]);
err3:	if(fd!=-1)close(fd);
err2:	if(sfd!=-1)close(sfd);
err1:	return err;
}
