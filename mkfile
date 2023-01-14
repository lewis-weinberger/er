# Based on mkfile from mischief/hack9
# Should work for Plan9 and plan9port

MKSHELL=$PLAN9/bin/rc
SYS=`{test -e /dev/cons && echo plan9 || echo p9p}

<plan9/mk.$SYS
