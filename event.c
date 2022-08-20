/*
 * Copyright 2000 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Niels Provos.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifdef USE_LOG
#include "log.h"
#else
#define LOG_DBG(x) // 也就是设置为空函数,不处理.
#define log_error(x) perror(x)
#endif

#include "event.h"

#ifndef howmany
#define howmany(x, y) (((x) + ((y)-1)) / (y))
#endif

/* Prototypes */
void event_add_post(struct event *); //因为这个是文件的内部函数,所以不写在.h里面.只写在.c里面.
//定义全局变量在event里面用.
TAILQ_HEAD(timeout_list, event)
timequeue;
TAILQ_HEAD(event_wlist, event)
writequeue;
TAILQ_HEAD(event_rlist, event)
readqueue;
TAILQ_HEAD(event_ilist, event)
addqueue; //定义4个queue的头

int event_inloop = 0; // 判断是否已经在运行事件监听了.
int event_fds;		  /* Highest fd in fd set */
int event_fdsz;
fd_set *event_readset;
fd_set *event_writeset;

void event_init(void)
{ //对上面的全局变量进行初始化.
	TAILQ_INIT(&timequeue);
	TAILQ_INIT(&writequeue);
	TAILQ_INIT(&readqueue);
	TAILQ_INIT(&addqueue); //这些事件
}

/*
 * Called with the highest fd that we know about.  If it is 0, completely
 * recalculate everything.  冲新计算fd,和fdset的内存. 如果传入的是0,那么就根据链表里面数据进行配置.所以这个用法就是set事件之后,然后调用这个函数events_recalc(0)进行默认配置即可. 最后配置全局变量 event_readset  和 event_writeset
 */

int events_recalc(int max) //给与fd 最大值.然后我们进行分配内存.
{
	fd_set *readset, *writeset; // 使用select
	struct event *ev;
	int fdsz;

	event_fds = max;

	if (!event_fds) //如果是0,那么就遍历来更新. 读写都跑一遍即可.
	{
		TAILQ_FOREACH(ev, &writequeue, ev_write_next) //这行是一个遍历,然后下面一行进行处理每一次遍历时候.等于这个宏把for代码拆开,他来提供for的头.
		if (ev->ev_fd > event_fds)
			event_fds = ev->ev_fd;

		TAILQ_FOREACH(ev, &readqueue, ev_read_next)
		if (ev->ev_fd > event_fds)
			event_fds = ev->ev_fd;
	}
	/* Number of bits per word of `fd_set' (some code assumes this is 32).  */ //文件描述符的最大位数. 最大fd 里面有多少个文件描述符.
	fdsz = howmany(event_fds + 1, NFDBITS) * sizeof(fd_mask);
	if (fdsz > event_fdsz)
	{ //根据大小, 分配内存即可.
		if ((readset = realloc(event_readset, fdsz)) == NULL)
		{ // realloc:如果重新分配成功则返回指向被分配内存的指针，也就是分配地址的首地址,否则返回空指针NULL。
			log_error("malloc");
			return (-1);
		}

		if ((writeset = realloc(event_writeset, fdsz)) == NULL)
		{
			log_error("malloc");
			free(readset);
			return (-1);
		}
		//到这里说明上面分配成功了.
		memset(readset + event_fdsz, 0, fdsz - event_fdsz); // 从readset + event_fdsz 设置 fdsz - event_fdsz这么多个数值为0.  event_fdsz 是已经分配好的. 把他扩充到fdsz, 扩充的部分全弄0即可.
		memset(writeset + event_fdsz, 0, fdsz - event_fdsz);

		event_readset = readset;
		event_writeset = writeset;
		event_fdsz = fdsz;
	}

	return (0);
}

int event_dispatch(void)
{					   //让事件循环监听.
	struct timeval tv; // timevalue
	struct event *ev, *old;
	int res, maxfd;

	/* Calculate the initial events that we are waiting for */
	if (events_recalc(0) == -1)
		return (-1);

	while (1)
	{
		memset(event_readset, 0, event_fdsz); //每次运行先清空fd,这些是select标准用法.
		memset(event_writeset, 0, event_fdsz);

		//写入fd_set里面.
		TAILQ_FOREACH(ev, &writequeue, ev_write_next)
		FD_SET(ev->ev_fd, event_writeset); // FD_SET(fd, &set); /*将fd加入set集合*/

		TAILQ_FOREACH(ev, &readqueue, ev_read_next)
		FD_SET(ev->ev_fd, event_readset);

		timeout_next(&tv);

		if ((res = select(event_fds + 1, event_readset,
						  event_writeset, NULL, &tv)) == -1)
		{ //调用select即可. 参数详解: 第一个是最大fd数量+1, 然后是读写fdset,exceptdf=NULL, tv是超时时间.
			if (errno != EINTR)
			{
				log_error("select"); //打印出错信息.
				return (-1);
			}
			continue;
		}

		LOG_DBG((LOG_MISC, 80, __FUNCTION__ ": select reports %d",
				 res));
		//开始设置.运行到这里说明select里面东西触发了.用FD_ISSET读取.
		maxfd = 0; //因为下面需要删除事件了. 所以maxfd需要重新计算了.
		event_inloop = 1;
		for (ev = TAILQ_FIRST(&readqueue); ev;) // for的终止条件就是ev!=NULL
		{
			old = TAILQ_NEXT(ev, ev_read_next);		// old是下一个时间.
			if (FD_ISSET(ev->ev_fd, event_readset)) //是读事件.那么就删除ev,调用cb即可.
			{
				event_del(ev); // 注意这里面逻辑,每一次时间触发之后,就删除他.
				(*ev->ev_callback)(ev->ev_fd, EV_READ,
								   ev->ev_arg);
			}
			else if (ev->ev_fd > maxfd) //更新一下最大值.不在set里面表示保留这个事件,那么我们就计算maxfd
				maxfd = ev->ev_fd;

			ev = old; //链表下移动.
		}

		for (ev = TAILQ_FIRST(&writequeue); ev;)
		{
			old = TAILQ_NEXT(ev, ev_read_next);
			if (FD_ISSET(ev->ev_fd, event_writeset))
			{
				event_del(ev);
				(*ev->ev_callback)(ev->ev_fd, EV_WRITE,
								   ev->ev_arg);
			}
			else if (ev->ev_fd > maxfd)
				maxfd = ev->ev_fd;

			ev = old;
		}
		///////////////////////////////////////////////////

		//运行到这说明当次的select激活完成了.处理add事件. 处理逻辑跟上面不一样,add是已经通过add函数加入到queue里面了.这里面需要遍历queue即可.每一次select触发之后进行处理add. 因为这样会保证上次add之前的结果select跑完.再处理select之后的结果.
		event_inloop = 0;

		for (ev = TAILQ_FIRST(&addqueue); ev;
			 ev = TAILQ_FIRST(&addqueue)) //这个循环每一次ev都重置为链表头.
		{
			TAILQ_REMOVE(&addqueue, ev, ev_add_next); // queue中去掉ev. 每一个add事件都进行处理.

			ev->ev_flags &= ~EVLIST_ADD; // add事件没什么cb函数.设置flag即可. 添加的具体实现在event_add_post(ev);实现.
			event_add_post(ev); //这行是307行的补充. 307行在非inloop时候只处理了读写.所以添加事件在这行进行处理. 处理逻辑就是flag先删除add标志,然后再调用add_post来添加他立里面的读写标志.

			if (ev->ev_fd > maxfd)
				maxfd = ev->ev_fd;
		}

		if (events_recalc(maxfd) == -1)
			return (-1);

		timeout_process();
	}

	return (0);
}

void event_set(struct event *ev, int fd, short events,
			   void (*callback)(int, short, void *), void *arg)
{
	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;
	ev->ev_flags = EVLIST_INIT;
}

/*
 * Checks if a specific event is pending or scheduled.     pending表示已经added, scheduled表示还没有add上.需要在下一轮dispatch时候再加.
 */

int event_pending(struct event *ev, short event, struct timeval *tv)
{
	int flags = ev->ev_flags;

	/*
	 * We might not have been able to add it to the actual queue yet,
	 * check if we will enqueue later.
	 */
	if (ev->ev_flags & EVLIST_ADD)
		flags |= (ev->ev_events & (EV_READ | EV_WRITE)); // flags抽取读写部分.

	event &= (EV_TIMEOUT | EV_READ | EV_WRITE);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) //如果存在timeout的读写.就跟新时间.
		*tv = ev->ev_timeout;

	return (flags & event); //返回是否有事件读写.
}
//添加事件到ev里面. 具体就是flag设置上, 也加入对应的链表中.
void event_add(struct event *ev, struct timeval *tv)
{
	LOG_DBG((LOG_MISC, 55,
			 "event_add: event: %p, %s%s%scall %p",
			 ev,
			 ev->ev_events & EV_READ ? "EV_READ " : " ",
			 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
			 tv ? "EV_TIMEOUT " : " ",
			 ev->ev_callback));
	if (tv != NULL)
	{
		struct timeval now;
		struct event *tmp;

		gettimeofday(&now, NULL);
		timeradd(&now, tv, &ev->ev_timeout);

		LOG_DBG((LOG_MISC, 55,
				 "event_add: timeout in %d seconds, call %p",
				 tv->tv_sec, ev->ev_callback));
		if (ev->ev_flags & EVLIST_TIMEOUT) // 一般来说这时flag应该是EVLIST_INIT,如果这里面flag不是这个EVLIST_INIT那么说明下面逻辑走过.所以需要先删除之前插入的结果,重新插入.来保证新插入的及时性!!!!!!!
			TAILQ_REMOVE(&timequeue, ev, ev_timeout_next);

		/* Insert in right temporal order */
		for (tmp = TAILQ_FIRST(&timequeue); tmp; tmp = TAILQ_NEXT(tmp, ev_timeout_next)) //遍历所有的超时事件.
		{
			if (timercmp(&ev->ev_timeout, &tmp->ev_timeout, <=))
				break; //如果要添加的事件的超时事件小于遍历到的tmp的超时时间.那么就在tmp前面插入即可. 这样我们的超时事件队列升序排列.
		}
		if (tmp)
			TAILQ_INSERT_BEFORE(tmp, ev, ev_timeout_next);
		else
			TAILQ_INSERT_TAIL(&timequeue, ev, ev_timeout_next);

		ev->ev_flags |= EVLIST_TIMEOUT; //这行会导致278行再重新add同一个event时候触发.
	}

	if (event_inloop) // 判断是否已经在运行事件监听了. 如果已经监听了.我们就不修改读写. 只处理添加事件.因为读写事件在监听时候,读写数据还在使用中,不要去打扰他.否则数据混乱. 但是add事件可以加入,他们不干扰.
	{
		/* We are in the event loop right now, we have to
		 * postpone the change until later.
		 */
		if (ev->ev_flags & EVLIST_ADD) //已经添加好标志了.就不用处理了.flag表示已经加入ev中了.// 这行是因为304行来触发. 当add同一个事件2次时候会发生.已经加过了就直接return即可.
			return;

		TAILQ_INSERT_TAIL(&addqueue, ev, ev_add_next); //否则就加标志.
		ev->ev_flags |= EVLIST_ADD; //这地方逻辑很精髓!!!!!!!!!也是flag真正的意义所在. 如果in_loop状态中我们就进行只加flag, 和addqueue的处理, 不进行flag里面读写部分的处理,从而保证了临界资源的保护!!!!!!!!!!!!!!!
	}
	else					//如果不在循环中,那么我们可以直接修改全部数据.
		event_add_post(ev); //否则我们就加入读写标志. 注意这里面的event只处理读写的.对于add的事件我们在209行处理.
}

void event_add_post(struct event *ev) //如果events里面有读写事件,但是flags里面没有就设置一下.
{
	if ((ev->ev_events & EV_READ) && !(ev->ev_flags & EVLIST_READ))
	{
		TAILQ_INSERT_TAIL(&readqueue, ev, ev_read_next);

		ev->ev_flags |= EVLIST_READ;
	}

	if ((ev->ev_events & EV_WRITE) && !(ev->ev_flags & EVLIST_WRITE))
	{
		TAILQ_INSERT_TAIL(&writequeue, ev, ev_write_next);

		ev->ev_flags |= EVLIST_WRITE;
	}
}

void event_del(struct event *ev)
{
	LOG_DBG((LOG_MISC, 80, "event_del: %p, callback %p",
			 ev, ev->ev_callback));

	if (ev->ev_flags & EVLIST_ADD) //如果flag里面存着add标志
	{
		TAILQ_REMOVE(&addqueue, ev, ev_add_next);

		ev->ev_flags &= ~EVLIST_ADD; //那么就清楚这个标志.  通过跟add取反然后取交即可.
	}

	if (ev->ev_flags & EVLIST_TIMEOUT)
	{
		TAILQ_REMOVE(&timequeue, ev, ev_timeout_next);

		ev->ev_flags &= ~EVLIST_TIMEOUT;
	}

	if (ev->ev_flags & EVLIST_READ)
	{
		TAILQ_REMOVE(&readqueue, ev, ev_read_next);

		ev->ev_flags &= ~EVLIST_READ;
	}

	if (ev->ev_flags & EVLIST_WRITE)
	{
		TAILQ_REMOVE(&writequeue, ev, ev_write_next);

		ev->ev_flags &= ~EVLIST_WRITE;
	}
}

int timeout_next(struct timeval *tv)
{
	struct timeval now;
	struct event *ev;

	if ((ev = TAILQ_FIRST(&timequeue)) == NULL) //如果时间事件里面是空的,
	{
		timerclear(tv); // 那么tv就清空.
		tv->tv_sec = TIMEOUT_DEFAULT;
		return (0);
	}

	if (gettimeofday(&now, NULL) == -1)
		return (-1); //获取今天时间放到now里面.

	if (timercmp(&ev->ev_timeout, &now, <=))
	{ //过期时间跟now比较. 看第一个是否<=第二个. 如果成功了.说明现在过时了.那么tv没意义了.清空他.
		timerclear(tv);
		return (0);
	}

	timersub(&ev->ev_timeout, &now, tv); // 做差放到tv里面. 然后打印说还剩多少秒就过时.

	LOG_DBG((LOG_MISC, 60, "timeout_next: in %d seconds", tv->tv_sec));
	return (0);
}

void timeout_process(void)
{
	struct timeval now;
	struct event *ev;

	gettimeofday(&now, NULL);

	while ((ev = TAILQ_FIRST(&timequeue)) != NULL)
	{
		if (timercmp(&ev->ev_timeout, &now, >))
			break;									   //现在还没到timeout不需要处理,直接break掉.
													   //下面处理超时.
		TAILQ_REMOVE(&timequeue, ev, ev_timeout_next); //那么就在队列里面删除这个ev
		ev->ev_flags &= ~EVLIST_TIMEOUT;			   //然后ev设置flag

		LOG_DBG((LOG_MISC, 60, "timeout_process: call %p",
				 ev->ev_callback));
		(*ev->ev_callback)(ev->ev_fd, EV_TIMEOUT, ev->ev_arg); //触发cb函数. 第一个是文件,第二个是触发什么时间,第三个是参数.
	}
}
