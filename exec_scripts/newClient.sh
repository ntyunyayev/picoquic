
#!/bin/bash
sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH ./dpdk_picoquicdemo --dpdk -l 0-1 -a 0000:8a:00.1 -- -X -D -A 50:6b:4b:f3:7c:71 -* 32 -@ 32 10.100.0.2 4443 /10000000000
#sudo ./dpdk_picoquicdemo dpdk -l 0-15 -a 0000:51:00.2 -a 0000:51:00.3 -a 0000:51:00.4 -a 0000:51:00.5 -a 0000:51:00.6 -a 0000:51:00.7 -a 0000:51:01.0 -a 0000:51:01.1  -- -A 50:6b:4b:f3:7c:70 -D localhost 4443 /1000000000
