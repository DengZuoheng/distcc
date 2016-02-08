/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */


                /* I put the shotgun in an Adidas bag and padded it
                 * out with four pairs of tennis socks, not my style
                 * at all, but that was what I was aiming for: If they
                 * think you're crude, go technical; if they think
                 * you're technical, go crude.  I'm a very technical
                 * boy.  So I decided to get as crude as possible.
                 * These days, though, you have to be pretty technical
                 * before you can even aspire to crudeness.
                 *              -- William Gibson, "Johnny Mnemonic" */
                //              -- 以前的人都是这样装逼的吗-_-


/**
 * @file
 *
 * Routines to decide on which machine to run a distributable job.
 //决定用哪个machine来执行分布式job
 *
 * The current algorithm (new in 1.2 and subject to change) is as follows.
 //卧槽, 这还是有算法的

 * CPU lock is held until the job is complete.
 *
 // 一直拿着cpu锁知道job完成
 * Once the request has been transmitted, the lock is released and a second
 * job can be sent.
 *
 // 一旦request传输完成. lock就释放然后下一个job就可以发送.

 * Servers which wish to limit their load can defer accepting jobs, and the
 * client will block with that lock held.
 // 如果server想限制他们的负载, 那么他们可以延迟接收的job, 这样, cklient会阻塞并
 //一直拿着锁
 *
 * cpp is probably cheap enough that we can allow it to run unlocked.  However
 * that is not true for local compilation or linking.
 *
 // c++也许比较低成本以至于我们可以不加锁就跑(卧槽), 然而, 本地不能这样玩
 * @todo Write a test harness for the host selection algorithm.  Perhaps a
 * really simple simulation of machines taking different amounts of time to
 * build stuff?
 *///todo:我们得写个测试

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/file.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "hosts.h"
#include "lock.h"
#include "where.h"
#include "exitcode.h"


static int dcc_lock_one(struct dcc_hostdef *hostlist,
                        struct dcc_hostdef **buildhost,
                        int *cpu_lock_fd);


int dcc_pick_host_from_list_and_lock_it(struct dcc_hostdef **buildhost,
                            int *cpu_lock_fd)
{
    struct dcc_hostdef *hostlist;
    int ret;
    int n_hosts;

    if ((ret = dcc_get_hostlist(&hostlist, &n_hosts)) != 0) {
        return EXIT_NO_HOSTS;
    }
    //移除掉不可用的
    if ((ret = dcc_remove_disliked(&hostlist)))//在backoff.c中实现
        return ret;

    if (!hostlist) {
        return EXIT_NO_HOSTS;
    }

    return dcc_lock_one(hostlist, buildhost, cpu_lock_fd);//后两者是返回值

    /* FIXME: Host list is leaked? *///泄露关我屁事, 反正golang有gc
}


static void dcc_lock_pause(void)
{
    /* This could do with some tuning.
     *
     * My assumption basically is that polling a little too often is
     * relatively cheap; sleeping when we should be working is bad.  However,
     * if we hit this code at all we're overloaded, so sleeping a while is
     * perhaps OK.
     //我觉得多轮询几次成本也不会太高, 但是我们需要work的时候睡了, 就不好了
     // 然后, 我们会运行到这里说明系统已经过载了, 所以睡会应该没关系
     *
     * We don't use exponential backoff, because that would tend to prefer
     * later arrivals and penalize jobs that have been waiting for a long
     * time.  This would mean more compiler processes hanging around than is
     * really necessary, and also by making jobs complete very-out-of-order is
     * more likely to find Makefile bugs. */
     // 我们没有采用指数的增长, 是因为我们担心睡太久, 这意味着我们的编译进程超过其
     //所需的量, 也意味着我们job的完成顺序不对, 很可能是makefile的问题

    unsigned pause_time_ms = 1000;

    char *pt = getenv("DISTCC_PAUSE_TIME_MSEC");//环境变量可以设置睡多少毫秒
    if (pt)
	pause_time_ms = atoi(pt);

	/*	This call to dcc_note_state() is made before the host is known, so it
		does not make sense and does nothing useful as far as I can tell.	*/
    /*	dcc_note_state(DCC_PHASE_BLOCKED, NULL, NULL, DCC_UNKNOWN);	*/

    rs_trace("nothing available, sleeping %ums...", pause_time_ms);

    if (pause_time_ms > 0)
	usleep(pause_time_ms * 1000);
}


/**
 * Find a host that can run a distributed compilation by examining local state.
 * It can be either a remote server or localhost (if that is in the list).
 *
 //通过检查本地状态, 找到一个可以分布式编译的主机

 * This function does not return (except for errors) until a host has been
 * selected.  If necessary it sleeps until one is free.
 *
 // 这个函数会阻塞直到有一个找到一个host
 * @todo We don't need transmit locks for local operations.
 //todo: 本地操作的话, 我们并不需要传输锁(但是这个函数里面就没有传输锁的操作啊, 这什么意思啊?)
 **/
static int dcc_lock_one(struct dcc_hostdef *hostlist,
                        struct dcc_hostdef **buildhost,//后两参数应该是返回值
                        int *cpu_lock_fd)
{
    struct dcc_hostdef *h;
    int i_cpu;
    int ret;

    while (1) {
        for (i_cpu = 0; i_cpu < 50; i_cpu++) {//这个50是什么意思?说明i_cpu只在[0,50]中取?
            for (h = hostlist; h; h = h->next) {//遍历hostlist, 找到一个可以锁的
                if (i_cpu >= h->n_slots)//如果i_cpu大于host的slot数量, 就不用它
                    //n_slots指出这个host能跑几个job, 是用户设置的, 这里的遍历也会遍历slot号
                    // 如果所有host的1号slot都锁了, 才找2号slot, 也算一种负载均衡策略吧
                    //至少不会把一号host跑满
                    continue;
                //然后尝试去获取锁
                ret = dcc_lock_host("cpu", h, i_cpu, 0, cpu_lock_fd);//这个定义在lock.c

                if (ret == 0) {
                    *buildhost = h;
                    //这个实现在state.c, 应该是i_cpu是一个slot, 传到my_state的全局变量
                    dcc_note_state_slot(i_cpu, strcmp(h->hostname, "localhost") == 0 ? DCC_LOCAL : DCC_REMOTE);
                    return 0;
                    //note完就返回了
                } else if (ret == EXIT_BUSY) {
                    continue;
                } else {
                    rs_log_error("failed to lock");
                    return ret;
                }
            }
        }
        //阻塞的, 知道可以获取锁
        dcc_lock_pause();
    }
}



/**
 * Lock localhost.  Used to get the right balance of jobs when some of
 * them must be local.
 **/
int dcc_lock_local(int *cpu_lock_fd)
{
    struct dcc_hostdef *chosen;

    return dcc_lock_one(dcc_hostdef_local, &chosen, cpu_lock_fd);
}

int dcc_lock_local_cpp(int *cpu_lock_fd)
{
    int ret;
    struct dcc_hostdef *chosen;
    //dcc_lock_one的第一个参数是host_list, 后面两个参数是返回值
    //dcc_host_local_cpp是一个全局对象, 定义与lock.c
    ret = dcc_lock_one(dcc_hostdef_local_cpp, &chosen, cpu_lock_fd);
    if (ret == 0) {
        //DCC_PHASE_CPP是一个slot号
        dcc_note_state(DCC_PHASE_CPP, NULL, chosen->hostname, DCC_LOCAL);
    }
    return ret;
}
