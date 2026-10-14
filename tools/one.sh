#!/bin/sh
exec ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$HOME/.ssh/known_hosts_owrt -i ~/.ssh/owrt_one root@192.168.68.73 "$@"
