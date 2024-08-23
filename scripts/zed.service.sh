#!/bin/bash


syslog_echo()
{
	/usr/bin/syslog -s -l notice "$1"
}

if [ -c /dev/zfs ] ; then
	if [ x"$(pgrep zed)" = x ] ; then
		/usr/local/sbin/zed
		if [ x"$(pgrep zed)" = x ] ; then
			syslog_echo "zed failed to start"
		else
			syslog_echo "zed started"
		fi
	fi
else
	if [ x"$(pgrep zed)" != x ] ; then
		sleep 1
		zedpid=`pgrep zed`
		if [ x"${zedpid}" != x ] ; then
			kill ${zedpid}
		fi
		if [ x"$(pgrep zed)" != x ] ; then
			syslog_echo "zed failed to stop"
		else
			syslog_echo "zed stopped"
		fi
	fi
fi
