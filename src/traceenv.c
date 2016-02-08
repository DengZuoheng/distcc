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


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "util.h"


/**
 * Setup client error/trace output.
 *
 // 相当于设置客户端的error, reace输出
 * Trace goes to the file specified by DISTCC_LOG, if any.  Otherwise, it goes
 * to stderr, except that UNCACHED_ERR_FD can redirect it elsewhere, for use
 * under ccache.
 *
 // 如果环境变量中有distcc_log, 就用之, 否则用stderr, 但是, 在用了ccache的情况下, 
 // 用UNCACHED_ERR_FD
 * The exact setting of log level is a little strange, but for a good
 * reason: if you ask for verbose, you get everything.  Otherwise, if
 * you set a file, you get INFO and above.  Otherwise, you only get
 * WARNING messages.  In practice this seems to be a nice balance.
 **/
 //应该能用golang的log模块代替
void dcc_set_trace_from_env(void)
{
    const char *logfile, *logfd_name;
    int fd;
    int failed_to_open_logfile = 0;
    int save_errno = 0;
    int level = RS_LOG_WARNING; /* by default, warnings only */

    /* let the decision on what to log rest on the loggers */
    /* the email-an-error functionality in emaillog.c depends on this */
    rs_trace_set_level(RS_LOG_DEBUG);

    if ((logfile = getenv("DISTCC_LOG")) && logfile[0]) {//获取环境变量并检查是否合法
        fd = open(logfile, O_WRONLY|O_APPEND|O_CREAT, 0666);
        if (fd != -1) {
            /* asked for a file, and we can open that file:
               include info messages */
            //如果成功开了日志文件, 我们就把level设置成info
            level = RS_LOG_INFO;
        } else {
            /* asked for a file, can't use it; use stderr instead */
            fd = STDERR_FILENO;
            save_errno = errno;
            failed_to_open_logfile = 1;
            //不行我们就把日志写到std_err, log level设为warning
        }
    } else {// 拿不到distcc_log, 就尝试拿uncached_err_fd, 
        /* not asked for file */
        if ((logfd_name = getenv("UNCACHED_ERR_FD")) == NULL ||
            (fd = atoi(logfd_name)) == 0) { //fd是直接拿来用的, 如果是0, 说明是没用的, 
            //没用的fd就换成std_err
            fd = STDERR_FILENO;
        }
    }

    if (dcc_getenv_bool("DISTCC_VERBOSE", 0)) {
        level = RS_LOG_DEBUG;//如果设置了distcc_verbose, 就把level设置为debug
    }
    //rs_logger_file是一个函数, 定义在trace.h, 实现在trace.c
    //rs_add_logger同理
    rs_add_logger(rs_logger_file, level, NULL, fd);

    //记得处理logfile开不成功的情况
    if (failed_to_open_logfile) {
        rs_log_error("failed to open logfile %s: %s",
                     logfile, strerror(save_errno));
    }
}
