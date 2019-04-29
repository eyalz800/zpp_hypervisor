#!/bin/bash
sshpass -p "{{ssh_password}}" scp -P {{ssh_port}} {{project_root}}/out/debug/native/zpp_loader.ko {{ssh_target}}:/tmp
sshpass -p "{{ssh_password}}" ssh -t {{ssh_target}} -p {{ssh_port}} 'sudo insmod /tmp/zpp_loader.ko 2> /dev/null'
