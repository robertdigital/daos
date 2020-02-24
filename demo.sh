#!/usr/bin/env bash

shopt -s expand_aliases
#alias daosctl="./install/bin/daosctl"
alias daosctl="./cmake-build-debug/daosctl"


function pause() {
	read -p "Press ENTER key to continue ..."
}
set -v

clear
rm /tmp/daos.log
#rm /tmp/daos_server.log -> has to be deleted before server restart

export D_LOG_MASK=DEBUG
export DD_SUBSYS=csum

daosctl destroy-pool --all -f

daosctl create-pool --size 1G


# Create container with checksum DISABLED and do I/O
# might need to specify ranks (-l0,1)???
#pause
daosctl create-container

daosctl write-string -x  --string "hello world"
daosctl read-string


#exit 0

# Write an array
daosctl write-string -x  --string "hello world"
daosctl read-string  --length 12
daosctl destroy-container
cat /tmp/daos.log


# Create container with checksum ENABLED and do some I/O
pause
daosctl create-container --csum-type=crc32
daosctl write-string -x  --string "hello world"
daosctl read-string  --length 12

# read so that server has to calculate checksums
pause
daosctl read-string  --length 11
daosctl destroy-container



# create object with --object-type=OC_RP_2G1




daosctl destroy-pool --all

# Cease displaying and stepping-thru scripted commands.
set +v
#trap debug