#!/bin/bash

rsync -ar module/zfs/zfs.kext/ /tmp/zfs.kext/
rsync -ar ../spl/module/spl/spl.kext/ /tmp/spl.kext/

chown -R root:wheel /tmp/spl.kext /tmp/zfs.kext

kextload -v /tmp/spl.kext

kextload -v -r /tmp/spl.kext /tmp/zfs.kext


# script -q /dev/null   log stream --source --predicate 'senderImagePath CONTAINS "zfs" OR senderImagePath CONTAINS "spl"' | cut -c 80-

# log stream --source --predicate 'sender == "zfs" OR sender == "spl"' --style compact