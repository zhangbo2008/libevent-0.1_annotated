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
#define LOG_DBG(x) // Ҳ��������Ϊ�պ���,������.
#define log_error(x) perror(x)
#endif

#include "event.h"

#ifndef howmany
#define howmany(x, y) (((x) + ((y)-1)) / (y))
#endif

/* Prototypes */
void event_add_post(struct event *); //��Ϊ������ļ����ڲ�����,���Բ�д��.h����.ֻд��.c����.
//����ȫ�ֱ�����event������.
TAILQ_HEAD(timeout_list, event)
timequeue;
TAILQ_HEAD(event_wlist, event)
writequeue;
TAILQ_HEAD(event_rlist, event)
readqueue;
TAILQ_HEAD(event_ilist, event)
addqueue; //����4��queue��ͷ

int event_inloop = 0; // �ж��Ƿ��Ѿ��������¼�������.
int event_fds;		  /* Highest fd in fd set */
int event_fdsz;
fd_set *event_readset;
fd_set *event_writeset;

void event_init(void)
{ //�������ȫ�ֱ������г�ʼ��.
	TAILQ_INIT(&timequeue);
	TAILQ_INIT(&writequeue);
	TAILQ_INIT(&readqueue);
	TAILQ_INIT(&addqueue); //��Щ�¼�
}

/*
 * Called with the highest fd that we know about.  If it is 0, completely
 * recalculate everything.  ���¼���fd,��fdset���ڴ�. ����������0,��ô�͸��������������ݽ�������.��������÷�����set�¼�֮��,Ȼ������������events_recalc(0)����Ĭ�����ü���. �������ȫ�ֱ��� event_readset  �� event_writeset
 */

int events_recalc(int max) //����fd ���ֵ.Ȼ�����ǽ��з����ڴ�.
{
	fd_set *readset, *writeset; // ʹ��select
	struct event *ev;
	int fdsz;

	event_fds = max;

	if (!event_fds) //�����0,��ô�ͱ���������. ��д����һ�鼴��.
	{
		TAILQ_FOREACH(ev, &writequeue, ev_write_next) //������һ������,Ȼ������һ�н��д���ÿһ�α���ʱ��.����������for�����,�����ṩfor��ͷ.
		if (ev->ev_fd > event_fds)
			event_fds = ev->ev_fd;

		TAILQ_FOREACH(ev, &readqueue, ev_read_next)
		if (ev->ev_fd > event_fds)
			event_fds = ev->ev_fd;
	}
	/* Number of bits per word of `fd_set' (some code assumes this is 32).  */ //�ļ������������λ��. ���fd �����ж��ٸ��ļ�������.
	fdsz = howmany(event_fds + 1, NFDBITS) * sizeof(fd_mask);
	if (fdsz > event_fdsz)
	{ //���ݴ�С, �����ڴ漴��.
		if ((readset = realloc(event_readset, fdsz)) == NULL)
		{ // realloc:������·���ɹ��򷵻�ָ�򱻷����ڴ��ָ�룬Ҳ���Ƿ����ַ���׵�ַ,���򷵻ؿ�ָ��NULL��
			log_error("malloc");
			return (-1);
		}

		if ((writeset = realloc(event_writeset, fdsz)) == NULL)
		{
			log_error("malloc");
			free(readset);
			return (-1);
		}
		//������˵���������ɹ���.
		memset(readset + event_fdsz, 0, fdsz - event_fdsz); // ��readset + event_fdsz ���� fdsz - event_fdsz��ô�����ֵΪ0.  event_fdsz ���Ѿ�����õ�. �������䵽fdsz, ����Ĳ���ȫŪ0����.
		memset(writeset + event_fdsz, 0, fdsz - event_fdsz);

		event_readset = readset;
		event_writeset = writeset;
		event_fdsz = fdsz;
	}

	return (0);
}

int event_dispatch(void)
{					   //���¼�ѭ������.
	struct timeval tv; // timevalue
	struct event *ev, *old;
	int res, maxfd;

	/* Calculate the initial events that we are waiting for */
	if (events_recalc(0) == -1)
		return (-1);

	while (1)
	{
		memset(event_readset, 0, event_fdsz); //ÿ�����������fd,��Щ��select��׼�÷�.
		memset(event_writeset, 0, event_fdsz);

		//д��fd_set����.
		TAILQ_FOREACH(ev, &writequeue, ev_write_next)
		FD_SET(ev->ev_fd, event_writeset); // FD_SET(fd, &set); /*��fd����set����*/

		TAILQ_FOREACH(ev, &readqueue, ev_read_next)
		FD_SET(ev->ev_fd, event_readset);

		timeout_next(&tv);

		if ((res = select(event_fds + 1, event_readset,
						  event_writeset, NULL, &tv)) == -1)
		{ //����select����. �������: ��һ�������fd����+1, Ȼ���Ƕ�дfdset,exceptdf=NULL, tv�ǳ�ʱʱ��.
			if (errno != EINTR)
			{
				log_error("select"); //��ӡ������Ϣ.
				return (-1);
			}
			continue;
		}

		LOG_DBG((LOG_MISC, 80, __FUNCTION__ ": select reports %d",
				 res));
		//��ʼ����.���е�����˵��select���涫��������.��FD_ISSET��ȡ.
		maxfd = 0; //��Ϊ������Ҫɾ���¼���. ����maxfd��Ҫ���¼�����.
		event_inloop = 1;
		for (ev = TAILQ_FIRST(&readqueue); ev;) // for����ֹ��������ev!=NULL
		{
			old = TAILQ_NEXT(ev, ev_read_next);		// old����һ��ʱ��.
			if (FD_ISSET(ev->ev_fd, event_readset)) //�Ƕ��¼�.��ô��ɾ��ev,����cb����.
			{
				event_del(ev); // ע���������߼�,ÿһ��ʱ�䴥��֮��,��ɾ����.
				(*ev->ev_callback)(ev->ev_fd, EV_READ,
								   ev->ev_arg);
			}
			else if (ev->ev_fd > maxfd) //����һ�����ֵ.����set�����ʾ��������¼�,��ô���Ǿͼ���maxfd
				maxfd = ev->ev_fd;

			ev = old; //�������ƶ�.
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

		//���е���˵�����ε�select���������.����add�¼�. �����߼������治һ��,add���Ѿ�ͨ��add�������뵽queue������.��������Ҫ����queue����.ÿһ��select����֮����д���add. ��Ϊ�����ᱣ֤�ϴ�add֮ǰ�Ľ��select����.�ٴ���select֮��Ľ��.
		event_inloop = 0;

		for (ev = TAILQ_FIRST(&addqueue); ev;
			 ev = TAILQ_FIRST(&addqueue)) //���ѭ��ÿһ��ev������Ϊ����ͷ.
		{
			TAILQ_REMOVE(&addqueue, ev, ev_add_next); // queue��ȥ��ev. ÿһ��add�¼������д���.

			ev->ev_flags &= ~EVLIST_ADD; // add�¼�ûʲôcb����.����flag����. ��ӵľ���ʵ����event_add_post(ev);ʵ��.
			event_add_post(ev); //������307�еĲ���. 307���ڷ�inloopʱ��ֻ�����˶�д.��������¼������н��д���. �����߼�����flag��ɾ��add��־,Ȼ���ٵ���add_post�������������Ķ�д��־.

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
 * Checks if a specific event is pending or scheduled.     pending��ʾ�Ѿ�added, scheduled��ʾ��û��add��.��Ҫ����һ��dispatchʱ���ټ�.
 */

int event_pending(struct event *ev, short event, struct timeval *tv)
{
	int flags = ev->ev_flags;

	/*
	 * We might not have been able to add it to the actual queue yet,
	 * check if we will enqueue later.
	 */
	if (ev->ev_flags & EVLIST_ADD)
		flags |= (ev->ev_events & (EV_READ | EV_WRITE)); // flags��ȡ��д����.

	event &= (EV_TIMEOUT | EV_READ | EV_WRITE);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) //�������timeout�Ķ�д.�͸���ʱ��.
		*tv = ev->ev_timeout;

	return (flags & event); //�����Ƿ����¼���д.
}
//����¼���ev����. �������flag������, Ҳ�����Ӧ��������.
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
		if (ev->ev_flags & EVLIST_TIMEOUT) // һ����˵��ʱflagӦ����EVLIST_INIT,���������flag�������EVLIST_INIT��ô˵�������߼��߹�.������Ҫ��ɾ��֮ǰ����Ľ��,���²���.����֤�²���ļ�ʱ��!!!!!!!
			TAILQ_REMOVE(&timequeue, ev, ev_timeout_next);

		/* Insert in right temporal order */
		for (tmp = TAILQ_FIRST(&timequeue); tmp; tmp = TAILQ_NEXT(tmp, ev_timeout_next)) //�������еĳ�ʱ�¼�.
		{
			if (timercmp(&ev->ev_timeout, &tmp->ev_timeout, <=))
				break; //���Ҫ��ӵ��¼��ĳ�ʱ�¼�С�ڱ�������tmp�ĳ�ʱʱ��.��ô����tmpǰ����뼴��. �������ǵĳ�ʱ�¼�������������.
		}
		if (tmp)
			TAILQ_INSERT_BEFORE(tmp, ev, ev_timeout_next);
		else
			TAILQ_INSERT_TAIL(&timequeue, ev, ev_timeout_next);

		ev->ev_flags |= EVLIST_TIMEOUT; //���лᵼ��278��������addͬһ��eventʱ�򴥷�.
	}

	if (event_inloop) // �ж��Ƿ��Ѿ��������¼�������. ����Ѿ�������.���ǾͲ��޸Ķ�д. ֻ��������¼�.��Ϊ��д�¼��ڼ���ʱ��,��д���ݻ���ʹ����,��Ҫȥ������.�������ݻ���. ����add�¼����Լ���,���ǲ�����.
	{
		/* We are in the event loop right now, we have to
		 * postpone the change until later.
		 */
		if (ev->ev_flags & EVLIST_ADD) //�Ѿ���Ӻñ�־��.�Ͳ��ô�����.flag��ʾ�Ѿ�����ev����.// ��������Ϊ304��������. ��addͬһ���¼�2��ʱ��ᷢ��.�Ѿ��ӹ��˾�ֱ��return����.
			return;

		TAILQ_INSERT_TAIL(&addqueue, ev, ev_add_next); //����ͼӱ�־.
		ev->ev_flags |= EVLIST_ADD; //��ط��߼��ܾ���!!!!!!!!!Ҳ��flag��������������. ���in_loop״̬�����Ǿͽ���ֻ��flag, ��addqueue�Ĵ���, ������flag�����д���ֵĴ���,�Ӷ���֤���ٽ���Դ�ı���!!!!!!!!!!!!!!!
	}
	else					//�������ѭ����,��ô���ǿ���ֱ���޸�ȫ������.
		event_add_post(ev); //�������Ǿͼ����д��־. ע���������eventֻ�����д��.����add���¼�������209�д���.
}

void event_add_post(struct event *ev) //���events�����ж�д�¼�,����flags����û�о�����һ��.
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

	if (ev->ev_flags & EVLIST_ADD) //���flag�������add��־
	{
		TAILQ_REMOVE(&addqueue, ev, ev_add_next);

		ev->ev_flags &= ~EVLIST_ADD; //��ô����������־.  ͨ����addȡ��Ȼ��ȡ������.
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

	if ((ev = TAILQ_FIRST(&timequeue)) == NULL) //���ʱ���¼������ǿյ�,
	{
		timerclear(tv); // ��ôtv�����.
		tv->tv_sec = TIMEOUT_DEFAULT;
		return (0);
	}

	if (gettimeofday(&now, NULL) == -1)
		return (-1); //��ȡ����ʱ��ŵ�now����.

	if (timercmp(&ev->ev_timeout, &now, <=))
	{ //����ʱ���now�Ƚ�. ����һ���Ƿ�<=�ڶ���. ����ɹ���.˵�����ڹ�ʱ��.��ôtvû������.�����.
		timerclear(tv);
		return (0);
	}

	timersub(&ev->ev_timeout, &now, tv); // ����ŵ�tv����. Ȼ���ӡ˵��ʣ������͹�ʱ.

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
			break;									   //���ڻ�û��timeout����Ҫ����,ֱ��break��.
													   //���洦��ʱ.
		TAILQ_REMOVE(&timequeue, ev, ev_timeout_next); //��ô���ڶ�������ɾ�����ev
		ev->ev_flags &= ~EVLIST_TIMEOUT;			   //Ȼ��ev����flag

		LOG_DBG((LOG_MISC, 60, "timeout_process: call %p",
				 ev->ev_callback));
		(*ev->ev_callback)(ev->ev_fd, EV_TIMEOUT, ev->ev_arg); //����cb����. ��һ�����ļ�,�ڶ����Ǵ���ʲôʱ��,�������ǲ���.
	}
}
